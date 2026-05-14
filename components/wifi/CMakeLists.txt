idf_component_register(SRCS "wifi.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif nvs_flash esp_event button relay app_mqtt pppos scheduler esp_driver_gpio esp_timer freertos json sht modbus platform wifi_provisioning led_status)
