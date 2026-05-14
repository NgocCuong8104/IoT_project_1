#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "mbedtls/md5.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "ota.h"
#include "platform.h"

static const char *TAG = "OTA";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static bool ota_in_progress = false;
static char ota_status_topic[64];
static char ota_current_job_id[37] = {0}; // Lưu job_id hiện tại để báo cáo trạng thái

// Biến toàn cục để quản lý trạng thái OTA đang chờ xử lý
static ota_trigger_t pending_ota = {0};
static bool ota_pending = false;
static SemaphoreHandle_t ota_mutex = NULL;

static bool validate_partition_checksum(const esp_partition_t *partition, size_t firmware_size, const char *expected_checksum);

// Hàm xử lý sự kiện MQTT, nhận lệnh OTA qua MQTT 
void ota_set_mqtt_client(void *client)
{
    mqtt_client = (esp_mqtt_client_handle_t)client;
    mqtt_connected = true;
    
    char *id = platform_get_id();
    snprintf(ota_status_topic, sizeof(ota_status_topic), "/device/%s/ota/status", id); // Topic để báo cáo trạng thái OTA
}

// Hàm kiểm tra xem MQTT đã kết nối chưa, để quyết định có nên báo cáo trạng thái OTA qua MQTT hay không
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
    char msg[300];
    const esp_app_desc_t *app = esp_app_get_description();
    if (ota_current_job_id[0] != '\0') {
        snprintf(msg, sizeof(msg), 
            "{\"ota\":\"DOWNLOADING\",\"progress\":%d,\"status\":\"%s\",\"current_version\":\"%s\",\"job_id\":\"%s\"}", 
            progress, status, app->version, ota_current_job_id);
    } else {
        snprintf(msg, sizeof(msg), 
            "{\"ota\":\"DOWNLOADING\",\"progress\":%d,\"status\":\"%s\",\"current_version\":\"%s\"}", 
            progress, status, app->version);
    }
    ota_publish_status(msg);
}

