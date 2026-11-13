// Renders every dash page to PPM images on a PC (no board needed):  firmware/host/build.sh
#include "lvgl.h"
#include "../main/dash_ui.h"
#include <cstdio>
#include <cstring>
#include <vector>
static const int W = 1024, H = 600;
static std::vector<uint32_t> fb(W * H);
static void flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  int w = lv_area_get_width(a);
  for (int y = a->y1; y <= a->y2; y++) std::memcpy(&fb[y * W + a->x1], px + (y - a->y1) * w * 4, w * 4);
  lv_display_flush_ready(d);
}
static void save(const char* path) {
  FILE* f = std::fopen(path, "wb"); std::fprintf(f, "P6 %d %d 255\n", W, H);
  for (uint32_t p : fb) { uint8_t rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)}; std::fwrite(rgb, 1, 3, f); }
  std::fclose(f);
}
int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : ".";
  lv_init();
  static std::vector<uint8_t> buf(W * H * 4);
  lv_display_t* d = lv_display_create(W, H);
  lv_display_set_buffers(d, buf.data(), nullptr, buf.size(), LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(d, flush);
  dash::Telemetry t;  // sample values, including some in the warning ranges
  t.rpm = 5650; t.kmh = 87; t.throttle = 78; t.load = 64; t.ect = 106; t.tft = 72; t.iat = 47;
  t.fuel = 14; t.volts = 14.2f; t.ltft1 = 7.0f; t.ltft2 = 11.7f; t.gear = 4; t.stopped = false;
  t.brakeGuess = false; t.decel = NAN; t.requesting = true; t.busActive = true; t.answersPerSec = 46;
  for (int night = 1; night >= 0; night--) {
    dash_ui_build(night, 80, DashUiHooks{});
    for (int pg = 0; pg < 5; pg++) {
      dash_ui_show_page(pg);
      if (pg == 0 && !night) { t.brakeGuess = true; t.decel = 7.4f; t.rpm = 1400; t.gear = 2; }
      dash_ui_update(t, "Bus OK at 500 kbit/s. Busiest IDs: 0x201 (100/s) 0x420 (50/s) 0x3B3 (10/s)");
      for (int i = 0; i < 20; i++) { lv_tick_inc(20); lv_timer_handler(); }
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(d);
      char path[256]; std::snprintf(path, sizeof path, "%s/%s-%d.ppm", out, night ? "night" : "day", pg);
      save(path);
    }
  }
  std::puts("preview: rendered 10 pages");
  return 0;
}
