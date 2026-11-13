#include "dash_core.h"
#include <cstring>

namespace dash {

size_t buildMode01(const uint8_t* pids, size_t n, uint8_t out[8]) {
  if (n > 6) n = 6;
  std::memset(out, 0, 8);
  out[0] = uint8_t(1 + n);
  out[1] = 0x01;
  std::memcpy(out + 2, pids, n);
  return n;
}

void buildMode22(uint16_t did, uint8_t out[8]) {
  std::memset(out, 0, 8);
  out[0] = 0x03; out[1] = 0x22; out[2] = uint8_t(did >> 8); out[3] = uint8_t(did);
}

static int pidLen(uint8_t pid) {
  switch (pid) {
    case 0x0C: case 0x42: return 2;
    case 0x0D: case 0x11: case 0x04: case 0x05: case 0x0F: case 0x2F: case 0x07: case 0x09: return 1;
    default: return 0;  // unknown PID: stop decoding, lengths after it are unknowable
  }
}

int decodeMode01(const uint8_t* p, size_t n, Telemetry& t) {
  if (n < 3 || p[0] != 0x41) return 0;
  int applied = 0;
  size_t i = 1;
  while (i < n) {
    uint8_t pid = p[i];
    int len = pidLen(pid);
    if (!len || i + 1 + len > n) break;
    const uint8_t* b = p + i + 1;
    switch (pid) {
      case 0x0C: t.rpm = ((b[0] << 8) | b[1]) / 4.0f; break;
      case 0x0D: t.kmh = b[0]; break;
      case 0x11: t.throttle = b[0] * 100.0f / 255; break;
      case 0x04: t.load = b[0] * 100.0f / 255; break;
      case 0x05: t.ect = b[0] - 40.0f; break;
      case 0x0F: t.iat = b[0] - 40.0f; break;
      case 0x2F: {  // fuel sender sloshes: average over ~30 s like the factory gauge
        float v = b[0] * 100.0f / 255;
        t.fuel = std::isnan(t.fuel) ? v : t.fuel + (v - t.fuel) * 0.03f;
        break;
      }
      case 0x42: t.volts = ((b[0] << 8) | b[1]) / 1000.0f; break;
      case 0x07: t.ltft1 = (b[0] - 128) * 100.0f / 128; break;
      case 0x09: t.ltft2 = (b[0] - 128) * 100.0f / 128; break;
    }
    applied++;
    i += 1 + len;
  }
  return applied;
}

bool decodeMode22(const uint8_t* p, size_t n, Telemetry& t) {
  if (n < 5 || p[0] != 0x62) return false;
  uint16_t did = uint16_t(p[1] << 8 | p[2]);
  if (did == DID_TFT) {
    float v = int16_t(uint16_t(p[3] << 8 | p[4])) / 16.0f;
    if (v > -40 && v < 160) { t.tft = v; return true; }  // nonsense means wrong formula: leave it blank
  }
  return false;
}

IsoTpRx::Result IsoTpRx::feed(const uint8_t* d, uint8_t len) {
  if (len < 1) return NONE;
  uint8_t type = d[0] >> 4;
  if (type == 0) {  // single frame
    size_t n = d[0] & 0x0F;
    if (n == 0 || n > 7 || n > size_t(len - 1)) return ERROR;
    std::memcpy(buf_, d + 1, n); want_ = have_ = n;
    return COMPLETE;
  }
  if (type == 1) {  // first frame
    if (len < 8) return ERROR;
    want_ = size_t((d[0] & 0x0F) << 8 | d[1]);
    if (want_ > sizeof buf_ || want_ < 8) { reset(); return ERROR; }
    std::memcpy(buf_, d + 2, 6); have_ = 6; nextSn_ = 1;
    return NEED_FLOW_CONTROL;
  }
  if (type == 2) {  // consecutive frame
    if (!want_ || (d[0] & 0x0F) != nextSn_) { reset(); return ERROR; }
    nextSn_ = (nextSn_ + 1) & 0x0F;
    size_t take = want_ - have_ < size_t(len - 1) ? want_ - have_ : size_t(len - 1);
    std::memcpy(buf_ + have_, d + 1, take); have_ += take;
    return have_ >= want_ ? COMPLETE : NONE;
  }
  return NONE;  // flow-control frames from the other side are not expected here
}

void flowControl(uint8_t out[8]) { std::memset(out, 0, 8); out[0] = 0x30; }

static const float kRatios[6] = {4.171f, 2.340f, 1.521f, 1.143f, 0.867f, 0.691f};

int nearestGear(float rpm, float kmh) {
  float ratio = rpm / (kmh * 25.82f) * 0.95f;
  int best = 1;
  for (int g = 2; g <= 6; g++)
    if (std::fabs(std::log(ratio / kRatios[g - 1])) < std::fabs(std::log(ratio / kRatios[best - 1]))) best = g;
  return best;
}

int GearEstimator::update(float rpm, float kmh, bool& stopped) {
  stopped = !(kmh >= 4) || std::isnan(rpm);
  if (stopped) { cur_ = 1; cand_ = n_ = 0; return cur_; }
  int g = nearestGear(rpm, kmh);
  if (g == cur_) { cand_ = n_ = 0; return cur_; }
  n_ = (g == cand_) ? n_ + 1 : 1; cand_ = g;
  if (n_ >= stable_) { cur_ = g; cand_ = n_ = 0; }
  return cur_;
}

bool BrakeGuess::update(float kmh, float throttle, uint32_t nowMs, float& decelOut) {
  if (std::isnan(kmh)) { decelOut = NAN; return false; }
  if (std::isnan(refKmh_)) { refKmh_ = kmh; refMs_ = nowMs; }
  uint32_t dt = nowMs - refMs_;
  if (dt >= 500) {  // re-evaluate twice a second
    lastDecel_ = (refKmh_ - kmh) / (dt / 1000.0f);
    bool closed = !(throttle > 4);  // closed or unknown: hard slowing then means brakes, not just lifting off
    last_ = closed && lastDecel_ > 3;
    refKmh_ = kmh; refMs_ = nowMs;
  }
  decelOut = lastDecel_;
  return last_;
}

int level(float v, const Limits& l) {
  if (std::isnan(v)) return 0;
  if (l.symmetric) v = std::fabs(v);
  if (l.bad < l.warn) return v <= l.bad ? 2 : v <= l.warn ? 1 : 0;
  return v >= l.bad ? 2 : v >= l.warn ? 1 : 0;
}

}  // namespace dash
