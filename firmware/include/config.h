#pragma once
#include <Arduino.h>

// ================== Build / Feature Flags ==================
#ifndef TEST_MODE
#define TEST_MODE 0
#endif

// ================== Signal / Frame Settings ==================
static constexpr float     CFG_FS_HZ          = 1000.0f;
static constexpr uint32_t  CFG_N_SAMPLES      = 256;
static constexpr float     CFG_F0_HZ          = 60.0f;
static constexpr char      CFG_WINDOW_NAME[]  = "hann";
static constexpr float     CFG_FRAME_PERIOD_S = 2.0f;

// ================== Simulation Pattern ==================
static constexpr float CFG_ON_DURATION_S   = 5.0f;
static constexpr float CFG_OFF_DURATION_S  = 5.0f;
static constexpr float CFG_I_RMS_OFF_A     = 3.0f;
static constexpr float CFG_I_RMS_ON_A      = 6.0f;
static constexpr float CFG_PHASE_DEG       = 30.0f;
static constexpr float CFG_H2_OFF          = 0.05f;
static constexpr float CFG_H2_ON           = 0.12f;
static constexpr float CFG_V_RMS_TARGET    = 120.0f;

// ================== Harmonics / Analysis ==================
static constexpr int   CFG_HARM_KMAX        = 5;
static constexpr float CFG_THDV_PLACEHOLDER = 0.01f;

// ================== Telemetry / Meta ==================
static constexpr char  CFG_FW_TAG[]  = "fw-esp-0-1-1-adcdebug";
static constexpr char  CFG_CAL_ID[]  = "cal-default";

// Firebase
#define CFG_DEVICE_ID     "esp32-devkitc-01"
#define CFG_FB_DB_URL     "https://home-power-monitor-752b7-default-rtdb.firebaseio.com"
#define CFG_FB_AUTH       ""

// WiFiManager AP
#define CFG_AP_NAME       "HomePower-Setup"
#define CFG_AP_PASS       "power1234"

// Push tuning
#define CFG_PUSH_ENABLE   1
#define CFG_PUSH_EVERY_N  1
#define CFG_HTTP_TIMEOUT  4000

// ================== External ADC (ADS8344) ==================
#if !TEST_MODE
static constexpr int      CFG_ADC_CS_PIN   = 27;
static constexpr uint32_t CFG_ADC_SPI_HZ   = 100000;
static constexpr uint8_t  CFG_ADC_SPI_MODE = SPI_MODE0;

// ADS8344 command bytes with internal clock bit set
// CH0 = current, CH1 = voltage
static constexpr uint8_t  CFG_ADC_CMD_CH_I = 0x8E;
static constexpr uint8_t  CFG_ADC_CMD_CH_V = 0x9E;

// Debug controls
static constexpr bool     CFG_ADC_DEBUG_SINGLE_CH   = true;
static constexpr uint8_t  CFG_ADC_DEBUG_FORCE_CMD   = 0xCE;
static constexpr bool     CFG_ADC_DEBUG_SKIP_DC_REM = true;
static constexpr bool     CFG_ADC_DEBUG_RAW_BYTES   = true;

// Reference voltage
static constexpr float    CFG_ADC_VREF_V = 3.3f;

// Calibration
static constexpr float CFG_V_BIAS_V = 1.65f;
static constexpr float CFG_I_BIAS_V = 1.65f;

// Debug scaling = unity
static constexpr float CFG_V_SCALE  = 1.0f;
static constexpr float CFG_I_SCALE  = 1.0f;
#endif