idf_component_register(SRCS "continuous_read_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer
                    PRIV_REQUIRES esp_driver_ledc esp_adc)