// VERSION COMPARE 
static int compare_version(const char *v1, const char *v2)
{
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;

    const char *v1_clean = (v1[0] == 'v' || v1[0] == 'V') ? v1 + 1 : v1;
    const char *v2_clean = (v2[0] == 'v' || v2[0] == 'V') ? v2 + 1 : v2;
    
    sscanf(v1_clean, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2_clean, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return (major1 > major2) ? 1 : -1;
    if (minor1 != minor2) return (minor1 > minor2) ? 1 : -1;
    if (patch1 != patch2) return (patch1 > patch2) ? 1 : -1;
    return 0;
}

// Hàm task OTA chính, thực hiện toàn bộ quá trình OTA từ kết nối, kiểm tra version, download, verify checksum đến hoàn tất và reboot nếu thành công
static void ota_task(void *pvParameter)
{
    ota_task_params_t *params = (ota_task_params_t *)pvParameter;
    if (!params || !params->url) {
        ESP_LOGE(TAG, "[OTA] Invalid parameters");
        if (params) free(params);
        vTaskDelete(NULL);
        return;
    }
    
    // Lưu job_id hiện tại nếu có, để báo cáo trạng thái OTA chính xác hơn
    if (params->job_id) {
        strncpy(ota_current_job_id, params->job_id, sizeof(ota_current_job_id) - 1);
        ota_current_job_id[sizeof(ota_current_job_id) - 1] = '\0';
    }
    
    ESP_LOGI(TAG, "OTA UPDATE STARTED (job_id: %.8s)", 
             params->job_id ? params->job_id : "N/A");
    ESP_LOGI(TAG, "URL: %s", params->url);
    
    // Báo tiến trình kết nối OTA qua MQTT
    ota_publish_progress(0, "connecting");

    esp_http_client_config_t config = {
        .url = params->url,
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
        char msg[250];
        if (ota_current_job_id[0] != '\0') { // Nếu có job_id, báo cả job_id trong trạng thái lỗi để dễ dàng theo dõi trên server
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"connect\",\"error\":\"%s\",\"job_id\":\"%s\"}", 
                     esp_err_to_name(ret), ota_current_job_id);
        } else {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"connect\",\"error\":\"%s\"}", 
                     esp_err_to_name(ret));
        }
        ota_publish_status(msg);
        goto ota_end;
    }
    
    ota_publish_progress(5, "connected");

    // Lấy thông tin firmware mới
    esp_app_desc_t new_app_info;
    ret = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get new firmware info: %s", esp_err_to_name(ret));
        char msg[250];
        if (ota_current_job_id[0] != '\0') {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"get_info\",\"error\":\"%s\",\"job_id\":\"%s\"}", 
                     esp_err_to_name(ret), ota_current_job_id);
        } else {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"get_info\",\"error\":\"%s\"}", 
                     esp_err_to_name(ret));
        }
        ota_publish_status(msg);
        esp_https_ota_abort(ota_handle);
        goto ota_end;
    }

    // Lấy thông tin firmware hiện tại
    const esp_app_desc_t *current_app_info = esp_app_get_description();

    ESP_LOGI(TAG, "Current version: %-14s", current_app_info->version);
    ESP_LOGI(TAG, "New version:     %-14s", new_app_info.version);

    // Báo version qua MQTT
    char version_msg[300];
    if (ota_current_job_id[0] != '\0') {
        snprintf(version_msg, sizeof(version_msg), 
            "{\"ota\":\"DOWNLOADING\",\"progress\":10,\"status\":\"checking_version\",\"current\":\"%s\",\"new\":\"%s\",\"job_id\":\"%s\"}", 
            current_app_info->version, new_app_info.version, ota_current_job_id);
    } else {
        snprintf(version_msg, sizeof(version_msg), 
            "{\"ota\":\"DOWNLOADING\",\"progress\":10,\"status\":\"checking_version\",\"current\":\"%s\",\"new\":\"%s\"}", 
            current_app_info->version, new_app_info.version);
    }
    ota_publish_status(version_msg);

    // So sánh version
    int cmp = compare_version(new_app_info.version, current_app_info->version);
    if (cmp <= 0) {
        ESP_LOGW(TAG, "New version is same or older. Aborting OTA.");
        char msg[300];
        if (ota_current_job_id[0] != '\0') {
            snprintf(msg, sizeof(msg), 
                "{\"ota\":\"FAILED\",\"stage\":\"version_check\",\"reason\":\"version_same_or_older\",\"current\":\"%s\",\"new\":\"%s\",\"job_id\":\"%s\"}", 
                current_app_info->version, new_app_info.version, ota_current_job_id);
        } else {
            snprintf(msg, sizeof(msg), 
                "{\"ota\":\"FAILED\",\"stage\":\"version_check\",\"reason\":\"version_same_or_older\",\"current\":\"%s\",\"new\":\"%s\"}", 
                current_app_info->version, new_app_info.version);
        }
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
            
            if (progress >= last_progress + 10) {
                last_progress = progress;
                ESP_LOGI(TAG, "OTA Progress: %d%% (%d / %d bytes)", progress, read_len, total);
                ota_publish_progress(progress, "downloading");
            }
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA Failed during download: %s", esp_err_to_name(ret));
        char msg[250];
        if (ota_current_job_id[0] != '\0') {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"download\",\"error\":\"%s\",\"job_id\":\"%s\"}", 
                     esp_err_to_name(ret), ota_current_job_id);
        } else {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"download\",\"error\":\"%s\"}", 
                     esp_err_to_name(ret));
        }
        ota_publish_status(msg);
        esp_https_ota_abort(ota_handle);
        goto ota_end;
    }

    ota_publish_progress(95, "verifying");

    // Kiểm tra checksum nếu server cung cấp, để đảm bảo integrity của firmware đã tải về trước khi hoàn tất OTA
    bool checksum_valid = true;
    if (params->checksum && params->checksum[0] != '\0') {
        ESP_LOGI(TAG, "Server provided checksum for validation: %s", params->checksum);
        
        int firmware_size = esp_https_ota_get_image_len_read(ota_handle);
        const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
        
        if (update_partition) {
            ESP_LOGI(TAG, "Reading %d bytes from partition %s for MD5 check...", 
                     firmware_size, update_partition->label);
            if (!validate_partition_checksum(update_partition, firmware_size, params->checksum)) {
                checksum_valid = false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to get update partition info");
            checksum_valid = false;
        }
    } else {
        ESP_LOGW(TAG, "No checksum provided - firmware NOT integrity-verified");
        ESP_LOGW(TAG, "Recommend: Always provide MD5 checksum from server");
        checksum_valid = true;  // Nếu không có checksum, vẫn cho phép OTA tiếp tục nhưng cảnh báo về việc không được xác min
    }

    if (!checksum_valid) {
        ESP_LOGE(TAG, "Firmware validation failed! Aborting update.");
        char msg[250];
        if (ota_current_job_id[0] != '\0') {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"verify\",\"error\":\"checksum_mismatch\",\"job_id\":\"%s\"}", ota_current_job_id);
        } else {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"verify\",\"error\":\"checksum_mismatch\"}");
        }
        ota_publish_status(msg);
        esp_https_ota_abort(ota_handle);
        goto ota_end;
    }

    // Hoàn tất OTA
    ret = esp_https_ota_finish(ota_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA SUCCESS");
        ESP_LOGI(TAG, "Firmware flashed successfully. Rebooting to version %s...", new_app_info.version);
        
        char msg[250];
        if (ota_current_job_id[0] != '\0') {
            snprintf(msg, sizeof(msg), 
                "{\"ota\":\"REBOOTING\",\"progress\":100,\"status\":\"rebooting\",\"job_id\":\"%s\"}",
                ota_current_job_id);
        } else {
            snprintf(msg, sizeof(msg), 
                "{\"ota\":\"REBOOTING\",\"progress\":100,\"status\":\"rebooting\"}");
        }
        ota_publish_status(msg);
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Finish Failed: %s", esp_err_to_name(ret));
        char msg[250];
        if (ota_current_job_id[0] != '\0') {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"finish\",\"error\":\"%s\",\"job_id\":\"%s\"}", 
                     esp_err_to_name(ret), ota_current_job_id);
        } else {
            snprintf(msg, sizeof(msg), "{\"ota\":\"FAILED\",\"stage\":\"finish\",\"error\":\"%s\"}", 
                     esp_err_to_name(ret));
        }
        ota_publish_status(msg);
    }

