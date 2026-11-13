// PC unit tests for the portable dash logic:  ./firmware/test/run.sh
#include "../main/dash_core.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace dash;

static bool near(float a, float b, float e = 0.01f) { return std::fabs(a - b) < e; }

int main() {
  // Request building
  uint8_t req[8]; const uint8_t pids[] = {0x0C, 0x0D, 0x11};
  buildMode01(pids, 3, req);
  assert(req[0] == 4 && req[1] == 0x01 && req[2] == 0x0C && req[4] == 0x11 && req[5] == 0);
  buildMode22(DID_TFT, req);
  assert(req[0] == 3 && req[1] == 0x22 && req[2] == 0x1E && req[3] == 0x1C);

  // Multi-PID reply split over ISO-TP: 41 0C 1A F8 0D 32 11 40 = 8 bytes -> first frame + one consecutive frame
  IsoTpRx rx; Telemetry t;
  const uint8_t ff[8] = {0x10, 0x08, 0x41, 0x0C, 0x1A, 0xF8, 0x0D, 0x32};
  const uint8_t cf[8] = {0x21, 0x11, 0x40, 0, 0, 0, 0, 0};
  assert(rx.feed(ff, 8) == IsoTpRx::NEED_FLOW_CONTROL);
  assert(rx.feed(cf, 8) == IsoTpRx::COMPLETE && rx.size() == 8);
  assert(decodeMode01(rx.payload(), rx.size(), t) == 3);
  assert(near(t.rpm, 1726) && near(t.kmh, 50) && near(t.throttle, 25.1f, 0.1f));

  // Single frame, slow set
  const uint8_t sf[8] = {0x06, 0x41, 0x05, 0x7D, 0x2F, 0x80, 0x00, 0x00};  // ECT 85 C, fuel 50 %
  assert(rx.feed(sf, 8) == IsoTpRx::COMPLETE);
  decodeMode01(rx.payload(), rx.size(), t);
  assert(near(t.ect, 85) && near(t.fuel, 50.2f, 0.1f));
  const uint8_t sf2[8] = {0x04, 0x41, 0x07, 0x8F, 0x00, 0, 0, 0};          // LTFT1 +11.7 %
  rx.feed(sf2, 8); decodeMode01(rx.payload(), rx.size(), t);
  assert(near(t.ltft1, 11.72f, 0.05f));

  // Out-of-order consecutive frame is rejected
  IsoTpRx rx2; rx2.feed(ff, 8);
  const uint8_t bad[8] = {0x22, 0, 0, 0, 0, 0, 0, 0};
  assert(rx2.feed(bad, 8) == IsoTpRx::ERROR);

  // Mode 22 trans temp: 0x02D0 / 16 = 45 C; absurd value ignored
  const uint8_t m22[] = {0x62, 0x1E, 0x1C, 0x02, 0xD0};
  assert(decodeMode22(m22, 5, t) && near(t.tft, 45));
  const uint8_t m22bad[] = {0x62, 0x1E, 0x1C, 0x7F, 0xFF};
  assert(!decodeMode22(m22bad, 5, t) && near(t.tft, 45));

  // Gears: 3rd at 60 km/h ~ 60*25.82*1.521/0.95 rpm
  assert(nearestGear(60 * 25.82f * 1.521f / 0.95f, 60) == 3);
  assert(nearestGear(100 * 25.82f * 0.691f / 0.95f, 100) == 6);
  GearEstimator ge; bool st;
  float rpm3 = 60 * 25.82f * 1.521f / 0.95f;
  assert(ge.update(rpm3, 60, st) == 1 && !st);  // needs 3 agreeing samples before switching
  ge.update(rpm3, 60, st);
  assert(ge.update(rpm3, 60, st) == 3);
  assert(ge.update(800, 0, st) == 1 && st);

  // Brake guess: 60 -> 57 km/h in 0.5 s with throttle closed = 6 km/h/s -> braking; with throttle open -> not
  BrakeGuess bg; float d;
  bg.update(60, 0, 1000, d); assert(bg.update(57, 0, 1500, d) && near(d, 6));
  BrakeGuess bg2; bg2.update(60, 30, 1000, d); assert(!bg2.update(57, 30, 1500, d));

  // Levels, including low-is-bad and symmetric (fuel trims)
  assert(level(5600, {0, 7000, 4500, 5500}) == 2 && level(4600, {0, 7000, 4500, 5500}) == 1);
  assert(level(8, {0, 100, 20, 10}) == 2 && level(15, {0, 100, 20, 10}) == 1 && level(50, {0, 100, 20, 10}) == 0);
  assert(level(-12, {-25, 25, 10, 20, true}) == 1 && level(NAN, {0, 1, 0, 0}) == 0);

  std::puts("dash_core: all tests passed");
  return 0;
}
