/*
  ============================================================
  MOTORHOME HOMESCREEN v3 -- Echte WLAN-Konfiguration
  ============================================================

  NEU gegenueber v2:
  - Settings: "WLAN" oeffnet echte Netzwerksuche
  - Liste gefundener Netzwerke antippen -> Passwort-Eingabe
    mit Bildschirmtastatur -> Verbinden
  - Erfolgreiche Zugangsdaten werden im Flash gespeichert
    (Preferences/NVS) und beim naechsten Start automatisch
    wiederverwendet (Auto-Reconnect)
  - Status-Icon oben links zeigt Verbindungsstatus:
    grau=getrennt, gelb=verbindet, gruen=verbunden

  NEU (25.08.2026): Frischwasser-Kachel zeigt jetzt den echten
  Fuellstand vom Frischwasser-Node (3 Schwellwert-Sensoren per
  HTTP/WLAN), statt eines statischen Platzhalters. Siehe Abschnitt
  "Frischwasser-Node" weiter unten.

  ANNAHME: Datum/Uhrzeit bleibt vorerst die manuelle Software-
  Uhr aus v2 -- echte NTP-Synchronisation ueber das jetzt
  vorhandene WLAN waere der naechste sinnvolle Ausbauschritt,
  aber bewusst noch nicht Teil dieser Aenderung.
  ============================================================
*/

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <Wire.h>

// Eigene Schriftarten mit deutschen Umlauten (Ä/Ö/Ü/ä/ö/ü/ß),
// erstellt via lv_font_conv, ersetzen die eingebauten
// lv_font_montserrat_14/20 (die nur ASCII enthalten).
LV_FONT_DECLARE(lv_font_montserrat_14_de);
LV_FONT_DECLARE(lv_font_montserrat_20_de);

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[480 * 40];

// Kalibrierung als lineare Formel (Steigung + Achsenabschnitt)
float slopeY = -479.0f / (3850 - 200);
float interceptY = 479.0f - slopeY * 200;
float slopeX = -319.0f / (3750 - (-50));
float interceptX = 319.0f - slopeX * (-50);
const uint8_t TOUCH_Z_THRESHOLD = 25; // erhoeht von 15 -- Rauschen erreichte bis zu z=14

// Display-Ausrichtung: false = normal (USB rechts), true = um 180 Grad
// gedreht (USB links). Wird dauerhaft gespeichert (Preferences/NVS).
bool displayFlipped = false;

// DIAGNOSE-MODUS: Zum Herausfinden, welcher TFT_eSPI-Rotationswert
// bei diesem Treiber tatsaechlich einer sauberen 180-Grad-Drehung
// entspricht (0-3 durchprobieren). Sobald bekannt, wird das oben
// auf einen festen Wert reduziert.
int rotationTestIndex = 1;

const int BACKLIGHT_PIN = 27;

lv_obj_t *scr_home;
lv_obj_t *scr_vedirect_detail;
lv_obj_t *scr_settings;
lv_obj_t *scr_calibration;
lv_obj_t *scr_wifi_list;
lv_obj_t *scr_wifi_password;
lv_obj_t *scr_kuehlschrank;

// VE.Direct UI-Elemente (vorab deklariert, da build_home_screen()
// schon vor der eigentlichen VE.Direct-Sektion im Code steht)
lv_obj_t *tile_solar_value = NULL;
lv_obj_t *ve_solarleistung_val = NULL;
lv_obj_t *ve_batteriespannung_val = NULL;
lv_obj_t *ve_ladestrom_val = NULL;
lv_obj_t *ve_tagesertrag_val = NULL;
lv_obj_t *ve_ladezustand_val = NULL;

// SHT31 (RT/H) -- ebenfalls vorab deklariert
lv_obj_t *tile_rth_value = NULL;

// KS Abluft -- Temperatur/Feuchte vom Lüfter-Node (ebenfalls vorab
// deklariert, siehe Abschnitt "Lüfter-Node" weiter unten)
lv_obj_t *tile_ksabluft_value = NULL;
lv_obj_t *tile_ksabluft_progress = NULL; // schmaler Balken: Zeit bis zur naechsten Abfrage

// Frischwasser -- Fuellstand vom Frischwasser-Node (ebenfalls vorab
// deklariert, siehe Abschnitt "Frischwasser-Node" weiter unten)
lv_obj_t *tile_frischwasser_value = NULL;
lv_obj_t *tile_frischwasser_fill = NULL; // dynamisch aktualisierter Fuellstands-Hintergrund

// Batterie -- Fuellstand (SOC) vom Victron Shunt (VE.Direct, UART1,
// GPIO35 -- ebenfalls vorab deklariert, siehe Abschnitt "Victron Shunt"
// weiter unten)
lv_obj_t *tile_batterie_value = NULL;
lv_obj_t *tile_batterie_fill = NULL;

// ---- Statusleiste / WLAN-Symbol (global, damit von loop() aus
//      aktualisierbar) ----
lv_obj_t *home_wifi_icon = NULL;
lv_obj_t *settings_wifi_status = NULL;

// ---- WLAN-Verbindungsstatus ----
bool wifiConnecting = false;
unsigned long wifiConnectStart = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 15000;
String pendingSSID;

// ---- Software-Uhr ----
volatile int clockHour = 12;
volatile int clockMinute = 0;
unsigned long lastMinuteMillis = 0;
lv_obj_t *time_label_settings = NULL;
lv_obj_t *time_source_label = NULL;
bool ntpSyncRequested = false;
bool ntpSynced = false;
unsigned long lastNtpCheckMillis = 0;

// ---- Kalibrierungs-Ablauf ----
bool calibrating = false;
int calibStep = 0;
struct CalPoint { int x, y; const char* name; };
CalPoint calTargets[4] = {
  {40,  35,  "oben-links"},
  {440, 35,  "oben-rechts"},
  {40,  270, "unten-links"},
  {440, 270, "unten-rechts"}
};
int calRawX[4], calRawY[4];
lv_obj_t *calib_label = NULL;
lv_obj_t *calib_crosshair_h = NULL;
lv_obj_t *calib_crosshair_v = NULL;

// ---- WLAN-UI-Elemente ----
lv_obj_t *wifi_list_obj = NULL;
lv_obj_t *wifi_ip_mac_label = NULL;
lv_obj_t *wifi_password_ssid_label = NULL;
lv_obj_t *wifi_password_status_label = NULL;
lv_obj_t *wifi_password_textarea = NULL;
lv_obj_t *wifi_keyboard = NULL;

// ---- Kuehlschrank-Knoten UI-Elemente ----
Preferences fridgePrefs;
lv_obj_t *fridge_ip_textarea = NULL;
lv_obj_t *fridge_status_label = NULL;
lv_obj_t *fridge_on_textarea = NULL;
lv_obj_t *fridge_off_textarea = NULL;
lv_obj_t *fridge_result_label = NULL;
lv_obj_t *fridge_keyboard = NULL;

// ---- Diagnose/Checks-Bildschirm (manuelle Sofort-Abfrage bekannter
//      Knoten, ohne IP am Handy abtippen zu muessen) ----
lv_obj_t *scr_checks;
bool checksScreenBuilt = false; // Diagnose-Seite wird erst bei Bedarf angelegt, siehe go_checks_cb()
lv_obj_t *check_frischwasser_result = NULL;
lv_obj_t *check_ksabluft_result = NULL;
lv_obj_t *check_kuehlschrank_result = NULL;

// --- Display-Flush ---
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();

  if (displayFlipped) {
    // Software-180-Grad-Drehung: Zielbereich spiegeln (statt der
    // Hardware-Rotation zu vertrauen, die bei diesem Treiber fuer
    // Hochformat-Modi nicht sauber funktioniert) UND die Pixel-
    // Reihenfolge komplett umkehren, da eine 180-Grad-Drehung
    // exakt einer umgekehrten Auslese des Puffers entspricht.
    int32_t mirroredX1 = 479 - area->x2;
    int32_t mirroredY1 = 319 - area->y2;

    uint32_t total = w * h;
    uint16_t *src = (uint16_t *)&color_p->full;
    uint16_t *reversed = (uint16_t *)malloc(total * sizeof(uint16_t));
    if (reversed) {
      for (uint32_t i = 0; i < total; i++) {
        reversed[i] = src[total - 1 - i];
      }
      tft.setAddrWindow(mirroredX1, mirroredY1, w, h);
      tft.pushColors(reversed, total, true);
      free(reversed);
    } else {
      // Speicher knapp -- Fallback ohne Drehung, besser als Absturz
      tft.setAddrWindow(area->x1, area->y1, w, h);
      tft.pushColors(src, total, true);
    }
  } else {
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  }

  tft.endWrite();
  lv_disp_flush_ready(disp);
}

bool readTouchSmoothed(int &px, int &py) {
  const int SAMPLES = 6; // erhoeht von 4 -- mehr Samples = Rauschen muesste noch oefter zufaellig treffen
  long sumRawX = 0, sumRawY = 0;
  int validCount = 0;

  for (int i = 0; i < SAMPLES; i++) {
    uint16_t rawX, rawY;
    if (tft.getTouchRaw(&rawX, &rawY)) {
      uint8_t z = tft.getTouchRawZ();
      if (z >= TOUCH_Z_THRESHOLD) {
        sumRawX += rawX;
        sumRawY += rawY;
        validCount++;
      }
    }
    delayMicroseconds(500);
  }

  if (validCount < SAMPLES) return false;

  int rawX = sumRawX / validCount;
  int rawY = sumRawY / validCount;

  px = (int)(slopeY * rawY + interceptY);
  py = (int)(slopeX * rawX + interceptX);

  // Bei gedrehtem Display (180 Grad) muessen auch die Touch-
  // Koordinaten gespiegelt werden, da der Touch-Sensor physisch
  // fest sitzt und sich nicht mitdreht.
  if (displayFlipped) {
    px = 479 - px;
    py = 319 - py;
  }

  px = constrain(px, 0, 479);
  py = constrain(py, 0, 319);
  return true;
}

