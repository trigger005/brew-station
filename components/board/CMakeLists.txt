# One board file per board; the LOLIN S3 Mini is the prototype.
idf_component_register(
    SRCS "board_lolin_s3_mini.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES esp_timer log
)
