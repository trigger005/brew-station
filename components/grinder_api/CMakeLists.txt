idf_component_register(
    SRCS "grinder_api.c"
    INCLUDE_DIRS "include"
    REQUIRES machine esp_http_server freertos
    PRIV_REQUIRES esp_timer log lwip
)
