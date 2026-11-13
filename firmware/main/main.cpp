// F-150 dash firmware for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (ESP-IDF >= 5.5).
// The truck side is read-only: it listens to the CAN bus, and in "Read data" mode it sends standard
// OBD-II requests (mode 01) plus one Ford read (mode 22) to the engine computer. It never writes or clears.
#include <atomic>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "dash_core.h"
#include "dash_ui.h"

static const char* TAG = "dash";

// CAN pins of the board's PH2.0 CAN connector (Waveshare 14_twai_transmit example defaults).
static constexpr gpio_num_t CAN_TX = GPIO_NUM_22, CAN_RX = GPIO_NUM_21;
static constexpr uint32_t PCM_REQ = 0x7E0, PCM_RESP = 0x7E8;  // engine computer, physical addressing
static constexpr uint32_t SCREEN_OFF_AFTER_MS = 60000;        // truck off: dim the screen after a minute

struct RxFrame { uint32_t id; uint8_t len; uint8_t d[8]; };
static QueueHandle_t rxq;
static twai_node_handle_t node = nullptr;

static portMUX_TYPE telLock = portMUX_INITIALIZER_UNLOCKED;
static dash::Telemetry shared;     // written by the CAN task, read by the UI timer (under telLock)
static char sharedBus[200] = "No bus traffic yet";
static std::atomic<bool> wantRequesting{false};
static bool night = true;
static int brightness = 80;

// ---------- settings (NVS) ----------
static void loadSettings() {
  nvs_handle_t h;
  if (nvs_open("dash", NVS_READONLY, &h) != ESP_OK) return;
  uint8_t v; int32_t b;
  if (nvs_get_u8(h, "night", &v) == ESP_OK) night = v;
  if (nvs_get_i32(h, "bright", &b) == ESP_OK) brightness = b;
  if (nvs_get_u8(h, "request", &v) == ESP_OK) wantRequesting = v;
  nvs_close(h);
}
static void saveSettings() {
  nvs_handle_t h;
  if (nvs_open("dash", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "night", night);
  nvs_set_i32(h, "bright", brightness);
  nvs_set_u8(h, "request", wantRequesting.load());
  nvs_commit(h);
  nvs_close(h);
}

// ---------- CAN ----------
static bool IRAM_ATTR onRx(twai_node_handle_t h, const twai_rx_done_event_data_t*, void*) {
  RxFrame f{};
  twai_frame_t fr{};
  fr.buffer = f.d;
  fr.buffer_len = sizeof f.d;
  if (twai_node_receive_from_isr(h, &fr) != ESP_OK) return false;
  f.id = fr.header.id;
  f.len = fr.header.dlc > 8 ? 8 : fr.header.dlc;
  BaseType_t woke = pdFALSE;
  xQueueSendFromISR(rxq, &f, &woke);
  return woke == pdTRUE;
}

// Listen-only mode never drives the bus (no ACKs, no frames), so a wiring or speed mistake can't upset the truck.
static esp_err_t startNode(bool listenOnly) {
  if (node) { twai_node_disable(node); twai_node_delete(node); node = nullptr; }
  twai_onchip_node_config_t cfg = {};
  cfg.io_cfg.tx = CAN_TX;
  cfg.io_cfg.rx = CAN_RX;
  cfg.io_cfg.quanta_clk_out = GPIO_NUM_NC;
  cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC;
  cfg.bit_timing.bitrate = 500000;  // F-150 HS-CAN on OBD pins 6/14
  cfg.fail_retry_cnt = 3;
  cfg.tx_queue_depth = 8;
  cfg.flags.enable_listen_only = listenOnly;
  ESP_RETURN_ON_ERROR(twai_new_node_onchip(&cfg, &node), TAG, "create CAN node");
  twai_event_callbacks_t cbs = {};
  cbs.on_rx_done = onRx;
  ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(node, &cbs, nullptr), TAG, "CAN callbacks");
  ESP_RETURN_ON_ERROR(twai_node_enable(node), TAG, "enable CAN");
  ESP_LOGI(TAG, "CAN started (%s)", listenOnly ? "listen only" : "read data");
  return ESP_OK;
}

