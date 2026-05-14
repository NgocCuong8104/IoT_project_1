idf_component_register(SRCS "input.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver freertos
                       PRIV_REQUIRES esp_timer)