void my_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  if (calibrating) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  int px, py;
  if (readTouchSmoothed(px, py)) {
    data->point.x = px;
    data->point.y = py;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ============================================================
// Kachel-Hilfsfunktion (Startseite)
// ============================================================
// Kachel mit optionalem Fuellstands-Hintergrund (fill_percent 0-100,
// oder -1 fuer keine Fuellanzeige -- normale Kachel wie bisher).
// Der Fuellbalken wird halbtransparent hinter Titel/Wert gezeichnet,
// damit der Text weiterhin gut lesbar bleibt.
lv_obj_t *create_tile(lv_obj_t *parent, int col, int row, const char *title,
                       const char *value, lv_color_t accent_color, int fill_percent = -1) {
  const int TILE_W = 112;
  const int TILE_H = 130;
  const int GAP = 6;
  const int START_X = 6;
  const int START_Y = 4;

  lv_obj_t *tile = lv_btn_create(parent);
  lv_obj_set_size(tile, TILE_W, TILE_H);
  lv_obj_set_pos(tile, START_X + col * (TILE_W + GAP), START_Y + row * (TILE_H + GAP));
  lv_obj_set_style_bg_color(tile, lv_color_hex(0x1E1E1E), 0);
  lv_obj_set_style_border_color(tile, accent_color, 0);
  lv_obj_set_style_border_width(tile, 2, 0);
  lv_obj_set_style_radius(tile, 8, 0);
  lv_obj_set_style_clip_corner(tile, true, 0); // Fuellung an abgerundeten Ecken abschneiden
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

  if (fill_percent >= 0) {
    int fh = (int)((TILE_H - 4) * (constrain(fill_percent, 0, 100) / 100.0f));
    lv_obj_t *fill = lv_obj_create(tile);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, TILE_W - 4, fh);
    lv_obj_set_pos(fill, 2, (TILE_H - 2) - fh);
    lv_obj_set_style_bg_color(fill, accent_color, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_40, 0);
    lv_obj_set_style_radius(fill, 6, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
  }

  lv_obj_t *title_label = lv_label_create(tile);
  lv_label_set_text(title_label, title);
  lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(title_label, TILE_W - 12);
  lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14_de, 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t *value_label = lv_label_create(tile);
  lv_label_set_text(value_label, value);
  lv_obj_set_style_text_font(value_label, &lv_font_montserrat_20_de, 0);
  lv_obj_set_style_text_color(value_label, accent_color, 0);
  // Sicherheitsnetz gegen Ueberlauf bei groesserer Schrift (siehe Notizen):
  // Breite begrenzen + bei Platzmangel mit "..." abschneiden statt in die
  // Nachbarkachel zu ragen. Bei der aktuellen 20px-Schrift greift das nicht.
  //
  // WICHTIG: Hoehe wird explizit fuer bis zu 2 Zeilen reserviert (per
  // Font-Metrik berechnet, nicht geschaetzt). Ohne diese Zeile wuerde
  // LV_LABEL_LONG_DOT die Hoehe anhand des *Platzhaltertexts* an dieser
  // Stelle einfrieren -- bei einzeiligen Platzhaltern wie "--" (RT/H,
  // KS Abluft) verschwand dadurch die spaeter per Code gesetzte zweite
  // Zeile (Feuchte) kommentarlos. Bug gefunden am 23.08.2026.
  lv_obj_set_width(value_label, TILE_W - 8);
  lv_obj_set_height(value_label, lv_font_get_line_height(&lv_font_montserrat_20_de) * 2 + 2);
  lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(value_label, LV_ALIGN_BOTTOM_MID, 0, -8);

  return tile;
}

// ============================================================
// Bildschirmwechsel-Callbacks
// ============================================================
static void go_home_cb(lv_event_t *e) { lv_scr_load(scr_home); }
static void go_settings_cb(lv_event_t *e) {
  lv_scr_load(scr_settings);
}
static void vedirect_tile_event_cb(lv_event_t *e) { lv_scr_load(scr_vedirect_detail); }
// Vorab deklariert -- die eigentliche Definition steht weiter unten
// beim Diagnose/Checks-Bildschirm, wird aber schon hier in add_status_bar()
// als Button-Callback gebraucht.
static void go_checks_cb(lv_event_t *e);

// ============================================================
// Statusleiste
// ============================================================
void add_status_bar(lv_obj_t *screen) {
  lv_obj_t *bar = lv_obj_create(screen);
  lv_obj_set_size(bar, 480, 36);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *wifi_icon = lv_label_create(bar);
  lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x666666), 0);
  lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, 8, 0);
  home_wifi_icon = wifi_icon; // global merken fuer spaetere Farb-Updates

  // Deutlich vergroessert (war 34x22) -- kleine Ecken-Buttons sind
  // beim resistiven Touch am ungenauesten zu treffen.
  lv_obj_t *settings_btn = lv_btn_create(bar);
  lv_obj_set_size(settings_btn, 64, 34);
  lv_obj_align(settings_btn, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(settings_btn, lv_color_hex(0x1E1E1E), 0);
  lv_obj_set_style_border_width(settings_btn, 0, 0);
  lv_obj_add_event_cb(settings_btn, go_settings_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *gear_label = lv_label_create(settings_btn);
  lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS);
  lv_obj_center(gear_label);

  // Diagnose/Checks-Button links neben dem Zahnrad (ANNAHME: LV_SYMBOL_LIST
  // als Icon fuer "Checks" gewaehlt, da LVGL kein spezifisches
  // Diagnose-Symbol mitbringt -- bei Bedarf leicht gegen ein anderes
  // eingebautes Symbol tauschbar).
  lv_obj_t *checks_btn = lv_btn_create(bar);
  lv_obj_set_size(checks_btn, 64, 34);
  lv_obj_align(checks_btn, LV_ALIGN_RIGHT_MID, -74, 0);
  lv_obj_set_style_bg_color(checks_btn, lv_color_hex(0x1E1E1E), 0);
  lv_obj_set_style_border_width(checks_btn, 0, 0);
  lv_obj_add_event_cb(checks_btn, go_checks_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *checks_label = lv_label_create(checks_btn);
  lv_label_set_text(checks_label, LV_SYMBOL_LIST);
  lv_obj_center(checks_label);
}

// Aktualisiert Farbe des WLAN-Symbols entsprechend Verbindungsstatus.
void update_wifi_icon() {
  if (!home_wifi_icon) return;
  if (WiFi.status() == WL_CONNECTED) {
    lv_obj_set_style_text_color(home_wifi_icon, lv_color_hex(0x4CAF50), 0); // gruen
  } else if (wifiConnecting) {
    lv_obj_set_style_text_color(home_wifi_icon, lv_color_hex(0xFFC107), 0); // gelb
  } else {
    lv_obj_set_style_text_color(home_wifi_icon, lv_color_hex(0x666666), 0); // grau
  }
}

void update_settings_wifi_status() {
  if (!settings_wifi_status) return;
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(settings_wifi_status, "Verbunden: %s", WiFi.SSID().c_str());
    lv_obj_set_style_text_color(settings_wifi_status, lv_color_hex(0x4CAF50), 0);
  } else if (wifiConnecting) {
    lv_label_set_text(settings_wifi_status, "Verbinde...");
    lv_obj_set_style_text_color(settings_wifi_status, lv_color_hex(0xFFC107), 0);
  } else {
    lv_label_set_text(settings_wifi_status, "Nicht verbunden");
    lv_obj_set_style_text_color(settings_wifi_status, lv_color_hex(0x888888), 0);
  }
}

