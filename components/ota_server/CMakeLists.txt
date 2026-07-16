idf_component_register(
    SRCS "ota_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES app_update esp_app_format esp_partition
    EMBED_FILES "index.html")
