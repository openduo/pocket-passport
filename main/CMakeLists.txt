# Pocket v1 firmware. The upstream demo sources (demo_*.c, ui_pixel*.c,
# main.c) stay on disk as reference and for the repository's host tests; they
# are not linked into this application.
idf_component_register(
    SRCS "pocket_main.c"
         "pocket_app.c"
         "pocket_audio.c"
         "pocket_ble.c"
         "pocket_ui.c"
         "pocket_power.c"
         "pocket_stats.c"
         "pocket_model.c"
         "pocket_proto.c"
         "pocket_ring.c"
         "pocket_hist.c"
         "pocket_text.c"
    INCLUDE_DIRS "." "../assets/images/avatar"
    REQUIRES bsp bt esp_partition esp_pm esp_driver_usb_serial_jtag esp_timer esp_driver_i2s esp_app_format nvs_flash lvgl esp_lvgl_port
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pocket_cjk_20.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pocket_digits_44.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/images/avatar/pocket_avatars.c"
)
