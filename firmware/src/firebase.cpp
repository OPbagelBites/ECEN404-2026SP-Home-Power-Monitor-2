#include "firebase.h"
#include "config.h"

#if defined(ARDUINO_ARCH_ESP32)
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
#elif defined(ARDUINO_ARCH_ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiClientSecureBearSSL.h>
#endif

#include <HTTPClient.h>
#include <WiFiManager.h>   // tzapu/WiFiManager

// ─────────────────────────────────────────────────────────────────────────────
// Optional config knobs expected from config.h (provide sane defaults if absent)

#ifndef CFG_HTTP_TIMEOUT
  #define CFG_HTTP_TIMEOUT 5000
#endif

#ifndef CFG_PUSH_ENABLE
  #define CFG_PUSH_ENABLE 1
#endif

// If your RTDB requires auth, put a database secret or custom token here.
// In config.h:  #define CFG_FB_AUTH "your_token"
// Leave undefined or empty to omit the auth param.
#ifndef CFG_FB_AUTH
  #define CFG_FB_AUTH ""
#endif

// Set to 1 to use client.setInsecure() (dev/demo).
// In production, prefer a real root CA (CFG_FB_ROOT_CA_PEM).
#ifndef CFG_USE_TLS_INSECURE
  #define CFG_USE_TLS_INSECURE 1
#endif

// If you want proper cert pinning, define CFG_FB_ROOT_CA_PEM in config.h:
// static const char CFG_FB_ROOT_CA_PEM[] PROGMEM = R"PEM(
// -----BEGIN CERTIFICATE-----
// ...
// -----END CERTIFICATE-----
// )PEM";
// Then set CFG_USE_TLS_INSECURE to 0.
#ifdef CFG_FB_ROOT_CA_PEM
  #define HAS_FB_CA 1
#else
  #define HAS_FB_CA 0
#endif

// Small retry budget for transient failures (5xx, timeouts).
#ifndef CFG_HTTP_RETRIES
  #define CFG_HTTP_RETRIES 2
#endif

#ifndef CFG_AP_NAME
  #define CFG_AP_NAME "HomePower-Setup"
#endif

#ifndef CFG_AP_PASS
  #define CFG_AP_PASS "changeme"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Helpers

static String build_url(const String& db_url,
                        const String& device_id,
                        const String& fw_tag,
                        uint32_t frame_id)
{
  // Normalize base URL (no trailing slash)
  String base = db_url;
  if (base.endsWith("/")) base.remove(base.length() - 1);

  // /devices/<device_id>/streams/<fw_tag>/frames/<frame_id>.json
  String u;
  u.reserve(base.length() + 64);
  u  = base;
  u += F("/devices/");
  u += device_id;
  u += F("/streams/");
  u += fw_tag;
  u += F("/frames/");
  u += String(frame_id);
  u += F(".json?print=silent");

  // Optional auth
  if (String(CFG_FB_AUTH).length() > 0) {
    u += F("&auth=");
    u += CFG_FB_AUTH;
  }
  return u;
}

static bool wifi_is_connected() {
  return WiFi.status() == WL_CONNECTED;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API

bool wifi_init() {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  // wm.resetSettings();  // ← add this
  bool ok = wm.autoConnect(CFG_AP_NAME, CFG_AP_PASS);
  return ok && (WiFi.status() == WL_CONNECTED);
}

bool fb_push_frame(const String& device_id,
                   const String& fw_tag,
                   uint32_t frame_id,
                   const String& jsonPayload)
{
#if !CFG_PUSH_ENABLE
  (void)device_id; (void)fw_tag; (void)frame_id; (void)jsonPayload;
  return true;
#endif

  if (!wifi_is_connected()) return false;

  // Expect these from config.h:
  //   #define CFG_FB_DB_URL "https://<project-id>-default-rtdb.firebaseio.com"
#ifndef CFG_FB_DB_URL
  #error "CFG_FB_DB_URL must be defined in config.h"
#endif

  String url = build_url(String(CFG_FB_DB_URL), device_id, fw_tag, frame_id);

  // Prepare TLS client
#if defined(ARDUINO_ARCH_ESP32)
  WiFiClientSecure client;
  #if CFG_USE_TLS_INSECURE
    client.setInsecure();
  #else
    #if HAS_FB_CA
      client.setCACert(CFG_FB_ROOT_CA_PEM);
    #else
      // Fallback to insecure if no CA provided (not recommended for prod)
      client.setInsecure();
    #endif
  #endif
#elif defined(ARDUINO_ARCH_ESP8266)
  BearSSL::WiFiClientSecure client;
  #if CFG_USE_TLS_INSECURE
    client.setInsecure();
  #else
    #if HAS_FB_CA
      client.setTrustAnchors(new BearSSL::X509List(CFG_FB_ROOT_CA_PEM));
    #else
      client.setInsecure();
    #endif
  #endif
#else
  WiFiClient client; // non-TLS fallback (not recommended)
#endif

  HTTPClient http;
  int attempt = 0;
  while (true) {
    if (!http.begin(client, url)) {
      http.end();
      return false;
    }

    http.setTimeout(CFG_HTTP_TIMEOUT);
    http.addHeader(F("Content-Type"), F("application/json"));

    // PUT creates/replaces this frame node. (PATCH would also work.)
    const int code = http.PUT(jsonPayload);
    http.end();

    // Success
    if (code >= 200 && code < 300) return true;

    // Unauthorized / forbidden → don't retry blindly
    if (code == 401 || code == 403) {
      Serial.printf("[fb] auth error: HTTP %d (check CFG_FB_AUTH)\n", code);
      return false;
    }

    // If we've used up retries or it's a hard client error, bail
    if (attempt >= CFG_HTTP_RETRIES) {
      Serial.printf("[fb] HTTP PUT failed: %d (url=%s)\n", code, url.c_str());
      return false;
    }

    // Brief backoff for transient issues (timeouts, 5xx, etc.)
    ++attempt;
    const uint32_t backoff_ms = 150u * attempt; // 150ms, 300ms, ...
    delay(backoff_ms);
  }
}
