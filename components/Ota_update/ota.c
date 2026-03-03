#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "ota.h"
#include "platform.h"

static const char *TAG = "OTA";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static bool ota_in_progress = false;
static char ota_status_topic[64];

// ==================== MQTT HELPER ====================
void ota_set_mqtt_client(void *client)
{
    mqtt_client = (esp_mqtt_client_handle_t)client;
    mqtt_connected = true;
    
    char *id = platform_get_id();
    snprintf(ota_status_topic, sizeof(ota_status_topic), "/device/%s/ota/status", id);
}

bool ota_is_mqtt_connected(void)
{
    return mqtt_client != NULL && mqtt_connected;
}

static void ota_publish_status(const char *msg)
{
    if (mqtt_client && mqtt_connected) {
        esp_mqtt_client_publish(mqtt_client, ota_status_topic, msg, 0, 1, 0);
    }
}

static void ota_publish_progress(int progress, const char *status)
{
    char msg[200];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(msg, sizeof(msg), 
        "{\"ota_progress\":%d,\"status\":\"%s\",\"current_version\":\"%s\"}", 
        progress, status, app->version);
    ota_publish_status(msg);
}

// ==================== VERSION COMPARE ====================
static int compare_version(const char *v1, const char *v2)
{
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return (major1 > major2) ? 1 : -1;
    if (minor1 != minor2) return (minor1 > minor2) ? 1 : -1;
    if (patch1 != patch2) return (patch1 > patch2) ? 1 : -1;
    return 0;
}

// ==================== OTA TASK ====================
static void ota_task(void *pvParameter)
{
    char *url = (char *)pvParameter;
    ESP_LOGI(TAG, "========== OTA UPDATE STARTED ==========");
    ESP_LOGI(TAG, "URL: %s", url);
    
    ota_publish_progress(0, "connecting");

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    // Bắt đầu OTA
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &ota_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA Begin Failed: %s", esp_err_to_name(ret));
        char msg[150];
        snprintf(msg, sizeof(msg), "{\"ota\":\"failed\",\"stage\":\"connect\",\"error\":\"%s\"}", esp_err_to_name(ret));
        ota_publish_status(msg);
        goto ota_end;
    }
    
    ota_publish_progress(5, "connected");

    // Lấy thông tin firmware mới
    esp_app_desc_t new_app_info;
    ret = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get new firmware info: %s", esp_err_to_name(ret));
        char msg[150];
        snprintf(msg, sizeof(msg), "{\"ota\":\"failed\",\"stage\":\"get_info\",\"error\":\"%s\"}", esp_err_to_name(ret));
        ota_publish_status(msg);
        esp_https_ota_abort(ota_handle);
        goto ota_end;
    }

    // Lấy thông tin firmware hiện tại
    const esp_app_desc_t *current_app_info = esp_app_get_description();

    ESP_LOGI(TAG, "Current version: %-14s", current_app_info->version);
    ESP_LOGI(TAG, "New version:     %-14s", new_app_info.version);

    // Báo version qua MQTT
    char version_msg[250];
    snprintf(version_msg, sizeof(version_msg), 
        "{\"ota_progress\":10,\"status\":\"checking_version\",\"current\":\"%s\",\"new\":\"%s\"}", 
        current_app_info->version, new_app_info.version);
    ota_publish_status(version_msg);

    // So sánh version
    int cmp = compare_version(new_app_info.version, current_app_info->version);
    if (cmp <= 0) {
        ESP_LOGW(TAG, "New version is same or older. Aborting OTA.");
        char msg[200];
        snprintf(msg, sizeof(msg), 
            "{\"ota\":\"skipped\",\"reason\":\"version_check\",\"current\":\"%s\",\"new\":\"%s\"}", 
            current_app_info->version, new_app_info.version);
        ota_publish_status(msg);
        esp_https_ota_abort(ota_handle);
        goto ota_end;
    }

    ESP_LOGI(TAG, "New version available! Downloading...");
    ota_publish_progress(15, "downloading");

    // Download và ghi firmware với progress reporting
    int last_progress = 15;
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        // Tính và báo tiến trình
        int total = esp_https_ota_get_image_size(ota_handle);
        int read_len = esp_https_ota_get_image_len_read(ota_handle);
        
        if (total > 0) {
            int progress = 15 + (read_len * 75) / total;
            
            // Báo mỗi 10%
            if (progress >= last_progress + 10) {
                last_progress = progress;
                ESP_LOGI(TAG, "OTA Progress: %d%% (%d / %d bytes)", progress, read_len, total);
                ota_publish_progress(progress, "downloading");
            }
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA Failed during download: %s", esp_err_to_name(ret));
        char msg[150];
        snprintf(msg, sizeof(msg), "{\"ota\":\"failed\",\"stage\":\"download\",\"error\":\"%s\"}", esp_err_to_name(ret));
        ota_publish_status(msg);
        esp_https_ota_abort(ota_handle);
        goto ota_end;
    }

    ota_publish_progress(95, "verifying");

    // Hoàn tất OTA
    ret = esp_https_ota_finish(ota_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "========== OTA SUCCESS ==========");
        ESP_LOGI(TAG, "Rebooting to version %s...", new_app_info.version);
        
        char msg[250];
        snprintf(msg, sizeof(msg), 
            "{\"ota\":\"success\",\"old_version\":\"%s\",\"new_version\":\"%s\",\"rebooting\":true}", 
            current_app_info->version, new_app_info.version);
        ota_publish_status(msg);
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Finish Failed: %s", esp_err_to_name(ret));
        char msg[150];
        snprintf(msg, sizeof(msg), "{\"ota\":\"failed\",\"stage\":\"finish\",\"error\":\"%s\"}", esp_err_to_name(ret));
        ota_publish_status(msg);
    }

