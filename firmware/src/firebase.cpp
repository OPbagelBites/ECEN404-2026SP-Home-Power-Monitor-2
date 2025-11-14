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

// ─────────────────────────────────────────────────────────────
// Optional config knobs (with defaults) expected from config.h

#ifndef CFG_HTTP_TIMEOUT
  #define CFG_HTTP_TIMEOUT 5000
#endif

#ifndef CFG_PUSH_ENABLE
  #define CFG_PUSH_ENABLE 1
#endif

#ifndef CFG_FB_AUTH
  #define CFG_FB_AUTH ""
#endif

#ifndef CFG_USE_TLS_INSECURE
  #define CFG_USE_TLS_INSECURE 1
#endif

#ifdef CFG_FB_ROOT_CA_PEM
  #define HAS_FB_CA 1
#else
  #define HAS_FB_CA 0
#endif

#ifndef CFG_HTTP_RETRIES
  #define CFG_HTTP_RETRIES 2
#endif

#ifndef CFG_AP_NAME
  #define CFG_AP_NAME "HomePower-Setup"
#endif

#ifndef CFG_AP_PASS
  #define CFG_AP_PASS "changeme"
#endif

// ─────────────────────────────────────────────────────────────
// Helpers

// RTDB disallows '.', '#', '$', '[', ']'
static String sanitize_key(String s) {
  s.replace(".", "_");
  s.replace("#", "_");
  s.replace("$", "_");
  s.replace("[", "_");
  s.replace("]", "_");
  return s;
}

// Build the frames collection URL for POST (append)
static String build_frames_url(const String& db_url,
                               const String& device_id,
                               const String& fw_tag)
{
  // Normalize base URL (no trailing slash)
  String base = db_url;
  if (base.endsWith("/")) base.remove(base.length() - 1);

  const String dev = sanitize_key(device_id);
  const String tag = sanitize_key(fw_tag);

  // /devices/<device_id>/streams/<fw_tag>/frames.json
  String u;
  u.reserve(base.length() + 64);
  u  = base;
  u += F("/devices/");
  u += dev;
  u += F("/streams/");
  u += tag;
  u += F("/frames.json?print=silent");

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

// ─────────────────────────────────────────────────────────────
// Public API

bool wifi_init() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // keep Wi-Fi awake for more reliable uploads

  WiFiManager wm;
  // If you ever need to forget saved Wi-Fi:
  // wm.resetSettings();

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

  // Expect from config.h:
  //   #define CFG_FB_DB_URL "https://<project-id>-default-rtdb.firebaseio.com"
#ifndef CFG_FB_DB_URL
  #error "CFG_FB_DB_URL must be defined in config.h"
#endif

  // Append-only URL (frames collection)
  const String url = build_frames_url(String(CFG_FB_DB_URL), device_id, fw_tag);

  // Prepare TLS client
#if defined(ARDUINO_ARCH_ESP32)
  WiFiClientSecure client;
  #if CFG_USE_TLS_INSECURE
    client.setInsecure();
  #else
    #if HAS_FB_CA
      client.setCACert(CFG_FB_ROOT_CA_PEM);
    #else
      client.setInsecure();  // fallback (OK for dev)
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

    // POST → append new child under frames (unique push key)
    const int code = http.POST(jsonPayload);
    http.end();

    // Success (RTDB usually returns 200 or 204 on success, 200 with JSON body containing the name)
    if (code >= 200 && code < 300) {
      // Uncomment if you want to see successes in Serial:
      // Serial.printf("[fb] POST ok: %d (frame_id=%lu)\n", code, (unsigned long)frame_id);
      return true;
    }

    // Unauthorized / forbidden → don't retry blindly
    if (code == 401 || code == 403) {
      Serial.printf("[fb] auth error: HTTP %d (check CFG_FB_AUTH / rules)\n", code);
      return false;
    }

    // If we've used up retries or it's a hard client error, bail
    if (attempt >= CFG_HTTP_RETRIES) {
      Serial.printf("[fb] HTTP POST failed: %d (url=%s)\n", code, url.c_str());
      return false;
    }

    // Brief backoff for transient issues (timeouts, 5xx, etc.)
    ++attempt;
    const uint32_t backoff_ms = 150u * attempt; // 150ms, 300ms, ...
    delay(backoff_ms);
  }
}