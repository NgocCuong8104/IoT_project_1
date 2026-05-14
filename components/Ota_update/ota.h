#ifndef OTA_H
#define OTA_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"

// OTA TRIGGER STRUCTURES
typedef struct {
    char url[256];
    char version[32];
    char checksum[65];
    char job_id[37];
    bool is_mandatory;
    uint32_t pending_time_ms; 
} ota_trigger_t;

#define OTA_PENDING_TIMEOUT_MS (24 * 60 * 60 * 1000)  

// OTA TASK PARAMETERS
typedef struct {
    char *url;
    char *job_id;      
    char *checksum;    
} ota_task_params_t;

// PUBLIC FUNCTIONS 
void ota_init(void);
void ota_mark_valid(void);
void ota_send_boot_confirmation(void);
void ota_start(const char *url);
void ota_start_with_job(const char *url, const char *job_id, const char *checksum);
void handle_ota_trigger(const cJSON *root);
void ota_start_expiration_timer(void);
bool ota_is_mqtt_connected(void);
void ota_set_mqtt_client(void *client);

#endif
