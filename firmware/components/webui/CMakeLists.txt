set(WEB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../web")
set(WEB_PAGE "${WEB_DIR}/dist/index.html.gz")

idf_component_register(
    SRCS "webui_server.cpp"
         "console_auth.cpp"
         "webui_json.cpp"
         "webui_http.cpp"
         "api_status.cpp"
         "api_config.cpp"
         "api_network.cpp"
         "api_test.cpp"
         "api_show.cpp"
         "api_ota.cpp"
         "ota_screen.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES deviceconfig display stream linear show netconfig health
                  esp_http_server app_update esp_timer json mbedtls
    EMBED_FILES "${WEB_PAGE}"
)

# Страница собирается сборкой, а не руками: иначе правка в web/src молча не
# попадает в прошивку.
idf_build_get_property(python PYTHON)
if(NOT python)
    message(FATAL_ERROR "Не найден python для сборки веб-интерфейса: запустите сборку из окружения ESP-IDF (. \$IDF_PATH/export.sh)")
endif()

file(GLOB_RECURSE WEB_SOURCES CONFIGURE_DEPENDS "${WEB_DIR}/src/*")
add_custom_command(
    OUTPUT "${WEB_PAGE}"
    COMMAND "${python}" "${WEB_DIR}/build.py" --quiet
    DEPENDS "${WEB_DIR}/build.py"
            "${WEB_DIR}/index.html"
            "${WEB_DIR}/package.json"
            "${WEB_DIR}/vite.config.ts"
            ${WEB_SOURCES}
    COMMENT "Сборка веб-интерфейса LEDCAST"
    VERBATIM
)

add_custom_target(webui_page DEPENDS "${WEB_PAGE}")
add_dependencies(${COMPONENT_LIB} webui_page)
