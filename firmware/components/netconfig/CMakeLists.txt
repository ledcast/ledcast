idf_component_register(
    SRCS "netconfig.cpp"
    INCLUDE_DIRS "include"
    REQUIRES deviceconfig health esp_wifi esp_netif esp_event esp_timer nvs_flash lwip
)