ota_end:
    ota_in_progress = false;
    memset(ota_current_job_id, 0, sizeof(ota_current_job_id));
    ESP_LOGI(TAG, "OTA TASK END");
    if (params) {
        if (params->url) free(params->url);
        if (params->job_id) free(params->job_id);
        if (params->checksum) free(params->checksum);
        free(params);
    }
    vTaskDelete(NULL);
}
 // Hàm gửi xác nhận boot thành công sau khi reboot vào firmware mới
void ota_send_boot_confirmation(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    uint32_t free_heap = esp_get_free_heap_size();
    char msg[350];
    
    // kiểm tra nếu có job_id của OTA trước đó
    if (ota_current_job_id[0] != '\0') {
        snprintf(msg, sizeof(msg), 
            "{\"ota\":\"boot_confirmed\",\"boot_confirmed\":true,\"version\":\"%s\",\"free_heap\":%lu,\"job_id\":\"%s\"}", 
            app->version, free_heap, ota_current_job_id);
    } else {
        snprintf(msg, sizeof(msg), 
            "{\"ota\":\"boot_confirmed\",\"boot_confirmed\":true,\"version\":\"%s\",\"free_heap\":%lu}", 
            app->version, free_heap);
    }
    ota_publish_status(msg);
    ESP_LOGI(TAG, "[OTA BOOT] Boot confirmation sent: %s", msg);
}

