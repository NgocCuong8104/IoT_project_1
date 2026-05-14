idf_component_register(SRCS "sht35.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp-modbus driver modbus freertos esp_timer esp_driver_gpio wifi app_mqtt button pppos scheduler relay json platform) 