idf_component_register(
    SRCS "stream_node.cpp"
         "stream_packet.cpp"
         "stream_codec.cpp"
         "stream_frame.cpp"
         "stream_paint.cpp"
         "stream_clock.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES deviceconfig
    PRIV_REQUIRES linear display framestats lwip esp_timer
)
