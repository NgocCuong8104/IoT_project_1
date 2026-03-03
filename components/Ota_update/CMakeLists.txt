idf_component_register(SRCS "ota.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client esp_https_ota esp_system nvs_flash app_update esp_timer json platform freertos mqtt mbedtls)