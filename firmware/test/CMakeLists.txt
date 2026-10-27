# Host tests: platform-independent firmware logic built for the development
# machine. ESP-IDF headers are replaced by the minimal shims in host/.
cmake_minimum_required(VERSION 3.16)
project(ledcast_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Werror)
endif()

set(FIRMWARE ${CMAKE_CURRENT_LIST_DIR}/..)
set(COMPONENTS ${FIRMWARE}/components)
set(HUB75 ${COMPONENTS}/hub75-panel/src)

add_library(ledcast_model STATIC
  ${COMPONENTS}/deviceconfig/config_model.cpp
  ${COMPONENTS}/deviceconfig/config_snapshot.cpp
  ${COMPONENTS}/deviceconfig/config_flush_queue.cpp
  ${COMPONENTS}/stream/stream_packet.cpp
  ${COMPONENTS}/stream/stream_codec.cpp
  ${COMPONENTS}/stream/stream_frame.cpp
  ${COMPONENTS}/stream/stream_clock.cpp
  ${COMPONENTS}/display/src/display_rebuild.cpp
  ${COMPONENTS}/colour/colour.cpp
)
target_include_directories(ledcast_model PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/host
  ${COMPONENTS}/deviceconfig/include
  ${COMPONENTS}/deviceconfig
  ${COMPONENTS}/stream/include
  ${COMPONENTS}/framestats/include
  ${COMPONENTS}/display/include
  ${COMPONENTS}/colour/include
)

enable_testing()

function(host_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES;LIBS" ${ARGN})
  add_executable(test_${name} test_${name}.cpp ${ARG_SOURCES})
  target_include_directories(test_${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${ARG_INCLUDES})
  target_link_libraries(test_${name} PRIVATE ${ARG_LIBS})
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

foreach(suite line_config config_domains config_snapshot config_flush stream colour
              display_rebuild framestats dma_memory)
  host_test(${suite} LIBS ledcast_model)
endforeach()

host_test(linear SOURCES ${COMPONENTS}/linear/linear.cpp
          INCLUDES ${COMPONENTS}/linear/include LIBS ledcast_model)
host_test(dma_blocks SOURCES ${COMPONENTS}/dmapool/dmapool.cpp
          INCLUDES ${CMAKE_CURRENT_LIST_DIR}/host ${COMPONENTS}/dmapool/include
                   ${COMPONENTS}/deviceconfig/include)

foreach(suite flip_wait flip_ownership bcm_timing)
  host_test(${suite} INCLUDES ${HUB75} ${HUB75}/platforms/esp32s3)
endforeach()

host_test(show_timing INCLUDES ${COMPONENTS}/show/include)
host_test(show_font INCLUDES ${COMPONENTS}/show/include)
host_test(show_blend INCLUDES ${COMPONENTS}/show/include LIBS ledcast_model)
host_test(frame_pacer INCLUDES ${COMPONENTS}/display/include)
host_test(captive_dns)
host_test(setup_policy)
host_test(power_cycles)
host_test(netconfig_retry INCLUDES ${COMPONENTS}/netconfig)
host_test(socket_address)
host_test(webui_admission INCLUDES ${COMPONENTS}/webui)
host_test(console_auth SOURCES ${COMPONENTS}/webui/console_auth.cpp
          INCLUDES ${COMPONENTS}/webui)