// ============================================================
// Startseite mit 8 Kacheln
// ============================================================
void build_home_screen() {
  scr_home = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_home, lv_color_hex(0x0A0A0A), 0);
  lv_obj_clear_flag(scr_home, LV_OBJ_FLAG_SCROLLABLE);

  add_status_bar(scr_home);

  lv_obj_t *tile_area = lv_obj_create(scr_home);
  lv_obj_set_size(tile_area, 480, 284);
  lv_obj_set_pos(tile_area, 0, 36);
  lv_obj_set_style_bg_color(tile_area, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_border_width(tile_area, 0, 0);
  lv_obj_set_style_radius(tile_area, 0, 0);
  lv_obj_set_style_pad_all(tile_area, 0, 0);
  lv_obj_clear_flag(tile_area, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *tile1 = create_tile(tile_area, 0, 0, "Solar", "-- W\n--", lv_color_hex(0xFFC107));
  lv_obj_add_event_cb(tile1, vedirect_tile_event_cb, LV_EVENT_CLICKED, NULL);
  tile_solar_value = lv_obj_get_child(tile1, 1); // Value-Label merken (2. Kind: Titel, Wert)

  // Batterie -- jetzt mit echtem SOC vom Victron Shunt (VE.Direct,
  // UART1/GPIO35), kein statischer Platzhalter mehr. Analog zur
  // Frischwasser-Kachel: kein fill_percent an create_tile uebergeben
  // (der ist fuer einen einmalig fest eingebrannten Wert gedacht),
  // stattdessen eigenes, per updateBatterieTile() aktualisierbares
  // Fuellelement -- siehe Abschnitt "Victron Shunt" weiter unten.
  lv_obj_t *tile_batterie = create_tile(tile_area, 1, 0, "Batterie", "--", lv_color_hex(0x4CAF50));
  tile_batterie_value = lv_obj_get_child(tile_batterie, 1); // kein fill_percent uebergeben -> Value ist Kind 1

  tile_batterie_fill = lv_obj_create(tile_batterie);
  lv_obj_remove_style_all(tile_batterie_fill);
  lv_obj_set_size(tile_batterie_fill, 112 - 4, 0); // Starthoehe 0, updateBatterieTile() setzt den echten Wert
  lv_obj_set_pos(tile_batterie_fill, 2, 130 - 2);
  lv_obj_set_style_bg_color(tile_batterie_fill, lv_color_hex(0x4CAF50), 0);
  lv_obj_set_style_bg_opa(tile_batterie_fill, LV_OPA_40, 0);
  lv_obj_set_style_radius(tile_batterie_fill, 6, 0);
  lv_obj_clear_flag(tile_batterie_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(tile_batterie_fill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_move_background(tile_batterie_fill); // hinter Titel/Wert, sonst verdeckt er den Text

  lv_obj_t *tile_rth = create_tile(tile_area, 2, 0, "RT/H", "--", lv_color_hex(0x03A9F4));
  tile_rth_value = lv_obj_get_child(tile_rth, 1);

  // Frischwasser -- jetzt mit echtem Fuellstand vom Frischwasser-Node
  // (3 Schwellwert-Sensoren), kein statischer Platzhalter mehr. Der
  // Fuellbalken wird nicht mehr ueber den fill_percent-Parameter von
  // create_tile gesetzt (der ist fuer einen einmalig fest eingebrannten
  // Wert gedacht), sondern als eigenes, per updateFrischwasserTile()
  // dynamisch aktualisierbares Element angelegt -- siehe Abschnitt
  // "Frischwasser-Node" weiter unten.
  lv_obj_t *tile_frischwasser = create_tile(tile_area, 3, 0, "Frischwasser", "--", lv_color_hex(0x03A9F4));
  tile_frischwasser_value = lv_obj_get_child(tile_frischwasser, 1); // kein fill_percent uebergeben -> Value ist Kind 1 (nicht 2)

  tile_frischwasser_fill = lv_obj_create(tile_frischwasser);
  lv_obj_remove_style_all(tile_frischwasser_fill);
  lv_obj_set_size(tile_frischwasser_fill, 112 - 4, 0); // Starthoehe 0, updateFrischwasserTile() setzt den echten Wert
  lv_obj_set_pos(tile_frischwasser_fill, 2, 130 - 2);
  lv_obj_set_style_bg_color(tile_frischwasser_fill, lv_color_hex(0x03A9F4), 0);
  lv_obj_set_style_bg_opa(tile_frischwasser_fill, LV_OPA_40, 0);
  lv_obj_set_style_radius(tile_frischwasser_fill, 6, 0);
  lv_obj_clear_flag(tile_frischwasser_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(tile_frischwasser_fill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_move_background(tile_frischwasser_fill); // hinter Titel/Wert, sonst verdeckt er den Text

  create_tile(tile_area, 0, 1, "Pipi-Box", "30 %", lv_color_hex(0xFF9800), 30);
  lv_obj_t *tile_ksabluft = create_tile(tile_area, 1, 1, "KS Abluft", "--", lv_color_hex(0xFF5722));
  tile_ksabluft_value = lv_obj_get_child(tile_ksabluft, 1);

  // Schmaler Balken unten in der Kachel: fuellt sich von 0% (direkt nach
  // einer Abfrage) auf 100% (kurz vor der naechsten, in 15 Minuten) --
  // rein visuelle Orientierung, keine exakte "Daten sind X alt"-Anzeige.
  tile_ksabluft_progress = lv_bar_create(tile_ksabluft);
  lv_obj_remove_style_all(tile_ksabluft_progress);
  lv_obj_set_size(tile_ksabluft_progress, 112 - 8, 4);
  lv_obj_align(tile_ksabluft_progress, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_bar_set_range(tile_ksabluft_progress, 0, 100);
  lv_bar_set_value(tile_ksabluft_progress, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(tile_ksabluft_progress, lv_color_hex(0x333333), 0); // Track
  lv_obj_set_style_bg_opa(tile_ksabluft_progress, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile_ksabluft_progress, 2, 0);
  lv_obj_set_style_bg_color(tile_ksabluft_progress, lv_color_hex(0xFF5722), LV_PART_INDICATOR); // Fuellung, Kachel-Akzentfarbe
  lv_obj_set_style_bg_opa(tile_ksabluft_progress, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(tile_ksabluft_progress, 2, LV_PART_INDICATOR);
  lv_obj_clear_flag(tile_ksabluft_progress, LV_OBJ_FLAG_CLICKABLE);

  create_tile(tile_area, 2, 1, "Kühlschrank", "5.0C\n-18.0C", lv_color_hex(0x00BCD4));
  create_tile(tile_area, 3, 1, "Gasflasche", "--- kg", lv_color_hex(0x9E9E9E));
}

// ============================================================
// VE.Direct Detailansicht
// ============================================================
// ============================================================
// SHT31-D -- Raumtemperatur/Feuchte (I2C, direkt am Display-Board)
// ============================================================
const int I2C_SDA_PIN = 32;
const int I2C_SCL_PIN = 25;
const uint8_t SHT31_ADDR = 0x44; // ADDR-Pin auf GND/offen

float rthTempC = NAN;
float rthHumidityPct = NAN;

// CRC8-Pruefsumme nach Sensirion-Spezifikation (Polynom 0x31, Start 0xFF)
// -- schuetzt vor vereinzelten fehlerhaften I2C-Uebertragungen.
uint8_t sht31Crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
  }
  return crc;
}

// Liest eine Einzelmessung vom SHT31. Gibt false zurueck, wenn der
// Sensor nicht antwortet oder die CRC-Pruefung fehlschlaegt.
bool readSHT31(float &tempC, float &humidityPct) {
  Wire.beginTransmission(SHT31_ADDR);
  Wire.write(0x24); // Single-Shot, hohe Genauigkeit, kein Clock-Stretching
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;

  delay(20); // Messzeit laut Datenblatt (>15ms)

  if (Wire.requestFrom((int)SHT31_ADDR, 6) != 6) return false;

  uint8_t data[6];
  for (int i = 0; i < 6; i++) data[i] = Wire.read();

  if (sht31Crc8(&data[0], 2) != data[2]) return false; // Temperatur-CRC
  if (sht31Crc8(&data[3], 2) != data[5]) return false; // Feuchte-CRC

  uint16_t rawTemp = (data[0] << 8) | data[1];
  uint16_t rawHum  = (data[3] << 8) | data[4];

  tempC = -45.0f + 175.0f * ((float)rawTemp / 65535.0f);
  humidityPct = 100.0f * ((float)rawHum / 65535.0f);
  return true;
}

void updateRthTile() {
  if (!tile_rth_value) return;
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1fC\n%.0f%%", rthTempC, rthHumidityPct);
  lv_label_set_text(tile_rth_value, buf);
}

void handleRthSensor() {
  static unsigned long lastRead = 0;
  // Alle 5 Sekunden messen -- Raumklima aendert sich langsam,
  // haeufigeres Messen ist unnoetig.
  if (millis() - lastRead < 5000) return;
  lastRead = millis();

  float t, h;
  if (readSHT31(t, h)) {
    rthTempC = t;
    rthHumidityPct = h;
    updateRthTile();
  }
  // Bei Fehler: letzter bekannter Wert bleibt stehen, kein Absturz.
}

// ============================================================
// Lüfter-Node -- Abluft-Temperatur/-Feuchte per HTTP (KS Abluft-Kachel)
// ============================================================
// ANNAHME: IP fest im Code hinterlegt (wie beim Lüfter-Node selbst),
// nicht wie beim Kühlschrank-Knoten über die UI konfigurierbar. Falls
// sich die IP im Netzwerk ändert oder mehrere Lüfter-Nodes hinzukommen,
// waere eine Konfigurationsmoeglichkeit wie bei fridge_ip_textarea
// sinnvoller -- bewusst noch nicht Teil dieser Aenderung.
const char *KSABLUFT_HOST = "192.168.1.228";

float ksabluftTempC = NAN;
float ksabluftHumidityPct = NAN;

void updateKsAbluftTile() {
  if (!tile_ksabluft_value) return;
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1fC\n%.0f%%", ksabluftTempC, ksabluftHumidityPct);
  lv_label_set_text(tile_ksabluft_value, buf);
}

// Fragt den Lüfter-Node per HTTP GET /status ab (liefert JSON mit
// "abluft_temp" und "abluft_humidity", siehe esp32-luefter-node-main.cpp
// handleStatus()). Beide Felder koennen "null" sein, wenn der SHT31
// dort gerade nicht erreichbar ist.
// Alle 15 Minuten abfragen -- Abluft-Temperatur/-Feuchte aendert sich
// langsam, haeufigeres Abfragen ist unnoetig. Reduziert ausserdem, wie
// oft der blockierende HTTP-Request (bis zu ~3s Timeout) die Touch-/
// LVGL-Reaktion des Displays kurz haengen lassen kann.
const unsigned long KSABLUFT_FETCH_INTERVAL_MS = 15UL * 60 * 1000;

// Zeitpunkt der letzten (versuchten) Abfrage -- global, nicht nur lokal
// static, damit updateKsAbluftProgressBar() den Fortschritt bis zur
// naechsten Abfrage berechnen kann.
unsigned long ksabluftLastFetch = 0;
bool ksabluftPrimed = false;

void handleKsAbluftSensor() {
  // ksabluftLastFetch wird beim allerersten Aufruf bewusst auf "jetzt
  // minus ein Intervall" initialisiert (nicht auf 0), damit direkt beim
  // Start abgefragt wird -- sonst zeigt die Kachel nach dem Einschalten
  // bis zu 15 Minuten lang nur "--", weil der erste Vergleich sonst
  // faelschlich "noch nicht faellig" ergibt. Bug gefunden am 23.08.2026.
  // Bewusst hier (nicht als globaler Initialwert) gesetzt, da millis()
  // beim Start von main.cpp vor der Hardware-Timer-Initialisierung noch
  // keinen verlaesslichen Wert liefern koennte -- diese Funktion laeuft
  // aber erst aus loop(), also sicher nach setup().
  if (!ksabluftPrimed) {
    ksabluftLastFetch = millis() - KSABLUFT_FETCH_INTERVAL_MS;
    ksabluftPrimed = true;
  }
  if (millis() - ksabluftLastFetch < KSABLUFT_FETCH_INTERVAL_MS) return;
  ksabluftLastFetch = millis();

  if (WiFi.status() != WL_CONNECTED) return; // kein Netz, kein Versuch

  HTTPClient http;
  String url = String("http://") + KSABLUFT_HOST + "/status";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      if (!doc["abluft_temp"].isNull()) {
        ksabluftTempC = doc["abluft_temp"].as<float>();
      }
      if (!doc["abluft_humidity"].isNull()) {
        ksabluftHumidityPct = doc["abluft_humidity"].as<float>();
      }
      updateKsAbluftTile();
    }
    // Bei JSON-Fehler: letzter bekannter Wert bleibt stehen, kein Absturz.
  }
  // Bei HTTP-Fehler (Node offline/Timeout): ebenfalls letzter Wert stehen
  // lassen -- kein Fehlertext auf der Startseite, um die Kachel ruhig
  // zu halten (anders als beim manuellen Abfrage-Button der Kuehlschrank-
  // Detailseite, wo ein Fehlertext sinnvoll ist).
  http.end();
}

// Aktualisiert den Fortschrittsbalken unten in der KS-Abluft-Kachel:
// 0% direkt nach einer Abfrage, 100% kurz vor der naechsten (in 15 Min).
// Rein visuelle Orientierung -- kein exaktes "Daten sind X alt"-Signal,
// da ksabluftLastFetch bei jedem Abfrageversuch aktualisiert wird, auch
// wenn WLAN fehlt oder der Lüfter-Node nicht antwortet.
void updateKsAbluftProgressBar() {
  if (!tile_ksabluft_progress) return;
  static unsigned long lastUpdate = 0;
  // Alle 5s reicht -- der Balken ist nur ~104px breit, feineres
  // Aktualisieren waere ohnehin nicht sichtbar.
  if (millis() - lastUpdate < 5000) return;
  lastUpdate = millis();

  unsigned long elapsed = millis() - ksabluftLastFetch;
  int percent = (int)((elapsed * 100UL) / KSABLUFT_FETCH_INTERVAL_MS);
  if (percent > 100) percent = 100;
  lv_bar_set_value(tile_ksabluft_progress, percent, LV_ANIM_OFF);
}

// ============================================================
// Frischwasser-Node -- Fuellstand per HTTP (Frischwasser-Kachel)
// ============================================================
// ANNAHME: IP fest im Code hinterlegt, analog zum Lüfter-Node. Aktuell
// per DHCP zugewiesen (192.168.1.197) -- noch keine feste Reservierung
// im Router bestaetigt (Stand 25.08.2026). Ohne Reservierung kann sich
// die IP nach einem Neustart des Nodes/Routers aendern; dann muesste
// hier manuell nachgezogen werden.
const char *FRISCHWASSER_HOST = "192.168.1.197";

// Drei diskrete Fuellstands-Sensoren (Schwellwert-Schalter, keine
// kontinuierliche Prozentmessung -- siehe wassersensoren-notizen.md).
// Level 1 = unterste Marke ("bald auffuellen"), Level 2 = ca. 50l,
// Level 3 = obere Marke (90-100l, "voll"). Die genauen Literwerte je
// Sensor sind noch nicht final vermessen (Stand 25.08.2026).
enum FrischwasserLevel { FW_LEVEL_0 = 0, FW_LEVEL_1, FW_LEVEL_2, FW_LEVEL_3 };

bool frischwasserDatenGueltig = false;
FrischwasserLevel frischwasserLevel = FW_LEVEL_0;

// Rein visuelle Naeherung fuer den Kachel-Fuellbalken -- KEINE exakten
// Liter-/Prozentwerte, nur zur groben optischen Einordnung gedacht.
const int FW_FILL_PERCENT[4]  = {8, 35, 65, 95};
const char *FW_LABEL_LINE1[4] = {"Leer", "Niedrig", "OK", "Voll"};

// Setzt Kachel-Text und Fuellbalken passend zu frischwasserLevel.
void updateFrischwasserTile() {
  if (!tile_frischwasser_value) return;
  // Nur noch der Klartext-Status (Leer/Niedrig/OK/Voll) -- die
  // zusaetzliche "Level N"-Zeile brachte keinen Mehrwert und wurde auf
  // Nutzerwunsch entfernt (27.08.2026).
  lv_label_set_text(tile_frischwasser_value, FW_LABEL_LINE1[frischwasserLevel]);

  if (tile_frischwasser_fill) {
    const int TILE_H_LOCAL = 130; // muss mit TILE_H in create_tile() uebereinstimmen
    int fh = (int)((TILE_H_LOCAL - 4) * (FW_FILL_PERCENT[frischwasserLevel] / 100.0f));
    lv_obj_set_size(tile_frischwasser_fill, 112 - 4, fh);
    lv_obj_set_pos(tile_frischwasser_fill, 2, (TILE_H_LOCAL - 2) - fh);
  }
}

// ANNAHME: Alle 60s abfragen -- haeufiger als KS Abluft (15 Min), da
// der Fuellstand fuer den Nutzer akut handlungsrelevant ist (z.B. vor
// dem Duschen wissen, ob noch genug Wasser da ist). Anpassbar.
const unsigned long FRISCHWASSER_FETCH_INTERVAL_MS = 60UL * 1000;

unsigned long frischwasserLastFetch = 0;
bool frischwasserPrimed = false;

// Fragt den Frischwasser-Node per HTTP GET /status ab. Erwartetes
// JSON-Format: {"sensor1":{"erkannt":bool,...},"sensor2":{...},
// "sensor3":{...}} (siehe esp32-frischwasser-node-main.cpp). Nimmt
// defensiv den hoechsten erkannten Level, falls der Zustand einmal
// nicht ganz monoton sein sollte (z.B. durch einen kurz wackelnden
// unteren Sensor) -- kein Absturz, im Zweifel nur ein fuer einen
// Abfragezyklus zu optimistischer Wert.
void handleFrischwasserSensor() {
  if (!frischwasserPrimed) {
    frischwasserLastFetch = millis() - FRISCHWASSER_FETCH_INTERVAL_MS;
    frischwasserPrimed = true;
  }
  if (millis() - frischwasserLastFetch < FRISCHWASSER_FETCH_INTERVAL_MS) return;
  frischwasserLastFetch = millis();

  if (WiFi.status() != WL_CONNECTED) return; // kein Netz, kein Versuch

  HTTPClient http;
  String url = String("http://") + FRISCHWASSER_HOST + "/status";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      bool l1 = doc["sensor1"]["erkannt"].as<bool>();
      bool l2 = doc["sensor2"]["erkannt"].as<bool>();
      bool l3 = doc["sensor3"]["erkannt"].as<bool>();

      FrischwasserLevel level = FW_LEVEL_0;
      if (l1) level = FW_LEVEL_1;
      if (l2) level = FW_LEVEL_2;
      if (l3) level = FW_LEVEL_3;

      frischwasserLevel = level;
      frischwasserDatenGueltig = true;
      updateFrischwasserTile();
    }
    // Bei JSON-Fehler: letzter bekannter Wert bleibt stehen, kein Absturz.
  } else {
    // Diagnose-Log (28.08.2026): Code und freier Heap mitschreiben, um
    // den Verdacht auf Speicherknappheit als Ursache fuer Code -1 zu
    // pruefen -- bei Bedarf per Serial-Monitor auslesen.
    Serial.printf("Frischwasser-Abfrage fehlgeschlagen: Code %d, freier Heap: %u Bytes\n",
                  code, ESP.getFreeHeap());
  }
  // Bei HTTP-Fehler (Node offline/Timeout): ebenfalls letzter Wert
  // stehen lassen, analog zum KS-Abluft-Muster.
  http.end();
}

// ============================================================
// VE.Direct -- Empfang und Auswertung (Victron SmartSolar MPPT)
// ============================================================
HardwareSerial VeSerial(2); // UART2, RX an GPIO39 (2-poliger Stecker)
String veLineBuf = "";

struct VeData {
  float batterySpannungV = 0;
  float ladestromA = 0;
  float solarleistungW = 0;
  float tagesertragKWh = 0;
  int ladezustandCode = -1;
  bool valid = false;
} veData;

// UI-Elemente, die bei jedem vollstaendigen VE.Direct-Datenblock
// aktualisiert werden (Deklaration bereits weiter oben erfolgt).

// Wandelt den Victron-Ladezustand-Code (CS) in einen lesbaren Text.
// Werte laut Victron VE.Direct-Protokolldokumentation.
const char *veChargeStateToText(int cs) {
  switch (cs) {
    case 0: return "Aus";
    case 2: return "Fehler";
    case 3: return "Bulk";
    case 4: return "Absorption";
    case 5: return "Float";
    case 7: return "Equalize";
    case 245: return "Startet...";
    case 252: return "Extern gesteuert";
    default: return "Unbekannt";
  }
}

void updateVeDisplay() {
  veData.valid = true;
  char buf[32];

  if (tile_solar_value) {
    // Zweite Zeile mit Ladezustand ergaenzt (27.08.2026) -- Watt allein
    // sagt nichts darueber aus, ob der Laderegler gerade z.B. Bulk,
    // Absorption oder Float faehrt.
    snprintf(buf, sizeof(buf), "%.0f W\n%s", veData.solarleistungW,
             veChargeStateToText(veData.ladezustandCode));
    lv_label_set_text(tile_solar_value, buf);
  }
  if (ve_solarleistung_val) {
    snprintf(buf, sizeof(buf), "%.0f W", veData.solarleistungW);
    lv_label_set_text(ve_solarleistung_val, buf);
  }
  if (ve_batteriespannung_val) {
    snprintf(buf, sizeof(buf), "%.2f V", veData.batterySpannungV);
    lv_label_set_text(ve_batteriespannung_val, buf);
  }
  if (ve_ladestrom_val) {
    snprintf(buf, sizeof(buf), "%.2f A", veData.ladestromA);
    lv_label_set_text(ve_ladestrom_val, buf);
  }
  if (ve_tagesertrag_val) {
    snprintf(buf, sizeof(buf), "%.2f kWh", veData.tagesertragKWh);
    lv_label_set_text(ve_tagesertrag_val, buf);
  }
  if (ve_ladezustand_val) {
    lv_label_set_text(ve_ladezustand_val, veChargeStateToText(veData.ladezustandCode));
  }
}

// Verarbeitet eine einzelne VE.Direct-Zeile im Format "Label\tWert".
void processVeLine(const String &line) {
  int tabPos = line.indexOf('\t');
  if (tabPos < 0) return; // keine gueltige Label/Wert-Zeile

  String label = line.substring(0, tabPos);
  String value = line.substring(tabPos + 1);

  if (label == "V") {
    veData.batterySpannungV = value.toFloat() / 1000.0; // mV -> V
  } else if (label == "I") {
    veData.ladestromA = value.toFloat() / 1000.0; // mA -> A
  } else if (label == "PPV") {
    veData.solarleistungW = value.toFloat(); // bereits in W
  } else if (label == "H20") {
    veData.tagesertragKWh = value.toFloat() / 100.0; // 0.01kWh -> kWh
  } else if (label == "CS") {
    veData.ladezustandCode = value.toInt();
  } else if (label == "Checksum") {
    // Ende eines Datenblocks -- Anzeige aktualisieren.
    updateVeDisplay();
  }
}

// Wird in loop() aufgerufen: liest verfuegbare Bytes, baut Zeilen
// zusammen und uebergibt vollstaendige Zeilen zur Auswertung.
void handleVeDirectSerial() {
  while (VeSerial.available()) {
    char c = VeSerial.read();
    if (c == '\n') {
      veLineBuf.trim(); // entfernt evtl. '\r' am Ende
      processVeLine(veLineBuf);
      veLineBuf = "";
    } else {
      veLineBuf += c;
      if (veLineBuf.length() > 100) veLineBuf = ""; // Sicherheitsnetz
    }
  }
}

// ============================================================
// Victron Shunt -- SOC per VE.Direct (Batterie-Kachel)
// ============================================================
// Zweiter, unabhaengiger VE.Direct-Empfang (eigenes Geraet, eigener
// Datenstrom) fuer den Batterie-Shunt (z.B. SmartShunt/BMV), analog
// zum Solar-Empfang oben. UART1, RX an GPIO35 -- freier Pin, kein
// Konflikt mit TFT/Touch (SPI: 12/13/14/15/33), I2C (32/25), Backlight
// (27) oder Solar-UART2 (39). GPIO35 ist wie GPIO39 ein reiner
// Input-Pin (ADC1, kein Pullup) -- passt fuer eine RX-only-Verbindung
// (kein TX vom ESP32 zum Shunt noetig), TX-Pin daher -1.
HardwareSerial ShuntSerial(1); // UART1, RX an GPIO35
String shuntLineBuf = "";

struct ShuntData {
  float socPercent = NAN;
  bool valid = false;
} shuntData;

void updateBatterieTile() {
  if (!tile_batterie_value || isnan(shuntData.socPercent)) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%.0f %%", shuntData.socPercent);
  lv_label_set_text(tile_batterie_value, buf);

  if (tile_batterie_fill) {
    const int TILE_H_LOCAL = 130; // muss mit TILE_H in create_tile() uebereinstimmen
    int pct = constrain((int)shuntData.socPercent, 0, 100);
    int fh = (int)((TILE_H_LOCAL - 4) * (pct / 100.0f));
    lv_obj_set_size(tile_batterie_fill, 112 - 4, fh);
    lv_obj_set_pos(tile_batterie_fill, 2, (TILE_H_LOCAL - 2) - fh);
  }
}

// Verarbeitet eine einzelne VE.Direct-Zeile vom Shunt im Format
// "Label\tWert". Vorerst wird nur SOC ausgewertet (auf Nutzerwunsch,
// V/A folgen bei Bedarf spaeter).
//
// ANNAHME (nicht am echten Geraet verifiziert): Feldname "SOC", Einheit
// Promille (0.1%-Schritte, z.B. "854" = 85.4%) -- laut Victron
// VE.Direct-Protokolldokumentation fuer Batterie-Monitore (BMV/SmartShunt).
// Falls die Kachel nach dem Flashen bei "--" bleibt, zuerst pruefen, ob
// der Shunt tatsaechlich dieses Feld unter diesem Namen sendet (z.B.
// kurz per Serial-Monitor auf einem freien UART mitloggen).
void processShuntLine(const String &line) {
  int tabPos = line.indexOf('\t');
  if (tabPos < 0) return;

  String label = line.substring(0, tabPos);
  String value = line.substring(tabPos + 1);

  if (label == "SOC") {
    shuntData.socPercent = value.toFloat() / 10.0f; // Promille -> Prozent
  } else if (label == "Checksum") {
    shuntData.valid = true;
    updateBatterieTile();
  }
}

void handleShuntSerial() {
  // DEBUG (temporaer, spaeter wieder entfernen): loggt jede empfangene
  // Zeile roh auf den Serial Monitor. Zeigt, ob ueberhaupt Daten auf
  // GPIO35 ankommen (Verkabelung/Baudrate) und ob der Feldname "SOC"
  // tatsaechlich so gesendet wird (unverifizierte Annahme, siehe Notizen).
  static unsigned long shuntDebugLastByte = 0;
  static bool shuntDebugEverSeen = false;
  if (ShuntSerial.available()) {
    shuntDebugEverSeen = true;
    shuntDebugLastByte = millis();
  } else if (!shuntDebugEverSeen && millis() > 10000 &&
             millis() - shuntDebugLastByte > 10000) {
    shuntDebugLastByte = millis();
    Serial.println("SHUNT DEBUG: seit 10s keine einzige Byte auf GPIO35 empfangen -- Verkabelung/Baudrate pruefen.");
  }

  while (ShuntSerial.available()) {
    char c = ShuntSerial.read();
    if (c == '\n') {
      shuntLineBuf.trim();
      Serial.print("SHUNT DEBUG RAW: [");
      Serial.print(shuntLineBuf);
      Serial.println("]");
      processShuntLine(shuntLineBuf);
      shuntLineBuf = "";
    } else {
      shuntLineBuf += c;
      if (shuntLineBuf.length() > 100) shuntLineBuf = ""; // Sicherheitsnetz
    }
  }
}

void build_vedirect_detail_screen() {
  scr_vedirect_detail = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_vedirect_detail, lv_color_hex(0x0A0A0A), 0);

  lv_obj_t *title = lv_label_create(scr_vedirect_detail);
  lv_label_set_text(title, "VE.Direct Details");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20_de, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  const char *labels[] = {
    "Solarleistung:", "Batteriespannung:", "Ladestrom:",
    "Tagesertrag:", "Ladezustand:"
  };
  lv_obj_t **valueLabelPtrs[] = {
    &ve_solarleistung_val, &ve_batteriespannung_val, &ve_ladestrom_val,
    &ve_tagesertrag_val, &ve_ladezustand_val
  };

  int y = 55;
  for (int i = 0; i < 5; i++) {
    lv_obj_t *l = lv_label_create(scr_vedirect_detail);
    lv_label_set_text(l, labels[i]);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14_de, 0);
    lv_obj_set_pos(l, 30, y);

    lv_obj_t *v = lv_label_create(scr_vedirect_detail);
    lv_label_set_text(v, "--"); // Platzhalter, bis erster Datenblock ankommt
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14_de, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_pos(v, 260, y);
    *valueLabelPtrs[i] = v;
    y += 32;
  }

  lv_obj_t *back_btn = lv_btn_create(scr_vedirect_detail);
  lv_obj_set_size(back_btn, 100, 40);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_add_event_cb(back_btn, go_home_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "Zurück");
  lv_obj_set_style_text_font(back_label, &lv_font_montserrat_14_de, 0);
  lv_obj_center(back_label);
}

