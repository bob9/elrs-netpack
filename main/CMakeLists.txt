idf_component_register(
    SRCS "elrs-netpack.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_netif esp_event nvs_flash net_config espnow_server tcp_server rtc_sync test_server tasks msp
)
