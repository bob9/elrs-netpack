idf_component_register(
    SRCS "rtc_sync.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf msp
    PRIV_REQUIRES console nvs_flash esp_netif lwip
)
