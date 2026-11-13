#include "dash_ui.h"
#include "lvgl.h"
#include <cstdio>
#include <cmath>

LV_FONT_DECLARE(font_speed)
LV_FONT_DECLARE(font_gear)
LV_FONT_DECLARE(font_value)
LV_FONT_DECLARE(font_label)
LV_FONT_DECLARE(font_small)

using dash::Limits;
using dash::Telemetry;

namespace {

struct Palette { uint32_t bg, panel, line, track, fg, dim, accent, needle, ok, warn, bad; };
const Palette kNight = {0x07080F, 0x0E1120, 0x1D2340, 0x1A1F38, 0xDFE4FF, 0x7A82A8, 0x38E1FF, 0xFF2E88, 0x3EF08A, 0xFFB020, 0xFF5C5C};
const Palette kDay   = {0xEEF1F6, 0xFFFFFF, 0xD5DBE6, 0xE3E8F0, 0x141A29, 0x5D6780, 0x0A74C9, 0xD6336C, 0x178A4C, 0xB86E00, 0xC92A2A};
Palette P = kNight;
inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// A round gauge: 270° arc, amber/red bands just outside it, big value, unit and name.
struct Gauge {
  lv_obj_t *card = nullptr, *arc = nullptr, *value = nullptr, *name = nullptr;
  Limits lim{}; int decimals = 0; float Telemetry::*field = nullptr;
};
enum { G_RPM, G_ECT, G_TFT, G_IAT, G_LOAD, G_THR, G_VOLT, G_FUEL, G_LT1, G_LT2, G_COUNT };
Gauge g[G_COUNT];

lv_obj_t *tv, *pageName, *status, *dots[8], *speedLbl, *brakeLbl, *decelLbl, *gearBox, *gearLbl, *gearNote, *busLbl, *modeBtnLbl;
int pageCount = 0;
DashUiHooks H;
bool gRequesting = false;

const char* kPageNames[] = {"Drive", "Temperatures", "Engine", "Fuel", "Settings"};

lv_obj_t* card(lv_obj_t* parent) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_grow(c, 1);
  lv_obj_set_height(c, lv_pct(100));
  lv_obj_set_style_bg_color(c, C(P.panel), 0);
  lv_obj_set_style_border_color(c, C(P.line), 0);
  lv_obj_set_style_border_width(c, 2, 0);
  lv_obj_set_style_radius(c, 18, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(c, 6, 0);
  return c;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* f, uint32_t color, const char* txt) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, C(color), 0);
  lv_label_set_text(l, txt);
  return l;
}

lv_obj_t* page(int i) {
  lv_obj_t* p = lv_tileview_add_tile(tv, i, 0, LV_DIR_HOR);
  lv_obj_set_style_pad_all(p, 8, 0);
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(p, 12, 0);
  pageCount = i + 1;
  return p;
}

// Arc angle (0..270 from the rotation start) for value v on a gauge.
int ang(float v, const Limits& l) {
  float f = (v - l.min) / (l.max - l.min);
  return int(270 * (f < 0 ? 0 : f > 1 ? 1 : f));
}

