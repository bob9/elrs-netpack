idf_component_register(
    SRCS "test_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_ringbuf msp
    PRIV_REQUIRES esp_http_server mbedtls
)
