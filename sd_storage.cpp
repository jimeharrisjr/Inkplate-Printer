#include "sd_storage.h"
#include "config.h"
#include "hardware.h"  // Inkplate.h → SdFat
#include <Arduino.h>

// SdFat instance defined by the Inkplate library
extern SdFat sd;

// ── State ───────────────────────────────────────────────────
static bool     sReady     = false;
static int      sDocIds[MAX_DOCUMENTS];
static int      sDocCount  = 0;
static int      sNextDocId = 1;
static File     sPageFile;

// ── Path helpers ────────────────────────────────────────────

static void docDirPath(char* buf, int bufLen, int docId) {
    snprintf(buf, bufLen, "/print/doc_%04d", docId);
}

static void pagePath(char* buf, int bufLen, int docId, int pageNum) {
    snprintf(buf, bufLen, "/print/doc_%04d/page_%03d.raw", docId, pageNum);
}

static void metaPath(char* buf, int bufLen, int docId) {
    snprintf(buf, bufLen, "/print/doc_%04d/meta.txt", docId);
}

// ── Manifest I/O ────────────────────────────────────────────

static bool loadManifest() {
    sDocCount  = 0;
    sNextDocId = 1;

    File f;
    if (!f.open("/print/manifest.txt", O_READ)) return true; // no manifest yet

    while (f.available() && sDocCount < MAX_DOCUMENTS) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        int id = line.toInt();
        if (id > 0) {
            sDocIds[sDocCount++] = id;
            if (id >= sNextDocId) sNextDocId = id + 1;
        }
    }
    f.close();

    Serial.printf("[SD]   Manifest loaded: %d documents, next ID %d\n",
                  sDocCount, sNextDocId);
    return true;
}

static bool saveManifest() {
    sd.remove("/print/manifest.txt");
    File f;
    if (!f.open("/print/manifest.txt", O_WRITE | O_CREAT | O_TRUNC)) {
        Serial.println("[SD]   ERROR: Cannot write manifest");
        return false;
    }
    for (int i = 0; i < sDocCount; i++) {
        f.println(sDocIds[i]);
    }
    f.close();
    return true;
}

// ── Meta file I/O ───────────────────────────────────────────

static bool readMetaField(int docId, const char* key,
                          char* value, int maxLen) {
    char path[48];
    metaPath(path, sizeof(path), docId);

    File f;
    if (!f.open(path, O_READ)) return false;

    int keyLen = strlen(key);
    bool found = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.startsWith(key) && (int)line.length() > keyLen &&
            line.charAt(keyLen) == '=') {
            String val = line.substring(keyLen + 1);
            val.trim();
            val.toCharArray(value, maxLen);
            found = true;
            break;
        }
    }
    f.close();
    return found;
}

// ── Meta file write helper ──────────────────────────────────

static bool writeMeta(int docId, const char* name,
                      int pages, int width, int height) {
    char path[48];
    metaPath(path, sizeof(path), docId);
    sd.remove(path);

    File f;
    if (!f.open(path, O_WRITE | O_CREAT | O_TRUNC)) return false;

    f.print("name=");   f.println(name);
    f.print("pages=");  f.println(pages);
    f.print("width=");  f.println(width);
    f.print("height="); f.println(height);
    f.close();
    return true;
}

// ── Delete helpers ──────────────────────────────────────────

static void deleteDocFiles(int docId) {
    char val[16];
    int pages = 0;
    if (readMetaField(docId, "pages", val, sizeof(val))) {
        pages = atoi(val);
    }

    char path[48];

    for (int p = 1; p <= pages + 3; p++) {
        pagePath(path, sizeof(path), docId, p);
        sd.remove(path);
    }

    metaPath(path, sizeof(path), docId);
    sd.remove(path);

    docDirPath(path, sizeof(path), docId);
    sd.rmdir(path);
}

