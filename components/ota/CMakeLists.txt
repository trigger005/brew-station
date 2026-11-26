idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES app_update bootloader_support esp_app_format esp_timer log freertos grinder_api machine
)
