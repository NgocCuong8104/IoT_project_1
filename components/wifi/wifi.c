#include "wifi.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "pppos.h"
#include "platform.h"
#include "led_status.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "protocomm_ble.h"
#include "nvs_flash.h"

#define PROV_TIMEOUT_MS 300000
#define PROV_POP "1111"
#define MAX_RETRY_DELAY_MS 60000

static const char *TAG = "WIFI_PROV";

EventGroupHandle_t s_wifi_event_group;

static bool is_wifi_connected = false;
static bool is_provisioning = false;
static bool is_ble_connected = false;
static bool prov_cred_failed = false;

static esp_netif_t *s_wifi_netif = NULL;
static uint32_t current_retry_delay_ms = 1000;

static TimerHandle_t prov_timeout_timer = NULL;
static TimerHandle_t wifi_retry_timer = NULL;
static TimerHandle_t soft_restart_prov_timer = NULL;

extern int app_mode;
extern void save_mode_and_restart(int32_t mode);
void wifi_start_smartconfig(void);
void wifi_reset_credentials_and_provision(void) {
    ESP_LOGW(TAG, "WiFi reset: clearing credentials");

    if (wifi_retry_timer) {
        xTimerStop(wifi_retry_timer, 0);
    }
    
    if (is_wifi_connected) {
        esp_wifi_disconnect();
        is_wifi_connected = false;
    }
    
    esp_err_t err = esp_wifi_restore();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "WiFi credentials cleared securely");
    }
    
    // lưu trạng thái để khi khởi động lại sẽ vào chế độ WiFi provisioning (app_mode = 1)
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "RUN_MODEM", 1);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    ESP_LOGW(TAG, "Restarting device to clear BLE Cache...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

//in ẩn SSID và password đã nhận được để debug
static void debug_print_wifi_credentials(wifi_sta_config_t *sta, const char* pretext) {
    if (sta == NULL) return;
    
    size_t passlen = strnlen((const char*) sta->password, sizeof(sta->password));
    
    ESP_LOGI(TAG, "%s SSID: %.*s", pretext,
             strnlen((const char *) sta->ssid, sizeof(sta->ssid)), 
             (const char *) sta->ssid);

    if (passlen > 0) {
        char masked_pass[65];
        strncpy(masked_pass, (const char*)sta->password, sizeof(masked_pass) - 1);
        masked_pass[sizeof(masked_pass) - 1] = '\0';
        
        if (passlen > 3) {
            memset(masked_pass + 1, '*', passlen - 2);
        } else {
            memset(masked_pass, '*', passlen);
        }
        ESP_LOGI(TAG, "%s Password: %s (len=%d)", pretext, masked_pass, (int)passlen);
    } else {
        ESP_LOGW(TAG, "%s Password: (empty)", pretext);
    }
}

static void soft_restart_prov_cb(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "Soft restarting BLE provisioning...");
    wifi_prov_mgr_reset_provisioning();
    
    wifi_start_smartconfig();
}

static void prov_timeout_callback(TimerHandle_t xTimer) {
    if (is_ble_connected) {
        xTimerReset(xTimer, 0);
    } else {
        xTimerStart(soft_restart_prov_timer, 0);
    }
}

static void wifi_retry_cb(TimerHandle_t xTimer) {
    esp_wifi_connect();
}

static void ensure_wifi_netif_created(void) {
    if (s_wifi_netif == NULL) {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
    }
}

bool wifi_is_connected(void) {
    return is_wifi_connected;
}