static void removeFromManifest(int docId) {
    int idx = -1;
    for (int i = 0; i < sDocCount; i++) {
        if (sDocIds[i] == docId) { idx = i; break; }
    }
    if (idx < 0) return;

    for (int i = idx; i < sDocCount - 1; i++) {
        sDocIds[i] = sDocIds[i + 1];
    }
    sDocCount--;
    saveManifest();
}

// ── Public API ──────────────────────────────────────────────

bool sdStorageInit() {
    if (!sd.exists("/print")) {
        if (!sd.mkdir("/print")) {
            Serial.println("[SD]   ERROR: Cannot create /print/");
            return false;
        }
        Serial.println("[SD]   Created /print/");
    }

    sReady = loadManifest();
    return sReady;
}

bool sdStorageReady() {
    return sReady;
}

int sdCreateDocument(const char* name) {
    if (!sReady) return -1;

    int docId = sNextDocId++;
    char path[48];
    docDirPath(path, sizeof(path), docId);

    if (!sd.mkdir(path)) {
        Serial.printf("[SD]   ERROR: Cannot create %s\n", path);
        return -1;
    }

    Serial.printf("[SD]   Created document %d: %s\n", docId,
                  name ? name : "(unnamed)");

    writeMeta(docId, (name && name[0]) ? name : "Untitled", 0, 0, 0);
    return docId;
}

bool sdOpenPageWrite(int docId, int pageNum) {
    if (!sReady) return false;
    if (sPageFile.isOpen()) sPageFile.close();

    char path[48];
    pagePath(path, sizeof(path), docId, pageNum);

    if (!sPageFile.open(path, O_WRITE | O_CREAT | O_TRUNC)) {
        Serial.printf("[SD]   ERROR: Cannot create %s\n", path);
        return false;
    }
    return true;
}

bool sdWritePageRow(const uint8_t* row, int len) {
    if (!sPageFile.isOpen()) return false;
    return sPageFile.write(row, len) == (size_t)len;
}

void sdClosePageWrite() {
    if (sPageFile.isOpen()) {
        sPageFile.flush();
        sPageFile.close();
    }
}

bool sdFinalizeDocument(int docId, int pageCount, int width, int height) {
    if (!sReady) return false;

    char name[128] = "Untitled";
    readMetaField(docId, "name", name, sizeof(name));

    writeMeta(docId, name, pageCount, width, height);

    if (sDocCount < MAX_DOCUMENTS) {
        sDocIds[sDocCount++] = docId;
        saveManifest();
        Serial.printf("[SD]   Finalized document %d: %d pages (%dx%d)\n",
                      docId, pageCount, width, height);
        return true;
    }

    Serial.println("[SD]   ERROR: Manifest full");
    return false;
}

int sdGetDocumentCount() {
    return sDocCount;
}

int sdGetDocumentId(int index) {
    if (index < 0 || index >= sDocCount) return -1;
    return sDocIds[index];
}

int sdGetPageCount(int docId) {
    char val[16];
    if (readMetaField(docId, "pages", val, sizeof(val))) {
        return atoi(val);
    }
    return 0;
}

bool sdGetDocumentName(int docId, char* buf, int maxLen) {
    return readMetaField(docId, "name", buf, maxLen);
}

bool sdGetDocumentDimensions(int docId, int* w, int* h) {
    char val[16];
    if (w) {
        *w = 0;
        if (readMetaField(docId, "width", val, sizeof(val)))
            *w = atoi(val);
    }
    if (h) {
        *h = 0;
        if (readMetaField(docId, "height", val, sizeof(val)))
            *h = atoi(val);
    }
    return (w && *w > 0) && (h && *h > 0);
}

bool sdDeleteDocument(int docId) {
    if (!sReady) return false;
    Serial.printf("[SD]   Deleting document %d\n", docId);
    deleteDocFiles(docId);
    removeFromManifest(docId);
    return true;
}

bool sdDeleteOldest() {
    if (sDocCount == 0) return false;
    return sdDeleteDocument(sDocIds[0]);
}
