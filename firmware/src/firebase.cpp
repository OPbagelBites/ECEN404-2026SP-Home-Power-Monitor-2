/**
 * firebase.cpp
 *
 * This module handles:
 *  - Wi-Fi onboarding using WiFiManager (captive portal / AP mode).
 *  - Posting JSON telemetry frames to Firebase Realtime Database (RTDB)
 *    via HTTPS POST.
 *
 * In the overall system:
 *  - `wifi_init()` is called once at boot from main.cpp to bring the ESP32
 *    online (or into AP setup mode).
 *  - `fb_push_frame()` is called periodically (every N frames) to append
 *    telemetry JSON under:
 *
 *      /devices/<device_id>/streams/<fw_tag>/frames/<push-id>
 *
 * Design goals:
 *  - Keep firmware-side Firebase logic simple and robust (retries, timeouts).
 *  - Use the same URL structure as the Python sandbox so back-end/ML code
 *    doesn't care whether data came from simulation or hardware.
 */

#include "firebase.h"
#include "config.h"

// ─────────────────────────────────────────────────────────────
// Platform-specific Wi-Fi / TLS client includes
// ─────────────────────────────────────────────────────────────
/**
 * We support ESP32 and ESP8266. Each has slightly different Wi-Fi and
 * secure client libraries, so we select headers at compile time.
 */
#if defined(ARDUINO_ARCH_ESP32)
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
#elif defined(ARDUINO_ARCH_ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiClientSecureBearSSL.h>
#endif

#include <HTTPClient.h>
#include <WiFiManager.h>   // tzapu/WiFiManager (captive portal + credential storage)

// ─────────────────────────────────────────────────────────────
// Optional config knobs with defaults
// (These can be overridden in config.h)
// ─────────────────────────────────────────────────────────────

/**
 * CFG_HTTP_TIMEOUT
 *  - Maximum time (ms) to wait for HTTP response when pushing a frame.
 */
#ifndef CFG_HTTP_TIMEOUT
  #define CFG_HTTP_TIMEOUT 5000
#endif

/**
 * CFG_PUSH_ENABLE
 *  - Global switch to enable/disable pushing data to Firebase.
 *  - When set to 0 we compile fb_push_frame() as a no-op (returns true).
 */
#ifndef CFG_PUSH_ENABLE
  #define CFG_PUSH_ENABLE 1
#endif

/**
 * CFG_FB_AUTH
 *  - Optional Firebase RTDB auth token (database secret or custom token).
 *  - If non-empty, it is appended as `?auth=<token>` in the URL.
 */
#ifndef CFG_FB_AUTH
  #define CFG_FB_AUTH ""
#endif

/**
 * CFG_USE_TLS_INSECURE
 *  - If 1, we tell the TLS client to skip certificate verification
 *    (setInsecure). This is convenient for development but less secure.
 *  - If 0 and CFG_FB_ROOT_CA_PEM is defined, we use a CA certificate.
 */
#ifndef CFG_USE_TLS_INSECURE
  #define CFG_USE_TLS_INSECURE 1
#endif

/**
 * HAS_FB_CA
 *  - Internal macro indicating whether a Firebase root CA PEM string
 *    was provided in config.h (CFG_FB_ROOT_CA_PEM).
 */
#ifdef CFG_FB_ROOT_CA_PEM
  #define HAS_FB_CA 1
#else
  #define HAS_FB_CA 0
#endif

/**
 * CFG_HTTP_RETRIES
 *  - Number of times we will retry an HTTP POST on transient failures
 *    (e.g., timeouts, 5xx codes) before giving up.
 */
#ifndef CFG_HTTP_RETRIES
  #define CFG_HTTP_RETRIES 2
#endif

/**
 * CFG_AP_NAME / CFG_AP_PASS
 *  - SSID and password for the WiFiManager captive-portal AP.
 *  - When the device cannot connect to a known Wi-Fi network, it creates
 *    an AP with this name so a user can connect and configure credentials.
 */
#ifndef CFG_AP_NAME
  #define CFG_AP_NAME "HomePower-Setup"
#endif

#ifndef CFG_AP_PASS
  #define CFG_AP_PASS "changeme"
#endif

// ─────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────

/**
 * sanitize_key()
 *
 * Firebase RTDB path segments cannot contain:
 *   '.', '#', '$', '[', ']'
 *
 * This helper replaces those characters with underscores so we can safely
 * move arbitrary device IDs / firmware tags into the RTDB path.
 *
 * Example:
 *   "esp32.dev#1" → "esp32_dev_1"
 */
