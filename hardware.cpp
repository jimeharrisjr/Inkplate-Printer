#include "hardware.h"

#ifdef INKPLATE_MODEL_MONO
static Inkplate display(INKPLATE_3BIT);
#else
static Inkplate display;
#endif

bool hwInit() {
    display.begin();
    display.clearDisplay();
    return true;
}

Inkplate& hwDisplay() {
    return display;
}

bool hwInitSD() {
    if (display.sdCardInit()) {
        Serial.println("[BOOT] SD card initialized");
        return true;
    }
    Serial.println("[BOOT] ERROR: SD card init failed");
    return false;
}

bool hwReadTouchpad(uint8_t pad) {
    return display.readTouchpad(pad) != 0;
}

void hwFullRefresh() {
    display.display();
}

bool hwPartialRefresh() {
#ifdef INKPLATE_MODEL_MONO
    display.partialUpdate();
    return true;
#else
    return false;
#endif
}

void hwSetDisplayMode1Bit(bool is1Bit) {
#ifdef INKPLATE_MODEL_MONO
    display.setDisplayMode(is1Bit ? INKPLATE_1BIT : INKPLATE_3BIT);
#endif
    // Color model has no mode switching
}

void hwClearDisplay() {
    display.clearDisplay();
}

const char* hwModelName() {
#ifdef INKPLATE_MODEL_MONO
    return "Inkplate 6 Mono (800x600)";
#else
    return "Inkplate 6 Color (600x448)";
#endif
}
