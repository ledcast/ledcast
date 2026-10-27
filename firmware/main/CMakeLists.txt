idf_component_register(
    SRCS "main.cpp" "power_cycle_screen.cpp"
    INCLUDE_DIRS "."
    REQUIRES deviceconfig linear display stream show netconfig webui health dmapool
)
