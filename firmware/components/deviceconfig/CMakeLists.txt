idf_component_register(
    SRCS "config_model.cpp" "config_json.cpp" "config_store.cpp" "config_flush_queue.cpp" "config_snapshot.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash json esp_timer
)
