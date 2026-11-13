/* LVGL config for the PC preview only (the board uses the BSP's Kconfig settings). */
#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 32
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_ARC 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TILEVIEW 1
#define LV_USE_FLEX 1
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#endif
