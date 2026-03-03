#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h" 
#include "scheduler.h"
#include "relay.h"
#include "mqtt.h"
#include "platform.h"

static const char *TAG = "SCHEDULER";

RelaySchedule_t schedules[5]; 
sensor_config_t sensor_conf;
static int last_sensor_action = 0;

static void save_schedule(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "json_sched", schedules, sizeof(schedules));
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Schedule saved to NVS!");
    }
}

static void load_schedule(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(schedules);
        nvs_get_blob(h, "json_sched", schedules, &len);
        nvs_close(h);
        ESP_LOGI(TAG, "Schedule loaded from NVS!");
    } else {
        ESP_LOGW(TAG, "No saved schedule in NVS, using defaults.");
        memset(schedules, 0, sizeof(schedules));
    }
}

static void save_sensor_conf(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "sensor_blob", &sensor_conf, sizeof(sensor_config_t));
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Sensor Config saved to NVS!");
    }
}

static void load_sensor_conf(void) {
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(sensor_config_t);
        nvs_get_blob(h, "sensor_blob", &sensor_conf, &len);
        nvs_close(h);
        ESP_LOGI(TAG, "Sensor Config loaded from NVS!");
    } else {
        ESP_LOGW(TAG, "No sensor config in NVS. Init default.");
        memset(&sensor_conf, 0, sizeof(sensor_config_t));
        sensor_conf.action1_RL1 = -1; sensor_conf.action1_RL2 = -1; sensor_conf.action1_RL3 = -1; sensor_conf.action1_RL4 = -1; sensor_conf.action1_RL5 = -1;
        sensor_conf.action2_RL1 = -1; sensor_conf.action2_RL2 = -1; sensor_conf.action2_RL3 = -1; sensor_conf.action2_RL4 = -1; sensor_conf.action2_RL5 = -1;
        strncpy(sensor_conf.type, "temp", sizeof(sensor_conf.type) - 1);
        sensor_conf.day_mask = 0x7F;
        sensor_conf.temp_high = 100.0;
        sensor_conf.temp_low = -50.0;
    }
}

static int get_relay_val(cJSON *obj, const char *key_upper, const char *key_lower) {
    cJSON *item = cJSON_GetObjectItem(obj, key_upper);
    if (!item) item = cJSON_GetObjectItem(obj, key_lower);
    if (item) return item->valueint;
    return -1;
}