lv_obj_t* band(lv_obj_t* parent, int size, int a0, int a1, uint32_t color) {
  if (a1 <= a0) return nullptr;
  lv_obj_t* z = lv_arc_create(parent);
  lv_obj_set_size(z, size, size);
  lv_obj_center(z);
  lv_arc_set_rotation(z, 135);
  lv_arc_set_bg_angles(z, a0, a1);
  lv_obj_remove_style(z, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(z, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(z, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(z, C(color), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(z, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(z, false, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(z, LV_OPA_TRANSP, LV_PART_INDICATOR);
  return z;
}

void gauge(int id, lv_obj_t* pageObj, const char* name, const char* unit, float Telemetry::*field, Limits lim, int decimals = 0) {
  Gauge& G = g[id];
  G.lim = lim; G.decimals = decimals; G.field = field;
  G.card = card(pageObj);
  lv_obj_t* box = lv_obj_create(G.card);  // square area holding the arcs and the number
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 300, 300);
  const int s = 260;
  // Bands sit just outside the main arc so the limits show before the value gets there.
  if (lim.symmetric) {  // fuel trims: zero in the middle, bands on both ends
    band(box, s + 34, ang(-lim.bad, lim), ang(-lim.warn, lim), P.warn);
    band(box, s + 34, 0, ang(-lim.bad, lim), P.bad);
    band(box, s + 34, ang(lim.warn, lim), ang(lim.bad, lim), P.warn);
    band(box, s + 34, ang(lim.bad, lim), 270, P.bad);
  } else if (lim.bad < lim.warn) {
    band(box, s + 34, 0, ang(lim.bad, lim), P.bad);
    band(box, s + 34, ang(lim.bad, lim), ang(lim.warn, lim), P.warn);
  } else if (std::isfinite(lim.warn)) {
    band(box, s + 34, ang(lim.warn, lim), ang(lim.bad, lim), P.warn);
    band(box, s + 34, ang(lim.bad, lim), 270, P.bad);
  }
  G.arc = lv_arc_create(box);
  lv_obj_set_size(G.arc, s, s);
  lv_obj_center(G.arc);
  lv_arc_set_rotation(G.arc, 135);
  lv_arc_set_bg_angles(G.arc, 0, 270);
  lv_arc_set_range(G.arc, 0, 1000);
  lv_arc_set_value(G.arc, 0);
  if (lim.symmetric) { lv_arc_set_mode(G.arc, LV_ARC_MODE_SYMMETRICAL); lv_arc_set_value(G.arc, 500); }
  lv_obj_remove_style(G.arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(G.arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(G.arc, 24, LV_PART_MAIN);
  lv_obj_set_style_arc_color(G.arc, C(P.track), LV_PART_MAIN);
  lv_obj_set_style_arc_width(G.arc, 24, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(G.arc, C(P.accent), LV_PART_INDICATOR);
  G.value = label(box, &font_value, P.fg, "-");
  lv_obj_align(G.value, LV_ALIGN_CENTER, 0, -6);
  lv_obj_t* u = label(box, &font_small, P.dim, unit);
  lv_obj_align(u, LV_ALIGN_CENTER, 0, 48);
  G.name = label(G.card, &font_label, P.dim, name);
}

void setGauge(Gauge& G, float v) {
  if (!G.arc) return;
  if (std::isnan(v)) { lv_label_set_text(G.value, "-"); lv_arc_set_value(G.arc, G.lim.symmetric ? 500 : 0); return; }
  float f = (v - G.lim.min) / (G.lim.max - G.lim.min);
  lv_arc_set_value(G.arc, int(1000 * (f < 0 ? 0 : f > 1 ? 1 : f)));
  char buf[16];
  if (G.decimals) std::snprintf(buf, sizeof buf, G.lim.symmetric ? "%+.*f" : "%.*f", G.decimals, v);
  else std::snprintf(buf, sizeof buf, G.lim.symmetric ? "%+d" : "%d", int(std::lround(v)));
  lv_label_set_text(G.value, buf);
  int lv = dash::level(v, G.lim);
  uint32_t col = lv == 2 ? P.bad : lv == 1 ? P.warn : P.accent;
  lv_obj_set_style_arc_color(G.arc, C(col), LV_PART_INDICATOR);
  lv_obj_set_style_text_color(G.value, C(lv ? col : P.fg), 0);
  lv_obj_set_style_border_color(G.card, C(lv == 2 ? P.bad : P.line), 0);
}

void onPage(lv_event_t*) {
  lv_obj_t* act = lv_tileview_get_tile_active(tv);
  int idx = lv_obj_get_index(act);
  lv_label_set_text(pageName, kPageNames[idx]);
  for (int i = 0; i < pageCount; i++) {
    lv_obj_set_width(dots[i], i == idx ? 28 : 10);
    lv_obj_set_style_bg_color(dots[i], C(i == idx ? P.accent : P.line), 0);
  }
}

void onTheme(lv_event_t* e) {
  bool night = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  if (H.setNight) H.setNight(night);
}
void onBright(lv_event_t* e) {
  if (H.setBrightness) H.setBrightness(lv_slider_get_value((lv_obj_t*)lv_event_get_target(e)));
}
void onMode(lv_event_t*) {
  gRequesting = !gRequesting;
  if (H.setRequesting) H.setRequesting(gRequesting);
}

lv_obj_t* settingRow(lv_obj_t* parent, const char* text) {
  lv_obj_t* r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  label(r, &font_label, P.fg, text);
  return r;
}

lv_obj_t* button(lv_obj_t* parent, const char* text, lv_obj_t** lblOut) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_style_bg_color(b, C(P.accent), 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_pad_hor(b, 18, 0);
  lv_obj_set_style_pad_ver(b, 12, 0);
  lv_obj_t* l = label(b, &font_label, P.panel, text);
  if (lblOut) *lblOut = l;
  return b;
}

}  // namespace

void dash_ui_build(bool night, int brightness, const DashUiHooks& hooks) {
  H = hooks;
  P = night ? kNight : kDay;
  for (auto& x : g) x = Gauge{};
  lv_obj_t* scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, C(P.bg), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);

  // Top bar
  lv_obj_t* bar = lv_obj_create(scr);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, lv_pct(100), 48);
  lv_obj_set_style_pad_hor(bar, 18, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, 16, 0);
  label(bar, &font_label, P.fg, "F-150 5.0");
  pageName = label(bar, &font_label, P.dim, "Drive");
  status = label(bar, &font_small, P.dim, "Listening");
  lv_obj_set_flex_grow(status, 1);
  lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);

  // Swipe pages
  tv = lv_tileview_create(scr);
  lv_obj_set_size(tv, lv_pct(100), lv_display_get_vertical_resolution(nullptr) - 48 - 26);
  lv_obj_set_y(tv, 48);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(tv, onPage, LV_EVENT_VALUE_CHANGED, nullptr);

  // 1 Drive: RPM | speed | gear
  lv_obj_t* p = page(0);
  gauge(G_RPM, p, "RPM", "rpm", &Telemetry::rpm, {0, 7000, 4500, 5500});
  lv_obj_t* sc = card(p);
  lv_obj_set_flex_grow(sc, 12); lv_obj_set_flex_grow(g[G_RPM].card, 10); // speed gets the most room for 3 digits
  speedLbl = label(sc, &font_speed, P.fg, "0");
  label(sc, &font_label, P.dim, "KM/H");
  brakeLbl = label(sc, &font_label, P.dim, "BRAKE");
  lv_obj_set_style_border_width(brakeLbl, 3, 0);
  lv_obj_set_style_border_color(brakeLbl, C(P.line), 0);
  lv_obj_set_style_radius(brakeLbl, 10, 0);
  lv_obj_set_style_pad_hor(brakeLbl, 22, 0);
  lv_obj_set_style_pad_ver(brakeLbl, 6, 0);
  decelLbl = label(sc, &font_small, P.dim, "-");
  lv_obj_t* gc = card(p);
  lv_obj_set_flex_grow(gc, 8);
  label(gc, &font_label, P.dim, "GEAR");
  gearBox = lv_obj_create(gc);
  lv_obj_remove_flag(gearBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(gearBox, 190, 230);
  lv_obj_set_style_bg_opa(gearBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(gearBox, 6, 0);
  lv_obj_set_style_border_color(gearBox, C(P.accent), 0);
  lv_obj_set_style_radius(gearBox, 22, 0);
  gearLbl = label(gearBox, &font_gear, P.accent, "1");
  lv_obj_center(gearLbl);
  gearNote = label(gc, &font_small, P.dim, "Stopped");

  // 2 Temperatures, 3 Engine, 4 Fuel
  p = page(1);
  gauge(G_ECT, p, "COOLANT", "°C", &Telemetry::ect, {40, 120, 104, 110});
  gauge(G_TFT, p, "TRANS TEMP", "°C", &Telemetry::tft, {20, 130, 100, 115});
  gauge(G_IAT, p, "INTAKE AIR", "°C", &Telemetry::iat, {0, 80, 55, 65});
  p = page(2);
  gauge(G_LOAD, p, "LOAD", "%", &Telemetry::load, {0, 100, 85, 95});
  gauge(G_THR, p, "THROTTLE", "%", &Telemetry::throttle, {0, 100, 70, 90});
  gauge(G_VOLT, p, "BATTERY", "volts", &Telemetry::volts, {10, 16, 14.9f, 15.3f}, 1);
  p = page(3);
  gauge(G_FUEL, p, "FUEL", "%", &Telemetry::fuel, {0, 100, 20, 10});
  gauge(G_LT1, p, "FUEL TRIM B1", "% long term", &Telemetry::ltft1, {-25, 25, 10, 20, true});
  gauge(G_LT2, p, "FUEL TRIM B2", "% long term", &Telemetry::ltft2, {-25, 25, 10, 20, true});

  // 5 Settings
  p = page(4);
  lv_obj_t* s1 = card(p);
  lv_obj_set_flex_align(s1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  label(s1, &font_label, P.dim, "DISPLAY");
  lv_obj_t* r = settingRow(s1, "Night mode");
  lv_obj_t* sw = lv_switch_create(r);
  if (night) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, C(P.accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, onTheme, LV_EVENT_VALUE_CHANGED, nullptr);
  label(s1, &font_label, P.fg, "Brightness");
  lv_obj_t* sl = lv_slider_create(s1);
  lv_obj_set_width(sl, lv_pct(95));
  lv_slider_set_range(sl, 10, 100);
  lv_slider_set_value(sl, brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, C(P.accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, C(P.accent), LV_PART_KNOB);
  lv_obj_add_event_cb(sl, onBright, LV_EVENT_RELEASED, nullptr);

  lv_obj_t* s2 = card(p);
  lv_obj_set_flex_align(s2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  label(s2, &font_label, P.dim, "TRUCK CONNECTION");
  lv_obj_t* note = label(s2, &font_small, P.fg,
      "Listen only: the dash just listens to the truck and sends nothing. "
      "Use it first to check the wiring. Then switch on Read data so the "
      "dash asks the engine computer for values. It only ever reads.");
  lv_obj_set_width(note, lv_pct(100));
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_t* mb = button(s2, gRequesting ? "Stop reading (listen only)" : "Read data from truck", &modeBtnLbl);
  lv_obj_add_event_cb(mb, onMode, LV_EVENT_CLICKED, nullptr);
  busLbl = label(s2, &font_small, P.dim, "No bus traffic yet");
  lv_obj_set_width(busLbl, lv_pct(100));
  lv_label_set_long_mode(busLbl, LV_LABEL_LONG_WRAP);

  // Page dots
  lv_obj_t* dotRow = lv_obj_create(scr);
  lv_obj_remove_style_all(dotRow);
  lv_obj_set_size(dotRow, lv_pct(100), 26);
  lv_obj_align(dotRow, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(dotRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dotRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dotRow, 10, 0);
  for (int i = 0; i < pageCount; i++) {
    dots[i] = lv_obj_create(dotRow);
    lv_obj_remove_style_all(dots[i]);
    lv_obj_set_size(dots[i], i ? 10 : 28, 10);
    lv_obj_set_style_radius(dots[i], 5, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dots[i], C(i ? P.line : P.accent), 0);
  }
}

void dash_ui_show_page(int i) {
  lv_tileview_set_tile_by_index(tv, i, 0, LV_ANIM_OFF);
  onPage(nullptr);
}

void dash_ui_update(const Telemetry& t, const char* busSummary) {
  for (auto& G : g) if (G.field) setGauge(G, t.*(G.field));

  char buf[48];
  std::snprintf(buf, sizeof buf, "%d", std::isnan(t.kmh) ? 0 : int(std::lround(t.kmh)));
  lv_label_set_text(speedLbl, buf);
  bool brake = t.brakeGuess;
  lv_obj_set_style_border_color(brakeLbl, C(brake ? P.bad : P.line), 0);
  lv_obj_set_style_text_color(brakeLbl, C(brake ? P.bad : P.dim), 0);
  if (t.decel > 0.5f) std::snprintf(buf, sizeof buf, "Slowing %.1f km/h/s · %.2f g", t.decel, t.decel / 35.3f);
  else std::snprintf(buf, sizeof buf, "-");
  lv_label_set_text(decelLbl, buf);

  std::snprintf(buf, sizeof buf, "%d", t.gear);
  lv_label_set_text(gearLbl, buf);
  lv_obj_set_style_opa(gearBox, t.stopped ? LV_OPA_50 : LV_OPA_COVER, 0);
  lv_label_set_text(gearNote, t.stopped ? "Stopped" : "Estimated");

  gRequesting = t.requesting;
  if (modeBtnLbl) lv_label_set_text(modeBtnLbl, t.requesting ? "Stop reading (listen only)" : "Read data from truck");
  if (!t.busActive) std::snprintf(buf, sizeof buf, "No truck data: check wiring / ignition");
  else if (t.requesting) std::snprintf(buf, sizeof buf, "Live · %u answers/s", unsigned(t.answersPerSec));
  else std::snprintf(buf, sizeof buf, "Listening · %u frames/s", unsigned(t.framesPerSec));
  lv_label_set_text(status, buf);
  if (busSummary) lv_label_set_text(busLbl, busSummary);
}
