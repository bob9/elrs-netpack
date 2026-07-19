idf_component_register(
    SRCS "elrs-netpack.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_netif esp_event nvs_flash net_config espnow_server tcp_server rtc_sync test_server ota_server log_server tasks msp
)
