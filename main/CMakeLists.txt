idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES wifi webserver relay button app_mqtt pppos scheduler esp_driver_gpio esp_timer freertos json sht modbus platform Ota_update 
                    PRIV_REQUIRES app_update)
    