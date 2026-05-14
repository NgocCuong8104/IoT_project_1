idf_component_register(SRCS "pppos.c"
                    INCLUDE_DIRS "."
                    REQUIRES app_mqtt esp_modem esp_netif nvs_flash esp_event driver wifi freertos platform wifi led_status)
