// Coffee Station — reads weight from a Varia AKU scale over BLE on a LOLIN (Wemos) S3 Mini.
//
// Serial monitor (115200):
//   prints weight in grams whenever it changes
//   type  t = tare, s = timer start, p = timer stop, r = timer reset
// BOOT button (GPIO0): single click = tare, double click = start timer
// Onboard RGB LED (GPIO47): blue = searching, green = connected

#include <Arduino.h>
#include "VariaAku.h"

static constexpr uint8_t kButtonPin = 0;   // BOOT button
static constexpr uint8_t kRgbLedPin = 47;  // WS2812 on the S3 Mini

static constexpr uint32_t kDebounceMs = 30;
static constexpr uint32_t kDoubleClickMs = 350;  // max gap between the two clicks

VariaAku scale;

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWrite(kRgbLedPin, r, g, b);
}

static void onSingleClick() {
    Serial.println(scale.tare() ? "Tare sent" : "Tare failed (not connected)");
}

static void onDoubleClick() {
    Serial.println(scale.timerStart() ? "Timer start sent" : "Timer start failed (not connected)");
}

// Detects single vs double clicks on the BOOT button (active LOW).
// A single click is reported only after the double-click window expires.
static void pollButton() {
    static bool stablePressed = false;
    static bool lastRaw = false;
    static uint32_t lastChangeMs = 0;
    static uint8_t clicks = 0;
    static uint32_t lastReleaseMs = 0;

    uint32_t now = millis();
    bool raw = digitalRead(kButtonPin) == LOW;

    if (raw != lastRaw) {
        lastRaw = raw;
        lastChangeMs = now;
    }

    if (now - lastChangeMs >= kDebounceMs && raw != stablePressed) {
        stablePressed = raw;
        if (!stablePressed) {  // released -> one click completed
            clicks++;
            lastReleaseMs = now;
            if (clicks >= 2) {
                clicks = 0;
                onDoubleClick();
            }
        }
    }

    if (clicks == 1 && !stablePressed && now - lastReleaseMs > kDoubleClickMs) {
        clicks = 0;
        onSingleClick();
    }
}

void setup() {
    Serial.begin(115200);
    delay(1500);  // give USB CDC time to enumerate
    Serial.println("\n=== Coffee Station: Varia AKU reader ===");

    pinMode(kButtonPin, INPUT_PULLUP);
    setLed(0, 0, 20);

    scale.onConnectionChange([](bool connected) {
        Serial.println(connected ? "Scale connected" : "Scale disconnected");
    });

    scale.onWeight([](float grams) {
        static float last = NAN;
        if (isnan(last) || fabsf(grams - last) >= 0.05f) {
            Serial.printf("Weight: %.1f g\n", grams);
            last = grams;
        }
    });

    scale.begin();
}

void loop() {
    scale.loop();

    // Status LED
    static bool lastConnected = false;
    bool connected = scale.isConnected();
    if (connected != lastConnected) {
        connected ? setLed(0, 20, 0) : setLed(0, 0, 20);
        lastConnected = connected;
    }

    pollButton();

    // Serial commands
    while (Serial.available()) {
        switch (Serial.read()) {
            case 't': Serial.println(scale.tare() ? "Tare sent" : "Not connected"); break;
            case 's': Serial.println(scale.timerStart() ? "Timer start" : "Not connected"); break;
            case 'p': Serial.println(scale.timerStop() ? "Timer stop" : "Not connected"); break;
            case 'r': Serial.println(scale.timerReset() ? "Timer reset" : "Not connected"); break;
            default: break;
        }
    }

    delay(5);
}
