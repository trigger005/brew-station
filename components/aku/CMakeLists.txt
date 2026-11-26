idf_component_register(
    SRCS "aku_frame.c" "aku_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_timer freertos
    PRIV_REQUIRES log
)
