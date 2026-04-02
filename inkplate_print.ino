#include "config.h"
#include "hardware.h"
#include "wifi_manager.h"
#include "ipp_server.h"
#include "sd_storage.h"
#include "navigation.h"
#include "display.h"

static bool sdReady = false;

// Draw the boot screen showing model and SD status.
static void showBootScreen() {
    Inkplate& disp = hwDisplay();
    hwClearDisplay();

    disp.setTextColor(COLOR_BLACK, COLOR_WHITE);

    disp.setTextSize(3);
    disp.setCursor(10, 10);
    disp.print("Inkplate Printer");

    disp.setTextSize(2);
    disp.setCursor(10, 55);
    disp.print("Model: ");
    disp.print(hwModelName());

    disp.setCursor(10, 85);
    disp.print("SD Card: ");
    if (sdReady) {
        disp.print("OK");
    } else {
        disp.print("NOT FOUND");
    }

    disp.setCursor(10, 125);
    disp.print("Starting WiFi...");

    hwFullRefresh();
}

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("[BOOT] Inkplate Printer starting...");
    Serial.print("[BOOT] Model: ");
    Serial.println(hwModelName());

    if (!hwInit()) {
        Serial.println("[BOOT] FATAL: Display init failed");
        while (1) { delay(1000); }
    }
    Serial.println("[BOOT] Display initialized");

    sdReady = hwInitSD();
    if (sdReady) {
        sdStorageInit();
    }

    showBootScreen();

    wifiManagerInit();

    if (wifiIsConnected()) {
        ippServerInit();
    }

    // Show stored documents or empty screen
    navigationInit();

    Serial.println("[BOOT] Setup complete");
}

void loop() {
    wifiManagerLoop();
    ippServerLoop();
    navigationLoop();
    displayLoop();

    delay(50); // ~20 Hz polling for responsive touchpad input
}
