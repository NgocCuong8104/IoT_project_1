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

static const char *TAG = "PPPOS_COMPONENT";
static EventGroupHandle_t event_group = NULL;
static bool is_modem_connected = false;

#define MODEM_TX_PIN      17
#define MODEM_RX_PIN      16
#define MODEM_RST_PIN     5
#define MODEM_UART_NUM    UART_NUM_2

// APN mặc định, sẽ được thay đổi tự động dựa trên IMSI
#define MODEM_APN_DEFAULT "v-internet"

static char detected_apn[32] = MODEM_APN_DEFAULT;

static esp_modem_dce_t *dce = NULL;
extern int app_mode;

bool pppos_is_connected(void) {
    return is_modem_connected;
}

static void modem_reset(void) {
    gpio_reset_pin(MODEM_RST_PIN);
    gpio_set_direction(MODEM_RST_PIN, GPIO_MODE_OUTPUT);
    
    gpio_set_level(MODEM_RST_PIN, 1); 
    vTaskDelay(pdMS_TO_TICKS(2000)); 
    
    gpio_set_level(MODEM_RST_PIN, 0);
    
    vTaskDelay(pdMS_TO_TICKS(12000)); 
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Modem Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        is_modem_connected = true;
        ESP_LOGI(TAG, "Modem Connected! Starting MQTT...");
        mqtt_init();
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Modem Disconnected");
        is_modem_connected = false;
    }
}

static bool wait_for_network(void) {
    int retry = 0;
    while (retry < 20) { 
        int rssi = 0, ber = 0;
        
        esp_err_t err = esp_modem_get_signal_quality(dce, &rssi, &ber);
        
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Signal Quality: RSSI=%d, BER=%d", rssi, ber);
            
            if (rssi != 99 && rssi > 5) {
                ESP_LOGI(TAG, "Network Signal OK.");
                return true; 
            }
        } else {
            ESP_LOGW(TAG, "Failed to read signal (Modem not responding?)");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    ESP_LOGE(TAG, "Timeout: No Network Signal!");
    return false;
}

void pppos_disconnect(void) {
    if (dce != NULL&& is_modem_connected) {
        ESP_LOGW(TAG, "Stop 4G");
        esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        is_modem_connected = false;
    }
}

void pppos_start_connect(void) {
    if (is_modem_connected) {
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
        esp_modem_sync(dce); 
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
void pppos_reconnect(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); 

        if (app_mode != 2) {
            continue;
        }

        if (!is_modem_connected) {
            ESP_LOGW(TAG, "Reconnect...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (!is_modem_connected) {
                 pppos_start_connect(); 
            }
        }
    }
}
void pppos_init(void) {
    modem_reset();

    event_group = xEventGroupCreate();
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
    bool apn_changed = false;
    if (esp_modem_get_imsi(dce, imsi) == ESP_OK) {
        ESP_LOGI(TAG, "SIM OK. IMSI: %s", imsi);
        
        char old_apn[32];
        strcpy(old_apn, detected_apn);
        
        if (strncmp(imsi, "45204", 5) == 0) {
            strcpy(detected_apn, "v-internet");
            ESP_LOGI(TAG, "Detected: VIETTEL -> APN: %s", detected_apn);
        } else if (strncmp(imsi, "45201", 5) == 0) {
            strcpy(detected_apn, "m-wap");
            ESP_LOGI(TAG, "Detected: MOBIFONE -> APN: %s", detected_apn);
        } else if (strncmp(imsi, "45202", 5) == 0) {
            strcpy(detected_apn, "m3-world");
            ESP_LOGI(TAG, "Detected: VINAPHONE -> APN: %s", detected_apn);
        } else if (strncmp(imsi, "45205", 5) == 0) {
            strcpy(detected_apn, "internet");
            ESP_LOGI(TAG, "Detected: VIETNAMOBILE -> APN: %s", detected_apn);
        } else {
            ESP_LOGW(TAG, "Unknown carrier, using default APN: %s", detected_apn);
        }
        
        // Kiểm tra xem APN có thay đổi không
        if (strcmp(old_apn, detected_apn) != 0) {
            apn_changed = true;
        }
    } else {
        ESP_LOGE(TAG, "SIM Error: Cannot read IMSI (Check SIM Card)");
    }

    // Nếu APN thay đổi, tạo lại modem với APN đúng
    if (apn_changed) {
        ESP_LOGI(TAG, "Re-initializing modem with correct APN: %s", detected_apn);
        esp_modem_destroy(dce);
        esp_netif_destroy(esp_netif);
        
        esp_modem_dce_config_t new_dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(detected_apn);
        esp_netif_config_t new_netif_config = ESP_NETIF_DEFAULT_PPP();
        esp_netif = esp_netif_new(&new_netif_config);
        
        dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &new_dce_config, esp_netif);
        
        if (dce == NULL) {
            ESP_LOGE(TAG, "Failed to re-initialize Modem with new APN!");
            return;
        }
        
        // Sync lại sau khi tạo mới
        for(int i=0; i<5; i++) {
            if(esp_modem_sync(dce) == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    xTaskCreate(pppos_reconnect, "pppos_reconnect", 4096, NULL, 5, NULL);
    
    vTaskDelay(pdMS_TO_TICKS(2000));
}