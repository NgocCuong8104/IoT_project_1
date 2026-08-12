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
#include "freertos/semphr.h"
#include "cJSON.h" 
#include "scheduler.h"
#include "relay.h"
#include "mqtt.h"
#include "platform.h"
#include "sht35.h"

static const char *TAG = "SCHEDULER";

RelaySchedule_t schedules[NUM_RELAYS]; // Mảng lưu trữ tối đa NUM_RELAYS lịch cho NUM_RELAYS relay (! chú ý chỗ này)
sensor_config_t sensor_conf;
input_state_t input_state;
sensor_network_t sensor_net;
static int last_sensor_action = 0;
static int last_processed_minute = -1;
static int set_time_from_rtc = 0;

// Flags để đánh dấu cần save dữ liệu (tránh block khi hold Mutex)
static bool need_save_schedule = false;
static bool need_save_input_state = false;
static bool need_save_sensor_network = false;

bool is_time_in_range(int now_h, int now_m, TimePoint_t start, TimePoint_t stop);

static void save_input_state(void); // lưu trạng thái input vào flash
void save_sensor_network(void); // lưu trữ thông tin cảm biến vào flash

// Mutex để bảo vệ truy cập vào lịch và cấu hình cảm biến, đảm bảo khi đọc/ghi từ MQTT hoặc các task khác
static SemaphoreHandle_t scheduler_mutex = NULL;    

SemaphoreHandle_t scheduler_get_mutex(void) {
    return scheduler_mutex;
}

void scheduler_lock(void) {
    if (scheduler_mutex) {
        xSemaphoreTake(scheduler_mutex, portMAX_DELAY);
    }
}

void scheduler_unlock(void) {
    if (scheduler_mutex) {
        xSemaphoreGive(scheduler_mutex);
    }
}

static int get_int_from_json(cJSON *parent, const char *key, int default_val) {
    if (!parent) return default_val;
    
    cJSON *item = cJSON_GetObjectItem(parent, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    
    ESP_LOGW(TAG, "Missing or invalid integer key: %s", key);
    return default_val;
}

static double get_double_from_json(cJSON *parent, const char *key, double default_val) {
    if (!parent) return default_val;
    
    cJSON *item = cJSON_GetObjectItem(parent, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    
    ESP_LOGW(TAG, "Missing or invalid double key: %s", key);
    return default_val;
}

static const char* get_string_from_json(cJSON *parent, const char *key, const char *default_val) {
    if (!parent) return default_val;
    
    cJSON *item = cJSON_GetObjectItem(parent, key);
    if (item && cJSON_IsString(item)) {
        return item->valuestring;
    }
    
    ESP_LOGW(TAG, "Missing or invalid string key: %s", key);
    return default_val;
}

static int get_relay_val(cJSON *obj, const char *key_upper, const char *key_lower) { // hàm này giúp lấy giá trị relay từ JSON, hỗ trợ cả key viết hoa và viết thường để tăng tính linh hoạt của JSON đầu vào
    if (!obj) return -1;
    
    cJSON *item = cJSON_GetObjectItem(obj, key_upper);
    if (!item) item = cJSON_GetObjectItem(obj, key_lower);
    
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return -1;
}


static void save_schedule(void) {
    RelaySchedule_t temp_schedules[NUM_RELAYS];  // Buffer để tránh race condition

    scheduler_lock();
    for (int i = 0; i < NUM_RELAYS; i++) {
        schedules[i].version = CONFIG_VERSION;
    }
    memcpy(temp_schedules, schedules, sizeof(schedules));
    scheduler_unlock();

    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    // Đọc dữ liệu hiện tại để so sánh trước khi ghi
    RelaySchedule_t existing_schedules[NUM_RELAYS];
    size_t len = sizeof(existing_schedules);
    esp_err_t read_err = nvs_get_blob(h, "json_sched", existing_schedules, &len);

    // Chỉ ghi nếu dữ liệu đã thay đổi hoặc chưa tồn tại
    if (read_err != ESP_OK || memcmp(temp_schedules, existing_schedules, sizeof(temp_schedules)) != 0) {
        err = nvs_set_blob(h, "json_sched", temp_schedules, sizeof(temp_schedules));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write schedule: %s", esp_err_to_name(err));
        } else {
            nvs_commit(h);
            ESP_LOGI(TAG, "Schedule saved to NVS (selective write)");
        }
    } else {
        ESP_LOGI(TAG, "Schedule unchanged, skipping flash write");
    }

    nvs_close(h);
}

static void load_schedule(void) {
    RelaySchedule_t temp_schedules[NUM_RELAYS];  // Buffer để tránh race condition
    // bool load_success = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for schedule: %s", esp_err_to_name(err));
        memset(temp_schedules, 0, sizeof(temp_schedules));
    } else {
        size_t len = sizeof(temp_schedules);
        err = nvs_get_blob(h, "json_sched", temp_schedules, &len);
        
        if (err == ESP_OK) {
            for (int i = 0; i < NUM_RELAYS; i++) {
                if (temp_schedules[i].version != CONFIG_VERSION) {
                    ESP_LOGW(TAG, "Schedule version mismatch, reinitializing...");
                    memset(temp_schedules, 0, sizeof(temp_schedules));
                    break;
                }
            }
            // load_success = true;
            ESP_LOGI(TAG, "Schedule loaded from NVS");
        } else {
            ESP_LOGW(TAG, "No saved schedule in NVS, using defaults");
            memset(temp_schedules, 0, sizeof(temp_schedules));
        }
        nvs_close(h);
    }
   
    scheduler_lock();
    memcpy(schedules, temp_schedules, sizeof(schedules));
    scheduler_unlock();
}

