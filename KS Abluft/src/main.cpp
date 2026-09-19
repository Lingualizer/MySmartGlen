/*
 * Lüfter-Node: Abluft-Temperaturüberwachung + Lüftersteuerung
 * -------------------------------------------------------------
 * - 1x SHT31 (I2C) am Abluft/Lüftungsgitter (ersetzt DS18B20)
 * - Lüfter über BC547 NPN-Transistor an GPIO5 (active-HIGH)
 * - Hysterese-Schwellwerte in NVS Preferences gespeichert,
 *   per HTTP /config änderbar
 * - WLAN: feste Zugangsdaten (Variante A)
 * - HTTP-API: GET /status liefert aktuellen Zustand als JSON
 *
 * Sensorwechsel DS18B20 -> SHT31 (I2C statt 1-Wire):
 * - Benötigt Library "Adafruit SHT31 Library" (+ Abhängigkeit
 *   "Adafruit BusIO"), siehe lib_deps in platformio.ini.
 * - GPIO4 (ehem. 1-Wire-Bus) ist aktuell frei; laut Original-Kommentar
 *   ggf. später für weitere DS18B20 (Kühlschrank/Gefrierfach) reserviert.
 * - SHT31 liefert zusätzlich Luftfeuchtigkeit; diese wird hier
 *   ergänzend im JSON mit ausgegeben (Erweiterung ggü. Original,
 *   nicht Teil der ursprünglichen Anforderung).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include "Adafruit_SHT31.h"
#include <ArduinoJson.h>

// ---------- WLAN Zugangsdaten (Variante A: fest im Code) ----------
const char* WIFI_SSID     = "Travelingualizer";
const char* WIFI_PASSWORD = "L1@n3P3t3r";

// ---------- Pin-Belegung ----------
#define I2C_SDA        21   // SHT31 Datenleitung (I2C)
#define I2C_SCL        22   // SHT31 Taktleitung (I2C)
#define FAN_PIN        5    // BC547 Basis über 1kOhm, active-HIGH

#define SHT31_I2C_ADDR 0x44 // Standardadresse; 0x45 falls ADR-Pin auf HIGH

// ---------- Hysterese Default-Werte (nur falls NVS leer) ----------
// ANNAHME, nicht validiert: Startwerte, bis reale Messwerte
// (Oberflächentemperatur unter Sonneneinstrahlung) vorliegen.
#define DEFAULT_TEMP_ON   45.0f   // Lüfter EIN ab dieser Temperatur
#define DEFAULT_TEMP_OFF  35.0f   // Lüfter AUS unterhalb dieser Temperatur

// ---------- Globale Objekte ----------
Adafruit_SHT31 sht31 = Adafruit_SHT31();
WebServer server(80);
Preferences prefs;

float tempOn  = DEFAULT_TEMP_ON;
float tempOff = DEFAULT_TEMP_OFF;
float currentTemp = NAN;
float currentHumidity = NAN;
bool fanState = false;
bool sensorOk = false; // wird in setup() gesetzt, falls SHT31 beim Start gefunden wurde

unsigned long lastMeasurement = 0;
const unsigned long MEASURE_INTERVAL_MS = 5000; // alle 5s messen

// ---------- Hysterese-Werte aus NVS laden ----------
void loadThresholds() {
  prefs.begin("luefter", true); // read-only
  tempOn  = prefs.getFloat("temp_on",  DEFAULT_TEMP_ON);
  tempOff = prefs.getFloat("temp_off", DEFAULT_TEMP_OFF);
  prefs.end();
}

// ---------- Hysterese-Werte in NVS speichern ----------
void saveThresholds(float onVal, float offVal) {
  prefs.begin("luefter", false); // read-write
  prefs.putFloat("temp_on", onVal);
  prefs.putFloat("temp_off", offVal);
  prefs.end();
}

// ---------- Temperatur/Feuchte messen + Lüfterlogik ----------
void updateTemperatureAndFan() {
  float t = sht31.readTemperature();
  float h = sht31.readHumidity();

  if (isnan(t) || isnan(h)) {
    // Sensor nicht erreichbar - Lüfter aus Sicherheitsgründen NICHT
    // automatisch abschalten, aber Fehler klar signalisieren.
    Serial.println("Fehler: SHT31 nicht erreichbar!");
    return;
  }

  currentTemp = t;
  currentHumidity = h;

  // Hysterese: EIN bei Überschreiten tempOn, AUS bei Unterschreiten tempOff
  if (!fanState && currentTemp >= tempOn) {
    fanState = true;
    digitalWrite(FAN_PIN, HIGH);
    Serial.println("Lüfter EIN");
  } else if (fanState && currentTemp <= tempOff) {
    fanState = false;
    digitalWrite(FAN_PIN, LOW);
    Serial.println("Lüfter AUS");
  }
}

// ---------- HTTP: GET /status ----------
void handleStatus() {
  StaticJsonDocument<256> doc;
  if (isnan(currentTemp)) {
    doc["abluft_temp"] = nullptr;
  } else {
    doc["abluft_temp"] = (double)currentTemp;
  };
  if (isnan(currentHumidity)) {
    doc["abluft_humidity"] = nullptr;
  } else {
    doc["abluft_humidity"] = (double)currentHumidity;
  };
  doc["fan_state"] = fanState;
  doc["temp_on"] = tempOn;
  doc["temp_off"] = tempOff;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// ---------- HTTP: GET/POST /config ----------
// Aufruf zum Ändern: /config?temp_on=45.0&temp_off=35.0
void handleConfig() {
  bool changed = false;

  if (server.hasArg("temp_on")) {
    tempOn = server.arg("temp_on").toFloat();
    changed = true;
  }
  if (server.hasArg("temp_off")) {
    tempOff = server.arg("temp_off").toFloat();
    changed = true;
  }

  if (changed) {
    if (tempOff >= tempOn) {
      server.send(400, "application/json",
        "{\"error\":\"temp_off muss kleiner als temp_on sein\"}");
      return;
    }
    saveThresholds(tempOn, tempOff);
  }

  StaticJsonDocument<128> doc;
  doc["temp_on"] = tempOn;
  doc["temp_off"] = tempOff;
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);

  loadThresholds();

  Wire.begin(I2C_SDA, I2C_SCL);
  sensorOk = sht31.begin(SHT31_I2C_ADDR);
  if (!sensorOk) {
    Serial.println("Fehler: SHT31 beim Start nicht gefunden! Pruefe Verkabelung/I2C-Adresse.");
  }

  // WLAN verbinden (feste Zugangsdaten)
  WiFi.mode(WIFI_STA);
  WiFi.begin("Travelingualizer", "L1@n3P3t3r");
  Serial.print("Verbinde mit WLAN");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Verbunden. IP-Adresse: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WLAN-Verbindung fehlgeschlagen - Lüftersteuerung läuft trotzdem autonom weiter.");
  }

  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/config", HTTP_POST, handleConfig);
  server.begin();

  // Erste Messung sofort
  updateTemperatureAndFan();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastMeasurement >= MEASURE_INTERVAL_MS) {
    lastMeasurement = now;
    updateTemperatureAndFan();
  }
}