// ============================================================
// Datum/Uhrzeit
// ============================================================
static void update_time_label() {
  if (time_label_settings) {
    lv_label_set_text_fmt(time_label_settings, "%02d:%02d", clockHour, clockMinute);
  }
  if (time_source_label) {
    lv_label_set_text(time_source_label, ntpSynced ? "(automatisch, NTP)" : "(manuell)");
    lv_obj_set_style_text_color(time_source_label,
      ntpSynced ? lv_color_hex(0x4CAF50) : lv_color_hex(0x888888), 0);
  }
}

// Startet die NTP-Synchronisation. ANNAHME: Zeitzone fest auf
// Deutschland (CET/CEST mit automatischer Sommerzeit-Umschaltung)
// eingestellt -- passend zu deinem Hauptreisegebiet, auch wenn du
// zeitweise in Spanien/Marokko unterwegs bist (dort gilt ebenfalls
// CET, Marokko nutzt seit 2018 durchgehend UTC+1 mit Ausnahme
// waehrend Ramadan -- diese Sonderregel wird hier NICHT beruecksichtigt).
void startNtpSync() {
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  ntpSyncRequested = true;
}

// Prueft, ob die NTP-Zeit inzwischen gueltig angekommen ist, und
// uebernimmt sie in die Anzeige-Variablen.
void checkNtpResult() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 100)) { // 100ms Timeout, nicht blockierend lang
    clockHour = timeinfo.tm_hour;
    clockMinute = timeinfo.tm_min;
    ntpSynced = true;
    update_time_label();
    Serial.printf("NTP-Zeit uebernommen: %02d:%02d\n", clockHour, clockMinute);
  }
}

