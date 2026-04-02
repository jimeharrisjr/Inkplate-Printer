#include "display.h"
#include "config.h"
#include "hardware.h"
#include "wifi_manager.h"
#include "sd_storage.h"

#include <Arduino.h>

// SdFat instance defined by the Inkplate library
extern SdFat sd;

// ── State ───────────────────────────────────────────────────
static char     sNotification[64] = "";
static bool     sRapidNav         = false;
static unsigned long sLastNavTime = 0;

// Stored state for deferred full-quality re-render
static int      sPendingDocId     = -1;
static int      sPendingPageNum   = 0;
static int      sPendingDocIndex  = 0;
static int      sPendingDocCount  = 0;
static int      sPendingPageCount = 0;

// ── Row buffer for reading page data from SD ────────────────
static uint8_t sRowBuf[SCREEN_WIDTH];

// ── Page rendering ──────────────────────────────────────────

static void renderPageFromSD(int docId, int pageNum, bool is1Bit) {
    // Pages are stored in portrait (600 wide × 800 tall).
    // Render rotated 90° CW onto the 800×600 landscape display:
    //   stored (sx, sy) → display (pageH-1-sy, sx)
    int pageW = SCREEN_HEIGHT, pageH = SCREEN_WIDTH;  // 600×800 default
    sdGetDocumentDimensions(docId, &pageW, &pageH);

    char path[48];
    snprintf(path, sizeof(path), "/print/doc_%04d/page_%03d.raw",
             docId, pageNum);

    File f;
    if (!f.open(path, O_READ)) {
        Serial.printf("[DISP] Cannot open %s\n", path);
        return;
    }

    Inkplate& d = hwDisplay();

    for (int sy = 0; sy < pageH; sy++) {
        int bytesRead = f.read(sRowBuf, pageW);
        if (bytesRead <= 0) break;

        int dx = pageH - 1 - sy;  // display X = column from the right
        if (dx < 0 || dx >= SCREEN_WIDTH) continue;

        for (int sx = 0; sx < bytesRead; sx++) {
            int dy = sx;  // display Y = row
            if (dy >= SCREEN_HEIGHT) break;

            uint8_t color = sRowBuf[sx];
            if (is1Bit) {
                d.drawPixel(dx, dy, color >= 4 ? 1 : 0);
            } else {
                d.drawPixel(dx, dy, color);
            }
        }
    }

    f.close();
}

// ── Status bar ──────────────────────────────────────────────

static void drawStatusBar(int docIndex, int docCount,
                          int pageNum, int pageCount,
                          bool is1Bit) {
    Inkplate& d = hwDisplay();

    int barY = SCREEN_HEIGHT - 16;
    uint8_t bg = is1Bit ? 1 : COLOR_WHITE;
    uint8_t fg = is1Bit ? 0 : COLOR_BLACK;

    d.fillRect(0, barY, SCREEN_WIDTH, 16, bg);
    d.setTextColor(fg, bg);
    d.setTextSize(1);
    d.setCursor(4, barY + 4);
    d.printf("Doc %d/%d   Page %d/%d",
             docIndex + 1, docCount, pageNum, pageCount);

    if (wifiIsConnected()) {
        // Right-aligned WiFi indicator
        int wx = SCREEN_WIDTH - 30;
        d.setCursor(wx, barY + 4);
        d.print("WiFi");
    }
}

// ── Notification banner ─────────────────────────────────────

static void drawNotification(bool is1Bit) {
    if (sNotification[0] == '\0') return;

    Inkplate& d = hwDisplay();
    int bannerY = SCREEN_HEIGHT - 32; // above status bar

    uint8_t bg = is1Bit ? 0 : COLOR_BLACK;
    uint8_t fg = is1Bit ? 1 : COLOR_WHITE;

    d.fillRect(0, bannerY, SCREEN_WIDTH, 14, bg);
    d.setTextColor(fg, bg);
    d.setTextSize(1);
    d.setCursor(4, bannerY + 3);
    d.print(sNotification);

    // Clear notification after drawing it once
    sNotification[0] = '\0';
}

// ── Full render + refresh helper ────────────────────────────

