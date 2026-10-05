# PipiTank-Sensortest

Serieller Funktionstest fuer die zwei XKC-Y25-NPN-Sensoren gemaess
`Urintank-Schaltplan.pdf`.

## Belegung

| Signal | Anschluss |
| --- | --- |
| Sensor 1 OUT, gelb | ESP32 GPIO 18 |
| Sensor 2 OUT, gelb | ESP32 GPIO 19 |
| Beide Sensoren GND, blau | Gemeinsame Masse mit ESP32 |
| Beide Sensoren VCC, braun | 12-V-Versorgung gemaess Schaltplan |
| Mode, schwarz | Nicht angeschlossen gemaess Schaltplan |

Die gelben NPN-Open-Collector-Ausgaenge werden mit `INPUT_PULLUP` gelesen.
LOW bedeutet Sensor erkannt, HIGH bedeutet nicht erkannt. Die 12-V-Versorgung
nicht mit einem ESP32-GPIO verbinden; nur die OUT-Leitung geht an den GPIO.

## Testen

Im Ordner `PipiTank` ausfuehren:

```sh
pio run
pio run --target upload
pio device monitor
```

Der serielle Monitor laeuft mit 115200 Baud und gibt beide Pegel zweimal pro
Sekunde samt Rohwert aus.