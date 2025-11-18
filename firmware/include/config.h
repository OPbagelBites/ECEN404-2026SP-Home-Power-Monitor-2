#pragma once
#include <Arduino.h>   // <-- add this so uint32_t, String, etc. are defined

// ================== Build / Feature Flags ==================
#ifndef TEST_MODE
// 1 = generate synthetic V/I like your Python sandbox
// 0 = read real ADC samples (we'll wire this up next)
#define TEST_MODE 1
#endif

// ================== Signal / Frame Settings ==================
static constexpr float     CFG_FS_HZ          = 4096.0f;   // sample rate
static constexpr uint32_t  CFG_N_SAMPLES      = 1024;      // samples per frame
static constexpr float     CFG_F0_HZ          = 60.0f;     // line frequency
static constexpr char      CFG_WINDOW_NAME[]  = "hann";
static constexpr float     CFG_FRAME_PERIOD_S = 3.0f;     // seconds per frame (~4 fps)

// ================== Simulation Pattern (ON↔OFF) ==================
static constexpr float CFG_ON_DURATION_S   = 5.0f;
static constexpr float CFG_OFF_DURATION_S  = 5.0f;
static constexpr float CFG_I_RMS_OFF_A     = 3.0f;
static constexpr float CFG_I_RMS_ON_A      = 6.0f;
static constexpr float CFG_PHASE_DEG       = 30.0f;       // current lags voltage by +30°
static constexpr float CFG_H2_OFF          = 0.05f;       // 2nd harmonic ratio (sim)
static constexpr float CFG_H2_ON           = 0.12f;
static constexpr float CFG_V_RMS_TARGET    = 120.0f;

// ================== Harmonics / Analysis ==================
static constexpr int   CFG_HARM_KMAX        = 5;           // up to 5th harmonic
static constexpr float CFG_THDV_PLACEHOLDER = 0.01f;       // voltage THD in sim

// ================== Telemetry / Meta ==================
static constexpr char  CFG_FW_TAG[]  = "fw-esp-0-1-0";
static constexpr char  CFG_CAL_ID[]  = "cal-default";

// Firebase (replace these 2 with your project info)
#define CFG_DEVICE_ID     "esp32-devkitc-01"
#define CFG_FB_DB_URL     "https://home-power-monitor-752b7-default-rtdb.firebaseio.com"
#define CFG_FB_AUTH       ""    // leave empty while rules allow open writes

// WiFiManager access point details
#define CFG_AP_NAME       "HomePower-Setup"
#define CFG_AP_PASS       "power1234"

// Network + push tuning
#define CFG_PUSH_ENABLE   1           // 1=on, 0=off
#define CFG_PUSH_EVERY_N  1           // push every frame (or 2/4 if you want to rate-limit)
#define CFG_HTTP_TIMEOUT  4000        // ms
