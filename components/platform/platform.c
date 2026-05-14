#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"    
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "platform.h"
#include "ds3231.h"
#include"scheduler.h"

#define DS3231_SDA_PIN              21
#define DS3231_SCL_PIN              22
#define DS3231_I2C_FREQ             400000

#define MIN_VALID_TIME              1672531200UL
#define MAX_VALID_TIME              2147483647UL

#define NTP_MAX_BACKWARD_DRIFT      (24 * 3600)

#define NVS_NAMESPACE               "platform"
#define NVS_TIME_BACKUP_KEY         "last_time"
#define NVS_LAST_NTP_SYNC_KEY       "last_ntp_sync"
#define NVS_NTP_FAILS_KEY           "ntp_fails"
static const char *TAG = "PLATFORM";

static char deviceID[37] = {0};
static SemaphoreHandle_t rtc_mutex = NULL;

static platform_time_status_t time_status = PLATFORM_TIME_UNINITIALIZED;
static int rtc_ready = 0;
static time_t last_successful_ntp_sync = 0;
static uint32_t ntp_sync_fails = 0;
static time_t backup_interval_start = 0;
static const uint32_t BACKUP_INTERVAL = 3600;

static int is_time_valid(time_t timestamp) {
    return (timestamp > MIN_VALID_TIME) && (timestamp < MAX_VALID_TIME);
}

static void acquire_time_mutex(void) {
    if (rtc_mutex != NULL) {
        xSemaphoreTake(rtc_mutex, portMAX_DELAY);
    }
}

static void release_time_mutex(void) {
    if (rtc_mutex != NULL) {
        xSemaphoreGive(rtc_mutex);
    }
}

static time_t read_time_backup_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS open for read failed: %s", esp_err_to_name(ret));
        return 0;
    }

    int32_t backed_time = 0;
    ret = nvs_get_i32(handle, NVS_TIME_BACKUP_KEY, &backed_time);
    nvs_close(handle);

    if (ret == ESP_OK && is_time_valid(backed_time)) {
        return (time_t)backed_time;
    }

    return 0;
}

static esp_err_t write_time_backup_to_nvs(time_t timestamp) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_i32(handle, NVS_TIME_BACKUP_KEY, (int32_t)timestamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS set time failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    return ret;
}

static void update_nvs_tracking(time_t ntp_time, uint32_t fails) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return;

    nvs_set_i32(handle, NVS_LAST_NTP_SYNC_KEY, (int32_t)ntp_time);
    nvs_set_u32(handle, NVS_NTP_FAILS_KEY, fails);
    nvs_commit(handle);
    nvs_close(handle);
}

static void read_nvs_tracking(void) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) return;

    int32_t ntp_time = 0;
    nvs_get_i32(handle, NVS_LAST_NTP_SYNC_KEY, &ntp_time);
    last_successful_ntp_sync = (time_t)ntp_time;

    nvs_get_u32(handle, NVS_NTP_FAILS_KEY, &ntp_sync_fails);
    nvs_close(handle);
}

char* platform_get_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return (char *)desc->version;
}

static void device_generate_id(void)
{
    snprintf(deviceID, sizeof(deviceID), "1433bb17-38fd-4faf-ba7d-06d31b4193de");
}

char* platform_get_id(void)
{
    if(deviceID[0] == '\0')
    {
        device_generate_id();
    }
    return deviceID;
}

