/*
 * Frischwasser-Node -- Sensor-Test (v1)
 * -------------------------------------------------------------
 * Liest die 3 XKC-Y25-V-Fuellstandssensoren ueber ihre
 * Spannungsteiler an GPIO4/16/17 und gibt den Zustand aus:
 *  - per Serial (alle 1s, fuers Testen mit USB-Kabel direkt am Tank)
 *  - per HTTP (GET / als Klartext, GET /status als JSON) -- kein
 *    USB-Kabel noetig, sobald der Node im WLAN haengt
 *
 * ANNAHME (noch nicht verifiziert): Die drei Sensoren haengen an
 * GPIO4, GPIO16 und GPIO17 -- so im Schaltplan vorgeschlagen. Falls
 * beim Verkabeln andere Pins verwendet wurden, unten bei
 * PIN_SENSOR1/2/3 anpassen.
 *
 * ANNAHME: Schwarze Ader (Mode) aller 3 Sensoren liegt an VCC
 * ("high bei Erkennung", wie im Schaltplan empfohlen). Falls
 * stattdessen an GND verdrahtet, in readSensors() die Logik
 * invertieren (HIGH <-> LOW).
 *
 * ANNAHME: WLAN-Zugangsdaten wie beim Luefter-Node uebernommen
 * (gleiches Fahrzeug-Netz) -- bei Bedarf unten anpassen.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

const char* WIFI_SSID     = "Travelingualizer";
const char* WIFI_PASSWORD = "L1@n3P3t3r";

// ---------- Pin-Belegung (ANNAHME, siehe Kommentar oben) ----------
#define PIN_SENSOR1 4   // Sensor 1
#define PIN_SENSOR2 16  // Sensor 2
#define PIN_SENSOR3 17  // Sensor 3

WebServer server(80);

bool sensor1Erkannt = false;
bool sensor2Erkannt = false;
bool sensor3Erkannt = false;

// Liest alle 3 Sensoren neu ein. HIGH = Fluessigkeit erkannt (siehe
// ANNAHME oben zur Mode-Ader-Verdrahtung).
void readSensors() {
  sensor1Erkannt = digitalRead(PIN_SENSOR1) == HIGH;
  sensor2Erkannt = digitalRead(PIN_SENSOR2) == HIGH;
  sensor3Erkannt = digitalRead(PIN_SENSOR3) == HIGH;
}

void handleStatus() {
  readSensors();
  StaticJsonDocument<200> doc;
  doc["sensor1"] = sensor1Erkannt;
  doc["sensor2"] = sensor2Erkannt;
  doc["sensor3"] = sensor3Erkannt;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleRoot() {
  readSensors();
  char buf[300];
  snprintf(buf, sizeof(buf),
    "Frischwasser-Node -- Sensor-Test\n\n"
    "Sensor 1 (GPIO%d): %s\n"
    "Sensor 2 (GPIO%d): %s\n"
    "Sensor 3 (GPIO%d): %s\n",
    PIN_SENSOR1, sensor1Erkannt ? "ERKANNT" : "---",
    PIN_SENSOR2, sensor2Erkannt ? "ERKANNT" : "---",
    PIN_SENSOR3, sensor3Erkannt ? "ERKANNT" : "---");
  server.send(200, "text/plain", buf);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Frischwasser-Node: Sensor-Test ===");

  // Kein INPUT_PULLUP noetig: die V-Sensoren treiben ihren Ausgang
  // aktiv (Push-Pull) ueber den Spannungsteiler, anders als die
  // NPN-Sensoren beim Luefter-Node-Muster.
  pinMode(PIN_SENSOR1, INPUT);
  pinMode(PIN_SENSOR2, INPUT);
  pinMode(PIN_SENSOR3, INPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Verbinde mit WLAN");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Verbunden! IP-Adresse: ");
    Serial.println(WiFi.localIP());
    Serial.print("MAC-Adresse (fuer DHCP-Reservierung im Router): ");
    Serial.println(WiFi.macAddress());
  } else {
    Serial.println("WLAN-Verbindung fehlgeschlagen -- Sensor-Test");
    Serial.println("laeuft trotzdem lokal per Serial-Monitor weiter.");
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("HTTP-Server gestartet: http://<IP>/  und  http://<IP>/status (JSON)");
}

void loop() {
  server.handleClient();

  // Einmal pro Sekunde den aktuellen Zustand auf die Serial-Konsole
  // schreiben -- praktisch beim Anbringen der Sensoren am Tank, um
  // sofort zu sehen, ob ein Sensor beim Erreichen des Wasserspiegels
  // sauber umschaltet.
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    readSensors();
    Serial.printf("Sensor1=%s  Sensor2=%s  Sensor3=%s\n",
                  sensor1Erkannt ? "ERKANNT" : "---",
                  sensor2Erkannt ? "ERKANNT" : "---",
                  sensor3Erkannt ? "ERKANNT" : "---");
  }
}
