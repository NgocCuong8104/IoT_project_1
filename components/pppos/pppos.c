#include "pppos.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "driver/uart.h"
#include "mqtt.h"
#include "wifi.h"
#include "esp_wifi.h"
#include "led_status.h"

static const char *TAG = "PPPOS_COMPONENT";
static EventGroupHandle_t event_group = NULL;
static EventGroupHandle_t mqtt_event_group = NULL;
static volatile bool is_modem_connected = false;
static volatile bool is_mqtt_initialized = false;

#define MQTT_INIT_BIT     0x01
#define MODEM_LOST_BIT    0x02

#define MODEM_TX_PIN      17
#define MODEM_RX_PIN      16
#define MODEM_RST_PIN     5
#define MODEM_UART_NUM    UART_NUM_2

#define MODEM_APN_DEFAULT "v-internet"

static char detected_apn[32] = MODEM_APN_DEFAULT;

static esp_modem_dce_t *dce = NULL;

// Hàm kiểm tra kết nối PPPoS
extern volatile int app_mode;

bool pppos_is_connected(void) {
    return is_modem_connected;
}

static void modem_reset(void) {
    gpio_reset_pin(MODEM_RST_PIN);
    gpio_set_direction(MODEM_RST_PIN, GPIO_MODE_OUTPUT);
    
    gpio_set_level(MODEM_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(MODEM_RST_PIN, 1);
    
    // Chờ modem khởi động (1-3 giây)
    vTaskDelay(pdMS_TO_TICKS(3000));
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Modem Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        is_modem_connected = true;
        
        led_status_on();
        ESP_LOGI(TAG, "4G connected - LED ON");
        
        if (mqtt_event_group != NULL) {
            xEventGroupSetBits(mqtt_event_group, MQTT_INIT_BIT);
        }
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Modem Disconnected");
        
        is_modem_connected = false;

        is_mqtt_initialized = false;
        
        led_status_off();
        
        // Báo hiệu reconnect task thức dậy ngay
        if (mqtt_event_group != NULL) {
            xEventGroupSetBits(mqtt_event_group, MODEM_LOST_BIT);
        }
    }
}

static bool wait_for_network(void) {
    int retry = 0;
    while (retry < 20) { 
        int rssi = 0, ber = 0;
        
        esp_err_t err = esp_modem_get_signal_quality(dce, &rssi, &ber);
        
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Signal Quality: RSSI=%d, BER=%d", rssi, ber);
            
            // RSSI thường là một số âm, giá trị càng cao (gần 0) thì tín hiệu càng tốt. Giá trị 99 thường có nghĩa là không thể đo được tín hiệu.
            if (rssi != 99 && rssi > 10) {
                ESP_LOGI(TAG, "Network Signal OK.");
                return true; 
            }
        } else {
            ESP_LOGW(TAG, "Failed to read signal ");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    ESP_LOGE(TAG, "Timeout: No Network Signal!");
    return false;
}

// Hàm ngắt kết nối PPPoS
void pppos_disconnect(void) {
    if (dce != NULL && is_modem_connected) {
        ESP_LOGW(TAG, "Stop 4G");
        esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        is_modem_connected = false; 
    }
}

void pppos_start_connect(void) {
    if (pppos_is_connected()) {
        ESP_LOGW(TAG, "4G da ket noi roi.");
        return;
    }
    
    if (dce == NULL) {
        ESP_LOGE(TAG, "Modem chua khoi tao phan cung!");
        return;
    }

    ESP_LOGI(TAG, "DANG KET NOI 4G (PPPoS)");

    app_mode = 2;

    if (wifi_is_connected()) {
         ESP_LOGW(TAG, "Stop wifi");
         esp_wifi_disconnect(); 
         esp_wifi_stop();
    }
    // modem_reset();

    uart_flush_input(MODEM_UART_NUM);
    uart_flush(MODEM_UART_NUM);

    bool sync_ok = false;
    for (int i = 0; i < 10; i++) {
        if (esp_modem_sync(dce) == ESP_OK) {
            sync_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!sync_ok) {
        ESP_LOGE(TAG, "Modem not responding -> Performing Hard Reset");
        modem_reset();
        sync_ok = (esp_modem_sync(dce) == ESP_OK);
    }

    if (sync_ok) {
        if (wait_for_network()) {
            if (esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
                ESP_LOGE(TAG, "Loi gui lenh ket noi!");
            }
        }
    } else {
        ESP_LOGE(TAG, "Modem khong phan hoi sau khi Reset!");
    }
}

void pppos_mqtt_task(void *arg) {
    // Kiểm tra mqtt_event_group trước khi sử dụng
    if (mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "mqtt_event_group is NULL! Task cannot run.");
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            mqtt_event_group,
            MQTT_INIT_BIT,
            pdTRUE,   
            pdFALSE,   
            portMAX_DELAY
        );
        
        if (bits & MQTT_INIT_BIT) {
            // kiểm tra lại trạng thái kết nối trước khi init MQTT
            if (!is_mqtt_initialized) {
                ESP_LOGI(TAG, "Initializing MQTT from separate task...");
                mqtt_init();
                is_mqtt_initialized = true;
                ESP_LOGI(TAG, "MQTT initialized successfully");
            } else {
                ESP_LOGD(TAG, "MQTT already initialized, skipping re-init");
            }
        }
    }
}