uint32_t platform_get_boot_time(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void platform_init_time_system(void) {
    if (rtc_mutex == NULL) {
        rtc_mutex = xSemaphoreCreateMutex();
    }

    esp_err_t ret = ds3231_init(DS3231_SDA_PIN, DS3231_SCL_PIN, DS3231_I2C_FREQ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ERROR: DS3231 init failed: %s", esp_err_to_name(ret));
        time_status = PLATFORM_TIME_COLD_BOOT_FAIL;
        goto cold_boot_fallback;
    }

    time_t ds3231_time = 0;
    ret = ds3231_read_time(&ds3231_time);

    if (ret != ESP_OK || !is_time_valid(ds3231_time)) {
        ESP_LOGW(TAG, "Warning: DS3231 read failed or invalid time");
        ds3231_deinit();
        time_status = PLATFORM_TIME_COLD_BOOT_FAIL;
        goto cold_boot_fallback;
    }

    {
        struct tm *timeinfo = localtime(&ds3231_time);
        ESP_LOGI(TAG, "OK: DS3231 time restored: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                 timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
        
        struct timeval tv = {
            .tv_sec = ds3231_time,
            .tv_usec = 0
        };
        settimeofday(&tv, NULL);
        write_time_backup_to_nvs(ds3231_time);
        
        ds3231_deinit();
        time_status = PLATFORM_TIME_COLD_BOOT_OK;
        backup_interval_start = ds3231_time;
        goto cold_boot_success;
    }

cold_boot_fallback:
    {
        time_t nvs_time = read_time_backup_from_nvs();
        if (nvs_time > 0) {
            struct tm *timeinfo = localtime(&nvs_time);
            ESP_LOGW(TAG, "Fallback: Using NVS backup: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                     timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
            
            struct timeval tv = {
                .tv_sec = nvs_time,
                .tv_usec = 0
            };
            settimeofday(&tv, NULL);
            time_status = PLATFORM_TIME_COLD_BOOT_FALLBACK;
            backup_interval_start = nvs_time;
        } else {
            ESP_LOGE(TAG, "ERROR: All time sources failed. Using epoch time");
            time_status = PLATFORM_TIME_COLD_BOOT_FAIL;
        }
    }

cold_boot_success:
    rtc_ready = 1;
    read_nvs_tracking();
}

void platform_sntp_sync_callback(struct timeval *tv) {
    if (!tv) {
        ESP_LOGE(TAG, "ERROR: SNTP callback - Invalid timeval pointer");
        ntp_sync_fails++;
        update_nvs_tracking(last_successful_ntp_sync, ntp_sync_fails);
        return;
    }

    time_t ntp_time = tv->tv_sec;
    if (!is_time_valid(ntp_time)) {
        ESP_LOGE(TAG, "ERROR: NTP time out of valid range. Rejecting.");
        ntp_sync_fails++;
        update_nvs_tracking(last_successful_ntp_sync, ntp_sync_fails);
        return;
    }

    time_t current_time = time(NULL);
    time_t time_diff = (ntp_time > current_time) ? 
                       (ntp_time - current_time) : 
                       (current_time - ntp_time);

    if (time_diff > NTP_MAX_BACKWARD_DRIFT) {
        ESP_LOGE(TAG, "ERROR: NTP time jump too large (%lld seconds). Rejecting.", time_diff);
        ntp_sync_fails++;
        update_nvs_tracking(last_successful_ntp_sync, ntp_sync_fails);
        return;
    }

    if (ntp_time < current_time) {
        ESP_LOGW(TAG, "Warning: NTP time went backward by %lld seconds.", time_diff);
    }

    // Try to calibrate DS3231, but don't fail system if RTC is unavailable
    esp_err_t ret = ds3231_init(DS3231_SDA_PIN, DS3231_SCL_PIN, DS3231_I2C_FREQ);
    if (ret == ESP_OK) {
        ret = ds3231_write_time(ntp_time);
        ds3231_deinit();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WARNING: DS3231 calibration failed: %s. System will use NVS backup on next restart.", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "OK: DS3231 calibrated with NTP time");
        }
    } else {
        ESP_LOGW(TAG, "WARNING: Cannot init DS3231 (missing/broken): %s. System will continue using network time.", esp_err_to_name(ret));
    }

    // Always update time status when NTP validates successfully (independent of RTC status)
    {
        acquire_time_mutex();
        last_successful_ntp_sync = ntp_time;
        ntp_sync_fails = 0;
        time_status = PLATFORM_TIME_NTP_SYNCED;
        backup_interval_start = ntp_time;

        release_time_mutex();
    }

    write_time_backup_to_nvs(ntp_time);
    update_nvs_tracking(ntp_time, ntp_sync_fails);

    ESP_LOGI(TAG, "OK: NTP time synchronized successfully");

    static bool is_bool_checked = false;
    if(!is_bool_checked) {
        scheduler_check_at_boot();
        is_bool_checked = true;
    }
}

int platform_is_rtc_ready(void) {
    acquire_time_mutex();
    int result = rtc_ready;
    release_time_mutex();
    return result;
}

platform_time_status_t platform_get_time_status(void) {
    acquire_time_mutex();
    platform_time_status_t status = time_status;
    release_time_mutex();
    return status;
}

int32_t platform_time_since_last_ntp_sync(void) {
    acquire_time_mutex();
    if (last_successful_ntp_sync == 0) {
        release_time_mutex();
        return -1;
    }
    
    time_t diff = time(NULL) - last_successful_ntp_sync;
    release_time_mutex();
    
    return (int32_t)diff;
}

int platform_is_time_valid(void) {
    time_t now = time(NULL);
    return is_time_valid(now);
}

uint32_t platform_get_ntp_sync_fails(void) {
    acquire_time_mutex();
    uint32_t fails = ntp_sync_fails;
    release_time_mutex();
    return fails;
}

esp_err_t platform_backup_time_to_nvs(void) {
    acquire_time_mutex();
    time_t now = time(NULL);
    if (backup_interval_start > 0 && (now - backup_interval_start) < BACKUP_INTERVAL) {
        release_time_mutex();
        return ESP_OK;
    }
    backup_interval_start = now;
    release_time_mutex();

    esp_err_t ret = write_time_backup_to_nvs(now);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Time backup to NVS successful");
    } else {
        ESP_LOGW(TAG, "Time backup to NVS failed: %s", esp_err_to_name(ret));
    }

    return ret;
}