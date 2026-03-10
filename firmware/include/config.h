#pragma once
#include <Arduino.h>

// ================== Build / Feature Flags ==================
#ifndef TEST_MODE
// 1 = generate synthetic V/I (SIM)
// 0 = read real samples from external ADC (ADS8344 on PCB)
#define TEST_MODE 0
#endif

// ================== Signal / Frame Settings ==================
static constexpr float     CFG_FS_HZ          = 4096.0f;
static constexpr uint32_t  CFG_N_SAMPLES      = 1024;
static constexpr float     CFG_F0_HZ          = 60.0f;
static constexpr char      CFG_WINDOW_NAME[]  = "hann";
static constexpr float     CFG_FRAME_PERIOD_S = 3.0f;

// ================== Simulation Pattern (ON↔OFF) ==================
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
static constexpr char  CFG_FW_TAG[]  = "fw-esp-0-1-1";
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
// CS is IO27 per your PCB
static constexpr int   CFG_ADC_CS_PIN   = 27;

// ADS8344: CH0 = Current, CH1 = Voltage per your schematic
static constexpr uint8_t CFG_ADC_CH_I   = 0;
static constexpr uint8_t CFG_ADC_CH_V   = 1;

// Reference voltage tied to 3V3 on your board
static constexpr float   CFG_ADC_VREF_V = 3.3f;

// SPI clock for ADS8344
static constexpr uint32_t CFG_ADC_SPI_HZ = 2000000;

// Debug knobs
static constexpr bool CFG_ADC_DEBUG_RAW_BYTES   = true;   // print raw SPI bytes periodically
static constexpr bool CFG_ADC_DEBUG_SKIP_DC_REM = false;   // leave raw DC bias in place while debugging
static constexpr bool CFG_ADC_USE_DUMMY_READ    = false;   // throw away first conversion after channel select

// ADS8344 command bits
// Command format: S A2 A1 A0 S/D PD1 PD0
static constexpr uint8_t CFG_ADC_CMD_START      = 0x80;   // bit 7
static constexpr uint8_t CFG_ADC_CMD_SINGLE_EN  = 0x04;   // S/D = 1
static constexpr uint8_t CFG_ADC_CMD_PD_ALWAYS  = 0x03;   // PD1=1 PD0=1 keep powered/reference on

// ================== Calibration ==================
static constexpr float CFG_V_BIAS_V        = 1.65f;
static constexpr float CFG_I_BIAS_V        = 1.65f;

// Real-world scaling factors.
// These convert post-ADC volts into real units after DC removal.
//
// Example meaning:
// if 0.010 V RMS at ADC corresponds to 120 V RMS real mains,
// then CFG_V_SCALE = 120 / 0.010 = 12000.
//
// if 0.020 V RMS at ADC corresponds to 5.0 A RMS real current,
// then CFG_I_SCALE = 5.0 / 0.020 = 250.
static constexpr float CFG_V_SCALE         = 113.85f;   // replace after first calibration
static constexpr float CFG_I_SCALE         = 1.0f;   // replace after first calibration
#endif