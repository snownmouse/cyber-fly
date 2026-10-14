idf_component_register(
    SRCS "main.c" "fly_app.c" "fly_game.c" "fly_view.c" "fly_screen_mirror.c"
          "fly_link.c" "fly_mic.c" "fly_wifi.c" "fly_imu.c"
    INCLUDE_DIRS "."
    REQUIRES mosaico_game_app mosaico_game_2d
             mosaico_raylib_fast mosaico_raylib_port raylib esp-gsp
             esp-mosaico-bsp esp_codec_dev esp_wifi esp_netif esp_timer
             esp_psram lwip)

# Task C: compile the board-side brain-downlink receiver (UDP task + core).
# The host simulator and the offline interop harness build the same file with
# this left at 0, so the transport half simply disappears there.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FLY_LINK_ESP_UDP=1)

set(canvas_png "${CMAKE_CURRENT_BINARY_DIR}/canvas_480.png")
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/make_canvas_png.py" "${canvas_png}"
    RESULT_VARIABLE canvas_result)
if(NOT canvas_result EQUAL 0)
    message(FATAL_ERROR "Could not generate Raylib Canvas placeholder")
endif()
set(CANVAS_IMAGE "${canvas_png}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/scene.json.in"
               "${CMAKE_CURRENT_BINARY_DIR}/scene.json" @ONLY)
gsp_add_bundle(${COMPONENT_LIB}
    SCENES "${CMAKE_CURRENT_BINARY_DIR}/scene.json"
    PIXEL_FORMAT rgb565)