static void time_plus_hour_cb(lv_event_t *e) { clockHour = (clockHour + 1) % 24; ntpSynced = false; update_time_label(); }
static void time_minus_hour_cb(lv_event_t *e) { clockHour = (clockHour + 23) % 24; ntpSynced = false; update_time_label(); }
static void time_plus_min_cb(lv_event_t *e) { clockMinute = (clockMinute + 1) % 60; ntpSynced = false; update_time_label(); }
static void time_minus_min_cb(lv_event_t *e) { clockMinute = (clockMinute + 59) % 60; ntpSynced = false; update_time_label(); }

// ============================================================
// Display-Drehung (180 Grad) -- per Software, nicht Hardware
// ============================================================
void applyDisplayRotation() {
  // Hardware-Rotation bewusst IMMER auf 1 (bekannt funktionierend).
  // Die eigentliche 180-Grad-Drehung passiert in my_disp_flush()
  // per Software-Spiegelung, da die Hardware-Rotationsmodi 0/2
  // bei diesem Treiber ohne zusaetzliche Offset-Konfiguration
  // nicht funktionieren.
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(NULL);
}

static void toggle_display_rotation_cb(lv_event_t *e) {
  displayFlipped = !displayFlipped;

  prefs.begin("display", false);
  prefs.putBool("flipped", displayFlipped);
  prefs.end();

  applyDisplayRotation();
}

// ============================================================
// Touch-Kalibrierung
// ============================================================
void calib_show_step();

static void calib_start_cb(lv_event_t *e) {
  calibrating = true;
  calibStep = 0;
  lv_scr_load(scr_calibration);
  calib_show_step();
}

void calib_show_step() {
  CalPoint p = calTargets[calibStep];
  lv_label_set_text_fmt(calib_label, "Kalibrierung %d/4\nBitte Kreuz berühren", calibStep + 1);
  lv_obj_set_pos(calib_crosshair_h, p.x - 12, p.y - 1);
  lv_obj_set_pos(calib_crosshair_v, p.x - 1, p.y - 12);
}

void calib_process() {
  static unsigned long stableStart = 0;
  static uint16_t lastX = 0, lastY = 0;

  uint16_t rawX, rawY;
  if (tft.getTouchRaw(&rawX, &rawY)) {
    if (abs((int)rawX - (int)lastX) < 50 && abs((int)rawY - (int)lastY) < 50 && stableStart != 0) {
      if (millis() - stableStart > 400) {
        calRawX[calibStep] = rawX;
        calRawY[calibStep] = rawY;
        calibStep++;
        stableStart = 0;

        if (calibStep >= 4) {
          float rawY_left  = (calRawY[0] + calRawY[2]) / 2.0f;
          float rawY_right = (calRawY[1] + calRawY[3]) / 2.0f;
          slopeY = (440.0f - 40.0f) / (rawY_right - rawY_left);
          interceptY = 40.0f - slopeY * rawY_left;

          float rawX_top    = (calRawX[0] + calRawX[1]) / 2.0f;
          float rawX_bottom = (calRawX[2] + calRawX[3]) / 2.0f;
          slopeX = (270.0f - 35.0f) / (rawX_bottom - rawX_top);
          interceptX = 35.0f - slopeX * rawX_top;

          calibrating = false;
          lv_label_set_text(calib_label, "Fertig!");
          delay(600);
          lv_scr_load(scr_settings);
          return;
        } else {
          calib_show_step();
        }
      }
    } else {
      stableStart = millis();
    }
    lastX = rawX;
    lastY = rawY;
  } else {
    stableStart = 0;
  }
}

void build_calibration_screen() {
  scr_calibration = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_calibration, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(scr_calibration, LV_OBJ_FLAG_SCROLLABLE);

  calib_label = lv_label_create(scr_calibration);
  lv_label_set_text(calib_label, "Kalibrierung");
  lv_obj_set_style_text_font(calib_label, &lv_font_montserrat_14_de, 0);
  lv_obj_set_style_text_align(calib_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(calib_label, LV_ALIGN_CENTER, 0, 0);

  calib_crosshair_h = lv_obj_create(scr_calibration);
  lv_obj_set_size(calib_crosshair_h, 24, 3);
  lv_obj_set_style_bg_color(calib_crosshair_h, lv_color_hex(0xFFEB3B), 0);
  lv_obj_set_style_border_width(calib_crosshair_h, 0, 0);

  calib_crosshair_v = lv_obj_create(scr_calibration);
  lv_obj_set_size(calib_crosshair_v, 3, 24);
  lv_obj_set_style_bg_color(calib_crosshair_v, lv_color_hex(0xFFEB3B), 0);
  lv_obj_set_style_border_width(calib_crosshair_v, 0, 0);
}

// ============================================================
// WLAN -- Netzwerksuche
// ============================================================
static void wifi_item_clicked_cb(lv_event_t *e) {
  lv_obj_t *btn = lv_event_get_target(e);
  const char *ssid = lv_list_get_btn_text(wifi_list_obj, btn);
  pendingSSID = String(ssid);

  lv_label_set_text(wifi_password_ssid_label, pendingSSID.c_str());
  lv_textarea_set_text(wifi_password_textarea, "");
  lv_label_set_text(wifi_password_status_label, "");
  lv_scr_load(scr_wifi_password);
}

static void wifi_scan_and_show_cb(lv_event_t *e) {
  lv_obj_clean(wifi_list_obj);
  lv_obj_t *scanning_lbl = lv_list_add_text(wifi_list_obj, "Suche läuft...");
  lv_obj_set_style_text_font(scanning_lbl, &lv_font_montserrat_14_de, 0);
  lv_scr_load(scr_wifi_list);
  lv_timer_handler(); // sofort anzeigen, bevor die (blockierende) Suche startet
  lv_refr_now(NULL);

  int n = WiFi.scanNetworks();

  lv_obj_clean(wifi_list_obj);
  if (n <= 0) {
    lv_list_add_text(wifi_list_obj, "Keine Netzwerke gefunden");
  } else {
    for (int i = 0; i < n; i++) {
      String label = WiFi.SSID(i);
      if (label.length() == 0) continue;
      lv_obj_t *btn = lv_list_add_btn(wifi_list_obj, LV_SYMBOL_WIFI, label.c_str());
      lv_obj_add_event_cb(btn, wifi_item_clicked_cb, LV_EVENT_CLICKED, NULL);
    }
  }
}

// ============================================================
// WLAN -- Passworteingabe + Verbinden
// ============================================================
static void wifi_connect_attempt() {
  String pass = lv_textarea_get_text(wifi_password_textarea);
  lv_label_set_text(wifi_password_status_label, "Verbinde...");
  lv_obj_set_style_text_color(wifi_password_status_label, lv_color_hex(0xFFC107), 0);

  WiFi.begin(pendingSSID.c_str(), pass.c_str());
  wifiConnecting = true;
  wifiConnectStart = millis();
  update_wifi_icon();
  update_settings_wifi_status();
}

static void wifi_connect_btn_cb(lv_event_t *e) {
  wifi_connect_attempt();
}

static void wifi_textarea_ready_cb(lv_event_t *e) {
  wifi_connect_attempt();
}

static void wifi_textarea_focus_cb(lv_event_t *e) {
  lv_keyboard_set_textarea(wifi_keyboard, wifi_password_textarea);
  lv_obj_clear_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// Zeigt IP- und MAC-Adresse an -- die MAC wird fuer eine feste
// DHCP-Reservierung im Router benoetigt.
void update_wifi_ip_mac_label() {
  if (!wifi_ip_mac_label) return;
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(wifi_ip_mac_label, "IP: %s   MAC: %s",
                           WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
  } else {
    lv_label_set_text_fmt(wifi_ip_mac_label, "MAC: %s (noch nicht verbunden)",
                           WiFi.macAddress().c_str());
  }
}

void build_wifi_list_screen() {
  scr_wifi_list = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_wifi_list, lv_color_hex(0x0A0A0A), 0);

  lv_obj_t *title = lv_label_create(scr_wifi_list);
  lv_label_set_text(title, "WLAN-Netzwerke");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20_de, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  wifi_list_obj = lv_list_create(scr_wifi_list);
  lv_obj_set_size(wifi_list_obj, 440, 200);
  lv_obj_align(wifi_list_obj, LV_ALIGN_TOP_MID, 0, 42);

  wifi_ip_mac_label = lv_label_create(scr_wifi_list);
  lv_obj_set_style_text_font(wifi_ip_mac_label, &lv_font_montserrat_14_de, 0);
  lv_obj_set_style_text_color(wifi_ip_mac_label, lv_color_hex(0x888888), 0);
  lv_obj_align(wifi_ip_mac_label, LV_ALIGN_TOP_MID, 0, 248);
  update_wifi_ip_mac_label();

  lv_obj_t *rescan_btn = lv_btn_create(scr_wifi_list);
  lv_obj_set_size(rescan_btn, 110, 36);
  lv_obj_align(rescan_btn, LV_ALIGN_BOTTOM_LEFT, 20, -8);
  lv_obj_add_event_cb(rescan_btn, wifi_scan_and_show_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *rescan_lbl = lv_label_create(rescan_btn);
  lv_label_set_text(rescan_lbl, "Neu suchen");
  lv_obj_center(rescan_lbl);

  lv_obj_t *back_btn = lv_btn_create(scr_wifi_list);
  lv_obj_set_size(back_btn, 100, 36);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -8);
  lv_obj_add_event_cb(back_btn, go_settings_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, "Zurück");
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14_de, 0);
  lv_obj_center(back_lbl);
}

