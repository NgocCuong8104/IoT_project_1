#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"    
#include "platform.h"

static char deviceID[37] = {0}; 

char* platform_get_version(void)
    {
    const esp_app_desc_t *desc = esp_app_get_description();
    return (char *)desc->version;
}

static void device_generate_id(void)
{
    // uint8_t mac[6];
    // esp_read_mac(mac, ESP_MAC_WIFI_STA); 
    
    // snprintf(deviceID, sizeof(deviceID), "esp32_%02X%02X%02X", mac[3], mac[4], mac[5]);
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