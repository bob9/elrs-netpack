idf_component_register(
    SRCS "net_config.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif
    PRIV_REQUIRES console nvs_flash
)
