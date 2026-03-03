#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"

#include "relay.h"
#include "wifi.h"
#include "web.h"
#include "button.h"
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
// #include "freertos/queue.h"
// #include <stdint.h>
// #include <stdlib.h>
// #include <time.h>

#define SHT35_UART      UART_NUM_1
#define UART_TX_PIN     19
#define UART_RX_PIN     18
#define UART_BUF_SIZE   256

#define RS485_DE_RE_PIN GPIO_NUM_NC
#define SHT35_MODBUS_ADDR 0x01

static const char *TAG = "MAIN";

int app_mode = 0;
float current_temp = 0.0; 
float current_hum = 0.0;

// QueueHandle_t relay_queue = NULL;

#define NVS_MODEM_KEY "RUN_MODEM"

void save_mode_and_restart(int32_t mode) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_i32(my_handle, NVS_MODEM_KEY, mode);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        
        ESP_LOGW(TAG, "DA LUU CHE DO %ld. HE THONG SE KHOI DONG LAI...", mode);
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        esp_restart(); 
    } else {
        ESP_LOGE(TAG, "fail");
    }
}
int32_t load_last_mode(void) {
    nvs_handle_t my_handle;
    int32_t saved_mode = 0;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_get_i32(my_handle, NVS_MODEM_KEY, &saved_mode);
        nvs_close(my_handle);
    }
    return saved_mode;
}
void setup_time(void) {
    ESP_LOGI(TAG, "Initializing SNTP for timestamp...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    setenv("TZ", "GMT-7", 1);
    tzset();
}
static void on_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "Bat dau ket noi MQTT...");

    setup_time();
    mqtt_init();

    char *id = platform_get_id();
    char topic[64];
    char payload[128];
    snprintf(topic, sizeof(topic), "device/%s/info", id);
    snprintf(payload, sizeof(payload), "{\"status\":\"online\", \"version\":\"%s\", \"uptime\":%lu}", platform_get_version(), platform_get_boot_time());
    mqtt_publish(topic, payload);
}
/*void modem_start_connect(void) {
if (app_mode != 2) {
        ESP_LOGI(TAG, "giu Button 2 -> Chuyen sang 4G...");
        save_mode_and_restart(2);
    } else {
        ESP_LOGI(TAG, "dang o che do 4G roi.");
    }
}
*/
// void backend_task(void *arg) {
//     relay_event_t event;
//     ESP_LOGI(TAG, "Backend Processing Task Started");

//     while (1) {
//         if (xQueueReceive(relay_queue, &event, portMAX_DELAY) == pdTRUE) {
        
//             relay_commit_nvs(event.relay_idx, event.new_state);

//             mqtt_send_response(event.relay_idx, event.new_state); 
//         }
//     }
// }
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
    char mqtt_payload[100];
    char mqtt_topic[64];
    char *dev_id = platform_get_id();

    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        dataSHT35 data = SHT35_getData(&sht35);
        if (data.humidity != 0 || data.temperatureC != 0) {
            ESP_LOGI(TAG, "SHT35 Data - Temp: %.2f C, Humidity: %.2f %%", data.temperatureC, data.humidity);

            extern sensor_config_t sensor_conf;

            current_temp = data.temperatureC;
            current_hum = data.humidity;

            if (strcmp(sensor_conf.type, "temp") == 0) {
                scheduler_process_sensor(data.temperatureC);
            } else {
                scheduler_process_sensor(data.humidity);
            }

            snprintf(mqtt_topic, sizeof(mqtt_topic), "/device/%s/sensor/sht35/data", dev_id);
            snprintf(mqtt_payload, sizeof(mqtt_payload), "{\"temperature\": %.2f, \"humidity\": %.2f}", data.temperatureC, data.humidity);
            
            mqtt_publish(mqtt_topic, mqtt_payload);
        } else {
            ESP_LOGW(TAG, "Failed to read data from SHT35 sensor");
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void task_report_status(void *arg) {
    while (1) {
        mqtt_send_status();

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Log thông tin firmware
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware Version: %s", app_desc->version);
    ESP_LOGI(TAG, "Build: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free Heap: %lu bytes", esp_get_free_heap_size());

    // Kiểm tra OTA rollback
    ota_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &on_got_ip, NULL));

    // relay_queue = xQueueCreate(10, sizeof(relay_event_t));
    // if (relay_queue == NULL) {
    //     ESP_LOGE(TAG, "Failed to create relay event queue");
    // } else {
    //     xTaskCreate(backend_task, "backend_task", 8192, NULL, 5, NULL);
    // }

    relay_init();
    button_init();
    sensor_hardware_init();

    app_mode = load_last_mode();

    if (app_mode == 1) {
        wifi_init_sta();
        webserver_init();
    } 
    else if (app_mode == 2) {
        pppos_init();
        pppos_start_connect();
    } 
    else {
        ESP_LOGW(TAG, "Offline.");
    }
    // relay_init();
    // button_init();
    // sensor_hardware_init();
    // wifi_init_sta();
    // pppos_init();
    scheduler_init();

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(task_report_status, "reporter", 4096, NULL, 5, NULL);
}