#include <Arduino.h>

namespace {

constexpr uint8_t PIN_SENSOR_1 = 18;
constexpr uint8_t PIN_SENSOR_2 = 19;
constexpr unsigned long REPORT_INTERVAL_MS = 500;

void printSensorState(const char *name, uint8_t pin) {
  const int rawLevel = digitalRead(pin);
  const bool detected = rawLevel == LOW;

  Serial.printf("%s (GPIO %u): %-9s raw=%s\n",
                name,
                pin,
                detected ? "ERKANNT" : "NICHT ERKANNT",
                rawLevel == LOW ? "LOW" : "HIGH");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SENSOR_1, INPUT_PULLUP);
  pinMode(PIN_SENSOR_2, INPUT_PULLUP);

  Serial.println();
  Serial.println("=== PipiTank: XKC-Y25-NPN Sensortest ===");
  Serial.println("Erkennung = LOW; frei = HIGH");
  Serial.println("Mode-Adern bleiben gemaess Schaltplan unbeschaltet.");
}

void loop() {
  static unsigned long lastReport = 0;
  const unsigned long now = millis();
  if (now - lastReport < REPORT_INTERVAL_MS) {
    return;
  }
  lastReport = now;

  printSensorState("Sensor 1", PIN_SENSOR_1);
  printSensorState("Sensor 2", PIN_SENSOR_2);
  Serial.println();
}