idf_component_register(
    SRCS "test_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf msp esp_http_server
    PRIV_REQUIRES mbedtls rtc_sync
)
