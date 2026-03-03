idf_component_register(SRCS "scheduler.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_timer freertos wifi app_mqtt button webserver pppos relay json sht modbus platform)