void scheduler_update_from_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON Malformed!");
        return;
    }
    for (int i = 0; i < 5; i++) {
        char key[10];
        snprintf(key, sizeof(key), "relay%d", i + 1); 
        
        cJSON *relay_obj = cJSON_GetObjectItem(root, key);
        
        if (relay_obj && !cJSON_IsNull(relay_obj)) {
            ESP_LOGI(TAG, "Processing %s...", key);
            
            schedules[i].day_mask = 0;
            cJSON *days = cJSON_GetObjectItem(relay_obj, "dayOfWeek");
            if (cJSON_IsArray(days)) {
                int count = cJSON_GetArraySize(days);
                for (int j = 0; j < count; j++) {
                    int d = cJSON_GetArrayItem(days, j)->valueint;
                    if (d >= 0 && d <= 6) {
                        schedules[i].day_mask |= (1 << d); 
                    }
                }
            }

            memset(schedules[i].slots, 0, sizeof(schedules[i].slots)); 
            
            cJSON *sched_arr = cJSON_GetObjectItem(relay_obj, "schedule");
            if (cJSON_IsArray(sched_arr)) {
                int count = cJSON_GetArraySize(sched_arr);
                if (count > MAX_SLOTS) count = MAX_SLOTS;

                for (int k = 0; k < count; k++) {
                    cJSON *item = cJSON_GetArrayItem(sched_arr, k);
                    cJSON *start = cJSON_GetObjectItem(item, "timeStart");
                    cJSON *stop = cJSON_GetObjectItem(item, "timeStop");

                    if (start && stop) {
                        schedules[i].slots[k].active = true;
                        schedules[i].slots[k].start.h = cJSON_GetObjectItem(start, "h")->valueint;
                        schedules[i].slots[k].start.m = cJSON_GetObjectItem(start, "m")->valueint;
                        schedules[i].slots[k].stop.h  = cJSON_GetObjectItem(stop, "h")->valueint;
                        schedules[i].slots[k].stop.m  = cJSON_GetObjectItem(stop, "m")->valueint;
                    }
                }
            }
        } 
        else if (relay_obj && cJSON_IsNull(relay_obj)) {
            ESP_LOGW(TAG, "Clearing Schedule for %s", key);
            memset(&schedules[i], 0, sizeof(RelaySchedule_t));
        }
    }
    
    cJSON *sensor_obj = cJSON_GetObjectItem(root, "sensor");
    if (sensor_obj) {
        ESP_LOGI(TAG, "Updating Sensor Config...");
        
        last_sensor_action = 0;

        sensor_conf.action1_RL1 = -1; sensor_conf.action1_RL2 = -1; sensor_conf.action1_RL3 = -1; sensor_conf.action1_RL4 = -1; sensor_conf.action1_RL5 = -1;
        sensor_conf.action2_RL1 = -1; sensor_conf.action2_RL2 = -1; sensor_conf.action2_RL3 = -1; sensor_conf.action2_RL4 = -1; sensor_conf.action2_RL5 = -1;

        cJSON *type = cJSON_GetObjectItem(sensor_obj, "type");
        if(type && type->valuestring) {
            strncpy(sensor_conf.type, type->valuestring, sizeof(sensor_conf.type) - 1);
            sensor_conf.type[sizeof(sensor_conf.type) - 1] = '\0';
        }
        
        sensor_conf.day_mask = 0;
        cJSON *days = cJSON_GetObjectItem(sensor_obj, "dayOfWeek");
        if (cJSON_IsArray(days)) {
            int count = cJSON_GetArraySize(days);
            for (int j = 0; j < count; j++) {
                int d = cJSON_GetArrayItem(days, j)->valueint;
                if (d >= 0 && d <= 6) sensor_conf.day_mask |= (1 << d);
            }
        } else sensor_conf.day_mask = 0x7F;

        cJSON *start = cJSON_GetObjectItem(sensor_obj, "timeStart");
        if (start) {
            sensor_conf.start.h = cJSON_GetObjectItem(start, "h")->valueint;
            sensor_conf.start.m = cJSON_GetObjectItem(start, "m")->valueint;
        }
        cJSON *stop = cJSON_GetObjectItem(sensor_obj, "timeStop");
        if (stop) {
            sensor_conf.stop.h = cJSON_GetObjectItem(stop, "h")->valueint;
            sensor_conf.stop.m = cJSON_GetObjectItem(stop, "m")->valueint;
        }

        cJSON *item;
        item = cJSON_GetObjectItem(sensor_obj, "temp_high"); if(item) sensor_conf.temp_high = item->valuedouble;
        item = cJSON_GetObjectItem(sensor_obj, "temp_low");  if(item) sensor_conf.temp_low  = item->valuedouble;
        
        item = cJSON_GetObjectItem(sensor_obj, "humi_high"); if(item) sensor_conf.humi_high = item->valuedouble;
        item = cJSON_GetObjectItem(sensor_obj, "humi_low");  if(item) sensor_conf.humi_low  = item->valuedouble;

        cJSON *act1 = cJSON_GetObjectItem(sensor_obj, "action1");
        if (act1) {
            sensor_conf.action1_RL1 = get_relay_val(act1, "RL1", "rl1");
            sensor_conf.action1_RL2 = get_relay_val(act1, "RL2", "rl2");
            sensor_conf.action1_RL3 = get_relay_val(act1, "RL3", "rl3");
            sensor_conf.action1_RL4 = get_relay_val(act1, "RL4", "rl4");
            sensor_conf.action1_RL5 = get_relay_val(act1, "RL5", "rl5");
        }

        cJSON *act2 = cJSON_GetObjectItem(sensor_obj, "action2");
        if (act2) {
            sensor_conf.action2_RL1 = get_relay_val(act2, "RL1", "rl1");
            sensor_conf.action2_RL2 = get_relay_val(act2, "RL2", "rl2");
            sensor_conf.action2_RL3 = get_relay_val(act2, "RL3", "rl3");
            sensor_conf.action2_RL4 = get_relay_val(act2, "RL4", "rl4");
            sensor_conf.action2_RL5 = get_relay_val(act2, "RL5", "rl5");
        }
        
        save_sensor_conf();
    }

    cJSON_Delete(root); 
    save_schedule();
}

