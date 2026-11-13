// LVGL screens for the 7" dash: swipe pages with three big readings each, plus settings.
#pragma once
#include "dash_core.h"

struct DashUiHooks {
  void (*setRequesting)(bool on) = nullptr;  // switch listen-only <-> asking the PCM for data
  void (*setBrightness)(int pct) = nullptr;
  void (*setNight)(bool night) = nullptr;    // remembered by the caller, then the UI is rebuilt
};

// Build (or rebuild after a theme change) the whole UI on the active screen.
void dash_ui_build(bool night, int brightness, const DashUiHooks& hooks);
// Jump to a page (0 = Drive). Used to restore the last page after a theme change.
void dash_ui_show_page(int i);
// Push the latest values; call from the LVGL task (inside the display lock) ~10x per second.
void dash_ui_update(const dash::Telemetry& t, const char* busSummary);