static void renderAndRefreshFull(int docId, int pageNum,
                                 int docIndex, int docCount,
                                 int pageCount) {
    hwSetDisplayMode1Bit(false); // 3-bit mode for quality
    hwClearDisplay();
    renderPageFromSD(docId, pageNum, false);
    drawNotification(false);
    drawStatusBar(docIndex, docCount, pageNum, pageCount, false);
    hwFullRefresh();
}

// ── Public API ──────────────────────────────────────────────

void displayLoop() {
#ifdef INKPLATE_MODEL_MONO
    // Deferred full-quality refresh after rapid navigation settles
    if (sRapidNav && millis() - sLastNavTime > 1000 && sPendingDocId > 0) {
        sRapidNav = false;
        Serial.println("[DISP] Rapid nav settled — full refresh");
        renderAndRefreshFull(sPendingDocId, sPendingPageNum,
                             sPendingDocIndex, sPendingDocCount,
                             sPendingPageCount);
    }
#endif
}

void displayShowPage(int docId, int pageNum,
                     int docIndex, int docCount, int pageCount,
                     bool forceFullRefresh) {
    // Store for deferred re-render
    sPendingDocId     = docId;
    sPendingPageNum   = pageNum;
    sPendingDocIndex  = docIndex;
    sPendingDocCount  = docCount;
    sPendingPageCount = pageCount;

    unsigned long now = millis();
    bool rapid = !forceFullRefresh &&
                 (now - sLastNavTime < 1500) &&
                 sLastNavTime > 0;
    sLastNavTime = now;

#ifdef INKPLATE_MODEL_MONO
    if (rapid) {
        // Rapid same-doc navigation: 1-bit partial update for speed
        hwSetDisplayMode1Bit(true);
        hwClearDisplay();
        renderPageFromSD(docId, pageNum, true);
        drawStatusBar(docIndex, docCount, pageNum, pageCount, true);
        hwPartialRefresh();
        sRapidNav = true;
        return;
    }
#endif

    // Normal: full-quality 3-bit (or color) render + full refresh
    sRapidNav = false;
    renderAndRefreshFull(docId, pageNum, docIndex, docCount, pageCount);
}

void displayShowEmpty() {
    hwSetDisplayMode1Bit(false);
    hwClearDisplay();
    sRapidNav = false;

    Inkplate& d = hwDisplay();
    d.setTextColor(COLOR_BLACK, COLOR_WHITE);

    d.setTextSize(2);
    d.setCursor(10, SCREEN_HEIGHT / 2 - 30);
    d.print("No documents.");
    d.setCursor(10, SCREEN_HEIGHT / 2);
    d.print("Print something!");

    if (wifiIsConnected()) {
        d.setTextSize(1);
        d.setCursor(10, SCREEN_HEIGHT / 2 + 30);
        d.print("Printer: ");
        d.print(PRINTER_NAME);
        d.setCursor(10, SCREEN_HEIGHT / 2 + 45);
        d.print("IP: ");
        d.print(wifiGetIP());
    } else if (wifiIsOffline()) {
        d.setTextSize(1);
        d.setCursor(10, SCREEN_HEIGHT / 2 + 30);
        d.print("(offline mode)");
    }

    hwFullRefresh();
}

void displayShowDeleteConfirm(const char* docName) {
    hwSetDisplayMode1Bit(false);
    hwClearDisplay();
    sRapidNav = false;

    Inkplate& d = hwDisplay();
    d.setTextColor(COLOR_BLACK, COLOR_WHITE);

    d.setTextSize(2);
    d.setCursor(10, SCREEN_HEIGHT / 2 - 50);
    d.print("Delete document?");

    d.setTextSize(1);
    d.setCursor(10, SCREEN_HEIGHT / 2 - 15);
    d.print(docName);

    d.setTextSize(2);
    d.setCursor(10, SCREEN_HEIGHT / 2 + 20);
    d.print("Pad 2 = Confirm");
    d.setCursor(10, SCREEN_HEIGHT / 2 + 50);
    d.print("Pad 1 = Cancel");

    hwFullRefresh();
}

void displaySetNotification(const char* msg) {
    if (msg) {
        strncpy(sNotification, msg, sizeof(sNotification) - 1);
        sNotification[sizeof(sNotification) - 1] = '\0';
    } else {
        sNotification[0] = '\0';
    }
}