void scheduler_process_sensor(float current_value) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if ((sensor_conf.day_mask & (1 << timeinfo.tm_wday)) == 0) return;

    int now_mins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int start_mins = sensor_conf.start.h * 60 + sensor_conf.start.m;
    int stop_mins = sensor_conf.stop.h * 60 + sensor_conf.stop.m;

    bool in_time_range = false;
    if (start_mins == stop_mins) {
        in_time_range = true; 
    } else if (start_mins < stop_mins) {
        if (now_mins >= start_mins && now_mins < stop_mins) in_time_range = true;
    } else {
        if (now_mins >= start_mins || now_mins < stop_mins) in_time_range = true;
    }
    if (!in_time_range) return;

    float high = 0, low = 0;
    bool is_temp = (strcmp(sensor_conf.type, "temp") == 0);

    if (is_temp) {
        sensor_conf.current_temp = current_value;
        high = sensor_conf.temp_high;
        low  = sensor_conf.temp_low;
    } else {
        sensor_conf.current_humi = current_value;
        high = sensor_conf.humi_high;
        low  = sensor_conf.humi_low;
    }

    if (current_value >= high) {
        if (last_sensor_action != 1) {
            ESP_LOGW(TAG, "Triggering Action 1");
            if (sensor_conf.action1_RL1 != -1) { relay_set(1, sensor_conf.action1_RL1); mqtt_send_state(1, sensor_conf.action1_RL1); }
            if (sensor_conf.action1_RL2 != -1) { relay_set(2, sensor_conf.action1_RL2); mqtt_send_state(2, sensor_conf.action1_RL2); }
            if (sensor_conf.action1_RL3 != -1) { relay_set(3, sensor_conf.action1_RL3); mqtt_send_state(3, sensor_conf.action1_RL3); }
            if (sensor_conf.action1_RL4 != -1) { relay_set(4, sensor_conf.action1_RL4); mqtt_send_state(4, sensor_conf.action1_RL4); }
            if (sensor_conf.action1_RL5 != -1) { relay_set(5, sensor_conf.action1_RL5); mqtt_send_state(5, sensor_conf.action1_RL5); }
            last_sensor_action = 1;
        }
    } else if (current_value <= low) {
        if (last_sensor_action != 2) {
            ESP_LOGW(TAG, "Triggering Action 2");
            if (sensor_conf.action2_RL1 != -1) { relay_set(1, sensor_conf.action2_RL1); mqtt_send_state(1, sensor_conf.action2_RL1); }
            if (sensor_conf.action2_RL2 != -1) { relay_set(2, sensor_conf.action2_RL2); mqtt_send_state(2, sensor_conf.action2_RL2); }  
            if (sensor_conf.action2_RL3 != -1) { relay_set(3, sensor_conf.action2_RL3); mqtt_send_state(3, sensor_conf.action2_RL3); }
            if (sensor_conf.action2_RL4 != -1) { relay_set(4, sensor_conf.action2_RL4); mqtt_send_state(4, sensor_conf.action2_RL4); }
            if (sensor_conf.action2_RL5 != -1) { relay_set(5, sensor_conf.action2_RL5); mqtt_send_state(5, sensor_conf.action2_RL5); }
            last_sensor_action = 2;
        }
    } else {
        if (last_sensor_action != 0) {
             last_sensor_action = 0; 
        }
    }
}

static bool parse_simple_time(const char *str, int *h, int *m) {
    if (sscanf(str, "%d:%d", h, m) == 2) {
        if (*h >= 00 && *h <= 23 && *m >= 00 && *m <= 59) return true;
    }
    return false;
}

void scheduler_set_on_time(int relay_idx, const char *time_str) {
    if (relay_idx < 1 || relay_idx > 5) return;
    int i = relay_idx - 1;
    int h, m;
    if (parse_simple_time(time_str, &h, &m)) {
        schedules[i].slots[0].active = true;
        schedules[i].slots[0].start.h = h;
        schedules[i].slots[0].start.m = m;
        if (schedules[i].day_mask == 0) schedules[i].day_mask = 0x7F; 
        save_schedule();
    }
}

