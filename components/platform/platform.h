#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

typedef enum {
    PLATFORM_TIME_UNINITIALIZED = 0,
    PLATFORM_TIME_COLD_BOOT_OK = 1,
    PLATFORM_TIME_COLD_BOOT_FALLBACK = 2,
    PLATFORM_TIME_COLD_BOOT_FAIL = 3,
    PLATFORM_TIME_NTP_SYNCED = 4,
    PLATFORM_TIME_NTP_FAILED = 5,
    PLATFORM_TIME_RUNNING = 6
} platform_time_status_t;

char* platform_get_version(void);
char* platform_get_id(void);
uint32_t platform_get_boot_time(void);

void platform_init_time_system(void);
void platform_sntp_sync_callback(struct timeval *tv);
int platform_is_rtc_ready(void);

platform_time_status_t platform_get_time_status(void);
int32_t platform_time_since_last_ntp_sync(void);
int platform_is_time_valid(void);
uint32_t platform_get_ntp_sync_fails(void);
esp_err_t platform_backup_time_to_nvs(void);

#endif