// hàm đánh dấu firmware mới là hợp lệ sau khi đã reboot vào firmware mới và xác nhận hệ thống ổn định
void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            
            uint32_t free_heap = esp_get_free_heap_size();
            
            ESP_LOGI(TAG, "System Stable - Free Heap: %lu bytes", free_heap);
            
            if (free_heap >= 30000) {
                ESP_LOGI(TAG, "[OTA VALIDATION] PASSED - Marking firmware VALID");
                esp_ota_mark_app_valid_cancel_rollback();
            } else {
                ESP_LOGE(TAG, "[OTA VALIDATION] FAILED (Low Heap %lu) - ROLLING BACK", free_heap);
                
                // Báo rollback về server
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "{\"ota\":\"FAILED\",\"stage\":\"boot_validation\","
                    "\"error\":\"low_heap_%lu\"}",
                    free_heap);
                ota_publish_status(msg);
                ESP_LOGI(TAG, "[OTA VALIDATION] Rollback status published to server");
                
                vTaskDelay(pdMS_TO_TICKS(2000));    
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        } else if (ota_state == ESP_OTA_IMG_VALID) {
            ESP_LOGD(TAG, "[OTA VALIDATION] Already marked VALID previously");
        }
    } else {
        ESP_LOGW(TAG, "[OTA VALIDATION] Failed to get OTA state");
    }
}

// hàm kiểm tra URL
static bool validate_url(const char *url) {
    if (!url || strlen(url) == 0) return false;
    if (strlen(url) > 255) return false;
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

// hàm tính MD5 của firmware
static bool compute_md5_partition(const esp_partition_t *partition, size_t file_size, char *md5_str)
{
    if (!partition || !md5_str) return false;
    
    mbedtls_md5_context context;
    mbedtls_md5_init(&context);
    
    size_t offset = 0;
    const size_t buf_size = 4096;
    uint8_t *buf = malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "MD5 buffer alloc failed");
        return false;
    }
    
    // Đọc dữ liệu từ partition và cập nhật vào context MD5
    while (offset < file_size) {
        size_t read_len = buf_size;
        if (offset + read_len > file_size) {
            read_len = file_size - offset;
        }
        
        if (esp_partition_read(partition, offset, buf, read_len) != ESP_OK) {
            ESP_LOGE(TAG, "Failed reading partition for MD5 at offset %d", offset);
            free(buf);
            return false;
        }
        
        mbedtls_md5_update(&context, buf, read_len);
        offset += read_len;
    }
    
    free(buf);
    
    uint8_t digest[16];
    mbedtls_md5_finish(&context, digest);
    
    for (int i = 0; i < 16; i++) {
        snprintf(md5_str + (i * 2), 3, "%02x", digest[i]);
    }
    md5_str[32] = '\0';
    return true;
}

// hàm kiểm tra checksum đã tải về có khớp với checksum mong đợi từ server hay không
static bool validate_partition_checksum(const esp_partition_t *partition, size_t firmware_size, const char *expected_checksum)
{
    if (!partition || !expected_checksum) {
        ESP_LOGW(TAG, "Missing partition or expected checksum");
        return false;
    }
    
    char computed_checksum[33] = {0}; // MD5 checksum có độ dài 32 ký tự + null terminator
    if (!compute_md5_partition(partition, firmware_size, computed_checksum)) {
        return false;
    }
    
    // So sánh checksum đã tính với checksum từ server
    if (strcasecmp(computed_checksum, expected_checksum) == 0) {
        ESP_LOGI(TAG, "Validation PASSED: %s", computed_checksum);
        return true;
    } else {
        ESP_LOGE(TAG, "Mismatch! Expected: %s, Got: %s", expected_checksum, computed_checksum);
        return false;
    }
}