static String sanitize_key(String s) {
  s.replace(".", "_");
  s.replace("#", "_");
  s.replace("$", "_");
  s.replace("[", "_");
  s.replace("]", "_");
  return s;
}

/**
 * build_frames_url()
 *
 * Constructs the full URL used to POST a new frame to Firebase RTDB.
 *
 * Structure:
 *   <db_url>/devices/<device_id>/streams/<fw_tag>/frames.json?print=silent[&auth=...]
 *
 * Where:
 *  - db_url:         CFG_FB_DB_URL, e.g. "https://myproj-default-rtdb.firebaseio.com"
 *  - device_id:      logical ID of this node (sanitized).
 *  - fw_tag:         firmware tag or stream name (sanitized).
 *  - print=silent:   tells RTDB to not echo the entire object back.
 *  - auth=...:       optional auth token from CFG_FB_AUTH.
 */
static String build_frames_url(const String& db_url,
                               const String& device_id,
                               const String& fw_tag)
{
  // Normalize base URL: strip trailing slash if present
  String base = db_url;
  if (base.endsWith("/")) base.remove(base.length() - 1);

  const String dev = sanitize_key(device_id);
  const String tag = sanitize_key(fw_tag);

  // Target collection:
  //   /devices/<device_id>/streams/<fw_tag>/frames.json
  String u;
  u.reserve(base.length() + 64);  // reserve space to avoid reallocations
  u  = base;
  u += F("/devices/");
  u += dev;
  u += F("/streams/");
  u += tag;
  u += F("/frames.json?print=silent");

  // Optional auth parameter (?auth=<token>)
  if (String(CFG_FB_AUTH).length() > 0) {
    u += F("&auth=");
    u += CFG_FB_AUTH;
  }
  return u;
}

/**
 * wifi_is_connected()
 *
 * Thin wrapper around WiFi.status() so we can easily stub or extend
 * connectivity logic in one place.
 */
static bool wifi_is_connected() {
  return WiFi.status() == WL_CONNECTED;
}

// ─────────────────────────────────────────────────────────────
// Public API: Wi-Fi initialization
// ─────────────────────────────────────────────────────────────

/**
 * wifi_init()
 *
 * Purpose:
 *  - Bring the ESP32/ESP8266 online using WiFiManager.
 *  - If previously saved credentials work, it connects automatically.
 *  - If connection fails, the device spawns an AP (CFG_AP_NAME/CFG_AP_PASS)
 *    and starts a captive portal so the user can configure Wi-Fi.
 *
 * Behavior:
 *  - WiFi is configured in station mode (WIFI_STA).
 *  - WiFi sleep is disabled for more reliable uploads.
 *  - Returns true only if we end up with WL_CONNECTED.
 *
 * Usage (from setup()):
 *   if (wifi_init()) {
 *     // connected, can push to Firebase
 *   } else {
 *     // still in AP / setup mode, no internet
 *   }
 */
bool wifi_init() {
  // Put Wi-Fi hardware in station mode (client)
  WiFi.mode(WIFI_STA);

  // Disable Wi-Fi power-save / sleep for more reliable connections
  WiFi.setSleep(false);

  WiFiManager wm;

  // Note: if we ever want to force "forget" of stored credentials,
  // we can call wm.resetSettings() here.
  // wm.resetSettings();

  // autoConnect():
  //  - Tries stored Wi-Fi credentials first.
  //  - If it fails, starts an AP with CFG_AP_NAME/CFG_AP_PASS and hosts a web portal.
  bool ok = wm.autoConnect(CFG_AP_NAME, CFG_AP_PASS);

  // We consider Wi-Fi initialization successful only if we are connected.
  return ok && (WiFi.status() == WL_CONNECTED);
}

// ─────────────────────────────────────────────────────────────
// Public API: Firebase Push
// ─────────────────────────────────────────────────────────────

/**
 * fb_push_frame()
 *
 * Purpose:
 *  - Append a single telemetry frame as JSON to Firebase Realtime Database.
 *
 * Arguments:
 *  - device_id:   logical ID for this device (maps to /devices/<device_id>).
 *  - fw_tag:      firmware tag or stream identifier (maps to /streams/<fw_tag>).
 *  - frame_id:    numeric ID for the frame (not directly used in RTDB path,
 *                 but useful for logging and debugging).
 *  - jsonPayload: the JSON string for the frame (already assembled upstream).
 *
 * Behavior:
 *  - If CFG_PUSH_ENABLE == 0, function is compiled as a no-op and returns true.
 *  - If Wi-Fi isn't connected, returns false immediately.
 *  - Builds a POST URL for the `frames` collection and sends the JSON.
 *  - Uses HTTPS with either:
 *      • setInsecure() (no cert checking) for dev, or
 *      • a root CA cert from CFG_FB_ROOT_CA_PEM if provided.
 *  - Retries up to CFG_HTTP_RETRIES times on transient errors (timeouts, 5xx).
 *  - Never retries on 401/403, since those indicate auth/rules misconfig.
 *
 * Return:
 *  - true  on success (HTTP 2xx).
 *  - false on failure (HTTP error, Wi-Fi down, auth problem, etc.).
 */
