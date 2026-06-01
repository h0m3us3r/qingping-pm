// ============================================================================
// Qingping PM module — custom ESPHome UART receiver for the post-patch
// 32-byte BM histogram frame.
//
// REQUIRES all four SWD patches from tools/swd_patcher/ (PATCH_UNFILTER=true
// and PATCH_HISTMODE=true, both on by default). Without Patch 4 the module
// emits a Plantower PMSX003-shaped frame instead and the layout below does
// NOT apply — see ../../docs/TECHNICAL.md §6a for that case.
//
// Frame layout (firmware-verified — see ../../docs/TECHNICAL.md §6b for the
// full trace; source builder is the block at flash 0x48F2 inside
// FUN_00004154 of ht32_flash_64k.bin):
//
//   off  size  meaning
//   ─────────────────────────────────────────────────────────────────────────
//    0   2    "BM" magic                                  0x42 0x4D
//    2   2    length field                                0x00 0x1C
//    4   2    bin[0]  STALE SRAM — never written by the histogram code
//                     (BSS-zeroed at cold boot, undefined after SYSRESETREQ);
//                     treat as garbage, do not interpret
//    6  22    bin[1..11]  11 × big-endian uint16 peak-height histogram bins.
//                     With the un-filter SWD patch applied (default in
//                     swd_patcher/, PATCH_UNFILTER=true), each value is the
//                     RAW count from the most recent ~1-second frame window.
//                     Without that patch, each value is the sum of the last
//                     7 frame windows (≈7-second boxcar).
//                     Each bin counts pulses whose peak height satisfied:
//                       bin[1]:  peak > 3100                  (absolute)
//                       bin[2]:  peak > baseline + 2430
//                       bin[3]:  peak > baseline + DAT_00004580 + 0x23
//                       bin[4]:  peak > baseline + 1630
//                       bin[5]:  peak > baseline + 1330
//                       bin[6]:  peak > baseline + 1030
//                       bin[7]:  peak > baseline + 810
//                       bin[8]:  peak > baseline + 650
//                       bin[9]:  peak > baseline + 450
//                       bin[10]: peak > baseline + 100
//                       bin[11]: peak > baseline + 50
//                     bin[1] = largest particles, bin[11] = smallest.
//   28   1    detector baseline low byte = low(state[0x34])
//                     state[0x34] is the 128-sample average of the FIR window
//                     right-shifted by 7. High byte always 0. No smoothing.
//   29   1    constant 0x00
//   30   2    additive checksum, big-endian = sum(bytes 0..29)
//
// What this frame is NOT: it is NOT a Plantower PMSX003 frame, and bytes 4..27
// are NOT PM1/PM2.5/PM10 µg/m³ in any form. The module also has NO temperature
// and NO humidity sensor — those values, when present in the parent Qingping
// monitor body, come from a separate sensor on the main board which reads
// these histogram bins via SWD and converts them internally.
// ============================================================================

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"
#include <cmath>

namespace esphome {
namespace qingping_pm {

class QingpingPM : public Component, public uart::UARTDevice {
 public:
  static constexpr size_t FRAME_LEN = 32;
  static constexpr size_t NUM_BINS  = 11;  // we drop the stale frame[0] slot

  // External-component setters (called by __init__.py / to_code()).
  void set_bin(size_t i, sensor::Sensor *s)    { if (i < NUM_BINS) bin_[i] = s; }
  void set_baseline(sensor::Sensor *s)         { baseline_ = s; }
  void set_frames_received(sensor::Sensor *s)  { frames_received_ = s; }
  void set_bad_checksums(sensor::Sensor *s)    { bad_checksums_ = s; }
  void set_particle_rate(sensor::Sensor *s)        { particle_rate_ = s; }
  void set_particle_rate_delta(sensor::Sensor *s)  { particle_rate_delta_ = s; }

  void setup() override {
    ESP_LOGI("qingping_pm", "Listening for 32-byte BM histogram frames");
  }

