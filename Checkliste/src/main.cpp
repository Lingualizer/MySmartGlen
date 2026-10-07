#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_3C.h>
#include <epd3c/GxEPD2_290_Z13c.h>

namespace {

constexpr uint8_t PIN_EPD_CS = 5;
constexpr uint8_t PIN_EPD_DC = 17;
constexpr uint8_t PIN_EPD_RST = 16;
constexpr uint8_t PIN_EPD_BUSY = 4;

constexpr unsigned long DEBOUNCE_MS = 35;
constexpr unsigned long LONG_PRESS_MS = 1200;
constexpr size_t VISIBLE_ITEMS = 3;
constexpr int16_t LIST_TOP = 38;
constexpr int16_t LINE_HEIGHT = 16;

GxEPD2_3C<GxEPD2_290_Z13c, GxEPD2_290_Z13c::HEIGHT> display(
  GxEPD2_290_Z13c(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

struct Button {
  uint8_t pin;
  bool stableState;
  bool lastReading;
  unsigned long lastChange;
};

struct ChecklistItem {
  const char *label;
  const char *secondLine;
  bool checked;
};

ChecklistItem checklist[] = {
    {"Hocker", nullptr, false},
    {"Tueren/Schubladen", "gesichert", false},
    {"Kuehlschrank", "umgestellt", false},
};
constexpr size_t CHECKLIST_COUNT = sizeof(checklist) / sizeof(checklist[0]);
static_assert(CHECKLIST_COUNT > 0, "The checklist must contain at least one item");

Button buttonUp = {33, HIGH, HIGH, 0};
Button buttonDown = {32, HIGH, HIGH, 0};
Button buttonSelect = {25, HIGH, HIGH, 0};
size_t cursorIndex = 0;
size_t firstVisibleItem = 0;
unsigned long selectPressStarted = 0;
bool longPressHandled = false;

enum class ButtonEvent {
  None,
  Pressed,
  Released,
};

bool allChecked() {
  for (const ChecklistItem &item : checklist) {
    if (!item.checked) {
      return false;
    }
  }
  return true;
}

ButtonEvent updateButton(Button &button, unsigned long now) {
  const bool reading = digitalRead(button.pin);
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.lastChange = now;
  }

  if (now - button.lastChange < DEBOUNCE_MS || reading == button.stableState) {
    return ButtonEvent::None;
  }

  button.stableState = reading;
  return reading == LOW ? ButtonEvent::Pressed : ButtonEvent::Released;
}

void moveCursor(int direction) {
  if (direction > 0) {
    cursorIndex = (cursorIndex + 1) % CHECKLIST_COUNT;
  } else {
    cursorIndex = (cursorIndex + CHECKLIST_COUNT - 1) % CHECKLIST_COUNT;
  }

  if (cursorIndex < firstVisibleItem) {
    firstVisibleItem = cursorIndex;
  } else if (cursorIndex >= firstVisibleItem + VISIBLE_ITEMS) {
    firstVisibleItem = cursorIndex - VISIBLE_ITEMS + 1;
  }
}

void drawChecklist() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(nullptr);

    display.setTextSize(4);
    display.setCursor(8, 0);
    display.print(allChecked() ? "BEREIT" : "ABFAHRT");
    display.drawLine(8, 35, 288, 35, GxEPD_BLACK);

    display.setTextSize(2);
    const size_t visibleCount = CHECKLIST_COUNT - firstVisibleItem < VISIBLE_ITEMS
        ? CHECKLIST_COUNT - firstVisibleItem
        : VISIBLE_ITEMS;
    int16_t textY = LIST_TOP;
    for (size_t visibleIndex = 0; visibleIndex < visibleCount; ++visibleIndex) {
      const size_t itemIndex = firstVisibleItem + visibleIndex;
      const ChecklistItem &item = checklist[itemIndex];
      const int16_t boxY = textY + 2;
      display.setCursor(8, textY);
      display.print(itemIndex == cursorIndex ? ">" : " ");
      display.drawRect(26, boxY, 12, 12, GxEPD_BLACK);
      if (item.checked) {
        display.drawLine(28, boxY + 5, 31, boxY + 8, GxEPD_BLACK);
        display.drawLine(31, boxY + 8, 36, boxY + 2, GxEPD_BLACK);
      }
      display.setCursor(46, textY);
      display.print(item.label);
      if (item.secondLine != nullptr) {
        textY += LINE_HEIGHT;
        display.setCursor(46, textY);
        display.print(item.secondLine);
      }
      textY += LINE_HEIGHT;
    }

    display.setTextSize(2);
    display.setCursor(250, 6);
    display.print(cursorIndex + 1);
    display.print("/");
    display.print(CHECKLIST_COUNT);
  } while (display.nextPage());
}

void resetChecklist() {
  for (ChecklistItem &item : checklist) {
    item.checked = false;
  }
  cursorIndex = 0;
  firstVisibleItem = 0;
  drawChecklist();
}

}  // namespace

void setup() {
  Serial.begin(115200);

  for (Button *button : {&buttonUp, &buttonDown, &buttonSelect}) {
    pinMode(button->pin, INPUT_PULLUP);
    button->stableState = digitalRead(button->pin);
    button->lastReading = button->stableState;
  }

  SPI.begin(18, 19, 23, PIN_EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(1);
  drawChecklist();
}

void loop() {
  const unsigned long now = millis();
  bool changed = false;

  if (updateButton(buttonUp, now) == ButtonEvent::Pressed) {
    moveCursor(-1);
    changed = true;
  }

  if (updateButton(buttonDown, now) == ButtonEvent::Pressed) {
    moveCursor(1);
    changed = true;
  }

  const ButtonEvent selectEvent = updateButton(buttonSelect, now);
  if (selectEvent == ButtonEvent::Pressed) {
    selectPressStarted = now;
    longPressHandled = false;
  } else if (selectEvent == ButtonEvent::Released) {
    if (!longPressHandled) {
      checklist[cursorIndex].checked = !checklist[cursorIndex].checked;
      changed = true;
    }
    longPressHandled = false;
  }

  if (buttonSelect.stableState == LOW && !longPressHandled &&
      now - selectPressStarted >= LONG_PRESS_MS) {
    resetChecklist();
    longPressHandled = true;
    changed = false;
  } else if (changed) {
    drawChecklist();
  }
}