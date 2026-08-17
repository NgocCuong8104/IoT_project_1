#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "input.h"
#include "relay.h"
#include "wifi.h"
#include "button.h"
#include "led_status.h"
#include "pppos.h"
#include "mqtt.h"
#include "scheduler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sht35.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "modbusSRC.h"
#include "platform.h"
#include "string.h"
#include "esp_sntp.h"
#include "ota.h"

#define SHT35_UART      UART_NUM_1
#define UART_TX_PIN     19
#define UART_RX_PIN     18
#define UART_BUF_SIZE   256

#define RS485_DE_RE_PIN GPIO_NUM_NC
#define SHT35_MODBUS_ADDR 0x01

static const char *TAG = "MAIN";

volatile int app_mode = 0;
float current_temp = 0.0; 
float current_hum = 0.0;
uint8_t sensor_valid = 0;  // 1 = cảm biến hợp lệ, 0 = lỗi cảm biến

// Mutex để bảo vệ truy cập vào sensor data (current_temp, current_hum, sensor_valid)
static SemaphoreHandle_t sensor_data_mutex = NULL;

SemaphoreHandle_t sensor_get_data_mutex(void) {
    return sensor_data_mutex;
}

void sensor_data_lock(void) {
    if (sensor_data_mutex) {
        xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
    }
}

void sensor_data_unlock(void) {
    if (sensor_data_mutex) {
        xSemaphoreGive(sensor_data_mutex);
    }
}

#define NVS_MODEM_KEY "RUN_MODEM"

void save_mode_and_restart(int32_t mode) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_i32(my_handle, NVS_MODEM_KEY, mode);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        
        ESP_LOGW(TAG, "Saved mode %ld. the system will restart...", mode);
        vTaskDelay(pdMS_TO_TICKS(3000)); 
        esp_restart(); 
    } else {
        ESP_LOGE(TAG, "Failed to save mode to NVS: %s", esp_err_to_name(err));
    }
}

int32_t load_last_mode(void) {
    nvs_handle_t my_handle;
    int32_t saved_mode = 0;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_get_i32(my_handle, NVS_MODEM_KEY, &saved_mode);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Loaded last mode from NVS: %ld", saved_mode);
    }
    return saved_mode;
}
void setup_time(void) {
    ESP_LOGI(TAG, "Initializing SNTP for timestamp with RTC sync-back...");
    
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    
    sntp_set_time_sync_notification_cb(platform_sntp_sync_callback);
    
    esp_sntp_init();
    
    ESP_LOGI(TAG, "SNTP initialized with RTC sync-back enabled");
}
static void on_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "Got IP address...");

    if (event_id == IP_EVENT_STA_GOT_IP) {
        mqtt_init();
        
        char *id = platform_get_id();
        char topic[64];
        char payload[128];
        snprintf(topic, sizeof(topic), "device/%s/info", id);
        snprintf(payload, sizeof(payload), "{\"status\":\"online\", \"version\":\"%s\", \"uptime\":%lu}", platform_get_version(), platform_get_boot_time());
        mqtt_publish(topic, payload);
    }
}
SHT35 sht35;

void sensor_hardware_init(void) {
    uart_config_t uart_cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(
        SHT35_UART,
        UART_BUF_SIZE,
        0,
        0,
        NULL,
        0
    ));
    ESP_ERROR_CHECK(uart_param_config(SHT35_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(
        SHT35_UART,
        UART_TX_PIN,
        UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));
    ESP_LOGI(TAG, "UART for SHT35 initialized");
    SHT35_init(
        &sht35,
        SHT35_UART,
        SHT35_MODBUS_ADDR              
    );
    SHT35_begin(&sht35);
    SHT35_setTimeout(&sht35, 1000);
    ESP_LOGI(TAG, "SHT35 Modbus initialized");
}

