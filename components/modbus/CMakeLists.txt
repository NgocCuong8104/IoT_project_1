idf_component_register(SRCS "modbusSRC.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_timer freertos wifi app_mqtt button webserver pppos scheduler relay sht json platform)