// hàm xử lý lện OTA từ mqtt, xác nhận mandatory/optional, lưu trạng thái chờ nếu optional, và bắt đầu OTA ngay nếu mandatory
void handle_ota_trigger(const cJSON *root)
{
    if (!root) return;

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        ESP_LOGW(TAG, "Missing 'action' field");
        return; // bỏ qua nếu không có trường action hoặc action không phải string
    }
    
    if (strcmp(action->valuestring, "update_available") == 0) {
        cJSON *url       = cJSON_GetObjectItem(root, "url");
        cJSON *version   = cJSON_GetObjectItem(root, "version");
        cJSON *checksum  = cJSON_GetObjectItem(root, "checksum");
        cJSON *mandatory = cJSON_GetObjectItem(root, "is_mandatory");
        cJSON *job_id    = cJSON_GetObjectItem(root, "job_id");

        // kiểm tra URL
        if (!url || !cJSON_IsString(url)) {
            ESP_LOGW(TAG, "Invalid URL");
            return;
        }
        
        // kiểm tra định dạng url
        if (!validate_url(url->valuestring)) {
            ESP_LOGW(TAG, "Invalid URL format");
            return;
        }

        // dùng semaphore để bảo vệ truy cập vào biến trạng thái OTA đang chờ xử lý
        if (ota_mutex) xSemaphoreTake(ota_mutex, portMAX_DELAY);

        // lưu thông tin vào biến toàn cục để xử lí nếu là OTA tuỳ chọn (optional)
        strncpy(pending_ota.url, url->valuestring, sizeof(pending_ota.url) - 1);
        pending_ota.url[sizeof(pending_ota.url) - 1] = '\0';
        
        // Lưu version, checksum, job_id, để báo cáo trạng thái chính xác hơn và hỗ trợ xác nhận OTA
        if (version && cJSON_IsString(version)) {
            strncpy(pending_ota.version, version->valuestring, sizeof(pending_ota.version) - 1);
            pending_ota.version[sizeof(pending_ota.version) - 1] = '\0';
        } else {
            pending_ota.version[0] = '\0';
        }
        
        if (checksum && cJSON_IsString(checksum)) {
            strncpy(pending_ota.checksum, checksum->valuestring, sizeof(pending_ota.checksum) - 1);
            pending_ota.checksum[sizeof(pending_ota.checksum) - 1] = '\0';
        } else {
            pending_ota.checksum[0] = '\0';
        }
        
        if (job_id && cJSON_IsString(job_id)) {
            strncpy(pending_ota.job_id, job_id->valuestring, sizeof(pending_ota.job_id) - 1);
            pending_ota.job_id[sizeof(pending_ota.job_id) - 1] = '\0';
        } else {
            pending_ota.job_id[0] = '\0';
        }
        
        pending_ota.is_mandatory = mandatory && cJSON_IsTrue(mandatory);
        pending_ota.pending_time_ms = esp_timer_get_time() / 1000;  

        if (ota_mutex) xSemaphoreGive(ota_mutex);

        if (pending_ota.is_mandatory) {
            // MANDATORY
            ESP_LOGW(TAG, "MANDATORY OTA — bắt đầu ngay: %s (job_id: %.8s)", 
                     pending_ota.version, pending_ota.job_id);
            ota_start_with_job(pending_ota.url, pending_ota.job_id, pending_ota.checksum);
        } else {

            // OPTIONAL
            ESP_LOGI(TAG, "Optional available: %s — chờ xác nhận (job_id: %.8s)", 
                     pending_ota.version, pending_ota.job_id);
            ota_pending = true;
            
            // Publish thông báo để app mobile biết có update tuỳ chọn
            char notify_msg[250];
            snprintf(notify_msg, sizeof(notify_msg),
                "{\"action\":\"ota_available\",\"version\":\"%s\",\"is_mandatory\":false,\"job_id\":\"%s\"}",
                pending_ota.version, pending_ota.job_id);
            ota_publish_status(notify_msg);
        }
    }
    else if (strcmp(action->valuestring, "user_confirmed_update") == 0) {
        // App mobile đã xác nhận 
        if (ota_mutex) xSemaphoreTake(ota_mutex, portMAX_DELAY);
        
        bool has_pending = ota_pending;
        char pending_url[256] = {0};
        char pending_job_id[37] = {0};
        char pending_checksum[65] = {0};
        
        if (has_pending) {
            strncpy(pending_url, pending_ota.url, sizeof(pending_url) - 1);
            strncpy(pending_job_id, pending_ota.job_id, sizeof(pending_job_id) - 1);
            strncpy(pending_checksum, pending_ota.checksum, sizeof(pending_checksum) - 1);
            ota_pending = false;
            memset(&pending_ota, 0, sizeof(pending_ota));
        }
        
        if (ota_mutex) xSemaphoreGive(ota_mutex);
        
        if (has_pending) {
            ESP_LOGI(TAG, "[✅ OTA] User confirmed — bắt đầu: %s (job_id: %.8s)", 
                     pending_url, pending_job_id);
            ota_start_with_job(pending_url, pending_job_id, pending_checksum);
        } else {
            ESP_LOGW(TAG, "[⚠️ OTA] No pending update to confirm");
        }
    }
}