void build_wifi_password_screen() {
  scr_wifi_password = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_wifi_password, lv_color_hex(0x0A0A0A), 0);

  wifi_password_ssid_label = lv_label_create(scr_wifi_password);
  lv_obj_set_style_text_font(wifi_password_ssid_label, &lv_font_montserrat_20_de, 0);
  lv_obj_align(wifi_password_ssid_label, LV_ALIGN_TOP_MID, 0, 6);

  wifi_password_textarea = lv_textarea_create(scr_wifi_password);
  // Klartext sichtbar statt Punkten -- einfacher zu kontrollieren
  // beim Tippen ueber die Bildschirmtastatur.
  lv_textarea_set_password_mode(wifi_password_textarea, false);
  lv_textarea_set_one_line(wifi_password_textarea, true);
  lv_textarea_set_placeholder_text(wifi_password_textarea, "Passwort");
  lv_obj_set_size(wifi_password_textarea, 300, 36);
  lv_obj_align(wifi_password_textarea, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_add_event_cb(wifi_password_textarea, wifi_textarea_focus_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(wifi_password_textarea, wifi_textarea_ready_cb, LV_EVENT_READY, NULL);

  lv_obj_t *connect_btn = lv_btn_create(scr_wifi_password);
  lv_obj_set_size(connect_btn, 110, 34);
  lv_obj_align(connect_btn, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_add_event_cb(connect_btn, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *connect_lbl = lv_label_create(connect_btn);
  lv_label_set_text(connect_lbl, "Verbinden");
  lv_obj_center(connect_lbl);

  wifi_password_status_label = lv_label_create(scr_wifi_password);
  lv_label_set_text(wifi_password_status_label, "");
  lv_obj_align(wifi_password_status_label, LV_ALIGN_TOP_MID, 0, 116);

  wifi_keyboard = lv_keyboard_create(scr_wifi_password);
  lv_obj_set_size(wifi_keyboard, 480, 150);
  lv_obj_align(wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *back_btn = lv_btn_create(scr_wifi_password);
  lv_obj_set_size(back_btn, 90, 30);
  lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -10, 6);
  lv_obj_add_event_cb(back_btn, go_settings_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, "Zurück");
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14_de, 0);
  lv_obj_center(back_lbl);
}

// ============================================================
// Einstellungsseite
// ============================================================
// ============================================================
// Kuehlschrank-Knoten -- Abfrage und Konfiguration per HTTP
// ============================================================
static void fridge_keyboard_hide_cb(lv_event_t *e) {
  lv_obj_add_flag(fridge_keyboard, LV_OBJ_FLAG_HIDDEN);
}
static void fridge_textarea_focus_cb(lv_event_t *e) {
  lv_obj_t *ta = lv_event_get_target(e);
  lv_keyboard_set_textarea(fridge_keyboard, ta);
  // IP-Adresse braucht Text+Punkte, Schwellwerte nur Zahlen -- Modus
  // passend zum jeweiligen Feld waehlen.
  if (ta == fridge_ip_textarea) {
    lv_keyboard_set_mode(fridge_keyboard, LV_KEYBOARD_MODE_NUMBER);
  } else {
    lv_keyboard_set_mode(fridge_keyboard, LV_KEYBOARD_MODE_NUMBER);
  }
  lv_obj_clear_flag(fridge_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void fridge_fetch_status_cb(lv_event_t *e) {
  String ip = lv_textarea_get_text(fridge_ip_textarea);
  if (ip.length() == 0) {
    lv_label_set_text(fridge_result_label, "Bitte IP-Adresse eingeben");
    lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xF44336), 0);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(fridge_result_label, "Kein WLAN verbunden");
    lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xF44336), 0);
    return;
  }

  lv_label_set_text(fridge_result_label, "Frage Knoten ab...");
  lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xFFC107), 0);
  lv_timer_handler(); // sofort anzeigen, bevor der (blockierende) Request startet
  lv_refr_now(NULL);

  HTTPClient http;
  String url = "http://" + ip + "/status";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      char buf[160];
      snprintf(buf, sizeof(buf), "Abluft: %.1fC  Innen: %.1fC  Gefrier: %.1fC  Luefter: %s",
               doc["abluft_c"].as<float>(), doc["innen_c"].as<float>(),
               doc["gefrier_c"].as<float>(), doc["luefter_an"].as<bool>() ? "AN" : "AUS");
      lv_label_set_text(fridge_status_label, buf);

      char onBuf[16], offBuf[16];
      snprintf(onBuf, sizeof(onBuf), "%.1f", doc["schwelle_ein"].as<float>());
      snprintf(offBuf, sizeof(offBuf), "%.1f", doc["schwelle_aus"].as<float>());
      lv_textarea_set_text(fridge_on_textarea, onBuf);
      lv_textarea_set_text(fridge_off_textarea, offBuf);

      lv_label_set_text(fridge_result_label, "Aktualisiert");
      lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0x4CAF50), 0);

      // Erfolgreiche IP merken
      fridgePrefs.begin("fridge", false);
      fridgePrefs.putString("ip", ip);
      fridgePrefs.end();
    } else {
      lv_label_set_text(fridge_result_label, "Antwort nicht lesbar (JSON-Fehler)");
      lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xF44336), 0);
    }
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), "Fehler: keine Antwort (Code %d)", code);
    lv_label_set_text(fridge_result_label, buf);
    lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xF44336), 0);
  }
  http.end();
}

static void fridge_save_thresholds_cb(lv_event_t *e) {
  String ip = lv_textarea_get_text(fridge_ip_textarea);
  String onStr = lv_textarea_get_text(fridge_on_textarea);
  String offStr = lv_textarea_get_text(fridge_off_textarea);

  if (ip.length() == 0 || onStr.length() == 0 || offStr.length() == 0) {
    lv_label_set_text(fridge_result_label, "Bitte alle Felder ausfuellen");
    lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xF44336), 0);
    return;
  }

  float onVal = onStr.toFloat();
  float offVal = offStr.toFloat();
  if (offVal >= onVal) {
    lv_label_set_text(fridge_result_label, "AUS muss kleiner als EIN sein");
    lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xF44336), 0);
    return;
  }

  lv_label_set_text(fridge_result_label, "Speichere...");
  lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xFFC107), 0);
  lv_timer_handler();
  lv_refr_now(NULL);

  HTTPClient http;
  String url = "http://" + ip + "/config";
  http.begin(url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "on=" + onStr + "&off=" + offStr;
  int code = http.POST(body);

  if (code == 303 || code == 200) {
    lv_label_set_text(fridge_result_label, "Gespeichert!");
    lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0x4CAF50), 0);
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), "Fehler beim Speichern (Code %d)", code);
    lv_label_set_text(fridge_result_label, buf);
    lv_obj_set_style_text_color(fridge_result_label, lv_color_hex(0xF44336), 0);
  }
  http.end();
}

static void go_kuehlschrank_cb(lv_event_t *e) {
  lv_scr_load(scr_kuehlschrank);
}