// Transmit is asynchronous, so each frame keeps its own buffer until the queue has moved past it.
static void sendFrame(uint32_t id, const uint8_t d[8]) {
  static uint8_t bufs[8][8];
  static int next = 0;
  uint8_t* b = bufs[next];
  next = (next + 1) % 8;
  std::memcpy(b, d, 8);
  twai_frame_t f = {};
  f.header.id = id;
  f.header.dlc = 8;
  f.buffer = b;
  f.buffer_len = 8;
  if (twai_node_transmit(node, &f, 0) != ESP_OK) ESP_LOGW(TAG, "CAN transmit queue full");
}

static uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }

static void canTask(void*) {
  dash::IsoTpRx iso;
  dash::GearEstimator gear;
  dash::BrakeGuess brake;
  dash::Telemetry t;
  bool requesting = wantRequesting.load();
  ESP_ERROR_CHECK(startNode(!requesting));

  struct IdCount { uint32_t id, n; };
  IdCount ids[96] = {};
  int nIds = 0;
  uint32_t frames = 0, answers = 0, statMs = nowMs(), lastFrameMs = 0, lastAnswerMs = 0;
  uint32_t lastReqMs = 0, step = 0;
  bool waiting = false;

  // What to ask for, in turn: RPM/speed/throttle most of the time, the slower values in between.
  static const uint8_t FAST[] = {0x0C, 0x0D, 0x11};
  static const uint8_t SLOW_A[] = {0x04, 0x05, 0x0F, 0x2F, 0x42, 0x07};
  static const uint8_t SLOW_B[] = {0x09};

  for (;;) {
    RxFrame f;
    while (xQueueReceive(rxq, &f, pdMS_TO_TICKS(5)) == pdTRUE) {
      frames++;
      lastFrameMs = nowMs();
      int k = 0;
      while (k < nIds && ids[k].id != f.id) k++;
      if (k == nIds && nIds < 96) ids[nIds++] = {f.id, 0};
      if (k < nIds) ids[k].n++;
      if (!requesting || f.id != PCM_RESP) continue;
      switch (iso.feed(f.d, f.len)) {
        case dash::IsoTpRx::NEED_FLOW_CONTROL: {
          uint8_t fc[8];
          dash::flowControl(fc);
          sendFrame(PCM_REQ, fc);
          break;
        }
        case dash::IsoTpRx::COMPLETE: {
          const uint8_t* p = iso.payload();
          if (p[0] == 0x41) dash::decodeMode01(p, iso.size(), t);
          else if (p[0] == 0x62) dash::decodeMode22(p, iso.size(), t);
          answers++;
          lastAnswerMs = nowMs();
          waiting = false;
          iso.reset();
          break;
        }
        case dash::IsoTpRx::ERROR: iso.reset(); break;
        default: break;
      }
    }

    uint32_t ms = nowMs();
    bool want = wantRequesting.load();
    if (want != requesting) {
      requesting = want;
      ESP_ERROR_CHECK(startNode(!requesting));
      iso.reset();
      waiting = false;
      if (!requesting) t = dash::Telemetry{};  // values would go stale: blank them
    }

    // One request at a time; give up on an answer after 100 ms. Pace at most one request per 20 ms.
    if (requesting && (!waiting || ms - lastReqMs > 100) && ms - lastReqMs >= 20) {
      uint8_t req[8];
      switch (step++ % 8) {
        case 2: dash::buildMode01(SLOW_A, sizeof SLOW_A, req); break;
        case 5: dash::buildMode01(SLOW_B, sizeof SLOW_B, req); break;
        case 7: dash::buildMode22(dash::DID_TFT, req); break;
        default: dash::buildMode01(FAST, sizeof FAST, req); break;
      }
      iso.reset();
      sendFrame(PCM_REQ, req);
      waiting = true;
      lastReqMs = ms;
    }
    if (requesting && lastAnswerMs && ms - lastAnswerMs > 3000) {  // engine computer went quiet (ignition off)
      t = dash::Telemetry{};
      lastAnswerMs = 0;
    }

    t.gear = gear.update(t.rpm, t.kmh, t.stopped);
    t.brakeGuess = brake.update(t.kmh, t.throttle, ms, t.decel);
    t.requesting = requesting;
    t.busActive = lastFrameMs && ms - lastFrameMs < 2000;

    char bus[200];
    bool newBus = false;
    if (ms - statMs >= 1000) {
      t.framesPerSec = frames;
      t.answersPerSec = answers;
      t.idsSeen = nIds;
      // Busiest IDs: proof the wiring and bus speed are right, and a start for finding broadcast data.
      int used = std::snprintf(bus, sizeof bus, "%u frames/s from %d IDs. Busiest:", unsigned(frames), nIds);
      for (int r = 0; r < 4; r++) {
        int best = -1;
        for (int i = 0; i < nIds; i++) if (ids[i].n && (best < 0 || ids[i].n > ids[best].n)) best = i;
        if (best < 0) break;
        used += std::snprintf(bus + used, sizeof bus - used, " 0x%03X (%u/s)", unsigned(ids[best].id), unsigned(ids[best].n));
        ids[best].n = 0;
      }
      for (int i = 0; i < nIds; i++) ids[i].n = 0;
      if (!frames) std::snprintf(bus, sizeof bus, "No bus traffic: check the CAN wires (OBD pins 6 and 14) and the ignition.");
      frames = answers = 0;
      statMs = ms;
      newBus = true;
    }

    portENTER_CRITICAL(&telLock);
    shared = t;
    if (newBus) std::memcpy(sharedBus, bus, sizeof bus);
    portEXIT_CRITICAL(&telLock);
  }
}