  void loop() override {
    while (available()) {
      uint8_t b;
      if (!read_byte(&b)) break;

      // Sync hunt: keep a sliding 4-byte preamble window and resync on
      // 42 4D 00 1C so a partial / corrupted frame can't desynchronise us
      // for long.
      if (idx_ < 4) {
        buf_[idx_++] = b;
        if (idx_ == 4 &&
            !(buf_[0] == 0x42 && buf_[1] == 0x4D &&
              buf_[2] == 0x00 && buf_[3] == 0x1C)) {
          // Slide window forward by one byte and retry.
          buf_[0] = buf_[1];
          buf_[1] = buf_[2];
          buf_[2] = buf_[3];
          idx_    = 3;
        }
        continue;
      }
      buf_[idx_++] = b;
      if (idx_ == FRAME_LEN) {
        handle_frame_();
        idx_ = 0;
      }
    }
  }

 private:
  uint8_t buf_[FRAME_LEN] = {0};
  size_t  idx_            = 0;
  uint32_t frames_seen_   = 0;
  uint32_t bad_checksums_seen_ = 0;

  sensor::Sensor *bin_[NUM_BINS]   = {nullptr};
  sensor::Sensor *baseline_        = nullptr;
  sensor::Sensor *frames_received_ = nullptr;
  sensor::Sensor *bad_checksums_   = nullptr;
  sensor::Sensor *particle_rate_       = nullptr;
  sensor::Sensor *particle_rate_delta_ = nullptr;

  // For the per-frame rate-delta calculation. Both are reset to fresh
  // sentinel values at construction so the very first frame publishes a
  // rate but NOT a delta (no prior point to subtract).
  float    prev_rate_ = NAN;
  uint32_t prev_ms_   = 0;

  static inline uint16_t be16_(const uint8_t *p) {
    return (uint16_t)p[0] << 8 | (uint16_t)p[1];
  }

  void handle_frame_() {
    // Verify additive checksum.
    uint16_t want = be16_(&buf_[FRAME_LEN - 2]);
    uint16_t got  = 0;
    for (size_t i = 0; i < FRAME_LEN - 2; i++) got = (uint16_t)(got + buf_[i]);
    if (want != got) {
      bad_checksums_seen_++;
      ESP_LOGW("qingping_pm",
               "checksum mismatch (got 0x%04X want 0x%04X) — frame dropped",
               got, want);
      if (bad_checksums_) bad_checksums_->publish_state(bad_checksums_seen_);
      return;
    }

    uint16_t bins[NUM_BINS];
    uint32_t total = 0;
    for (size_t i = 0; i < NUM_BINS; i++) {
      // Frame layout: bytes 4..5 are stale; useful bins start at byte 6.
      bins[i] = be16_(&buf_[6 + 2 * i]);
      total  += bins[i];
      if (bin_[i]) bin_[i]->publish_state(bins[i]);
    }
    uint8_t baseline = buf_[28];

    // With the unfilter SWD patch each bin is a raw 1-second window count,
    // so the sum of all bins is the total peak count over the last ~1 s
    // (= peaks/s). publish_state() runs once per BM frame (~1 Hz).
    if (particle_rate_) particle_rate_->publish_state((float)total);

    // Rate-of-change of that rate (counts/s²). Uses millis() rather than a
    // fixed 1 s assumption so an occasional dropped frame doesn't bias the
    // result. First frame produces no delta (prev_rate_ still NaN).
    uint32_t now = millis();
    if (particle_rate_delta_ && !std::isnan(prev_rate_)) {
      float dt = (now - prev_ms_) * 0.001f;
      if (dt > 0.1f) {
        float drate = ((float)total - prev_rate_) / dt;
        particle_rate_delta_->publish_state(drate);
        ESP_LOGD("qingping_pm", "rate=%u/s  prev=%.0f  dΔ=%+.1f/s²  dt=%.2fs",
                 (unsigned)total, prev_rate_, drate, dt);
      }
    }
    prev_rate_ = (float)total;
    prev_ms_   = now;

    if (baseline_)        baseline_->publish_state(baseline);
    frames_seen_++;
    if (frames_received_) frames_received_->publish_state(frames_seen_);
  }
};

}  // namespace qingping_pm
}  // namespace esphome
