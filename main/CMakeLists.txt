idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES aku board net web grinder_api machine ota autoshot nvs_flash esp_event log freertos
)
