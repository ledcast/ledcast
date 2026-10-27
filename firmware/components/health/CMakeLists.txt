idf_component_register(
    SRCS "health.cpp" "panic_record.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio app_update nvs_flash esp_timer esp_app_format
    PRIV_REQUIRES netconfig esp_system xtensa
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