static void save_sensor_conf(void) {
    sensor_config_t temp_conf;  // Buffer để tránh race condition (khi nhiều luồng cùng truy cập)

    scheduler_lock();
    sensor_conf.version = CONFIG_VERSION;
    memcpy(&temp_conf, &sensor_conf, sizeof(sensor_config_t));
    scheduler_unlock();

    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    sensor_config_t existing_conf;
    size_t len = sizeof(existing_conf);
    esp_err_t read_err = nvs_get_blob(h, "sensor_blob", &existing_conf, &len);

    if (read_err != ESP_OK || memcmp(&temp_conf, &existing_conf, sizeof(sensor_config_t)) != 0) {
        err = nvs_set_blob(h, "sensor_blob", &temp_conf, sizeof(sensor_config_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write sensor config: %s", esp_err_to_name(err));
        } else {
            nvs_commit(h);
            ESP_LOGI(TAG, "Sensor Config saved to NVS (selective write)");
        }
    } else {
        ESP_LOGI(TAG, "Sensor Config unchanged, skipping flash write");
    }

    nvs_close(h);
}

static void load_sensor_conf(void) {
    sensor_config_t temp_conf;  // Buffer để tránh race condition
    // bool load_success = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for sensor config: %s", esp_err_to_name(err));
        // Khởi tạo cấu hình mặc định nếu không có trong NVS
        memset(&temp_conf, 0, sizeof(sensor_config_t));
        temp_conf.version = CONFIG_VERSION;
        for (int i = 0; i < NUM_RELAYS; i++) {
            temp_conf.action1_relays[i] = -1;
            temp_conf.action2_relays[i] = -1;
        }
        strncpy(temp_conf.type, "temp", sizeof(temp_conf.type) - 1);
        temp_conf.day_mask = 0x7F;
        temp_conf.temp_high = 100.0;
        temp_conf.temp_low = -50.0;
        temp_conf.hysteresis_temp = HYSTERESIS_TEMP;
        temp_conf.hysteresis_humi = HYSTERESIS_HUMI;
    } else {
        size_t len = sizeof(sensor_config_t);
        err = nvs_get_blob(h, "sensor_blob", &temp_conf, &len);
    
        if (err == ESP_OK) {
            if (temp_conf.version != CONFIG_VERSION) {
                ESP_LOGW(TAG, "Sensor config version mismatch, resetting...");
                memset(&temp_conf, 0, sizeof(sensor_config_t));
                temp_conf.version = CONFIG_VERSION;
            }
            // load_success = true;
            ESP_LOGI(TAG, "Sensor Config loaded from NVS");
        } else {
            ESP_LOGW(TAG, "No sensor config in NVS. Init default");
            memset(&temp_conf, 0, sizeof(sensor_config_t));
            temp_conf.version = CONFIG_VERSION;
        }
        
        // Chuyển giá trị 0xFF thành -1 để dễ xử lý trong code (nếu có)
        for (int i = 0; i < NUM_RELAYS; i++) {
            if (temp_conf.action1_relays[i] == 0xFF) temp_conf.action1_relays[i] = -1;
            if (temp_conf.action2_relays[i] == 0xFF) temp_conf.action2_relays[i] = -1;
        }
        
        if (temp_conf.hysteresis_temp == 0) temp_conf.hysteresis_temp = HYSTERESIS_TEMP;
        if (temp_conf.hysteresis_humi == 0) temp_conf.hysteresis_humi = HYSTERESIS_HUMI;
        
        nvs_close(h);
    }
    
    scheduler_lock();
    memcpy(&sensor_conf, &temp_conf, sizeof(sensor_config_t));
    scheduler_unlock();
}

