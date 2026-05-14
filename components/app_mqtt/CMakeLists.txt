idf_component_register(SRCS "mqtt.c"
                    INCLUDE_DIRS "."
                    REQUIRES mqtt wifi relay button pppos scheduler sht json esp_driver_gpio esp_timer freertos modbus platform Ota_update input)