// task kiểm tra kiểm tra hạn OTA
static void ota_expiration_task(void *pvParameter)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); 
        
        if (!ota_pending) continue;
        
        if (ota_mutex) xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(100));
        
        if (ota_pending && pending_ota.pending_time_ms > 0) {
            uint32_t current_time = esp_timer_get_time() / 1000;
            uint32_t elapsed = current_time - pending_ota.pending_time_ms;
            
            if (elapsed >= OTA_PENDING_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Optional OTA expired after %lu ms (job_id: %.8s)", 
                         elapsed, pending_ota.job_id);
                
                char notify_msg[200];
                snprintf(notify_msg, sizeof(notify_msg),
                    "{\"action\":\"ota_expired\",\"job_id\":\"%s\",\"timeout_ms\":%u}",
                    pending_ota.job_id, OTA_PENDING_TIMEOUT_MS);
                ota_publish_status(notify_msg);
                
                ota_pending = false;
                memset(&pending_ota, 0, sizeof(pending_ota));
            }
        }
        
        if (ota_mutex) xSemaphoreGive(ota_mutex);
    }
}

void ota_start_expiration_timer(void)
{
    xTaskCreate(&ota_expiration_task, "ota_expiration", 2048, NULL, 2, NULL);
    ESP_LOGI(TAG, "Expiration timer started (timeout: 24 hours)");
}

// ==================== PUBLIC FUNCTIONS ====================
void ota_start_with_job(const char *url, const char *job_id, const char *checksum)
{
    if (ota_in_progress) {
        ESP_LOGW(TAG, "Already in progress!");
        return;
    }
    
    if (!url || !validate_url(url)) {
        ESP_LOGE(TAG, "Invalid URL");
        return;
    }
    
    // Create task parameters
    ota_task_params_t *params = malloc(sizeof(ota_task_params_t));
    if (!params) {
        ESP_LOGE(TAG, "Failed to allocate params");
        return;
    }
    
    params->url = strdup(url);
    params->job_id = job_id ? strdup(job_id) : NULL;
    params->checksum = checksum ? strdup(checksum) : NULL;
    
    if (!params->url) {
        ESP_LOGE(TAG, "Failed to allocate URL");
        free(params);
        return;
    }
    
    ota_in_progress = true;
    ESP_LOGI(TAG, "Starting update: URL=%s, job_id=%s", url, job_id ? job_id : "N/A");
    
    // Tạo task OTA để thực hiện quá trình OTA trong nền, tránh block các tác vụ khác của hệ thống
    if (xTaskCreate(&ota_task, "ota_task", 8192, params, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task - Memory leak prevention");
        if (params->url) free(params->url);
        if (params->job_id) free(params->job_id);
        if (params->checksum) free(params->checksum);
        free(params);
        ota_in_progress = false;  
        return;
    }
}

void ota_start(const char *url)
{
    ota_start_with_job(url, NULL, NULL);
}

void ota_init(void)
{
    // Initialize mutex for OTA trigger handling
    if (ota_mutex == NULL) {
        ota_mutex = xSemaphoreCreateMutex();
        if (ota_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create OTA mutex");
        }
    }
    
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    ESP_LOGI(TAG, "Running partition: %s (0x%lx)", running->label, running->address);
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "FIRST BOOT AFTER OTA UPDATE");
            ESP_LOGW(TAG, "Firmware waiting for system stability validation...");
            ESP_LOGW(TAG, "Validation will occur when MQTT connects successfully"); 
        } else if (ota_state == ESP_OTA_IMG_VALID) {
            ESP_LOGI(TAG, "Firmware state: VALID (already validated)");
        } else {
            ESP_LOGI(TAG, "Firmware state: OTHER (%d)", ota_state);
        }
    } else {
        ESP_LOGW(TAG, "Failed to get running partition state");
    }
    
    // bắt đầu timer để kiểm tra hết hạn OTA tùy chọn
    ota_start_expiration_timer();
}