void build_kuehlschrank_screen() {
  scr_kuehlschrank = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_kuehlschrank, lv_color_hex(0x0A0A0A), 0);
  lv_obj_clear_flag(scr_kuehlschrank, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr_kuehlschrank);
  lv_label_set_text(title, "Kühlschrank");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20_de, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  // IP-Adresse
  lv_obj_t *ip_label = lv_label_create(scr_kuehlschrank);
  lv_label_set_text(ip_label, "IP:");
  lv_obj_set_pos(ip_label, 16, 40);

  fridge_ip_textarea = lv_textarea_create(scr_kuehlschrank);
  lv_textarea_set_one_line(fridge_ip_textarea, true);
  lv_textarea_set_placeholder_text(fridge_ip_textarea, "192.168.1.101");
  lv_obj_set_size(fridge_ip_textarea, 160, 32);
  lv_obj_set_pos(fridge_ip_textarea, 46, 34);
  lv_obj_add_event_cb(fridge_ip_textarea, fridge_textarea_focus_cb, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *fetch_btn = lv_btn_create(scr_kuehlschrank);
  lv_obj_set_size(fetch_btn, 110, 32);
  lv_obj_set_pos(fetch_btn, 220, 34);
  lv_obj_add_event_cb(fetch_btn, fridge_fetch_status_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *fetch_lbl = lv_label_create(fetch_btn);
  lv_label_set_text(fetch_lbl, "Abfragen");
  lv_obj_center(fetch_lbl);

  // Live-Status
  fridge_status_label = lv_label_create(scr_kuehlschrank);
  lv_label_set_text(fridge_status_label, "Noch keine Daten");
  lv_obj_set_style_text_font(fridge_status_label, &lv_font_montserrat_14_de, 0);
  lv_label_set_long_mode(fridge_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(fridge_status_label, 440);
  lv_obj_set_pos(fridge_status_label, 16, 76);

  // Schwellwerte
  lv_obj_t *on_label = lv_label_create(scr_kuehlschrank);
  lv_label_set_text(on_label, "Luefter EIN ab (C):");
  lv_obj_set_pos(on_label, 16, 118);
  fridge_on_textarea = lv_textarea_create(scr_kuehlschrank);
  lv_textarea_set_one_line(fridge_on_textarea, true);
  lv_obj_set_size(fridge_on_textarea, 90, 32);
  lv_obj_set_pos(fridge_on_textarea, 190, 112);
  lv_obj_add_event_cb(fridge_on_textarea, fridge_textarea_focus_cb, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *off_label = lv_label_create(scr_kuehlschrank);
  lv_label_set_text(off_label, "Luefter AUS unter (C):");
  lv_obj_set_pos(off_label, 16, 158);
  fridge_off_textarea = lv_textarea_create(scr_kuehlschrank);
  lv_textarea_set_one_line(fridge_off_textarea, true);
  lv_obj_set_size(fridge_off_textarea, 90, 32);
  lv_obj_set_pos(fridge_off_textarea, 190, 152);
  lv_obj_add_event_cb(fridge_off_textarea, fridge_textarea_focus_cb, LV_EVENT_FOCUSED, NULL);

  lv_obj_t *save_btn = lv_btn_create(scr_kuehlschrank);
  lv_obj_set_size(save_btn, 110, 32);
  lv_obj_set_pos(save_btn, 300, 132);
  lv_obj_add_event_cb(save_btn, fridge_save_thresholds_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *save_lbl = lv_label_create(save_btn);
  lv_label_set_text(save_lbl, "Speichern");
  lv_obj_center(save_lbl);

  fridge_result_label = lv_label_create(scr_kuehlschrank);
  lv_label_set_text(fridge_result_label, "");
  lv_obj_set_pos(fridge_result_label, 16, 198);

  fridge_keyboard = lv_keyboard_create(scr_kuehlschrank);
  lv_obj_set_size(fridge_keyboard, 480, 150);
  lv_obj_align(fridge_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(fridge_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(fridge_keyboard, fridge_keyboard_hide_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(fridge_keyboard, fridge_keyboard_hide_cb, LV_EVENT_CANCEL, NULL);

  lv_obj_t *back_btn = lv_btn_create(scr_kuehlschrank);
  lv_obj_set_size(back_btn, 90, 30);
  lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -10, 8);
  lv_obj_add_event_cb(back_btn, go_settings_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, "Zurück");
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14_de, 0);
  lv_obj_center(back_lbl);

  // Zuletzt gespeicherte IP vorbelegen
  fridgePrefs.begin("fridge", true);
  String savedIp = fridgePrefs.getString("ip", "");
  fridgePrefs.end();
  if (savedIp.length() > 0) {
    lv_textarea_set_text(fridge_ip_textarea, savedIp.c_str());
  }
}

// ============================================================
// Diagnose/Checks -- Sofortabfrage bekannter Knoten per Knopfdruck
// ============================================================
// Zweck: Status eines Knotens direkt am Display pruefen (Erfolg/Fehler
// im Klartext), ohne IP am Handy-Browser abzutippen.

static void check_frischwasser_cb(lv_event_t *e) {
  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(check_frischwasser_result, "Kein WLAN verbunden");
    lv_obj_set_style_text_color(check_frischwasser_result, lv_color_hex(0xF44336), 0);
    return;
  }
  lv_label_set_text(check_frischwasser_result, "Frage ab...");
  lv_obj_set_style_text_color(check_frischwasser_result, lv_color_hex(0xFFC107), 0);
  lv_timer_handler(); // sofort anzeigen, bevor der (blockierende) Request startet
  lv_refr_now(NULL);

  HTTPClient http;
  String url = String("http://") + FRISCHWASSER_HOST + "/status";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      char buf[96];
      snprintf(buf, sizeof(buf), "OK -- S1:%s S2:%s S3:%s",
               doc["sensor1"]["erkannt"].as<bool>() ? "AN" : "AUS",
               doc["sensor2"]["erkannt"].as<bool>() ? "AN" : "AUS",
               doc["sensor3"]["erkannt"].as<bool>() ? "AN" : "AUS");
      lv_label_set_text(check_frischwasser_result, buf);
      lv_obj_set_style_text_color(check_frischwasser_result, lv_color_hex(0x4CAF50), 0);
    } else {
      lv_label_set_text(check_frischwasser_result, "Antwort nicht lesbar (JSON-Fehler)");
      lv_obj_set_style_text_color(check_frischwasser_result, lv_color_hex(0xF44336), 0);
    }
  } else {
    // Freien Heap mit anzeigen (28.08.2026) -- Diagnose-Hilfe fuer den
    // Verdacht auf Speicherknappheit als Ursache von Code -1, auch ohne
    // Serial-Monitor sichtbar.
    char buf[72];
    snprintf(buf, sizeof(buf), "Fehler: keine Antwort (Code %d, Heap %u)", code, ESP.getFreeHeap());
    lv_label_set_text(check_frischwasser_result, buf);
    lv_obj_set_style_text_color(check_frischwasser_result, lv_color_hex(0xF44336), 0);
  }
  http.end();
}

static void check_ksabluft_cb(lv_event_t *e) {
  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(check_ksabluft_result, "Kein WLAN verbunden");
    lv_obj_set_style_text_color(check_ksabluft_result, lv_color_hex(0xF44336), 0);
    return;
  }
  lv_label_set_text(check_ksabluft_result, "Frage ab...");
  lv_obj_set_style_text_color(check_ksabluft_result, lv_color_hex(0xFFC107), 0);
  lv_timer_handler();
  lv_refr_now(NULL);

  HTTPClient http;
  String url = String("http://") + KSABLUFT_HOST + "/status";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      char buf[96];
      if (!doc["abluft_temp"].isNull() && !doc["abluft_humidity"].isNull()) {
        snprintf(buf, sizeof(buf), "OK -- %.1fC  %.0f%%",
                 doc["abluft_temp"].as<float>(), doc["abluft_humidity"].as<float>());
      } else {
        snprintf(buf, sizeof(buf), "OK -- Antwort ok, aber Sensor liefert null");
      }
      lv_label_set_text(check_ksabluft_result, buf);
      lv_obj_set_style_text_color(check_ksabluft_result, lv_color_hex(0x4CAF50), 0);
    } else {
      lv_label_set_text(check_ksabluft_result, "Antwort nicht lesbar (JSON-Fehler)");
      lv_obj_set_style_text_color(check_ksabluft_result, lv_color_hex(0xF44336), 0);
    }
  } else {
    char buf[48];
    snprintf(buf, sizeof(buf), "Fehler: keine Antwort (Code %d)", code);
    lv_label_set_text(check_ksabluft_result, buf);
    lv_obj_set_style_text_color(check_ksabluft_result, lv_color_hex(0xF44336), 0);
  }
  http.end();
}

static void check_kuehlschrank_cb(lv_event_t *e) {
  fridgePrefs.begin("fridge", true);
  String ip = fridgePrefs.getString("ip", "");
  fridgePrefs.end();

  if (ip.length() == 0) {
    lv_label_set_text(check_kuehlschrank_result, "Keine IP konfiguriert (siehe Kuehlschrank-Seite)");
    lv_obj_set_style_text_color(check_kuehlschrank_result, lv_color_hex(0xF44336), 0);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(check_kuehlschrank_result, "Kein WLAN verbunden");
    lv_obj_set_style_text_color(check_kuehlschrank_result, lv_color_hex(0xF44336), 0);
    return;
  }
  lv_label_set_text(check_kuehlschrank_result, "Frage ab...");
  lv_obj_set_style_text_color(check_kuehlschrank_result, lv_color_hex(0xFFC107), 0);
  lv_timer_handler();
  lv_refr_now(NULL);

  HTTPClient http;
  String url = "http://" + ip + "/status";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      char buf[96];
      snprintf(buf, sizeof(buf), "OK -- Abluft:%.1fC Innen:%.1fC Gefrier:%.1fC",
               doc["abluft_c"].as<float>(), doc["innen_c"].as<float>(),
               doc["gefrier_c"].as<float>());
      lv_label_set_text(check_kuehlschrank_result, buf);
      lv_obj_set_style_text_color(check_kuehlschrank_result, lv_color_hex(0x4CAF50), 0);
    } else {
      lv_label_set_text(check_kuehlschrank_result, "Antwort nicht lesbar (JSON-Fehler)");
      lv_obj_set_style_text_color(check_kuehlschrank_result, lv_color_hex(0xF44336), 0);
    }
  } else {
    char buf[48];
    snprintf(buf, sizeof(buf), "Fehler: keine Antwort (Code %d)", code);
    lv_label_set_text(check_kuehlschrank_result, buf);
    lv_obj_set_style_text_color(check_kuehlschrank_result, lv_color_hex(0xF44336), 0);
  }
  http.end();
}

// Vorab deklariert, damit go_checks_cb() (steht davor im Code) die Seite
// bei Bedarf anlegen kann.
void build_checks_screen();

// VERDACHT (28.08.2026): Die Diagnose-Seite wurde als staendig im
// Speicher gehaltener Screen angelegt (build_checks_screen() lief schon
// in setup()). Danach fielen sowohl die automatische Frischwasser-/
// KS-Abluft-Aktualisierung als auch der manuelle "Pruefen"-Button
// reproduzierbar auf HTTP-Code -1 (Connection Refused) zurueck --
// bereits zum zweiten Mal exakt in Korrelation mit dem Hinzufuegen
// dieser Seite (vorher, ohne sie: funktionierte). Naheliegende, aber
// NICHT verifizierte Erklaerung: das Geraet laeuft nahe an seiner
// Speichergrenze (siehe DRAM-Overflow-Vorfall vom 27.08.2026), und die
// zusaetzlichen ~14 LVGL-Objekte dieser Seite (staendig im Speicher,
// auch wenn nie geoeffnet) reichen aus, um den fuer WLAN/HTTP-Verbindungen
// noetigen Speicher so weit zu verknappen, dass neue TCP-Verbindungen
// fehlschlagen. Gegenmassnahme: die Seite wird jetzt erst beim ersten
// Oeffnen angelegt (nicht mehr in setup()), damit sie im Normalbetrieb
// (Seite nie geoeffnet) keinen Speicher kostet. Das ist ein Test dieser
// Hypothese, keine bestaetigte Loesung -- bitte nach dem Flashen
// beobachten, ob die Kacheln jetzt wieder ohne Oeffnen der Diagnose-Seite
// stabil bleiben.
static void go_checks_cb(lv_event_t *e) {
  if (!checksScreenBuilt) {
    build_checks_screen();
    checksScreenBuilt = true;
    Serial.printf("Diagnose-Seite angelegt. Freier Heap danach: %u Bytes\n", ESP.getFreeHeap());
  }
  lv_scr_load(scr_checks);
}

// Hilfsfunktion: eine Zeile "Name (IP)" + "Pruefen"-Knopf + Ergebnis-
// Label darunter. Gibt das Ergebnis-Label zurueck, damit der Aufrufer
// es global merken kann.
lv_obj_t *add_check_row(lv_obj_t *parent, int y, const char *name_ip,
                         lv_event_cb_t check_cb) {
  lv_obj_t *name_label = lv_label_create(parent);
  lv_label_set_text(name_label, name_ip);
  lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14_de, 0);
  lv_obj_set_pos(name_label, 16, y);

  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 90, 34);
  lv_obj_set_pos(btn, 330, y - 6);
  lv_obj_add_event_cb(btn, check_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_lbl = lv_label_create(btn);
  lv_label_set_text(btn_lbl, "Prüfen");
  lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_14_de, 0);
  lv_obj_center(btn_lbl);

  lv_obj_t *result_label = lv_label_create(parent);
  lv_label_set_text(result_label, "Noch nicht geprueft");
  lv_obj_set_style_text_font(result_label, &lv_font_montserrat_14_de, 0);
  lv_obj_set_style_text_color(result_label, lv_color_hex(0x888888), 0);
  lv_label_set_long_mode(result_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(result_label, 440);
  lv_obj_set_pos(result_label, 16, y + 26);

  return result_label;
}

void build_checks_screen() {
  scr_checks = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_checks, lv_color_hex(0x0A0A0A), 0);
  lv_obj_clear_flag(scr_checks, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr_checks);
  lv_label_set_text(title, "Diagnose -- Knoten pruefen");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20_de, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  check_frischwasser_result = add_check_row(scr_checks, 60,
    "Frischwasser-Node (192.168.1.197)", check_frischwasser_cb);
  check_ksabluft_result = add_check_row(scr_checks, 130,
    "Lüfter-Node / KS-Abluft (192.168.1.228)", check_ksabluft_cb);
  check_kuehlschrank_result = add_check_row(scr_checks, 200,
    "Kühlschrank-Node (konfigurierte IP)", check_kuehlschrank_cb);

  lv_obj_t *back_btn = lv_btn_create(scr_checks);
  lv_obj_set_size(back_btn, 90, 30);
  lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -10, 8);
  lv_obj_add_event_cb(back_btn, go_home_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, "Zurück");
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14_de, 0);
  lv_obj_center(back_lbl);
}

