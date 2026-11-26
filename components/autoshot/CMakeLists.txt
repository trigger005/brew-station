idf_component_register(
    SRCS "autoshot.c"
    INCLUDE_DIRS "include"
    REQUIRES shot esp_common
    PRIV_REQUIRES aku web grinder_api machine heap log freertos nvs_flash
)