ota_end:
    ota_in_progress = false;
    ESP_LOGI(TAG, "OTA TASK END");
    free(url);
    vTaskDelete(NULL);
}

// ==================== PUBLIC FUNCTIONS ====================
void ota_start(const char *url)
{
    if (ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress!");
        return;
    }
    ota_in_progress = true;
    
    char *ota_url = strdup(url);
    if (ota_url == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for OTA URL");
        return;
    }
    
    ESP_LOGI(TAG, "Starting OTA task with URL: %s", url);
    xTaskCreate(&ota_task, "ota_task", 8192, ota_url, 5, NULL);
}

void ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    ESP_LOGI(TAG, "Running partition: %s (0x%lx)", running->label, running->address);
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "========== FIRST BOOT AFTER OTA ==========");
            ESP_LOGI(TAG, "Running diagnostics...");
            
            // Chờ hệ thống ổn định
            vTaskDelay(pdMS_TO_TICKS(5000));
            
            bool diagnostics_ok = true;
            
            // Kiểm tra 1: Heap memory
            uint32_t free_heap = esp_get_free_heap_size();
            if (free_heap < 50000) {
                ESP_LOGE(TAG, "FAIL: Low heap memory (%lu bytes)", free_heap);
                diagnostics_ok = false;
            } else {
                ESP_LOGI(TAG, "PASS: Heap memory OK (%lu bytes)", free_heap);
            }
            
            // Thêm các kiểm tra khác (WiFi, sensor, relay...)
            
            if (diagnostics_ok) {
                ESP_LOGI(TAG, "DIAGNOSTICS PASSED - Marking firmware VALID");
                esp_ota_mark_app_valid_cancel_rollback();
            } else {
                ESP_LOGE(TAG, "DIAGNOSTICS FAILED - ROLLING BACK...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        } else if (ota_state == ESP_OTA_IMG_VALID) {
            ESP_LOGI(TAG, "Firmware state: VALID");
        }
    }
}
