add_executable(pio_ws2812)

target_include_directories(pio_ws2812 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../../pico_w/wifi/udp_beacon
        ${CMAKE_CURRENT_LIST_DIR}/../../pico_w/wifi
)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/generated)

# generate the header file into the source tree as it is included in the RP2040 datasheet
pico_generate_pio_header(pio_ws2812 ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/generated)

target_sources(pio_ws2812 PRIVATE ws2812.c)

target_link_libraries(pio_ws2812 PRIVATE pico_stdlib hardware_pio pico_cyw43_arch_lwip_threadsafe_background hardware_adc)
pico_add_extra_outputs(pio_ws2812)

# add url via pico_set_program_url
example_auto_set_url(pio_ws2812)

add_executable(pio_ws2812_parallel)

pico_enable_stdio_usb(pio_ws2812_parallel 1)
pico_enable_stdio_uart(pio_ws2812_parallel 0)

target_include_directories(pio_ws2812_parallel PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../../pico_w/wifi/udp_beacon
        ${CMAKE_CURRENT_LIST_DIR}/../../pico_w/wifi
)

pico_generate_pio_header(pio_ws2812_parallel ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/generated)

target_sources(pio_ws2812_parallel PRIVATE ws2812_parallel.c)

target_compile_definitions(pio_ws2812_parallel PRIVATE
        PIN_DBG1=3)

target_link_libraries(pio_ws2812_parallel PRIVATE pico_stdlib hardware_pio hardware_dma pico_cyw43_arch_lwip_threadsafe_background hardware_adc)
pico_add_extra_outputs(pio_ws2812_parallel)

# add url via pico_set_program_url
example_auto_set_url(pio_ws2812_parallel)

# Additionally generate python and hex pioasm outputs for inclusion in the RP2040 datasheet
add_custom_target(pio_ws2812_datasheet DEPENDS ${CMAKE_CURRENT_LIST_DIR}/generated/ws2812.py)
add_custom_command(OUTPUT ${CMAKE_CURRENT_LIST_DIR}/generated/ws2812.py
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio
        COMMAND pioasm -o python ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio ${CMAKE_CURRENT_LIST_DIR}/generated/ws2812.py
        VERBATIM)
add_dependencies(pio_ws2812 pio_ws2812_datasheet)