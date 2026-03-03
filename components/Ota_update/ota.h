#ifndef OTA_H
#define OTA_H

#include "esp_err.h"

/**
 * @brief 
 * Gọi hàm này trong app_main() sau khi init NVS
 */
void ota_init(void);

/**
 * @brief 
 * @param url 
 */
void ota_start(const char *url);

/**
 * @brief Kiểm tra MQTT có đang kết nối không
 * @return true nếu connected
 */
bool ota_is_mqtt_connected(void);

/**
 * @brief Set MQTT client handle cho OTA progress reporting
 * @param client MQTT client handle
 */
void ota_set_mqtt_client(void *client);

#endif // OTA_H