void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        switch (event_id) {
            case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
                is_ble_connected = true;
                if (prov_timeout_timer != NULL) {
                    xTimerReset(prov_timeout_timer, 0);
                }
                break;
            case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
                is_ble_connected = false;
                break;
        }
        return;
    }

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            if (!is_provisioning && app_mode != 2) {
                esp_wifi_connect();
            }
        } 
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            is_wifi_connected = false;
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            
            led_status_off();

            if (!is_provisioning) {
                xTimerChangePeriod(wifi_retry_timer, pdMS_TO_TICKS(current_retry_delay_ms), 0);
                xTimerStart(wifi_retry_timer, 0);
                ESP_LOGI(TAG, "WiFi retry in %lu ms", current_retry_delay_ms);
                current_retry_delay_ms *= 2;
                if (current_retry_delay_ms > MAX_RETRY_DELAY_MS) {
                    current_retry_delay_ms = MAX_RETRY_DELAY_MS;
                }
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        is_wifi_connected = true;
        current_retry_delay_ms = 1000;
        xTimerStop(wifi_retry_timer, 0);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        led_status_on();
        ESP_LOGI(TAG, "WiFi connected - LED ON");
        return;
    }

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                is_provisioning = true;
                prov_cred_failed = false;
                if (prov_timeout_timer) xTimerStart(prov_timeout_timer, 0);
                break;

            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                debug_print_wifi_credentials(wifi_sta_cfg, "[RECV]");
                break;
            }

            case WIFI_PROV_CRED_FAIL: {
                prov_cred_failed = true;
                esp_err_t err = wifi_prov_mgr_reset_sm_state_on_failure();
                
                if (err != ESP_OK || !is_ble_connected) {
                    if (prov_timeout_timer) xTimerStop(prov_timeout_timer, 0);
                    wifi_prov_mgr_reset_provisioning();
                    xTimerStart(soft_restart_prov_timer, 0);
                }
                break;
            }

            case WIFI_PROV_CRED_SUCCESS:
                prov_cred_failed = false;
                is_provisioning = false;
                if (prov_timeout_timer) xTimerStop(prov_timeout_timer, 0);
                
                wifi_config_t wifi_cfg;
                if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
                    debug_print_wifi_credentials(&wifi_cfg.sta, "[SAVED]");
                }
                
                break;

            case WIFI_PROV_END:
                is_provisioning = false;
                if (prov_timeout_timer) xTimerStop(prov_timeout_timer, 0);
                
                if (prov_cred_failed) {
                    prov_cred_failed = false;
                    wifi_prov_mgr_reset_provisioning();
                    xTimerStart(soft_restart_prov_timer, 0);
                } else {
                    ESP_LOGI(TAG, "Provisioning complete, manager kept active");
                }
                break;

            default:
                break;
        }
    }
}

void wifi_start_smartconfig(void) {
    if (is_wifi_connected && app_mode == 1) return;
    
    ESP_LOGI(TAG, "Starting WiFi provisioning (BLE mode...");
    
    app_mode = 1;
    
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "RUN_MODEM", 1);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "WiFi mode saved to NVS");
    }

    if (pppos_is_connected()) {
        pppos_disconnect();
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    char *service_name = platform_get_id();
    
    esp_err_t err = wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1,
        PROV_POP,
        service_name,
        NULL
    );
    
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Manager was de-initialized. Re-initializing now...");
        
        wifi_prov_mgr_config_t prov_config = {
            .scheme = wifi_prov_scheme_ble,
            .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
        };
        ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_config));
        
        err = wifi_prov_mgr_start_provisioning(
            WIFI_PROV_SECURITY_1,
            PROV_POP,
            service_name,
            NULL
        );
        
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Re-initialized and Started Provisioning Successfully!");
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(err));
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ensure_wifi_netif_created();

    prov_timeout_timer = xTimerCreate("prov_timeout", pdMS_TO_TICKS(PROV_TIMEOUT_MS), pdFALSE, NULL, prov_timeout_callback);
    wifi_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000), pdFALSE, NULL, wifi_retry_cb);
    soft_restart_prov_timer = xTimerCreate("soft_restart", pdMS_TO_TICKS(100), pdFALSE, NULL, soft_restart_prov_cb);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_prov_mgr_config_t prov_config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_config));
    

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));
    
    bool auto_prov_started = false;

    if (!provisioned) {
        if (app_mode != 0) {
            ESP_LOGW(TAG, "BT5 reset detected: auto-starting BLE provisioning");
            wifi_start_smartconfig();  
            auto_prov_started = true;
        } else {
            ESP_LOGW(TAG, "WiFi not provisioned. Hold BT1 for 4s to enter provisioning mode");
        }
    } else {
        wifi_config_t wifi_cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
            debug_print_wifi_credentials(&wifi_cfg.sta, "[SAVED]");
        }
        
        ESP_LOGI(TAG, "WiFi provisioning manager kept active (BT5 reset ready)");
    }
    
    if (!auto_prov_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}