void scheduler_set_off_time(int relay_idx, const char *time_str) {
    if (relay_idx < 1 || relay_idx > 5) return;
    int i = relay_idx - 1;
    int h, m;
    if (parse_simple_time(time_str, &h, &m)) {
        schedules[i].slots[0].active = true;
        schedules[i].slots[0].stop.h = h;
        schedules[i].slots[0].stop.m = m;
        if (schedules[i].day_mask == 0) schedules[i].day_mask = 0x7F;
        save_schedule();
    }
}

bool is_time_in_range(int now_h, int now_m, TimePoint_t start, TimePoint_t stop) {
    int now_mins = now_h * 60 + now_m;
    int start_mins = start.h * 60 + start.m;
    int stop_mins = stop.h * 60 + stop.m;

    if (start_mins < stop_mins) {
        return (now_mins >= start_mins && now_mins < stop_mins);
    } else if (start_mins > stop_mins) {
        return (now_mins >= start_mins || now_mins < stop_mins);
    } else {
        return false; 
    }
}

void scheduler_check_at_boot(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    int current_min_total = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int current_wday_bit = (1 << timeinfo.tm_wday); 
    ESP_LOGW(TAG, "Checking missed schedules (Power Recovery)...");

    for (int i = 0; i < 5; i++) {
        bool should_be_on = false;

        if ((schedules[i].day_mask & current_wday_bit) == 0) continue;
        for (int k = 0; k < MAX_SLOTS; k++) {
            if (!schedules[i].slots[k].active) continue;

            int start_total = schedules[i].slots[k].start.h * 60 + schedules[i].slots[k].start.m;
            int stop_total  = schedules[i].slots[k].stop.h * 60 +  schedules[i].slots[k].stop.m;

            if (start_total < stop_total) { 
                if (current_min_total >= start_total && current_min_total < stop_total) should_be_on = true;
            } else { 
                if (current_min_total >= start_total || current_min_total < stop_total) should_be_on = true;
            }
        }

        if (should_be_on) {
            ESP_LOGW(TAG, "Restoring Relay %d -> ON", i+1);
            relay_on(i+1);
            mqtt_send_state(i+1, 1);
        }
    }
}

void scheduler_task(void *arg) {
//     ESP_LOGI(TAG, "Starting SNTP...");

//     setenv("TZ", "CST-7", 1);
//     tzset();

//     esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
//     esp_sntp_setservername(0, "pool.ntp.org");
//     esp_sntp_setservername(1, "time.google.com");
//     esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    while (timeinfo.tm_year < (2016 - 1900)) {
        retry++;
        if (retry % 5 == 0) {
            ESP_LOGW(TAG, "Waiting for NTP time sync... (Attempt: %d - Ensure WiFi is connected)", retry);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    
    ESP_LOGI(TAG, "Time synced: %s", asctime(&timeinfo));

    scheduler_check_at_boot();

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_sec == 0) {
            
            int current_wday_bit = (1 << timeinfo.tm_wday);
            ESP_LOGI(TAG, "Tick: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
            
            for (int i = 0; i < 5; i++) {
                if ((schedules[i].day_mask & current_wday_bit) == 0) continue;

                for (int k = 0; k < MAX_SLOTS; k++) {
                    if (!schedules[i].slots[k].active) continue;

                    TimePoint_t *start = &schedules[i].slots[k].start;
                    TimePoint_t *stop  = &schedules[i].slots[k].stop;

                    if (start->h == timeinfo.tm_hour && start->m == timeinfo.tm_min) {
                        ESP_LOGW(TAG, "TRIGGER ON Relay %d", i+1);
                        relay_on(i+1);
                        mqtt_send_state(i+1, 1);
                    }

                    else if (stop->h == timeinfo.tm_hour && stop->m == timeinfo.tm_min) {
                        ESP_LOGW(TAG, "TRIGGER OFF Relay %d", i+1);
                        relay_off(i+1);
                        mqtt_send_state(i+1, 0);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1500)); 
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }  
}

void scheduler_init(void) {
    load_schedule();
    load_sensor_conf();
    xTaskCreate(scheduler_task, "sched_task", 4096, NULL, 5, NULL);
}