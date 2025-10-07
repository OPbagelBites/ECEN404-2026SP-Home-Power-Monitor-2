#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("✅ ESP32 toolchain OK");
}

void loop() {
  Serial.println("Heartbeat...");
  delay(1000);
}