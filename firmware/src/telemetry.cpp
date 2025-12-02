/**
 * telemetry.cpp
 *
 * This module is responsible for taking all of the per-frame DSP results
 * (RMS, power, harmonics, events, etc.) and packing them into a single
 * JSON object that we stream out of the ESP32 and optionally push to
 * Firebase Realtime Database.
 *
 * High-level idea:
 *   - main.cpp computes scalar metrics (FrameCore), harmonic ratios, and
 *     event info (latest_event).
 *   - pack_frame_json() receives those inputs and builds a JSON frame
 *     with a fixed schema version.
 *   - This JSON is the "contract" between firmware, backend, and ML.
 *
 * JSON includes:
 *   - Metadata: schema version, sampling rate, frame index, window type.
 *   - Core electrical metrics: Vrms, Irms, P, S, true PF.
 *   - Fundamental-only metrics: v1_rms, i1_rms, phi_deg, P1, Q1, S1, PF_disp.
 *   - Harmonic content: THD_i, THD_v, harmonic ratios, odd/even sums.
 *   - Shape factors: crest factor, form factor.
 *   - State and deltas: ON/OFF state string, dIrms, dP.
 *   - Events: most recent ON/OFF event (if any).
 *   - Health flags: fft_ok, sync_ok, adc_ok.
 *   - Firmware + calibration identifiers.
 */

#include "telemetry.h"
#include <ArduinoJson.h>
#include <math.h>

// Keep this in sync with main.cpp's latest_event size.
// This is the capacity of the shared StaticJsonDocument used for events.
#ifndef EVENT_DOC_BYTES
  #define EVENT_DOC_BYTES 160
#endif

// Base capacity for the frame-level JSON document.
// If you add lots of new fields, bump this up.
#ifndef FRAME_DOC_BYTES
  #define FRAME_DOC_BYTES 1024
#endif

// Bring in the latest_event built in main.cpp.
// main.cpp owns the StaticJsonDocument; we just read/copy from it here.
extern StaticJsonDocument<EVENT_DOC_BYTES> latest_event;

// ─────────────────────────────────────────────────────────────
// Small rounding helpers
// ─────────────────────────────────────────────────────────────
/**
 * r3(x): round to 3 decimal places.
 * r2(x): round to 2 decimal places.
 *
 * Why we round:
 *   - Reduces JSON verbosity / noise.
 *   - Keeps features stable and avoids tiny floating-point jitter.
 *   - Still accurate enough for ML & visualization.
 */
static inline float r3(float x) { return roundf(x * 1000.0f) / 1000.0f; }
static inline float r2(float x) { return roundf(x *  100.0f) /  100.0f; }

/**
 * pack_frame_json(...)
 *
 * Inputs:
 *   t_ms       : timestamp in milliseconds (usually from utils::now_ms()).
 *   frame_id   : monotonic frame counter.
 *   fs         : sampling frequency [Hz].
 *   N          : number of samples in this frame.
 *   window_name: name of the window used (e.g., "hann").
 *   f0_hz      : mains fundamental frequency [Hz] (e.g., 60.0).
 *
 *   c          : FrameCore struct with all the scalar DSP metrics:
 *                - Vrms, Irms, P, S, PF_true, etc.
 *   h_i        : map of harmonic ratios for current (key = "freq_Hz",
 *                value = ratio vs fundamental).
 *   state      : string describing device state, e.g. "on" / "off".
 *   d_irms     : change in Irms vs previous frame (Irms - Irms_prev).
 *   d_p        : change in P vs previous frame (P - P_prev).
 *
 *   fft_ok     : health flag indicating FFT/harmonic analysis is valid.
 *   sync_ok    : health flag indicating timing / mains-sync is OK.
 *   adc_ok     : health flag indicating ADC/AFE is behaving.
 *
 *   fw         : firmware tag / version string.
 *   cal_id     : calibration ID string describing which calibration data
 *                was used to scale raw ADC units into real volts/amps.
 *
 * Return:
 *   - String containing the serialized JSON frame.
 *
 * Schema overview:
 *   {
 *     "schema": 1,
 *     "t": <ms>,
 *     "frame_id": <n>,
 *     "fs": <Hz>,
 *     "N": <samples>,
 *     "window": "hann",
 *     "freq_hz": 60.0,
 *     "rms_v": ...,
 *     "rms_i": ...,
 *     "p": ...,
 *     "s": ...,
 *     "pf_true": ...,
 *     "v1_rms": ...,
 *     "i1_rms": ...,
 *     "phi_deg": ...,
 *     "p1": ...,
 *     "q1": ...,
 *     "s1": ...,
 *     "pf_disp": ...,
 *     "thd_i": ...,
 *     "thd_v": ...,
 *     "h_i": { "120": 0.12, ... },
 *     "odd_sum_i": ...,
 *     "even_sum_i": ...,
 *     "crest_i": ...,
 *     "form_i": ...,
 *     "state": "on",
 *     "d_irms": ...,
 *     "d_p": ...,
 *     "events": [ { "type": "on", "t_ms": 1234567 } ],
 *     "fft_ok": true,
 *     "sync_ok": true,
 *     "adc_ok": true,
 *     "fw": "fw-tag",
 *     "cal_id": "cal-001"
 *   }
 */