void pppos_reconnect(void *arg) {
    // Chờ tín hiệu mất kết nối hoặc timeout 30s
    while (1) {
        if (app_mode != 2) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        bool connected = is_modem_connected;
        
        // Nếu kết nối mất, thự hiện reconnect lại ngay
        // Nếu vẫn kết nối, chỉ kiểm tra mỗi 30s
        uint32_t wait_time = connected ? 30000 : 5000;
        
        EventBits_t bits = xEventGroupWaitBits(
            mqtt_event_group,
            MODEM_LOST_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(wait_time)
        );
        

        // Nếu nhận được MODEM_LOST_BIT hoặc timeout
        if ((bits & MODEM_LOST_BIT) || !connected) {
            if (!connected && app_mode == 2) {
                ESP_LOGW(TAG, "Reconnecting...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                pppos_start_connect();
            }
        }
    }
}
void pppos_init(void) {
    modem_reset();

    // Tạo event group cho MQTT và reconnect
    event_group = xEventGroupCreate();
    mqtt_event_group = xEventGroupCreate();
    
    if (mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create mqtt_event_group!");
        return;
    }
    
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, &on_ip_event, NULL));

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = MODEM_TX_PIN;
    dte_config.uart_config.rx_io_num = MODEM_RX_PIN;
    dte_config.uart_config.baud_rate = 115200;
    dte_config.uart_config.port_num = MODEM_UART_NUM;
    dte_config.task_stack_size = 4096;

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(MODEM_APN_DEFAULT);
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);

    dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, esp_netif);

    if (dce == NULL) {
        ESP_LOGE(TAG, "Failed to initialize Modem Device!");
        return;
    }

    uart_flush_input(MODEM_UART_NUM);
    uart_flush(MODEM_UART_NUM);

    for(int i=0; i<5; i++) {
        if(esp_modem_sync(dce) == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    char name[32] = {0};
    if (esp_modem_get_module_name(dce, name) == ESP_OK) {
        ESP_LOGI(TAG, "Module Name: %s", name);
    } else {
        ESP_LOGE(TAG, "UART Error: Cannot communicate with Modem!");
        return;
    }

    char imsi[32] = {0};
    if (esp_modem_get_imsi(dce, imsi) == ESP_OK) {
        ESP_LOGI(TAG, "SIM OK. IMSI: %s", imsi);
        
        char new_apn[32];
        strcpy(new_apn, MODEM_APN_DEFAULT);
        
        if (strncmp(imsi, "45204", 5) == 0) {
            strcpy(new_apn, "v-internet");
            ESP_LOGI(TAG, "Detected: VIETTEL -> APN: %s", new_apn);
        } else if (strncmp(imsi, "45201", 5) == 0) {
            strcpy(new_apn, "m-wap");
            ESP_LOGI(TAG, "Detected: MOBIFONE -> APN: %s", new_apn);
        } else if (strncmp(imsi, "45202", 5) == 0) {
            strcpy(new_apn, "m3-world");
            ESP_LOGI(TAG, "Detected: VINAPHONE -> APN: %s", new_apn);
        } else if (strncmp(imsi, "45205", 5) == 0) {
            strcpy(new_apn, "internet");
            ESP_LOGI(TAG, "Detected: VIETNAMOBILE -> APN: %s", new_apn);
        } else {
            ESP_LOGW(TAG, "Unknown carrier, using default APN: %s", new_apn);
        }
        
        // Cập nhật APN toàn cục
        strcpy(detected_apn, new_apn);
    } else {
        ESP_LOGE(TAG, "SIM Error: Cannot read IMSI (Check SIM Card)");
        // Sử dụng APN mặc định nếu không đọc được IMSI
        strcpy(detected_apn, MODEM_APN_DEFAULT);
    }

    // Tạo task cho MQTT
    xTaskCreate(pppos_mqtt_task, "pppos_mqtt_task", 3072, NULL, 6, NULL);
    
    // Tạo task cho reconnect
    xTaskCreate(pppos_reconnect, "pppos_reconnect", 4096, NULL, 5, NULL);
    
    vTaskDelay(pdMS_TO_TICKS(2000));
}