void scheduler_update_from_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON Malformed!");
        return;
    }
    
    scheduler_lock();
    
    for (int i = 0; i < NUM_RELAYS; i++) {
        char key[10];
        snprintf(key, sizeof(key), "relay%d", i + 1); 
        
        cJSON *relay_obj = cJSON_GetObjectItem(root, key);
        
        if (relay_obj && !cJSON_IsNull(relay_obj)) {
            ESP_LOGI(TAG, "Processing %s...", key);
            
            schedules[i].version = CONFIG_VERSION;
            schedules[i].day_mask = 0;
            cJSON *days = cJSON_GetObjectItem(relay_obj, "dayOfWeek");
            if (cJSON_IsArray(days)) {
                int count = cJSON_GetArraySize(days);
                if (count > 7) count = 7;
                
                for (int j = 0; j < count; j++) {
                    cJSON *day_item = cJSON_GetArrayItem(days, j);
                    if (day_item && cJSON_IsNumber(day_item)) {
                        int d = day_item->valueint;
                        if (d >= 0 && d <= 6) {
                            schedules[i].day_mask |= (1 << d); 
                        }
                    }
                    // vTaskDelay(pdMS_TO_TICKS(1));
                }
            }

            memset(schedules[i].slots, 0, sizeof(schedules[i].slots)); 
            
            cJSON *sched_arr = cJSON_GetObjectItem(relay_obj, "schedule");
            if (cJSON_IsArray(sched_arr)) {
                int count = cJSON_GetArraySize(sched_arr);
                if (count > MAX_SLOTS) {
                    ESP_LOGW(TAG, "Schedule array too large (%d), capping at %d", count, MAX_SLOTS);
                    count = MAX_SLOTS;
                }

                for (int k = 0; k < count; k++) {
                    cJSON *item = cJSON_GetArrayItem(sched_arr, k);
                    if (!item) continue;
                    
                    cJSON *start = cJSON_GetObjectItem(item, "timeStart");
                    cJSON *stop = cJSON_GetObjectItem(item, "timeStop");

                    if (start && stop) {
                        schedules[i].slots[k].active = true;
                        schedules[i].slots[k].start.h = get_int_from_json(start, "h", 0);
                        schedules[i].slots[k].start.m = get_int_from_json(start, "m", 0);
                        schedules[i].slots[k].stop.h  = get_int_from_json(stop, "h", 0);
                        schedules[i].slots[k].stop.m  = get_int_from_json(stop, "m", 0);
                        
                        if (schedules[i].slots[k].start.h > 23) schedules[i].slots[k].start.h = 23;
                        if (schedules[i].slots[k].start.m > 59) schedules[i].slots[k].start.m = 59;
                        if (schedules[i].slots[k].stop.h > 23) schedules[i].slots[k].stop.h = 23;
                        if (schedules[i].slots[k].stop.m > 59) schedules[i].slots[k].stop.m = 59;
                    }
                    // vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
            // vTaskDelay(pdMS_TO_TICKS(1));
        } 
        else if (relay_obj && cJSON_IsNull(relay_obj)) {
            ESP_LOGW(TAG, "Clearing Schedule for %s", key);
            memset(&schedules[i], 0, sizeof(RelaySchedule_t));
        }
    }
    
    cJSON *sensor_obj = cJSON_GetObjectItem(root, "sensor");
    if (sensor_obj) {
        ESP_LOGI(TAG, "Updating Sensor Config...");
        
        sensor_conf.version = CONFIG_VERSION;
        last_sensor_action = 0;

        for (int i = 0; i < NUM_RELAYS; i++) {
            sensor_conf.action1_relays[i] = -1;
            sensor_conf.action2_relays[i] = -1;
        }

        const char *type = get_string_from_json(sensor_obj, "type", "temp");
        strncpy(sensor_conf.type, type, sizeof(sensor_conf.type) - 1);
        sensor_conf.type[sizeof(sensor_conf.type) - 1] = '\0';
        
        sensor_conf.day_mask = 0;
        cJSON *days = cJSON_GetObjectItem(sensor_obj, "dayOfWeek");
        if (cJSON_IsArray(days)) {
            int count = cJSON_GetArraySize(days);
            if (count > 7) count = 7;  
            
            for (int j = 0; j < count; j++) {
                cJSON *day_item = cJSON_GetArrayItem(days, j);
                if (day_item && cJSON_IsNumber(day_item)) {
                    int d = day_item->valueint;
                    if (d >= 0 && d <= 6) sensor_conf.day_mask |= (1 << d);
                }
            }
        } else {
            sensor_conf.day_mask = 0x7F;
        }

        cJSON *start = cJSON_GetObjectItem(sensor_obj, "timeStart");
        if (start) {
            sensor_conf.start.h = get_int_from_json(start, "h", 0);
            sensor_conf.start.m = get_int_from_json(start, "m", 0);
        }
        
        cJSON *stop = cJSON_GetObjectItem(sensor_obj, "timeStop");
        if (stop) {
            sensor_conf.stop.h = get_int_from_json(stop, "h", 0);
            sensor_conf.stop.m = get_int_from_json(stop, "m", 0);
        }

        // đảm bảo thời gian hợp lệ
        sensor_conf.temp_high = get_double_from_json(sensor_obj, "temp_high", 100.0); 
        sensor_conf.temp_low  = get_double_from_json(sensor_obj, "temp_low", -50.0);
        sensor_conf.humi_high = get_double_from_json(sensor_obj, "humi_high", 100.0);
        sensor_conf.humi_low  = get_double_from_json(sensor_obj, "humi_low", 0.0);
        sensor_conf.hysteresis_temp = get_double_from_json(sensor_obj, "hysteresis_temp", HYSTERESIS_TEMP);
        sensor_conf.hysteresis_humi = get_double_from_json(sensor_obj, "hysteresis_humi", HYSTERESIS_HUMI);

        cJSON *act1 = cJSON_GetObjectItem(sensor_obj, "action1");
        if (act1) {
            for (int i = 0; i < NUM_RELAYS; i++) {
                char relay_key[8];
                snprintf(relay_key, sizeof(relay_key), "RL%d", i + 1);
                sensor_conf.action1_relays[i] = get_relay_val(act1, relay_key, NULL);
            }
        }

        cJSON *act2 = cJSON_GetObjectItem(sensor_obj, "action2");
        if (act2) {
            for (int i = 0; i < NUM_RELAYS; i++) {
                char relay_key[8];
                snprintf(relay_key, sizeof(relay_key), "RL%d", i + 1);
                sensor_conf.action2_relays[i] = get_relay_val(act2, relay_key, NULL);
            }
        }
        
        save_sensor_conf();
    }

    // Xử lý Input Triggers từ JSON - Format mới
    for (int input_num = 1; input_num <= 3; input_num++) {
        char input_key[10];
        snprintf(input_key, sizeof(input_key), "input%d", input_num);
        
        cJSON *input_obj = cJSON_GetObjectItem(root, input_key);
        if (input_obj && !cJSON_IsNull(input_obj)) {
            InputConfig_t *input_cfg = &input_state.inputs[input_num - 1];
            memset(input_cfg, 0, sizeof(InputConfig_t));
            
            input_cfg->version = CONFIG_VERSION;
            input_cfg->active = true;
            input_cfg->num_actions = 0;
            
            // Khởi tạo constraint mặc định (-1 = bất kỳ)
            for (int i = 0; i < NUM_RELAYS; i++) {
                input_cfg->constraint[i] = -1;
            }
            
            // Đọc dayOfWeek
            input_cfg->day_mask = 0;
            cJSON *days = cJSON_GetObjectItem(input_obj, "dayOfWeek");
            if (cJSON_IsArray(days)) {
                int count = cJSON_GetArraySize(days);
                if (count > 7) count = 7;
                
                for (int j = 0; j < count; j++) {
                    cJSON *day_item = cJSON_GetArrayItem(days, j);
                    if (day_item && cJSON_IsNumber(day_item)) {
                        int d = day_item->valueint;
                        if (d >= 0 && d <= 6) input_cfg->day_mask |= (1 << d);
                    }
                }
            } else {
                input_cfg->day_mask = 0x7F;  // Tất cả ngày
            }
            
            // Đọc schedule (time slots)
            memset(input_cfg->slots, 0, sizeof(input_cfg->slots));
            cJSON *sched_arr = cJSON_GetObjectItem(input_obj, "schedule");
            if (cJSON_IsArray(sched_arr)) {
                int count = cJSON_GetArraySize(sched_arr);
                if (count > MAX_SLOTS) count = MAX_SLOTS;
                
                for (int k = 0; k < count; k++) {
                    cJSON *slot_item = cJSON_GetArrayItem(sched_arr, k);
                    if (!slot_item) continue;
                    
                    cJSON *start = cJSON_GetObjectItem(slot_item, "timeStart");
                    cJSON *stop = cJSON_GetObjectItem(slot_item, "timeStop");
                    
                    if (start && stop) {
                        input_cfg->slots[k].active = true;
                        input_cfg->slots[k].start.h = get_int_from_json(start, "h", 0);
                        input_cfg->slots[k].start.m = get_int_from_json(start, "m", 0);
                        input_cfg->slots[k].stop.h = get_int_from_json(stop, "h", 0);
                        input_cfg->slots[k].stop.m = get_int_from_json(stop, "m", 0);
                    }
                    // vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
            
            // Đọc constraint(ràng buộc) (điều kiện relay)
            cJSON *constraint_obj = cJSON_GetObjectItem(input_obj, "constraint");
            if (constraint_obj) {
                for (int i = 0; i < NUM_RELAYS; i++) {
                    char relay_key[8];
                    snprintf(relay_key, sizeof(relay_key), "relay%d", i + 1);
                    
                    cJSON *relay_item = cJSON_GetObjectItem(constraint_obj, relay_key);
                    if (relay_item && cJSON_IsNumber(relay_item)) {
                        input_cfg->constraint[i] = relay_item->valueint;
                    }
                }
            }
            
            // Đọc thresholds và actions
            for (int th = 1; th <= MAX_RULE_INPUTS; th++) {
                char threshold_key[16], action_key[16];
                snprintf(threshold_key, sizeof(threshold_key), "threshold%d", th);
                snprintf(action_key, sizeof(action_key), "action%d", th);
                
                cJSON *threshold_item = cJSON_GetObjectItem(input_obj, threshold_key);
                cJSON *action_obj = cJSON_GetObjectItem(input_obj, action_key);
                
                if (threshold_item && cJSON_IsNumber(threshold_item) && action_obj) {
                    if (input_cfg->num_actions >= MAX_RULE_INPUTS) break;
                    
                    InputAction_t *action = &input_cfg->actions[input_cfg->num_actions];
                    action->threshold_value = threshold_item->valueint;
                    
                    // Khởi tạo relay actions
                    for (int i = 0; i < NUM_RELAYS; i++) {
                        action->action_relays[i] = -1;
                    }
                    
                    // Đọc actions từ object
                    for (int i = 0; i < NUM_RELAYS; i++) {
                        char relay_key[8];
                        snprintf(relay_key, sizeof(relay_key), "relay%d", i + 1);
                        
                        cJSON *relay_item = cJSON_GetObjectItem(action_obj, relay_key);
                        if (relay_item && cJSON_IsNumber(relay_item)) {
                            action->action_relays[i] = relay_item->valueint;
                        }
                    }
                    
                    ESP_LOGI(TAG, "[INPUT%d THRESHOLD%d] Value: %d", input_num, th, action->threshold_value);
                    input_cfg->num_actions++;
                } else {
                    break;  // Không có threshold này
                }
                // vTaskDelay(pdMS_TO_TICKS(1));
            }
            
            ESP_LOGI(TAG, "Input %d Config: dayMask=0x%02x, numActions=%d", 
                     input_num, input_cfg->day_mask, input_cfg->num_actions);
        }
    }
    
    input_state.version = CONFIG_VERSION;
    need_save_input_state = true;  // đánh dấu cần save

    // hàm auto_id : nếu JSON có trường "auto_id" thì sẽ thực hiện lệnh đổi ID cảm biến
    cJSON *auto_id_obj = cJSON_GetObjectItem(root, "auto_id");
    bool need_auto_id_wait = false;
    uint8_t auto_id_value = 0;
    
    if (auto_id_obj) {
        int new_id = get_int_from_json(auto_id_obj, "new_id", -1);
        if (new_id > 1 && new_id <= 247) {
            ESP_LOGI(TAG, "Auto-ID: replace ID to %d", new_id);
            
            extern SHT35 sht35;
            extern void SHT35_ChangeID(SHT35 *dev, uint8_t old_id, uint8_t new_id);
            
            SHT35_ChangeID(&sht35, 0x00, (uint8_t)new_id);
            
            need_auto_id_wait = true;
            auto_id_value = (uint8_t)new_id;
        }
    }

    // hàm cập nhập cảm biến: nếu có trường "sensor_list" và "addresses" 
    cJSON *sensor_list_obj = cJSON_GetObjectItem(root, "sensor_list");
    if (sensor_list_obj) {
        cJSON *addresses = cJSON_GetObjectItem(sensor_list_obj, "addresses");
        if (cJSON_IsArray(addresses)) {
            // Không double lock - đã có lock ở đầu hàm
            sensor_net.count = 0;
            int count = cJSON_GetArraySize(addresses);
            
            for (int j = 0; j < count && j < MAX_SUPPORTED_SENSORS; j++) {
                cJSON *item = cJSON_GetArrayItem(addresses, j);
                if (cJSON_IsNumber(item)) {
                    sensor_net.addresses[sensor_net.count++] = item->valueint;
                }
            }
            
            need_save_sensor_network = true;  // Đánh dấu cần save
            ESP_LOGI(TAG, "Đã đồng bộ Sổ hộ khẩu: %d cảm biến", sensor_net.count);
        }
    }

    need_save_schedule = true;  // Đánh dấu cần save
    
    scheduler_unlock(); 
    cJSON_Delete(root);
    
    // Delay BÊN NGOÀI critical section
    if (need_auto_id_wait) {
        vTaskDelay(pdMS_TO_TICKS(1500));  // Không block các task khác
        add_new_sensor_to_network(auto_id_value);
    }
    
    if (need_save_input_state) {
        save_input_state();
        need_save_input_state = false;
    }
    if (need_save_sensor_network) {
        save_sensor_network();
        need_save_sensor_network = false;
    }
    if (need_save_schedule) {
        save_schedule();
        need_save_schedule = false;
    }
}

void scheduler_process_sensor(float current_value) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Biến cục bộ để chuẩn bị trước khi rời khỏi vùng khóa Mutex
    bool state_changed = false;
    int target_action = 0;
    
    // Khởi tạo các mảng cục bộ bằng -1 (Bỏ qua)
    int exec_relays[NUM_RELAYS];
    int off_relays_1[NUM_RELAYS];
    int off_relays_2[NUM_RELAYS];
    
    for (int i = 0; i < NUM_RELAYS; i++) {
        exec_relays[i] = -1;
        off_relays_1[i] = -1;
        off_relays_2[i] = -1;
    }

    scheduler_lock();

    // 1. Kiểm tra Ngày và Giờ (Thoát sớm nếu đang ngoài lịch hẹn)
    if ((sensor_conf.day_mask & (1 << timeinfo.tm_wday)) == 0 || 
        !is_time_in_range(timeinfo.tm_hour, timeinfo.tm_min, sensor_conf.start, sensor_conf.stop)) {
        scheduler_unlock();
        return;
    }

    // 2. Nạp cấu hình dựa trên JSON gửi xuống là "temp" hay "hum"
    float high = 0, low = 0, hysteresis = 0;
    bool is_temp = (strcmp(sensor_conf.type, "temp") == 0);

    if (is_temp) {
        sensor_conf.current_temp = current_value;
        high = sensor_conf.temp_high;
        low  = sensor_conf.temp_low;
        hysteresis = sensor_conf.hysteresis_temp;
    } else {
        sensor_conf.current_humi = current_value;
        high = sensor_conf.humi_high;
        low  = sensor_conf.humi_low;
        hysteresis = sensor_conf.hysteresis_humi;
    }

    // Kiểm tra hysteresis hợp lệ
    if (hysteresis < 0) {
        ESP_LOGW(TAG, "[SENSOR] Invalid hysteresis (%.2f), using default", hysteresis);
        hysteresis = is_temp ? HYSTERESIS_TEMP : HYSTERESIS_HUMI;
    }

    // trạng thái : 0 = Vùng an toàn, 1 = Vượt ngưỡng CAO, 2 = Dưới ngưỡng THẤP
    target_action = last_sensor_action; // Giữ nguyên trạng thái cũ làm mốc

    if (current_value >= high) {
        target_action = 1; // Vượt ngưỡng CAO -> Bật Action 1
    } 
    else if (current_value <= low) {
        target_action = 2; // Dưới ngưỡng THẤP -> Bật Action 2
    } 
    else {
        if (last_sensor_action == 1 && current_value < (high - hysteresis)) {
            ESP_LOGI(TAG, "[HYSTERESIS] Turning OFF from action1 (%.2f < %.2f)", 
                     current_value, high - hysteresis);
            target_action = 0; // done -> Cho về 0 (Tắt)
        } 
        else if (last_sensor_action == 2 && current_value > (low + hysteresis)) {
            ESP_LOGI(TAG, "[HYSTERESIS] Turning OFF from action2 (%.2f > %.2f)", 
                     current_value, low + hysteresis);
            target_action = 0; // done -> Cho về 0 (Tắt)
        }
    }

    // nếu trạng thái mới khác với trạng thái cũ, thì mới thực hiện thay đổi và ghi đè trạng thái mới
    if (target_action != last_sensor_action) {
        state_changed = true;
        last_sensor_action = target_action; // Ghi đè trạng thái mới
        
        // Copy cấu hình Rơ-le ra mảng cục bộ
        if (target_action == 1) {
            memcpy(exec_relays, sensor_conf.action1_relays, sizeof(exec_relays));
            ESP_LOGI(TAG, "[SENSOR] Action1 TRIGGERED (Value: %.2f >= High: %.2f)", current_value, high);
        } 
        else if (target_action == 2) {
            memcpy(exec_relays, sensor_conf.action2_relays, sizeof(exec_relays));
            ESP_LOGI(TAG, "[SENSOR] Action2 TRIGGERED (Value: %.2f <= Low: %.2f)", current_value, low);
        } 
        else if (target_action == 0) {
            // ở vùng an toàn, kiểm tra xem trước đó là action1 hay action2 để biết cần khôi phục rơ-le nào
            memcpy(off_relays_1, sensor_conf.action1_relays, sizeof(off_relays_1));
            memcpy(off_relays_2, sensor_conf.action2_relays, sizeof(off_relays_2));
            ESP_LOGI(TAG, "[SENSOR] SAFE ZONE - Restoring system (Value: %.2f)", current_value);
        }
    }

    // Nhả khóa Mutex ngay lập tức! Hệ điều hành FreeRTOS được giải phóng
    scheduler_unlock();

    if (state_changed) {
        
        // 1 : Vượt ngưỡng CAO -> Thực hiện Action 1, 2: Dưới ngưỡng THẤP -> Thực hiện Action 2
        if (target_action == 1 || target_action == 2) {
            for (int i = 0; i < NUM_RELAYS; i++) {
                if (exec_relays[i] != -1) { // Chỉ quan tâm các rơ-le được cấu hình 1 hoặc 0
                    relay_set(i + 1, exec_relays[i], false); // false = không ghi log ở đây, để tránh spam log
                    if (mqtt_is_connected()) mqtt_send_state(i + 1, exec_relays[i]);
                    ESP_LOGI(TAG, "[ACTION%d] Relay%d -> %d", target_action, i + 1, exec_relays[i]);
                }
            }
        } 
        
        // 2 : Trở về vùng an toàn -> Kiểm tra xem rơ-le nào cần khôi phục trạng thái (dựa trên action1/action2 trước đó)
        else if (target_action == 0) {
            for (int i = 0; i < NUM_RELAYS; i++) {
                // Rà soát: Rơ-le này có từng bị quản lý bởi Cảm biến không?
                if (off_relays_1[i] != -1 || off_relays_2[i] != -1) {
                    
                    // Nếu nó đang BẬT (Khác 0), thì mới gửi lệnh TẮT (0) để tránh spam MQTT
                    if (relay_get_status(i + 1) != 0) {
                        int old_status = relay_get_status(i + 1);
                        relay_set(i + 1, 0, false); // false = không ghi log ở đây, để tránh spam log
                        if (mqtt_is_connected()) mqtt_send_state(i + 1, 0);
                        ESP_LOGI(TAG, "[SAFE_ZONE] Relay%d -> OFF (was: %d)", i + 1, old_status);
                    }
                }
            }
        }
    }
}

// phân tích chuỗi thời gian
static bool parse_simple_time(const char *str, int *h, int *m) {
    if (str == NULL || h == NULL || m == NULL) return false;
    if (sscanf(str, "%d:%d", h, m) == 2) {
        if (*h >= 0 && *h <= 23 && *m >= 0 && *m <= 59) return true;
    }
    return false;
}

// Cập nhật thời gian bật/tắt relay
void scheduler_set_on_time(int relay_idx, const char *time_str) {
    if (relay_idx < 1 || relay_idx > NUM_RELAYS) return;
    int i = relay_idx - 1;
    int h, m;
    if (parse_simple_time(time_str, &h, &m)) {

        scheduler_lock();

        schedules[i].slots[0].active = true;
        schedules[i].slots[0].start.h = h;
        schedules[i].slots[0].start.m = m;
        if (schedules[i].day_mask == 0) schedules[i].day_mask = 0x7F; 

        scheduler_unlock();

        save_schedule();
    }
}

void scheduler_set_off_time(int relay_idx, const char *time_str) {
    if (relay_idx < 1 || relay_idx > NUM_RELAYS) return;
    int i = relay_idx - 1;
    int h, m;
    if (parse_simple_time(time_str, &h, &m)) {

        scheduler_lock();

        schedules[i].slots[0].active = true;
        schedules[i].slots[0].stop.h = h;
        schedules[i].slots[0].stop.m = m;
        if (schedules[i].day_mask == 0) schedules[i].day_mask = 0x7F;

        scheduler_unlock();
        
        save_schedule();
    }
}

// Kiểm tra nếu thời gian hiện tại nằm trong khoảng start-stop, có tính đến trường hợp qua đêm
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

// Kiểm tra các lịch trình đã bị bỏ lỡ trong thời gian mất kết nối và khôi phục trạng thái relay tương ứng
void scheduler_check_at_boot(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    int current_min_total = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int current_wday_bit = (1 << timeinfo.tm_wday); 
    ESP_LOGW(TAG, "Checking missed schedules (Power Recovery)...");

    int exec_relays[NUM_RELAYS];
    for (int i = 0; i < NUM_RELAYS; i++) {
        exec_relays[i] = -1;
    }

    scheduler_lock();
    
    for (int i = 0; i < NUM_RELAYS; i++) {
        // bool should_be_on = false;
        if ((schedules[i].day_mask & current_wday_bit) == 0) continue;
        for (int k = 0; k < MAX_SLOTS; k++) {
            if (!schedules[i].slots[k].active) continue;

            int start_total = schedules[i].slots[k].start.h * 60 + schedules[i].slots[k].start.m;
            int stop_total  = schedules[i].slots[k].stop.h * 60 +  schedules[i].slots[k].stop.m;

            if (start_total < stop_total) { 
                if (current_min_total >= start_total && current_min_total < stop_total) exec_relays[i] = 1; // ON
            } else { 
                if (current_min_total >= start_total || current_min_total < stop_total) exec_relays[i] = 1; // ON
            }
        }

        // if (exec_relays[i] == 1) {
        //     ESP_LOGW(TAG, "Restoring Relay %d -> ON", i+1);
        //     relay_on(i+1);
        //     if (mqtt_is_connected()) mqtt_send_state(i+1, 1);
        // }
    }
    scheduler_unlock();

    for (int i = 0; i < NUM_RELAYS; i++) {
        if (exec_relays[i] == 1) {
            ESP_LOGW(TAG, "Restoring Relay %d -> ON", i + 1);
            relay_on(i + 1);
            if (mqtt_is_connected()) mqtt_send_state(i + 1, 1);
        }
    }
}

// Lưu input state vào NVS
static void save_input_state(void) {
    input_state_t temp_state;  // FIX: Đúng tên kiểu dữ liệu

    scheduler_lock();
    input_state.version = CONFIG_VERSION;
    memcpy(&temp_state, &input_state, sizeof(input_state_t));
    scheduler_unlock();

    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for input state: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(h, "input_state", &temp_state, sizeof(input_state_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write input state: %s", esp_err_to_name(err));
    } else {
        nvs_commit(h);
        ESP_LOGI(TAG, "Input state saved to NVS");
    }
    
    nvs_close(h);
}

// Tải input state từ NVS
static void load_input_state(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for input state: %s", esp_err_to_name(err));
        // Khởi tạo mặc định
        memset(&input_state, 0, sizeof(input_state_t));
        input_state.version = CONFIG_VERSION;
        
        // Khởi tạo constraints mặc định (-1 = bất kỳ)
        for (int i = 0; i < 3; i++) {
            input_state.inputs[i].version = CONFIG_VERSION;
            input_state.inputs[i].active = false;
            input_state.inputs[i].day_mask = 0x7F;
            input_state.inputs[i].num_actions = 0;
            for (int j = 0; j < NUM_RELAYS; j++) {
                input_state.inputs[i].constraint[j] = -1;
            }
        }
        return;
    }

    size_t len = sizeof(input_state_t);
    err = nvs_get_blob(h, "input_state", &input_state, &len);
    
    if (err == ESP_OK) {
        if (input_state.version != CONFIG_VERSION) {
            ESP_LOGW(TAG, "Input state version mismatch, resetting...");
            memset(&input_state, 0, sizeof(input_state_t));
            input_state.version = CONFIG_VERSION;
        }
        ESP_LOGI(TAG, "Input state loaded from NVS");
        
        // Đảm bảo constraint khởi tạo đúng
        for (int i = 0; i < 3; i++) {
            if (input_state.inputs[i].version != CONFIG_VERSION) {
                input_state.inputs[i].version = CONFIG_VERSION;
            }
        }
    } else {
        ESP_LOGW(TAG, "No input state in NVS, using defaults");
        memset(&input_state, 0, sizeof(input_state_t));
        input_state.version = CONFIG_VERSION;
        
        // Khởi tạo constraints mặc định
        for (int i = 0; i < 3; i++) {
            input_state.inputs[i].version = CONFIG_VERSION;
            input_state.inputs[i].active = false;
            input_state.inputs[i].day_mask = 0x7F;
            input_state.inputs[i].num_actions = 0;
            for (int j = 0; j < NUM_RELAYS; j++) {
                input_state.inputs[i].constraint[j] = -1;
            }
        }
    }
    
    nvs_close(h);
}

// Xử lý input trigger khi có thay đổi trạng thái input
void scheduler_process_input(int input_idx, int new_state) {
    if (input_idx < 1 || input_idx > 3) {
        ESP_LOGW(TAG, "Invalid input index: %d", input_idx);
        return;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    bool execute_action = false;
    int exec_relays[NUM_RELAYS] = { -1, -1, -1, -1 }; // Mảng tạm để lưu trạng thái relay cần thực hiện

    scheduler_lock();

    InputConfig_t *input_cfg = &input_state.inputs[input_idx - 1];
    
    // Log state change
    ESP_LOGD(TAG, "[INPUT%d] State changed to %s", input_idx, new_state ? "CLOSED" : "OPEN");
    
    // Kiểm tra xem input này có active không
    if (!input_cfg->active) {
        ESP_LOGD(TAG, "[INPUT%d] Not active - skipping", input_idx);
        scheduler_unlock();
        return;
    }

    // Kiểm tra ngày trong tuần
    if ((input_cfg->day_mask & (1 << timeinfo.tm_wday)) == 0) {
        ESP_LOGD(TAG, "[INPUT%d] Day mask not matched (wday=%d)", input_idx, timeinfo.tm_wday);
        scheduler_unlock();
        return;
    }

    // Kiểm tra thời gian (schedule)
    bool in_schedule = false;
    if (input_cfg->slots[0].active) {
        // Nếu có schedule, kiểm tra xem hiện tại có nằm trong time range không
        for (int k = 0; k < MAX_SLOTS; k++) {
            if (!input_cfg->slots[k].active) continue;
            
            if (is_time_in_range(timeinfo.tm_hour, timeinfo.tm_min, 
                                 input_cfg->slots[k].start, input_cfg->slots[k].stop)) {
                in_schedule = true;
                break;
            }
        }
    } else {
        in_schedule = true;
    }

    if (!in_schedule) {
        ESP_LOGD(TAG, "[INPUT%d] Not in schedule time (%02d:%02d)", input_idx, 
                 timeinfo.tm_hour, timeinfo.tm_min);
        scheduler_unlock();
        return;
    }

    // Kiểm tra constraint (điều kiện relay)
    bool constraint_satisfied = true;
    for (int i = 0; i < NUM_RELAYS; i++) {
        if (input_cfg->constraint[i] == -1) {
            // -1 = bất kỳ, không cần kiểm tra
            continue;
        }
        
        int relay_status = relay_get_status(i + 1);
        if (relay_status != input_cfg->constraint[i]) {
            ESP_LOGD(TAG, "[INPUT%d] Constraint failed: Relay%d expected=%d, got=%d", 
                     input_idx, i + 1, input_cfg->constraint[i], relay_status);
            constraint_satisfied = false;
            break;
        }
    }

    if (!constraint_satisfied) {
        scheduler_unlock();
        return;
    }

    // lọc ra action phù hợp với threshold_value == new_state
    for (int i = 0; i < input_cfg->num_actions; i++) {
        InputAction_t *action = &input_cfg->actions[i];
        
        if (action->threshold_value == new_state) {
            ESP_LOGI(TAG, "[INPUT%d] Threshold %d matched -> Copying actions to buffer", 
                     input_idx, action->threshold_value);
            
            // Sao chép lệnh ra mảng cục bộ
            memcpy(exec_relays, action->action_relays, sizeof(exec_relays));
            execute_action = true;
            break;  // Chỉ xử lý action đầu tiên khớp
        }
    }
    
    if (!execute_action) {
        ESP_LOGD(TAG, "[INPUT%d] No matching threshold action", input_idx);
    }

    scheduler_unlock();

    if (execute_action) {
        for (int j = 0; j < NUM_RELAYS; j++) {
            if (exec_relays[j] != -1) {
                ESP_LOGI(TAG, "[INPUT%d] Relay%d -> %d", input_idx, j + 1, exec_relays[j]);
                relay_set(j + 1, exec_relays[j], false); // false = không ghi log ở đây, để tránh spam log
                
                // Chỉ gửi MQTT nếu có thay đổi trạng thái để tránh spam
                if (mqtt_is_connected()) {
                    mqtt_send_state(j + 1, exec_relays[j]);
                }
            }
        }
    }
}

void scheduler_task(void *arg) {
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    
    time(&now);
    localtime_r(&now, &timeinfo);
    
    ESP_LOGI(TAG, "System time at scheduler startup: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    platform_time_status_t status = platform_get_time_status();
    ESP_LOGI(TAG, "Platform time status: %d", status);
    
    if (status == PLATFORM_TIME_COLD_BOOT_OK || status == PLATFORM_TIME_COLD_BOOT_FALLBACK) {
        ESP_LOGI(TAG, "Time valid from Phase 1 (Status: %d), proceeding without NTP", status);
        set_time_from_rtc = 1; // Đánh dấu đã lấy thời gian từ RTC, không cần chờ NTP nữa
    }
    else if (status == PLATFORM_TIME_COLD_BOOT_FAIL) {
        ESP_LOGW(TAG, "Time invalid from Phase 1 (epoch), waiting for NTP sync...");
        
        while (status == PLATFORM_TIME_COLD_BOOT_FAIL || 
               status == PLATFORM_TIME_UNINITIALIZED) {
            retry++;
            if (retry % 5 == 0) {
                ESP_LOGW(TAG, "Waiting for NTP sync... Attempt: %d (WiFi required)", retry);
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            status = platform_get_time_status();
            
            if (status == PLATFORM_TIME_NTP_SYNCED) {
                ESP_LOGI(TAG, "NTP sync successful!");
                break;
            }
        }
    }
    
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "Scheduler proceeding with time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    // scheduler_check_at_boot();

    // task chính để kiểm tra lịch trình mỗi phút và thực hiện các hành động tương ứng
    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        int current_minute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        
        if (current_minute != last_processed_minute) {
            last_processed_minute = current_minute;
            
            int current_wday_bit = (1 << timeinfo.tm_wday);
            
            ESP_LOGI(TAG, "Scheduler Tick: %04d-%02d-%02d %02d:%02d:%02d (Weekday: %d)",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, timeinfo.tm_wday);
            
            // biến cục bộ để lưu lệnh cần thực hiện, tránh chạy trực tiếp trong mutex
            int exec_relays[NUM_RELAYS];
            for (int i = 0; i < NUM_RELAYS; i++) {
                exec_relays[i] = -1; // -1: Bỏ qua
            }
            
            scheduler_lock();
            
            for (int i = 0; i < NUM_RELAYS; i++) {
                if ((schedules[i].day_mask & current_wday_bit) == 0) continue;

                for (int k = 0; k < MAX_SLOTS; k++) {
                    if (!schedules[i].slots[k].active) continue;

                    TimePoint_t *start = &schedules[i].slots[k].start;
                    TimePoint_t *stop  = &schedules[i].slots[k].stop;

                    // Lưu lệnh ra biến thay vì chạy trực tiếp
                    if (start->h == timeinfo.tm_hour && start->m == timeinfo.tm_min) {
                        exec_relays[i] = 1; // Lệnh bật
                    }
                    else if (stop->h == timeinfo.tm_hour && stop->m == timeinfo.tm_min) {
                        exec_relays[i] = 0; // Lệnh tắt
                    }
                }
            }
            
            scheduler_unlock();
            
            // thực hiện các lệnh ngoài mutex để tránh block hệ thống
            for (int i = 0; i < NUM_RELAYS; i++) {
                if (exec_relays[i] == 1) {
                    ESP_LOGW(TAG, "Relay %d: START time reached", i + 1);
                    relay_set(i + 1, 1, false);
                    if (mqtt_is_connected()) mqtt_send_state(i + 1, 1);
                } 
                else if (exec_relays[i] == 0) {
                    ESP_LOGW(TAG, "Relay %d: STOP time reached", i + 1);
                    relay_set(i + 1, 0, false);
                    if (mqtt_is_connected()) mqtt_send_state(i + 1, 0);
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }  
}


// hàm khởi tạo cấu trúc mạng cảm biến về mặc định nếu không có dữ liệu hợp lệ trong NVS
static void init_default_sensor_network(sensor_network_t *net) {
    memset(net, 0, sizeof(sensor_network_t));
    net->version = CONFIG_VERSION;
    net->count = 1;          // Mặc định 1 cảm biến
    net->addresses[0] = 1;   // ID Modbus mặc định là 1
}

// hàm save: lưu cấu trúc mạng cảm biến vào NVS
void save_sensor_network(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for sensor network: %s", esp_err_to_name(err));
        return;
    }
    
    sensor_net.version = CONFIG_VERSION;
    err = nvs_set_blob(h, "snsr_net", &sensor_net, sizeof(sensor_network_t));
    if (err == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "Saved sensor network: %d sensors to NVS", sensor_net.count);
    } else {
        ESP_LOGE(TAG, "Failed to write sensor network: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

// hàm load: có kiểm tra version, bảo vệ tràn RAM, và fallback về mặc định nếu lỗi
void load_sensor_network(void) {
    sensor_network_t temp_net; 
    bool load_success = false;
    nvs_handle_t h;

    esp_err_t err = nvs_open("storage", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(sensor_network_t);
        err = nvs_get_blob(h, "snsr_net", &temp_net, &len);
        
        if (err == ESP_OK && temp_net.version == CONFIG_VERSION) {
            // bảo vệ chống tràn RAM nếu count bị lỗi 
            if (temp_net.count > MAX_SUPPORTED_SENSORS) {
                temp_net.count = MAX_SUPPORTED_SENSORS;
                ESP_LOGW(TAG, "Corrupted NVS count, clamped to %d", MAX_SUPPORTED_SENSORS);
            }
            load_success = true;
            ESP_LOGI(TAG, "Loaded sensor network: %d sensors from NVS", temp_net.count);
        } else {
            ESP_LOGW(TAG, "Version mismatch or blob error, resetting...");
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS for sensor network: %s", esp_err_to_name(err));
    }

    if (!load_success) {
        init_default_sensor_network(&temp_net);
        ESP_LOGI(TAG, "Using default sensor network configuration");
    }

    // Khóa Mutex đẩy dữ liệu lên Toàn cục
    scheduler_lock();
    memcpy(&sensor_net, &temp_net, sizeof(sensor_network_t));
    scheduler_unlock();
}

// Hàm thêm cảm biến mới vào mạng, có kiểm tra trùng lặp và cập nhật NVS an toàn
void add_new_sensor_to_network(uint8_t new_id) {
    bool need_save = false;

    scheduler_lock();
    
    // Kiểm tra trùng lặp
    bool exists = false;
    for (int i = 0; i < sensor_net.count; i++) {
        if (sensor_net.addresses[i] == new_id) {
            exists = true;
            break;
        }
    }
    
    if (exists) {
        ESP_LOGW(TAG, "ID %d already exists, skipping.", new_id);
    } 
    else if (sensor_net.count < MAX_SUPPORTED_SENSORS) {
        // Thêm vào nếu còn chỗ
        sensor_net.addresses[sensor_net.count] = new_id;
        sensor_net.count++;
        need_save = true;  // Bật cờ để lưu NVS sau khi nhả Mutex
        ESP_LOGI(TAG, "Added sensor ID %d. Total: %d", new_id, sensor_net.count);
    } 
    else {
        ESP_LOGE(TAG, "Network has reached maximum of %d sensors!", MAX_SUPPORTED_SENSORS);
    }

    scheduler_unlock();

    // Lưu xuống Flash khi nằm ngoài vùng Mutex
    if (need_save) {
        save_sensor_network();
    }
}

void scheduler_init(void) {
    // khởi tạo mutex nếu chưa có
    if (scheduler_mutex == NULL) {
        scheduler_mutex = xSemaphoreCreateMutex();
        if (scheduler_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create scheduler mutex!");
            return;
        }
    }
    
    load_schedule();
    load_sensor_conf();
    load_input_state();
    load_sensor_network();
    xTaskCreate(scheduler_task, "sched_task", 4096, NULL, 5, NULL);
}