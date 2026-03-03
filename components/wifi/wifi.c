#include "wifi.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "pppos.h"
#include "platform.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "WIFI_PROV";

EventGroupHandle_t s_wifi_event_group;
static bool is_wifi_connected = false;
static esp_netif_t *s_wifi_netif = NULL;
static int wifi_retry_count = 0;
static bool prov_cred_failed = false;
static bool is_provisioning = false;

#define WIFI_MAX_RETRY 5

extern int app_mode;
extern void save_mode_and_restart(int32_t mode);

#define PROV_POP "1111"

bool wifi_is_connected(void) {
    return is_wifi_connected;
}

static void ensure_wifi_netif_created(void) {
    if (s_wifi_netif == NULL) {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
         ESP_LOGI(TAG, "Created Default WiFi STA Netif");
    }
}

void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!is_provisioning) {
        esp_wifi_connect();
        }
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        is_wifi_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        if (is_provisioning) {
            // Đang provisioning, không retry, chờ WIFI_PROV_END restart BLE
            ESP_LOGW(TAG, "WiFi disconnected during provisioning");
        } else {
            // Đã provisioned xong, retry hoặc chuyển offline
            wifi_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d", wifi_retry_count, WIFI_MAX_RETRY);
            
            if (wifi_retry_count < WIFI_MAX_RETRY) {
                esp_wifi_connect();
            } else {
                wifi_retry_count = 0;
                ESP_LOGE(TAG, "Max retry reached, switching to OFFLINE mode");
                ESP_LOGW(TAG, "Hold Button 1 for 4s to start BLE provisioning");
                esp_wifi_stop();
                save_mode_and_restart(0);
            }
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        is_wifi_connected = true;
        wifi_retry_count = 0;  // Reset retry count khi kết nối thành công
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning Started");
                is_provisioning = true;
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received SSID: %s", (const char *) wifi_sta_cfg->ssid);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning Failed: %d", *reason);
                prov_cred_failed = true;
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning Successful");
                prov_cred_failed = false;
                is_provisioning = false;
                break;
            case WIFI_PROV_END:
                ESP_LOGI(TAG, "Provisioning End");
                is_provisioning = false;
                if (prov_cred_failed) {
                    ESP_LOGW(TAG, "Credentials failed, resetting provisioning and restarting...");
                    prov_cred_failed = false;
                    wifi_prov_mgr_reset_provisioning();
                    wifi_prov_mgr_deinit();
                    esp_restart();
                } else {
                    wifi_prov_mgr_deinit();
                }
                break;
            default:
                break;
        }
    }
}

void wifi_start_smartconfig(void) {
    if (is_wifi_connected && app_mode == 1) return;
    
    app_mode = 1;

    if (pppos_is_connected()) {
        pppos_disconnect();
    }

    // ensure_wifi_netif_created();

    // wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // esp_err_t err = esp_wifi_init(&cfg);
    // if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    //     ESP_LOGE(TAG, "Failed to init wifi: %s", esp_err_to_name(err));
    // }
    
    esp_wifi_set_mode(WIFI_MODE_STA);

    char *service_name = platform_get_id(); 
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char *pop = PROV_POP;
    const char *service_key = NULL; 

    // wifi_prov_mgr_config_t config = {
    //     .scheme = wifi_prov_scheme_ble,
    //     .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    // };

    // wifi_prov_mgr_init(config);

    ESP_LOGI(TAG, "Starting BLE Provisioning: %s", service_name);

    esp_err_t err = wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(err));
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ensure_wifi_netif_created();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    };

    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    if (!provisioned) {
        ESP_LOGI(TAG, "Not provisioned, starting BLE...");
        wifi_start_smartconfig();
    } else {
        ESP_LOGI(TAG, "Already provisioned, connecting...");
        wifi_prov_mgr_deinit(); 
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}