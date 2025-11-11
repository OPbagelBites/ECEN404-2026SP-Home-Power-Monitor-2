#pragma once
#include <Arduino.h>

bool wifi_init();
bool fb_push_frame(const String& device_id,
                   const String& fw_tag,
                   uint32_t frame_id,
                   const String& jsonPayload);
