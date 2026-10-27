idf_component_register(
    SRCS "src/display.cpp"
         "src/display_rebuild.cpp"
         "src/hub75_backend_matrixpanel.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES deviceconfig
    # esp_lcd и driver библиотека HUB75 требует у сборки по имени и падает с
    # FATAL_ERROR, если их нет среди собираемых компонентов.
    PRIV_REQUIRES hub75-panel colour dmapool esp_lcd driver esp_hw_support esp_driver_pcnt esp_timer framestats
)