void build_settings_screen() {
  scr_settings = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_settings, lv_color_hex(0x0A0A0A), 0);
  lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr_settings);
  lv_label_set_text(title, "Einstellungen");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20_de, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  int y = 38;
  const int rowH = 46;

  // --- WLAN ---
  lv_obj_t *wifi_label = lv_label_create(scr_settings);
  lv_label_set_text(wifi_label, "WLAN:");
  lv_obj_set_pos(wifi_label, 20, y + 8);

  settings_wifi_status = lv_label_create(scr_settings);
  lv_obj_set_pos(settings_wifi_status, 90, y + 8);
  update_settings_wifi_status();

  lv_obj_t *wifi_cfg_btn = lv_btn_create(scr_settings);
  lv_obj_set_size(wifi_cfg_btn, 130, 36);
  lv_obj_set_pos(wifi_cfg_btn, 320, y);
  lv_obj_add_event_cb(wifi_cfg_btn, wifi_scan_and_show_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *wifi_cfg_lbl = lv_label_create(wifi_cfg_btn);
  lv_label_set_text(wifi_cfg_lbl, "Konfigurieren");
  lv_obj_center(wifi_cfg_lbl);
  y += rowH;

  // --- Touch-Kalibrierung ---
  lv_obj_t *calib_row_label = lv_label_create(scr_settings);
  lv_label_set_text(calib_row_label, "Touch:");
  lv_obj_set_pos(calib_row_label, 20, y + 8);

  lv_obj_t *calib_btn = lv_btn_create(scr_settings);
  lv_obj_set_size(calib_btn, 160, 36);
  lv_obj_set_pos(calib_btn, 110, y);
  lv_obj_add_event_cb(calib_btn, calib_start_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *calib_btn_label = lv_label_create(calib_btn);
  lv_label_set_text(calib_btn_label, "Neu kalibrieren");
  lv_obj_center(calib_btn_label);
  y += rowH;

  // --- Display-Drehung ---
  lv_obj_t *rotation_row_label = lv_label_create(scr_settings);
  lv_label_set_text(rotation_row_label, "Display:");
  lv_obj_set_style_text_font(rotation_row_label, &lv_font_montserrat_14_de, 0);
  lv_obj_set_pos(rotation_row_label, 20, y + 8);

  lv_obj_t *rotation_btn = lv_btn_create(scr_settings);
  lv_obj_set_size(rotation_btn, 160, 36);
  lv_obj_set_pos(rotation_btn, 110, y);
  lv_obj_add_event_cb(rotation_btn, toggle_display_rotation_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *rotation_btn_label = lv_label_create(rotation_btn);
  lv_label_set_text(rotation_btn_label, "180° drehen");
  lv_obj_set_style_text_font(rotation_btn_label, &lv_font_montserrat_14_de, 0);
  lv_obj_center(rotation_btn_label);
  y += rowH;

  // --- Kuehlschrank-Knoten ---
  lv_obj_t *fridge_row_label = lv_label_create(scr_settings);
  lv_label_set_text(fridge_row_label, "Kühlschrank:");
  lv_obj_set_style_text_font(fridge_row_label, &lv_font_montserrat_14_de, 0);
  lv_obj_set_pos(fridge_row_label, 20, y + 8);

  lv_obj_t *fridge_btn = lv_btn_create(scr_settings);
  lv_obj_set_size(fridge_btn, 160, 36);
  lv_obj_set_pos(fridge_btn, 140, y);
  lv_obj_add_event_cb(fridge_btn, go_kuehlschrank_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *fridge_btn_label = lv_label_create(fridge_btn);
  lv_label_set_text(fridge_btn_label, "Einstellungen");
  lv_obj_center(fridge_btn_label);
  y += rowH;

  lv_obj_t *time_row_label = lv_label_create(scr_settings);
  lv_label_set_text(time_row_label, "Uhrzeit:");
  lv_obj_set_pos(time_row_label, 20, y + 10);

  time_label_settings = lv_label_create(scr_settings);
  lv_obj_set_style_text_font(time_label_settings, &lv_font_montserrat_20_de, 0);
  lv_obj_set_pos(time_label_settings, 110, y + 6);

  time_source_label = lv_label_create(scr_settings);
  lv_obj_set_style_text_font(time_source_label, &lv_font_montserrat_14_de, 0);
  lv_obj_set_pos(time_source_label, 340, y + 12);

  update_time_label();

  lv_obj_t *h_plus = lv_btn_create(scr_settings);
  lv_obj_set_size(h_plus, 30, 30);
  lv_obj_set_pos(h_plus, 190, y + 4);
  lv_obj_add_event_cb(h_plus, time_plus_hour_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *h_plus_l = lv_label_create(h_plus); lv_label_set_text(h_plus_l, "+H"); lv_obj_center(h_plus_l);

  lv_obj_t *h_minus = lv_btn_create(scr_settings);
  lv_obj_set_size(h_minus, 30, 30);
  lv_obj_set_pos(h_minus, 225, y + 4);
  lv_obj_add_event_cb(h_minus, time_minus_hour_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *h_minus_l = lv_label_create(h_minus); lv_label_set_text(h_minus_l, "-H"); lv_obj_center(h_minus_l);

  lv_obj_t *m_plus = lv_btn_create(scr_settings);
  lv_obj_set_size(m_plus, 30, 30);
  lv_obj_set_pos(m_plus, 265, y + 4);
  lv_obj_add_event_cb(m_plus, time_plus_min_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *m_plus_l = lv_label_create(m_plus); lv_label_set_text(m_plus_l, "+M"); lv_obj_center(m_plus_l);

  lv_obj_t *m_minus = lv_btn_create(scr_settings);
  lv_obj_set_size(m_minus, 30, 30);
  lv_obj_set_pos(m_minus, 300, y + 4);
  lv_obj_add_event_cb(m_minus, time_minus_min_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *m_minus_l = lv_label_create(m_minus); lv_label_set_text(m_minus_l, "-M"); lv_obj_center(m_minus_l);

  // --- Zurueck ---
  lv_obj_t *back_btn = lv_btn_create(scr_settings);
  lv_obj_set_size(back_btn, 100, 36);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_add_event_cb(back_btn, go_home_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "Zurück");
  lv_obj_set_style_text_font(back_label, &lv_font_montserrat_14_de, 0);
  lv_obj_center(back_label);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Motorhome Homescreen v3 (WLAN) ===");

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  tft.init();
  // Gespeicherte Display-Ausrichtung laden (die eigentliche Drehung
  // passiert per Software in my_disp_flush, hier nur Wert laden --
  // applyDisplayRotation() darf hier NICHT aufgerufen werden, da es
  // LVGL-Funktionen nutzt, LVGL aber erst gleich unten initialisiert wird)
  prefs.begin("display", true);
  displayFlipped = prefs.getBool("flipped", false);
  prefs.end();
  tft.setRotation(1);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, 480 * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 480;
  disp_drv.ver_res = 320;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  build_home_screen();
  build_vedirect_detail_screen();
  build_settings_screen();
  build_calibration_screen();
  build_wifi_list_screen();
  build_wifi_password_screen();
  build_kuehlschrank_screen();
  // build_checks_screen() bewusst NICHT hier -- wird erst beim ersten
  // Oeffnen der Diagnose-Seite angelegt, siehe go_checks_cb() und den
  // Kommentar dort (Verdacht auf Speicherknappheit, 28.08.2026).

  Serial.printf("Setup: alle Standard-Screens angelegt. Freier Heap: %u Bytes\n", ESP.getFreeHeap());

  // VE.Direct UART starten (GPIO39, RX-only, 19200 Baud 8N1 --
  // Standard-Baudrate aller VE.Direct-Geraete)
  VeSerial.begin(19200, SERIAL_8N1, 39, -1);

  // Zweiter VE.Direct-Empfang fuer den Batterie-Shunt (GPIO35,
  // RX-only, ebenfalls 19200 Baud 8N1)
  ShuntSerial.begin(19200, SERIAL_8N1, 35, -1);

  // I2C fuer SHT31 (RT/H) starten
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  lv_scr_load(scr_home);
  update_wifi_icon();

  lastMinuteMillis = millis();

  // Gespeicherte WLAN-Zugangsdaten laden und automatisch verbinden,
  // falls vorhanden (ANNAHME: Klartext-Speicherung im NVS-Flash --
  // ausreichend fuer dieses Projekt, aber nicht fuer sicherheits-
  // kritische Anwendungen gedacht).
  prefs.begin("wifi", true); // read-only
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();

  if (savedSSID.length() > 0) {
    Serial.printf("Gespeichertes WLAN gefunden: %s -- versuche Verbindung...\n", savedSSID.c_str());
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    wifiConnecting = true;
    wifiConnectStart = millis();
    update_wifi_icon();
  }

  Serial.println("Setup abgeschlossen.");
}

void loop() {
  lv_timer_handler();

  handleVeDirectSerial();
  handleShuntSerial();
  handleRthSensor();
  handleKsAbluftSensor();
  updateKsAbluftProgressBar();
  handleFrischwasserSensor();

  if (calibrating) {
    calib_process();
  }

  // WLAN-Verbindungsversuch ueberwachen
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;
      Serial.printf("WLAN verbunden: %s\n", WiFi.SSID().c_str());
      Serial.printf("IP-Adresse: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("MAC-Adresse: %s (fuer DHCP-Reservierung im Router)\n", WiFi.macAddress().c_str());

      // Zugangsdaten speichern (nur wenn ueber die Passwort-Eingabe
      // ausgeloest -- pendingSSID ist dann gesetzt)
      if (pendingSSID.length() > 0 && pendingSSID == WiFi.SSID()) {
        prefs.begin("wifi", false);
        prefs.putString("ssid", WiFi.SSID());
        prefs.putString("pass", lv_textarea_get_text(wifi_password_textarea));
        prefs.end();
        pendingSSID = "";
      }

      update_wifi_icon();
      update_settings_wifi_status();
      update_wifi_ip_mac_label();
      if (wifi_password_status_label) {
        lv_label_set_text(wifi_password_status_label, "Verbunden!");
        lv_obj_set_style_text_color(wifi_password_status_label, lv_color_hex(0x4CAF50), 0);
      }

      // Sobald WLAN steht: NTP-Zeitsynchronisation anstossen
      startNtpSync();
    } else if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT) {
      wifiConnecting = false;
      Serial.println("WLAN-Verbindung fehlgeschlagen (Timeout).");
      update_wifi_icon();
      update_settings_wifi_status();
      if (wifi_password_status_label) {
        lv_label_set_text(wifi_password_status_label, "Fehlgeschlagen");
        lv_obj_set_style_text_color(wifi_password_status_label, lv_color_hex(0xF44336), 0);
      }
    }
  }

  // Software-Uhr: nur hochzaehlen, wenn (noch) keine NTP-Synchronisation
  // erfolgt ist. Sobald NTP synchronisiert hat, kommt die Zeit direkt
  // aus checkNtpResult() -- das manuelle Hochzaehlen wuerde sonst mit
  // der echten Systemzeit driften.
  if (!ntpSynced && millis() - lastMinuteMillis >= 60000) {
    lastMinuteMillis += 60000;
    clockMinute++;
    if (clockMinute >= 60) { clockMinute = 0; clockHour = (clockHour + 1) % 24; }
    update_time_label();
  }

  // NTP-Ergebnis regelmaessig pruefen (auch nach erfolgreicher erster
  // Synchronisation, damit sich die angezeigte Minute weiter aktualisiert)
  if (millis() - lastNtpCheckMillis >= 20000) {
    lastNtpCheckMillis = millis();
    checkNtpResult();
  }
  static unsigned long lastResyncMillis = 0;
  if (ntpSynced && millis() - lastResyncMillis >= 6UL * 60 * 60 * 1000) {
    lastResyncMillis = millis();
    startNtpSync();
  }

  delay(5);
}
