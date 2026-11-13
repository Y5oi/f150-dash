file(GLOB FONT_SRCS fonts/*.c)
idf_component_register(
    SRCS main.cpp dash_core.cpp dash_ui.cpp ${FONT_SRCS}
    INCLUDE_DIRS .
    REQUIRES esp32_p4_wifi6_touch_lcd_7b lvgl__lvgl esp_driver_twai esp_timer nvs_flash)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