bool fb_push_frame(const String& device_id,
                   const String& fw_tag,
                   uint32_t frame_id,
                   const String& jsonPayload)
{
  // If pushing is globally disabled at compile time, do nothing but pretend success.
#if !CFG_PUSH_ENABLE
  (void)device_id; (void)fw_tag; (void)frame_id; (void)jsonPayload;
  return true;
#endif

  // Connectivity prerequisite: must have an active Wi-Fi connection.
  if (!wifi_is_connected()) return false;

  /**
   * Expect from config.h:
   *   #define CFG_FB_DB_URL "https://<project-id>-default-rtdb.firebaseio.com"
   *
   * If this macro is missing, fail at compile time so the firmware doesn't
   * silently POST to nowhere.
   */
#ifndef CFG_FB_DB_URL
  #error "CFG_FB_DB_URL must be defined in config.h"
#endif

  // Construct the append-only URL for the frames collection.
  const String url = build_frames_url(String(CFG_FB_DB_URL), device_id, fw_tag);

  // ── Prepare TLS client (platform-specific) ────────────────────────────────
#if defined(ARDUINO_ARCH_ESP32)
  WiFiClientSecure client;
  #if CFG_USE_TLS_INSECURE
    // Development mode: accept any certificate (not recommended for production).
    client.setInsecure();
  #else
    #if HAS_FB_CA
      // Production mode: verify server against provided root CA certificate.
      client.setCACert(CFG_FB_ROOT_CA_PEM);
    #else
      // If no CA given but TLS is requested, fall back to insecure as last resort.
      client.setInsecure();
    #endif
  #endif

#elif defined(ARDUINO_ARCH_ESP8266)
  BearSSL::WiFiClientSecure client;
  #if CFG_USE_TLS_INSECURE
    client.setInsecure();
  #else
    #if HAS_FB_CA
      // On ESP8266, CA certs are wrapped as X509List objects.
      client.setTrustAnchors(new BearSSL::X509List(CFG_FB_ROOT_CA_PEM));
    #else
      client.setInsecure();
    #endif
  #endif

#else
  // Fallback for non-ESP targets (no TLS). This is not recommended for real deployments.
  WiFiClient client;
#endif

  HTTPClient http;
  int attempt = 0;

  // Retry loop for transient errors.
  while (true) {
    // Initialize HTTPClient with our secure client and target URL.
    if (!http.begin(client, url)) {
      // If begin() fails, we can't even open the connection.
      http.end();
      return false;
    }

    http.setTimeout(CFG_HTTP_TIMEOUT);
    http.addHeader(F("Content-Type"), F("application/json"));

    // POST → RTDB creates a new child under "frames" with a unique push ID.
    const int code = http.POST(jsonPayload);

    // We're done with this request (regardless of success or failure).
    http.end();

    // HTTP 2xx → success
    //
    // Firebase RTDB usually returns:
    //   - 200 with a small JSON body: { "name": "<push-id>" }
    //   - 204 (No Content) in some configurations.
    if (code >= 200 && code < 300) {
      // For quieter output we don't log success by default.
      // Uncomment for debugging:
      // Serial.printf("[fb] POST ok: %d (frame_id=%lu)\n", code, (unsigned long)frame_id);
      return true;
    }

    // Unauthorized or forbidden → likely misconfigured auth token or DB rules.
    // Retrying won't help, so we log and bail.
    if (code == 401 || code == 403) {
      Serial.printf("[fb] auth error: HTTP %d (check CFG_FB_AUTH / rules)\n", code);
      return false;
    }

    // If we've exhausted retries or got a hard client error, give up.
    if (attempt >= CFG_HTTP_RETRIES) {
      Serial.printf("[fb] HTTP POST failed: %d (url=%s)\n", code, url.c_str());
      return false;
    }

    // Otherwise, treat as transient (e.g., timeout, 5xx).
    // Increment attempt counter and back off briefly before retrying.
    ++attempt;
    const uint32_t backoff_ms = 150u * attempt; // first retry: 150 ms, then 300 ms, etc.
    delay(backoff_ms);
  }
}
