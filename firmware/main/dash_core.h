// Portable dash logic: no ESP-IDF or LVGL here, so it is unit-tested on a PC (firmware/test).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace dash {

// One snapshot of what the dash shows. NAN = not read yet / not available.
struct Telemetry {
  float rpm = NAN, kmh = NAN, throttle = NAN, load = NAN;
  float ect = NAN, iat = NAN, tft = NAN, fuel = NAN, volts = NAN;
  float ltft1 = NAN, ltft2 = NAN;
  int gear = 1;            // estimated 1..6
  bool stopped = true;
  bool brakeGuess = false; // inferred from deceleration with throttle closed
  float decel = NAN;       // km/h per second, positive = slowing
  uint32_t framesPerSec = 0, idsSeen = 0, answersPerSec = 0;
  bool requesting = false; // false = listen-only (safe first boot)
  bool busActive = false;
};

// ---- OBD-II mode 01: build one request for up to 6 PIDs, decode the (possibly multi-PID) reply ----
// Request payload for ISO-TP single frame: [len, 0x01, pid...], padded to 8 bytes.
size_t buildMode01(const uint8_t* pids, size_t n, uint8_t out[8]);
// Request payload for a mode 22 DID read: [0x03, 0x22, hi, lo, pad...].
void buildMode22(uint16_t did, uint8_t out[8]);
// Decode a full ISO-TP payload starting with 0x41. Returns number of PIDs applied.
int decodeMode01(const uint8_t* p, size_t n, Telemetry& t);
// Decode a mode 22 reply (0x62 hi lo data...) for the DIDs the dash knows. Returns true if applied.
bool decodeMode22(const uint8_t* p, size_t n, Telemetry& t);

constexpr uint16_t DID_TFT = 0x1E1C; // shortcut: Ford trans fluid temp, community-sourced formula; check against FORScan TFT once.

// ---- ISO-TP receive (one response ID). Handles single frames and first+consecutive frames. ----
class IsoTpRx {
 public:
  enum Result { NONE, NEED_FLOW_CONTROL, COMPLETE, ERROR };
  // Feed one CAN data field. On COMPLETE, payload()/size() hold the whole message.
  Result feed(const uint8_t* d, uint8_t len);
  const uint8_t* payload() const { return buf_; }
  size_t size() const { return have_; }
  void reset() { want_ = have_ = 0; nextSn_ = 1; }
 private:
  uint8_t buf_[64] = {};
  size_t want_ = 0, have_ = 0;
  uint8_t nextSn_ = 1;
};
// Flow-control frame the tester sends after a first frame: continue, no block limit, no gap.
void flowControl(uint8_t out[8]);

// ---- Gear from RPM and speed (no shaft speeds over standard OBD) ----
// 6R80 ratios; 25.82 output-shaft rpm per km/h measured from Yousef's FORScan log; 0.95 offsets converter slip.
class GearEstimator {
 public:
  // Returns the held gear; `stopped` set when below 4 km/h. A new gear must repeat `stable` times.
  int update(float rpm, float kmh, bool& stopped);
 private:
  int cur_ = 1, cand_ = 0, n_ = 0;
  static constexpr int stable_ = 3;
};
int nearestGear(float rpm, float kmh);

// ---- Brake guess: slowing faster than 3 km/h/s over ~0.5 s while the throttle is closed ----
class BrakeGuess {
 public:
  bool update(float kmh, float throttle, uint32_t nowMs, float& decelOut);
 private:
  float refKmh_ = NAN; uint32_t refMs_ = 0; bool last_ = false; float lastDecel_ = NAN;
};

// ---- Warning level for gauges: 0 normal, 1 amber, 2 red. bad < warn means "low is bad" (fuel). ----
struct Limits { float min, max, warn, bad; bool symmetric = false; };
int level(float v, const Limits& l);

}  // namespace dash
