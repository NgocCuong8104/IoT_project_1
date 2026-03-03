idf_component_register(SRCS "web.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server relay wifi json app_mqtt button pppos scheduler sht modbus freertos esp_driver_gpio esp_timer platform)
