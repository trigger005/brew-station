idf_component_register(
    SRCS "results.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_timer log freertos
)