String pack_frame_json(
  uint64_t t_ms,
  uint32_t frame_id,
  float fs,
  uint32_t N,
  const char* window_name,
  float f0_hz,
  const FrameCore& c,
  const std::map<String, float>& h_i,
  const char* state,
  float d_irms,
  float d_p,
  bool fft_ok,
  bool sync_ok,
  bool adc_ok,
  const char* fw,
  const char* cal_id
) {
  // Allocate a static-size document on the stack (FRAME_DOC_BYTES bytes).
  // This avoids heap fragmentation and makes memory use predictable.
  StaticJsonDocument<FRAME_DOC_BYTES> doc;

  // ── 1) Schema / metadata fields ──────────────────────────────────────────
  doc["schema"]          = 1;    // schema version (bump if structure changes)
  doc["delta_convention"] = "current_minus_previous";  // how we define d_*
  doc["h_i_units"]        = "relative_to_fundamental"; // harmonic ratios

  // Time/base frame information
  doc["t"]       = t_ms;        // timestamp in ms
  doc["frame_id"] = frame_id;   // logical frame index
  doc["fs"]      = fs;          // sampling frequency [Hz]
  doc["N"]       = N;           // number of samples per frame
  doc["window"]  = window_name; // e.g. "hann"
  doc["freq_hz"] = r3(f0_hz);   // mains fundamental frequency (rounded)

  // ── 2) Core power metrics ────────────────────────────────────────────────
  doc["rms_v"]   = r3(c.vrms);      // voltage RMS [V]
  doc["rms_i"]   = r3(c.irms);      // current RMS [A]
  doc["p"]       = r3(c.P);         // active (real) power [W]
  doc["s"]       = r3(c.S);         // apparent power [VA]
  doc["pf_true"] = r3(c.PF_true);   // true power factor = P / S

  // ── 3) Fundamental-only metrics (phasor-based) ───────────────────────────
  doc["v1_rms"]  = r3(c.v1_rms);    // fundamental RMS of V
  doc["i1_rms"]  = r3(c.i1_rms);    // fundamental RMS of I

  // Phase angle: phi = angle(V1) - angle(I1)
  doc["phi_deg"] = r2(c.phi_deg);
  doc["phi_convention"] = "phi = angle(V1) - angle(I1); positive = current lags";

  // Fundamental P/Q/S and displacement PF
  doc["p1"]      = r3(c.P1);        // fundamental active power [W]
  doc["q1"]      = r3(c.Q1);        // fundamental reactive power [var]
  doc["s1"]      = r3(c.S1);        // fundamental apparent power [VA]
  doc["pf_disp"] = r3(c.PF_disp);   // displacement PF = cos(phi)

  // ── 4) Harmonic distortion metrics ───────────────────────────────────────
  doc["thd_i"] = r3(c.thd_i);       // current THD
  doc["thd_v"] = r3(c.thd_v);       // voltage THD (placeholder in current FW)

  // Harmonics object: map of frequency → ratio
  {
    JsonObject hobj = doc["h_i"].to<JsonObject>();
    for (const auto& kv : h_i) {
      // e.g. kv.first = "120", kv.second = 0.123
      hobj[kv.first] = r3(kv.second);
    }
  }

  // Aggregate odd/even harmonic sums (relative to fundamental)
  doc["odd_sum_i"]  = r3(c.odd_sum_i);   // sum of odd harmonic ratios (k=3,5,...)
  doc["even_sum_i"] = r3(c.even_sum_i);  // sum of even harmonic ratios (k=2,4,...)

  // ── 5) Waveform shape factors ────────────────────────────────────────────
  doc["crest_i"] = r3(c.crest_i);   // crest factor of current
  doc["form_i"]  = r3(c.form_i);    // form factor of current

  // ── 6) State + deltas ────────────────────────────────────────────────────
  doc["state"]  = state;            // "on"/"off" or other state string
  doc["d_irms"] = r3(d_irms);       // ΔIrms since previous frame
  doc["d_p"]    = r3(d_p);          // ΔP since previous frame

  // ── 7) Events array ──────────────────────────────────────────────────────
  /**
   * "events" is an array so that in the future we can include multiple
   * events per frame, but currently we only ever add the single most
   * recent event from latest_event.
   *
   * latest_event is built in main.cpp:
   *   if (fabs(dIrms) > threshold) {
   *     latest_event["type"] = "on"/"off";
   *     latest_event["t_ms"] = now_ms();
   *   }
   */
  {
    JsonArray events = doc["events"].to<JsonArray>();
    if (!latest_event.isNull()) {
      // We add latest_event by value (copy), not by reference.
      events.add(latest_event.as<JsonObject>());
    }
  }

  // ── 8) Health flags ──────────────────────────────────────────────────────
  /**
   * These flags are for self-diagnosis:
   *   - fft_ok : true if harmonic/FFT pipeline is valid.
   *   - sync_ok: true if mains-sync / timing is valid.
   *   - adc_ok : true if ADC/AFE is behaving.
   *
   * In the early firmware, these may all be set to true, but they give
   * us hooks to mark frames as "suspect" in the future.
   */
  doc["fft_ok"]  = fft_ok;
  doc["sync_ok"] = sync_ok;
  doc["adc_ok"]  = adc_ok;

  // ── 9) Firmware & calibration identifiers ────────────────────────────────
  /**
   * fw:
   *  - Tag indicating which firmware build produced this frame.
   *  - Lets backend/ML code adjust expectations if schema / features change.
   *
   * cal_id:
   *  - Identifies which calibration coefficients were used to scale
   *    raw ADC counts into physical units (V, A).
   */
  doc["fw"]     = fw;
  doc["cal_id"] = cal_id;

  // ── 10) Serialize to String and return ───────────────────────────────────
  String out;
  serializeJson(doc, out);
  return out;
}
