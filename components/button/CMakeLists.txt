idf_component_register(SRCS "button.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_timer freertos relay wifi app_mqtt webserver pppos scheduler sht json modbus platform)