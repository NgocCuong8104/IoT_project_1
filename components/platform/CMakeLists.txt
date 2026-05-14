idf_component_register(SRCS "platform.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer app_update esp_system nvs_flash ds3231 scheduler)