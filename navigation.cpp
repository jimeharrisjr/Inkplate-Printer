#include "navigation.h"
#include "display.h"
#include "hardware.h"
#include "sd_storage.h"
#include "config.h"

#include <Inkplate.h>
#include <Arduino.h>

// ── Navigation state ────────────────────────────────────────
enum NavState {
    NAV_IDLE,           // No documents
    NAV_VIEWING,        // Viewing a page
    NAV_DELETE_CONFIRM  // Delete confirmation dialog
};

static NavState sState     = NAV_IDLE;
static int      sDocIndex  = -1;   // index into manifest (0 = oldest)
static int      sDocId     = -1;   // current document ID
static int      sPageNum   = 1;    // current page (1-based)
static int      sPageCount = 0;    // pages in current document

// ── Touchpad debounce state ─────────────────────────────────
static bool     sPadPrev[3]      = {false, false, false};
static unsigned long sPadDownTime[3] = {0, 0, 0};
static bool     sPadWasDown[3]   = {false, false, false};

// All-three-held tracking for delete gesture
static unsigned long sAllHeldSince = 0;

// Delete confirmation timeout
static unsigned long sDeleteTime = 0;

// ── Helpers ─────────────────────────────────────────────────

static void syncViewingState() {
    int docCount = sdGetDocumentCount();

    if (docCount == 0) {
        sState     = NAV_IDLE;
        sDocIndex  = -1;
        sDocId     = -1;
        sPageNum   = 1;
        sPageCount = 0;
        return;
    }

    // Default to newest document
    if (sDocIndex < 0 || sDocIndex >= docCount)
        sDocIndex = docCount - 1;

    sDocId     = sdGetDocumentId(sDocIndex);
    sPageCount = sdGetPageCount(sDocId);
    if (sPageNum < 1) sPageNum = 1;
    if (sPageNum > sPageCount) sPageNum = sPageCount;
    sState = NAV_VIEWING;
}

static void showCurrentPage(bool forceFullRefresh) {
    if (sDocId < 0 || sPageCount == 0) {
        displayShowEmpty();
        return;
    }
    displayShowPage(sDocId, sPageNum,
                    sDocIndex, sdGetDocumentCount(), sPageCount,
                    forceFullRefresh);
    displaySetNotification(NULL); // clear notification on navigation
}

// ── Navigation actions ──────────────────────────────────────

static void prevPage() {
    int docCount = sdGetDocumentCount();
    bool docChanged = false;

    if (sPageNum > 1) {
        sPageNum--;
    } else if (docCount > 1) {
        // Wrap to previous document's last page
        sDocIndex = (sDocIndex - 1 + docCount) % docCount;
        sDocId     = sdGetDocumentId(sDocIndex);
        sPageCount = sdGetPageCount(sDocId);
        sPageNum   = sPageCount;
        docChanged = true;
    }
    // Single doc, page 1: no-op

    showCurrentPage(docChanged);
}

static void nextPage() {
    int docCount = sdGetDocumentCount();
    bool docChanged = false;

    if (sPageNum < sPageCount) {
        sPageNum++;
    } else if (docCount > 1) {
        // Wrap to next document's first page
        sDocIndex = (sDocIndex + 1) % docCount;
        sDocId     = sdGetDocumentId(sDocIndex);
        sPageCount = sdGetPageCount(sDocId);
        sPageNum   = 1;
        docChanged = true;
    }

    showCurrentPage(docChanged);
}

static void nextDocument() {
    int docCount = sdGetDocumentCount();
    if (docCount <= 1) return;

    sDocIndex = (sDocIndex + 1) % docCount;
    sDocId     = sdGetDocumentId(sDocIndex);
    sPageCount = sdGetPageCount(sDocId);
    sPageNum   = 1;
    showCurrentPage(true); // document change = full refresh
}

static void initiateDelete() {
    if (sDocId < 0) return;

    char name[64] = "Untitled";
    sdGetDocumentName(sDocId, name, sizeof(name));

    displayShowDeleteConfirm(name);
    sState      = NAV_DELETE_CONFIRM;
    sDeleteTime = millis();
}

static void confirmDelete() {
    if (sDocId < 0) return;

    Serial.printf("[NAV]  Deleting document %d\n", sDocId);
    sdDeleteDocument(sDocId);
    syncViewingState();

    if (sState == NAV_VIEWING) {
        showCurrentPage(true);
    } else {
        displayShowEmpty();
    }
}

static void cancelDelete() {
    sState = NAV_VIEWING;
    showCurrentPage(true);
}

// ── Public API ──────────────────────────────────────────────

void navigationInit() {
    syncViewingState();

    if (sState == NAV_VIEWING) {
        showCurrentPage(true);
    } else {
        displayShowEmpty();
    }
}

void navigationLoop() {
    // Read touchpads
    bool pads[3];
    pads[0] = hwReadTouchpad(PAD1);
    pads[1] = hwReadTouchpad(PAD2);
    pads[2] = hwReadTouchpad(PAD3);

    unsigned long now = millis();

    // Detect releases (press-then-release with >50ms hold = valid tap)
    bool released[3] = {false, false, false};
    for (int i = 0; i < 3; i++) {
        if (pads[i] && !sPadPrev[i]) {
            // Just pressed
            sPadDownTime[i] = now;
            sPadWasDown[i]  = true;
        }
        if (!pads[i] && sPadPrev[i]) {
            // Just released
            if (sPadWasDown[i] && now - sPadDownTime[i] > 50) {
                released[i] = true;
            }
            sPadWasDown[i] = false;
        }
        sPadPrev[i] = pads[i];
    }

    // ── All-three held = delete gesture ─────────────────────
    bool allHeld = pads[0] && pads[1] && pads[2];
    if (allHeld) {
        if (sAllHeldSince == 0) sAllHeldSince = now;
        if (sState == NAV_VIEWING && now - sAllHeldSince > 3000) {
            sAllHeldSince = 0;
            initiateDelete();
            return; // consume this input cycle
        }
    } else {
        sAllHeldSince = 0;
    }

    // ── State-specific input ────────────────────────────────
    switch (sState) {
        case NAV_IDLE:
            // Check if documents appeared (e.g., printed while idle)
            if (sdGetDocumentCount() > 0) {
                syncViewingState();
                showCurrentPage(true);
            }
            break;

        case NAV_VIEWING:
            // Don't act on individual pads while all three are held
            if (allHeld) break;

            if (released[0])      prevPage();
            else if (released[1]) nextPage();
            else if (released[2]) nextDocument();
            break;

        case NAV_DELETE_CONFIRM:
            if (released[1])                     confirmDelete();
            else if (released[0])                cancelDelete();
            else if (now - sDeleteTime > 5000)   cancelDelete(); // timeout
            break;
    }
}

void navNotifyNewDocument() {
    displaySetNotification("New document received");

    if (sState == NAV_IDLE) {
        syncViewingState();
        if (sState == NAV_VIEWING) {
            showCurrentPage(true);
        }
    }
    // If already viewing, notification appears on next render
}