void sensor_task(void *arg) {
    char mqtt_payload[128];
    char mqtt_topic[64];
    char *dev_id = platform_get_id();
    uint32_t read_count = 0;

    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        float total_temp = 0, total_hum = 0;
        int valid_sensors = 0;

        extern sensor_network_t sensor_net; // Lấy danh sách từ scheduler
        
        uint8_t local_count = 0;
        uint8_t local_addresses[MAX_SUPPORTED_SENSORS];
        
        scheduler_lock(); 
        
        local_count = sensor_net.count;
        if (local_count > 0) {
            memcpy(local_addresses, sensor_net.addresses, local_count);
        }
        
        scheduler_unlock(); 

        if (local_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        for (int i = 0; i < local_count; i++) {
            sht35.addr = local_addresses[i]; // Cập nhật địa chỉ cảm biến trước khi đọc dữ liệu
            
            dataSHT35 data = SHT35_getData(&sht35);
            
            if (data.is_valid && data.humidity > 0 && data.temperatureC > -40) {
                ESP_LOGI(TAG, "[Sensor ID: %d] Temp: %.2f C, Hum: %.2f %%", 
                         sht35.addr, data.temperatureC, data.humidity);
                
                total_temp += data.temperatureC;
                total_hum += data.humidity;
                valid_sensors++;

                snprintf(mqtt_topic, sizeof(mqtt_topic), "/device/%s/sensor/%d", dev_id, sht35.addr);
                snprintf(mqtt_payload, sizeof(mqtt_payload), "{\"addr\":%d, \"temp\": %.2f, \"hum\": %.2f}", 
                         sht35.addr, data.temperatureC, data.humidity);
                mqtt_publish(mqtt_topic, mqtt_payload);
            } else {
                ESP_LOGW(TAG, "[Sensor ID: %d] Disconnected", sht35.addr);
            }
            
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (valid_sensors > 0) {
            // Bảo vệ khi ghi sensor data
            sensor_data_lock();
            current_temp = total_temp / valid_sensors;
            current_hum = total_hum / valid_sensors;
            sensor_valid = 1;
            sensor_data_unlock();

            // Bảo vệ khi đọc sensor_config từ scheduler
            extern sensor_config_t sensor_conf;
            scheduler_lock();
            bool is_temp_sensor = (strcmp(sensor_conf.type, "temp") == 0);
            scheduler_unlock();
            
            if (is_temp_sensor) {
                scheduler_process_sensor(current_temp);
            } else {
                scheduler_process_sensor(current_hum);
            }
            
            sensor_data_lock();
            ESP_LOGI(TAG, "average: Temp=%.2fC, Hum=%.2f%% (from %d/%d sensors valid)", 
                     current_temp, current_hum, valid_sensors, local_count);
            sensor_data_unlock();
        } else {
            sensor_data_lock();
            sensor_valid = 0;
            sensor_data_unlock();
        }
        
        if (read_count % 30 == 0) {
            ESP_LOGI(TAG, "Sensor read #%lu, Heap: %lu bytes", read_count, esp_get_free_heap_size());
        }
        read_count++;
        
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void task_report_status(void *arg) {
    uint32_t report_count = 0;
    static bool ota_validated = false; 
    
    while (1) {
        // extern bool mqtt_is_connected(void);
        
        if (mqtt_is_connected()) {
            mqtt_send_status();
            
            if (!ota_validated) {
                ota_validated = true;
                ota_mark_valid();
                ota_send_boot_confirmation();
                ESP_LOGI(TAG, "[OTA] System validated after first status report - firmware marked VALID");
            }
        } else {
            if (report_count % 5 == 0) {
                ESP_LOGW(TAG, "MQTT not connected, status not sent. Free Heap: %lu", 
                         esp_get_free_heap_size());
            }
        }
        
        if (report_count % 30 == 0) {
            ESP_LOGI(TAG, "Status report #%lu, Heap: %lu bytes", report_count, 
                     esp_get_free_heap_size());
        }
        
        report_count++;
        vTaskDelay(pdMS_TO_TICKS(30000)); // thời gian giữa các lần gửi trạng thái (30 giây)
    }
}

void my_dry_callback_alert(int contact_index, int new_state) {
    ESP_LOGI(TAG, "Dry Contact %d changed to %s", contact_index, new_state ? "CLOSED" : "OPEN");
    // Xử lý input trigger từ scheduler
    scheduler_process_input(contact_index, new_state);  
    // Gửi trạng thái lên MQTT
    mqtt_send_status();
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Tạo mutex để bảo vệ sensor data
    sensor_data_mutex = xSemaphoreCreateMutex();
    if (sensor_data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor_data_mutex");
    }

    setenv("TZ", "GMT-7", 1);
    tzset();

    platform_init_time_system();
    
    // setup_time();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware Version: %s", app_desc->version);
    ESP_LOGI(TAG, "Build: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free Heap: %lu bytes", esp_get_free_heap_size());

    ota_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &on_got_ip, NULL));

    dry_contact_input_init();
    dry_contact_register_callback(my_dry_callback_alert);
    dry_contact_start_monitor();

    setup_time();
    relay_init();
    button_init();
    led_status_init();
    sensor_hardware_init();

    app_mode = load_last_mode();

    wifi_init_sta();
    
    if (app_mode == 2) {
        pppos_init();
        pppos_start_connect();
    } 
    else if (app_mode == 0) {
        led_status_off();
    }
    scheduler_init();

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(task_report_status, "reporter", 4096, NULL, 5, NULL);
}