// ---------- UI glue (runs in the LVGL task) ----------
static void rebuildUi(void*);
static DashUiHooks hooks() {
  DashUiHooks h;
  h.setRequesting = [](bool on) { wantRequesting = on; saveSettings(); };
  h.setBrightness = [](int pct) { brightness = pct; bsp_display_brightness_set(pct); saveSettings(); };
  // Rebuilding deletes the switch that fired the event, so do it after the event finishes.
  h.setNight = [](bool n) { night = n; saveSettings(); lv_async_call(rebuildUi, nullptr); };
  return h;
}
static void rebuildUi(void*) {
  dash_ui_build(night, brightness, hooks());
  dash_ui_show_page(4);  // the change was made on the Settings page
}

static void uiTick(lv_timer_t*) {
  static dash::Telemetry t;
  static char bus[200];
  static bool screenOn = true;
  static uint32_t quietSince = 0;
  portENTER_CRITICAL(&telLock);
  t = shared;
  std::memcpy(bus, sharedBus, sizeof bus);
  portEXIT_CRITICAL(&telLock);
  dash_ui_update(t, bus);

  // Screen off a minute after the truck goes quiet; back on with the first frame.
  uint32_t ms = nowMs();
  if (t.busActive) quietSince = 0;
  else if (!quietSince) quietSince = ms;
  bool on = !quietSince || ms - quietSince < SCREEN_OFF_AFTER_MS;
  if (on != screenOn) {
    screenOn = on;
    bsp_display_brightness_set(on ? brightness : 0);
  }
}

extern "C" void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  loadSettings();

  // Same display setup as Waveshare's 09_lvgl_demo_v9 example for this board.
  bsp_display_cfg_t cfg = {};
  cfg.lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
  cfg.rotation = ESP_LV_ADAPTER_ROTATE_180;
  cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
  cfg.touch_flags.swap_xy = 0;
  cfg.touch_flags.mirror_x = 1;
  cfg.touch_flags.mirror_y = 1;
  lv_display_t* display = bsp_display_start_with_config(&cfg);
  ESP_ERROR_CHECK(display ? ESP_OK : ESP_FAIL);
  bsp_display_brightness_set(brightness);

  ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT);
  dash_ui_build(night, brightness, hooks());
  lv_timer_create(uiTick, 100, nullptr);
  bsp_display_unlock();

  rxq = xQueueCreate(256, sizeof(RxFrame));
  xTaskCreate(canTask, "can", 6144, nullptr, 5, nullptr);
}
