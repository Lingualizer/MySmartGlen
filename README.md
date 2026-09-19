## Hardware-Übersicht

### Controller & Module
| 🟡 Testaufbau | 🟢 Verbaut | 🔴 Geplant |
| Node ID | Board / Chip | Standby / Load | Bus / IP | Einbauort | Status | Link |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `Display` | ESP32-S3 (4" Touch) | ~150mA | 192.168.1.220 | Sitzgruppe | 🟡 |  |
| `KS Abluft` | ESP32 NodeMCU | ~80mA | 192.168.1.228 | Hinter Kühlschrank-Gitter | 🟡 | https://www.amazon.de/dp/B0FRF24HL9 |
| `Frischschwasser` | ESP32 NodeMCU | ~50mA | 192.168.1.197 | Sitzbank / Tankbereich | 🟡 | https://www.amazon.de/dp/B0FRF24HL9 |

### Verbaute Sensoren & Aktoren
| Komponente | Typ / Modell | Angeschlossen an | Signal / Protokoll | Pins (ESP32) |
| :--- | :--- | :--- | :--- | :--- |
| Abluft-Sensor | SHT31 | `esp-fridge` | OneWire | GPIO 4 |
| Innen-Temp. | ? | `esp-fridge` | OneWire | GPIO 4 (Bus) |
| Gefrierfach-Temp. | ? | `esp-fridge` | OneWire | GPIO 4 (Bus) |
| Abluft-Lüfter | 12V Noctua PWM | `esp-fridge` | PWM / MOSFET | GPIO 16 (PWM), GPIO 17 (Tacho) |
| Urintank Level | XKC-Y25-V (Kapazitiv) | `esp-tanks` | Digital (High/Low) | GPIO 18 |
| Frischwasser | 10-Stufen-Sonde | `esp-tanks` | Analog / ADC | GPIO 34 |