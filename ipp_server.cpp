#include "ipp_server.h"
#include "ipp_protocol.h"
#include "config.h"
#include "hardware.h"
#include "wifi_manager.h"
#include "sd_storage.h"
#include "pwg_parser.h"
#include "navigation.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>

// ── Constants ───────────────────────────────────────────────
static const unsigned long CLIENT_TIMEOUT_MS = 5000;
static const int IPP_RESPONSE_BUF_SIZE = 4096;

// ── State ───────────────────────────────────────────────────
static WiFiServer   sServer(IPP_PORT);
static char         sPrinterURI[80]  = "";
static char         sPrinterUUID[52] = "";
static uint32_t     sNextJobId       = 1;
static bool         sPrinting        = false;
static bool         sNewDocPending   = false;  // deferred notification flag
static bool         sServerStarted   = false;
static uint8_t      sRespBuf[IPP_RESPONSE_BUF_SIZE];

// ── Parsed HTTP request ─────────────────────────────────────
struct HttpRequest {
    char method[8];
    char path[64];
    int  contentLength;
    bool chunked;
    bool expectContinue;
};

// =============================================================
// Low-level I/O helpers
// =============================================================

// Read one line from TCP stream (for HTTP headers and chunk headers).
static int readLine(WiFiClient& client, char* buf, int maxLen) {
    int i = 0;
    unsigned long start = millis();
    while (i < maxLen - 1) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') break;
            if (c != '\r') buf[i++] = c;
            start = millis();
        } else if (millis() - start > CLIENT_TIMEOUT_MS) {
            return -1;
        } else {
            delay(1);
        }
    }
    buf[i] = '\0';
    return i;
}

// Read exactly n bytes from TCP stream (raw, no chunked handling).
static int readRaw(WiFiClient& client, uint8_t* buf, int n) {
    int got = 0;
    unsigned long start = millis();
    while (got < n) {
        if (client.available()) {
            int r = client.read(buf + got, n - got);
            if (r > 0) { got += r; start = millis(); }
        } else if (millis() - start > CLIENT_TIMEOUT_MS) {
            break;
        } else {
            delay(1);
        }
    }
    return got;
}

// =============================================================
// Body reader — transparently handles chunked transfer encoding
// =============================================================

static WiFiClient* sBodyClient  = NULL;
static bool  sBodyChunked       = false;
static int   sChunkRemaining    = 0;
static bool  sBodyDone          = false;

static void initBodyReader(WiFiClient& client, bool chunked) {
    sBodyClient     = &client;
    sBodyChunked    = chunked;
    sChunkRemaining = 0;
    sBodyDone       = false;
    // reset chunk debug counter (defined in ippReadBody)
}

// Public: read n bytes from the request body.
// Handles both Content-Length and chunked transfer encoding.
int ippReadBody(uint8_t* buf, int n) {
    if (!sBodyClient || sBodyDone || n <= 0) return 0;

    if (!sBodyChunked) {
        return readRaw(*sBodyClient, buf, n);
    }

    // Chunked mode
    static int chunkDbgCount = 0;
    int total = 0;
    while (total < n && !sBodyDone) {
        // Need a new chunk?
        if (sChunkRemaining <= 0) {
            char line[16];
            if (readLine(*sBodyClient, line, sizeof(line)) < 0) {
                Serial.printf("[CHUNK] readLine TIMEOUT (done=%d)\n", sBodyDone);
                sBodyDone = true; break;
            }
            sChunkRemaining = (int)strtol(line, NULL, 16);
            if (chunkDbgCount < 10) {
                Serial.printf("[CHUNK] size=%d (line='%s')\n",
                              sChunkRemaining, line);
                chunkDbgCount++;
            }
            if (sChunkRemaining <= 0) {
                sBodyDone = true; break;
            }
        }

        int toRead = (n - total) < sChunkRemaining
                     ? (n - total) : sChunkRemaining;
        int got = readRaw(*sBodyClient, buf + total, toRead);
        total += got;
        sChunkRemaining -= got;

        // After exhausting a chunk, consume the trailing CRLF
        if (sChunkRemaining <= 0) {
            char crlf[4];
            readLine(*sBodyClient, crlf, sizeof(crlf));
        }

        if (got <= 0) {
            if (chunkDbgCount < 15) {
                Serial.printf("[CHUNK] readRaw got 0 (wanted=%d rem=%d)\n",
                              toRead, sChunkRemaining);
                chunkDbgCount++;
            }
            break;
        }
    }
    return total;
}

// Drain any remaining body data (for cleanup after request handling).
static void drainRemainingBody() {
    uint8_t tmp[256];
    while (!sBodyDone) {
        int r = ippReadBody(tmp, sizeof(tmp));
        if (r <= 0) break;
    }
}

// =============================================================
// HTTP parsing
// =============================================================

static bool parseHttpRequest(WiFiClient& client, HttpRequest& req) {
    memset(&req, 0, sizeof(req));
    req.contentLength = -1;

    char line[256];
    if (readLine(client, line, sizeof(line)) <= 0) return false;

    char* sp1 = strchr(line, ' ');
    if (!sp1) return false;
    *sp1 = '\0';
    strncpy(req.method, line, sizeof(req.method) - 1);

    char* pathStart = sp1 + 1;
    char* sp2 = strchr(pathStart, ' ');
    if (sp2) *sp2 = '\0';
    strncpy(req.path, pathStart, sizeof(req.path) - 1);

    while (true) {
        int len = readLine(client, line, sizeof(line));
        if (len <= 0) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            req.contentLength = atoi(line + 15);
        else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            if (strcasestr(line + 18, "chunked")) req.chunked = true;
        } else if (strncasecmp(line, "Expect:", 7) == 0) {
            if (strcasestr(line + 7, "100-continue")) req.expectContinue = true;
        }
    }
    return true;
}

// =============================================================
// IPP request attribute parser
// =============================================================
//
// Reads through IPP attribute groups after the 8-byte header.
// Extracts job-name if present. Returns the number of body bytes
// consumed (not counting the 8-byte header already read).
// After return, the stream is positioned at the document data
// (for Print-Job) or at end of body (for other operations).

static int parseRequestAttributes(char* jobName, int jobNameMax) {
    int consumed = 0;
    if (jobName && jobNameMax > 0) jobName[0] = '\0';

    while (true) {
        uint8_t tag;
        if (ippReadBody(&tag, 1) < 1) return consumed;
        consumed++;

        if (tag < 0x10) {
            if (tag == IPP_TAG_END) return consumed;
            continue;
        }

        uint8_t buf2[2];
        if (ippReadBody(buf2, 2) < 2) return consumed;
        consumed += 2;
        uint16_t nameLen = ((uint16_t)buf2[0] << 8) | buf2[1];

        char name[64] = {};
        if (nameLen > 0) {
            int readable = nameLen < 63 ? nameLen : 63;
            ippReadBody((uint8_t*)name, readable);
            consumed += readable;
            if (nameLen > 63) {
                // skip excess name bytes
                uint8_t tmp[64];
                int skip = nameLen - 63;
                while (skip > 0) {
                    int r = ippReadBody(tmp, skip < 64 ? skip : 64);
                    if (r <= 0) break;
                    skip -= r; consumed += r;
                }
            }
        }

        if (ippReadBody(buf2, 2) < 2) return consumed;
        consumed += 2;
        uint16_t valueLen = ((uint16_t)buf2[0] << 8) | buf2[1];

        if (jobName && nameLen > 0 &&
            strcmp(name, "job-name") == 0 &&
            valueLen > 0 && valueLen < (uint16_t)jobNameMax) {
            ippReadBody((uint8_t*)jobName, valueLen);
            jobName[valueLen] = '\0';
            consumed += valueLen;
        } else if (valueLen > 0) {
            uint8_t tmp[128];
            int skip = valueLen;
            while (skip > 0) {
                int r = ippReadBody(tmp, skip < 128 ? skip : 128);
                if (r <= 0) break;
                skip -= r; consumed += r;
            }
        }
    }
}

// =============================================================
// Send an IPP response over HTTP
// =============================================================

static void sendIppResponse(WiFiClient& client, const IppWriter& w) {
    client.printf(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/ipp\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", w.size());
    client.write(w.data(), w.size());
}

// Helper: write the standard operation-attributes preamble
static void writeResponsePreamble(IppWriter& w,
                                  uint8_t vMajor, uint8_t vMinor,
                                  uint16_t status, uint32_t reqId) {
    w.writeHeader(vMajor, vMinor, status, reqId);
    w.beginGroup(IPP_TAG_OPERATION);
    w.addCharset("attributes-charset", "utf-8");
    w.addLanguage("attributes-natural-language", "en");
}

// =============================================================
// Get-Printer-Attributes (0x000B)
// =============================================================

static void handleGetPrinterAttributes(WiFiClient& client,
                                       uint8_t vMaj, uint8_t vMin,
                                       uint32_t reqId) {
    IppWriter w(sRespBuf, sizeof(sRespBuf));
    writeResponsePreamble(w, vMaj, vMin, IPP_STATUS_OK, reqId);

    // ── Printer attributes group ────────────────────────────
    w.beginGroup(IPP_TAG_PRINTER);

    // URIs and security
    w.addUri("printer-uri-supported", sPrinterURI);
    w.addKeyword("uri-security-supported", "none");
    w.addKeyword("uri-authentication-supported", "none");

    // Identity
    w.addName("printer-name", PRINTER_NAME);
    w.addText("printer-info", "Inkplate E-Ink Display");
    w.addText("printer-make-and-model", "Inkplate E-Ink Display");
    w.addText("printer-location", "");
    w.addUri("printer-uuid", sPrinterUUID);
    w.addUri("printer-more-info", sPrinterURI);
    w.addText("printer-device-id",
              "MFG:Inkplate;MDL:E-Ink Display;CMD:PWGRaster,URF;");

    // State
    w.addEnum("printer-state", sPrinting ? IPP_PSTATE_PROCESSING
                                         : IPP_PSTATE_IDLE);
    w.addKeyword("printer-state-reasons",
                 sPrinting ? "moving-to-paused" : "none");
    w.addBoolean("printer-is-accepting-jobs", !sPrinting);
    w.addInteger("queued-job-count", 0);
    w.addInteger("printer-up-time", (int32_t)(millis() / 1000));
    w.addKeyword("compression-supported", "none");

    // IPP Everywhere — critical for macOS driverless detection
    w.addKeyword("ipp-features-supported", "ipp-everywhere");

    // Supported IPP versions and operations
    w.addKeyword("ipp-versions-supported", "2.0");

    w.addEnum("operations-supported", IPP_OP_PRINT_JOB);
    w.addEnumExtra(IPP_OP_VALIDATE_JOB);
    w.addEnumExtra(IPP_OP_CANCEL_JOB);
    w.addEnumExtra(IPP_OP_GET_JOB_ATTRS);
    w.addEnumExtra(IPP_OP_GET_JOBS);
    w.addEnumExtra(IPP_OP_GET_PRINTER_ATTRS);

    // Character sets and language
    w.addCharset("charset-configured", "utf-8");
    w.addCharset("charset-supported", "utf-8");
    w.addLanguage("natural-language-configured", "en");
    w.addLanguage("generated-natural-language-supported", "en");

    // Document format
    w.addMimeType("document-format-default", "image/pwg-raster");
    w.addMimeType("document-format-supported", "image/pwg-raster");
    w.addMimeTypeExtra("image/urf");
    w.addMimeTypeExtra("application/octet-stream");

    // Media — keyword form
    w.addKeyword("media-default", MEDIA_NAME);
    w.addKeyword("media-supported", MEDIA_NAME);

    // media-col-default (collection) — required for IPP Everywhere
    w.beginCollection("media-col-default");
      w.memberName("media-size");
      w.beginCollectionExtra();
        w.memberName("x-dimension");
        w.memberInteger(MEDIA_WIDTH_HMM);
        w.memberName("y-dimension");
        w.memberInteger(MEDIA_HEIGHT_HMM);
      w.endCollection();
      w.memberName("media-top-margin");
      w.memberInteger(0);
      w.memberName("media-bottom-margin");
      w.memberInteger(0);
      w.memberName("media-left-margin");
      w.memberInteger(0);
      w.memberName("media-right-margin");
      w.memberInteger(0);
    w.endCollection();

    // media-col-ready — same as default (one media loaded)
    w.beginCollection("media-col-ready");
      w.memberName("media-size");
      w.beginCollectionExtra();
        w.memberName("x-dimension");
        w.memberInteger(MEDIA_WIDTH_HMM);
        w.memberName("y-dimension");
        w.memberInteger(MEDIA_HEIGHT_HMM);
      w.endCollection();
      w.memberName("media-top-margin");
      w.memberInteger(0);
      w.memberName("media-bottom-margin");
      w.memberInteger(0);
      w.memberName("media-left-margin");
      w.memberInteger(0);
      w.memberName("media-right-margin");
      w.memberInteger(0);
    w.endCollection();

    // media-col-database — same single entry
    w.beginCollection("media-col-database");
      w.memberName("media-size");
      w.beginCollectionExtra();
        w.memberName("x-dimension");
        w.memberInteger(MEDIA_WIDTH_HMM);
        w.memberName("y-dimension");
        w.memberInteger(MEDIA_HEIGHT_HMM);
      w.endCollection();
      w.memberName("media-top-margin");
      w.memberInteger(0);
      w.memberName("media-bottom-margin");
      w.memberInteger(0);
      w.memberName("media-left-margin");
      w.memberInteger(0);
      w.memberName("media-right-margin");
      w.memberInteger(0);
    w.endCollection();

    // Resolution
    w.addResolution("printer-resolution-default",
                    PRINTER_DPI, PRINTER_DPI, IPP_RES_PER_INCH);
    w.addResolution("printer-resolution-supported",
                    PRINTER_DPI, PRINTER_DPI, IPP_RES_PER_INCH);

    // Output bin
    w.addKeyword("output-bin-default", "face-up");
    w.addKeyword("output-bin-supported", "face-up");

    // Capabilities
    w.addBoolean("color-supported", IS_COLOR_MODEL);
    w.addInteger("pages-per-minute", 1);
    w.addInteger("copies-default", 1);
    w.addRange("copies-supported", 1, 1);
    w.addKeyword("sides-default", "one-sided");
    w.addKeyword("sides-supported", "one-sided");
    w.addEnum("print-quality-default", 4);   // normal
    w.addEnum("print-quality-supported", 4);
    w.addKeyword("pdl-override-supported", "attempted");
    w.addInteger("number-up-default", 1);
    w.addRange("number-up-supported", 1, 1);
    w.addKeyword("multiple-document-handling-supported",
                 "single-document");

    // PWG Raster specifics
    w.addResolution("pwg-raster-document-resolution-supported",
                    PRINTER_DPI, PRINTER_DPI, IPP_RES_PER_INCH);
#ifdef INKPLATE_MODEL_MONO
    w.addKeyword("pwg-raster-document-type-supported", "sgray_8");
#else
    w.addKeyword("pwg-raster-document-type-supported", "srgb_8");
#endif

    // Apple Raster (URF) support
    char urfRes[16];
    snprintf(urfRes, sizeof(urfRes), "RS%d", PRINTER_DPI);
#ifdef INKPLATE_MODEL_MONO
    w.addKeyword("urf-supported", "W8");
    w.addKeywordExtra(urfRes);
    w.addKeywordExtra("DM1");
    w.addKeywordExtra("V1.4");
#else
    w.addKeyword("urf-supported", "SRGB24");
    w.addKeywordExtra(urfRes);
    w.addKeywordExtra("DM1");
    w.addKeywordExtra("V1.4");
#endif

    w.endAttributes();

    if (w.overflow()) {
        Serial.println("[IPP]  ERROR: Response buffer overflow!");
    }
    sendIppResponse(client, w);
}

// =============================================================
// Validate-Job (0x0004)
// =============================================================

static void handleValidateJob(WiFiClient& client,
                              uint8_t vMaj, uint8_t vMin,
                              uint32_t reqId) {
    IppWriter w(sRespBuf, sizeof(sRespBuf));
    writeResponsePreamble(w, vMaj, vMin, IPP_STATUS_OK, reqId);
    w.endAttributes();
    sendIppResponse(client, w);
}

// =============================================================
// Print-Job (0x0002)
// =============================================================

static void handlePrintJob(WiFiClient& client, HttpRequest& req,
                           uint8_t vMaj, uint8_t vMin,
                           uint32_t reqId, int bodyConsumed,
                           const char* jobName) {
    if (sPrinting) {
        Serial.println("[IPP]  Busy, rejecting Print-Job");
        IppWriter w(sRespBuf, sizeof(sRespBuf));
        writeResponsePreamble(w, vMaj, vMin, IPP_STATUS_SERVER_BUSY, reqId);
        w.endAttributes();
        sendIppResponse(client, w);
        return;
    }

    if (!sdStorageReady()) {
        Serial.println("[IPP]  No SD card, rejecting Print-Job");
        IppWriter w(sRespBuf, sizeof(sRespBuf));
        writeResponsePreamble(w, vMaj, vMin, IPP_STATUS_INTERNAL_ERROR, reqId);
        w.endAttributes();
        sendIppResponse(client, w);
        return;
    }

    sPrinting = true;
    uint32_t jobId = sNextJobId++;
    Serial.printf("[IPP]  Print-Job #%u accepted\n", (unsigned)jobId);

    // Auto-delete oldest document if storage is tight
    // (heuristic: keep at most MAX_DOCUMENTS - 1 so there's room)
    while (sdGetDocumentCount() >= MAX_DOCUMENTS - 1) {
        sdDeleteOldest();
    }

    // Create document on SD
    int docId = sdCreateDocument(jobName);
    int pages = 0;

    if (docId > 0) {
        // Calculate remaining document bytes
        int docBytes = -1; // unknown
        if (req.contentLength > 0) {
            docBytes = req.contentLength - bodyConsumed;
        }

        // Parse PWG Raster and store pages
        pages = pwgParseDocument(docId, docBytes);

        if (pages <= 0) {
            Serial.println("[IPP]  No pages parsed — deleting document");
            sdDeleteDocument(docId);
        }
    }

    // Drain any remaining body data via the body reader
    drainRemainingBody();

    sPrinting = false;

    // Build response
    uint16_t status = (pages > 0) ? IPP_STATUS_OK : IPP_STATUS_INTERNAL_ERROR;
    IppWriter w(sRespBuf, sizeof(sRespBuf));
    writeResponsePreamble(w, vMaj, vMin, status, reqId);

    w.beginGroup(IPP_TAG_JOB);
    w.addInteger("job-id", (int32_t)jobId);
    w.addEnum("job-state", (pages > 0) ? IPP_JSTATE_COMPLETED
                                       : IPP_JSTATE_ABORTED);
    w.addKeyword("job-state-reasons",
                 (pages > 0) ? "job-completed-successfully"
                             : "document-format-error");

    char jobURI[96];
    snprintf(jobURI, sizeof(jobURI), "%s/jobs/%u",
             sPrinterURI, (unsigned)jobId);
    w.addUri("job-uri", jobURI);

    w.endAttributes();
    sendIppResponse(client, w);
    Serial.printf("[IPP]  Print-Job #%u finished: %d pages\n",
                  (unsigned)jobId, pages);

    if (pages > 0) {
        sNewDocPending = true; // handled in ippServerLoop after connection closes
    }
}

// =============================================================
// Get-Jobs (0x000A)
// =============================================================

static void handleGetJobs(WiFiClient& client,
                          uint8_t vMaj, uint8_t vMin,
                          uint32_t reqId) {
    IppWriter w(sRespBuf, sizeof(sRespBuf));
    writeResponsePreamble(w, vMaj, vMin, IPP_STATUS_OK, reqId);
    // No job-attributes groups — empty job list
    w.endAttributes();
    sendIppResponse(client, w);
}

// =============================================================
// Cancel-Job (0x0008) / Get-Job-Attributes (0x0009)
// =============================================================

static void handleCancelJob(WiFiClient& client,
                            uint8_t vMaj, uint8_t vMin,
                            uint32_t reqId) {
    // We don't track jobs yet — just return not-found
    IppWriter w(sRespBuf, sizeof(sRespBuf));
    writeResponsePreamble(w, vMaj, vMin, IPP_STATUS_NOT_FOUND, reqId);
    w.endAttributes();
    sendIppResponse(client, w);
}

static void handleGetJobAttributes(WiFiClient& client,
                                   uint8_t vMaj, uint8_t vMin,
                                   uint32_t reqId) {
    IppWriter w(sRespBuf, sizeof(sRespBuf));
    writeResponsePreamble(w, vMaj, vMin, IPP_STATUS_NOT_FOUND, reqId);
    w.endAttributes();
    sendIppResponse(client, w);
}

// =============================================================
// Unsupported operation
// =============================================================

static void handleUnsupportedOp(WiFiClient& client,
                                uint8_t vMaj, uint8_t vMin,
                                uint32_t reqId) {
    IppWriter w(sRespBuf, sizeof(sRespBuf));
    writeResponsePreamble(w, vMaj, vMin, IPP_STATUS_OP_NOT_SUPPORTED, reqId);
    w.endAttributes();
    sendIppResponse(client, w);
}

// =============================================================
// Status page (GET /)
// =============================================================

static void handleStatusPage(WiFiClient& client) {
    char body[700];
    int bodyLen = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s</title></head>"
        "<body style='font-family:sans-serif;max-width:600px;margin:20px auto;padding:0 16px'>"
        "<h1>%s</h1>"
        "<table>"
        "<tr><td><b>Model</b></td><td>%s</td></tr>"
        "<tr><td><b>IP</b></td><td>%s</td></tr>"
        "<tr><td><b>Port</b></td><td>%d</td></tr>"
        "<tr><td><b>URI</b></td><td>%s</td></tr>"
        "<tr><td><b>Status</b></td><td>%s</td></tr>"
        "<tr><td><b>Documents</b></td><td>%d</td></tr>"
        "</table>"
        "</body></html>",
        PRINTER_NAME, PRINTER_NAME,
        hwModelName(), wifiGetIP(), IPP_PORT, sPrinterURI,
        sPrinting ? "Printing" : "Idle",
        sdGetDocumentCount());

    // Cap bodyLen to actual buffer size (snprintf returns desired length)
    if (bodyLen >= (int)sizeof(body)) bodyLen = (int)sizeof(body) - 1;

    client.printf(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", bodyLen);
    client.write((const uint8_t*)body, bodyLen);
}

// =============================================================
// 404
// =============================================================

static void handleNotFound(WiFiClient& client) {
    const char* body = "404 Not Found";
    client.printf(
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s", (int)strlen(body), body);
}

// =============================================================
// Main IPP request dispatcher
// =============================================================

static void handleIPPRequest(WiFiClient& client, HttpRequest& req) {
    if (req.expectContinue) {
        client.print("HTTP/1.1 100 Continue\r\n\r\n");
    }

    // Initialize body reader (handles chunked transfer encoding)
    initBodyReader(client, req.chunked);

    // Read 8-byte IPP header via body reader
    uint8_t hdr[8];
    if (ippReadBody(hdr, 8) < 8) {
        Serial.println("[IPP]  ERROR: Failed to read IPP header");
        client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
        return;
    }

    uint8_t  vMaj       = hdr[0];
    uint8_t  vMin       = hdr[1];
    uint16_t operationId = ((uint16_t)hdr[2] << 8) | hdr[3];
    uint32_t requestId   = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) |
                           ((uint32_t)hdr[6] << 8)  |  (uint32_t)hdr[7];

    Serial.printf("[IPP]  op=0x%04X reqId=%u ver=%d.%d\n",
                  operationId, (unsigned)requestId, vMaj, vMin);

    // Parse request attributes via body reader
    char jobName[128] = {};
    int attrBytes = parseRequestAttributes(jobName, sizeof(jobName));
    int bodyConsumed = 8 + attrBytes;

    if (jobName[0]) {
        Serial.printf("[IPP]  job-name: %s\n", jobName);
    }

    // Dispatch by operation
    switch (operationId) {
        case IPP_OP_GET_PRINTER_ATTRS:
            handleGetPrinterAttributes(client, vMaj, vMin, requestId);
            break;

        case IPP_OP_VALIDATE_JOB:
            handleValidateJob(client, vMaj, vMin, requestId);
            break;

        case IPP_OP_PRINT_JOB:
            handlePrintJob(client, req, vMaj, vMin, requestId,
                           bodyConsumed, jobName);
            break;

        case IPP_OP_GET_JOBS:
            handleGetJobs(client, vMaj, vMin, requestId);
            break;

        case IPP_OP_CANCEL_JOB:
            handleCancelJob(client, vMaj, vMin, requestId);
            break;

        case IPP_OP_GET_JOB_ATTRS:
            handleGetJobAttributes(client, vMaj, vMin, requestId);
            break;

        default:
            Serial.printf("[IPP]  Unsupported operation 0x%04X\n", operationId);
            handleUnsupportedOp(client, vMaj, vMin, requestId);
            break;
    }
}

// =============================================================
// Connection handler
// =============================================================

static void handleClient(WiFiClient& client) {
    client.setTimeout(CLIENT_TIMEOUT_MS);

    HttpRequest req;
    if (!parseHttpRequest(client, req)) {
        client.stop();
        return;
    }

    Serial.printf("[HTTP] %s %s (len=%d chunked=%d)\n",
                  req.method, req.path, req.contentLength, req.chunked);

    if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0) {
        handleStatusPage(client);
    } else if (strcmp(req.method, "POST") == 0 &&
               (strcmp(req.path, "/ipp/print") == 0 ||
                strcmp(req.path, "/ipp/print/") == 0 ||
                strcmp(req.path, "/") == 0)) {
        handleIPPRequest(client, req);
    } else {
        handleNotFound(client);
    }

    client.flush();
    client.stop();
}

// =============================================================
// Public API
// =============================================================

void ippServerInit() {
    // Build printer URI
    snprintf(sPrinterURI, sizeof(sPrinterURI),
             "ipp://%s:%d/ipp/print", wifiGetIP(), IPP_PORT);

    // Generate printer UUID from MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(sPrinterUUID, sizeof(sPrinterUUID),
             "urn:uuid:e3a1e500-0000-1000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // ── mDNS ────────────────────────────────────────────────
    if (!MDNS.begin(PRINTER_NAME)) {
        Serial.println("[MDNS] ERROR: Failed to start mDNS");
        return;
    }
    Serial.printf("[MDNS] Hostname: %s.local\n", PRINTER_NAME);

    MDNS.addService("ipp", "tcp", IPP_PORT);
    MDNS.addServiceTxt("ipp", "tcp", "txtvers",  "1");
    MDNS.addServiceTxt("ipp", "tcp", "qtotal",   "1");
    MDNS.addServiceTxt("ipp", "tcp", "rp",       "ipp/print");
    MDNS.addServiceTxt("ipp", "tcp", "ty",       PRINTER_NAME);
    MDNS.addServiceTxt("ipp", "tcp", "product",  "(Inkplate E-Ink Display)");
    MDNS.addServiceTxt("ipp", "tcp", "pdl",      "image/pwg-raster,image/urf");
    MDNS.addServiceTxt("ipp", "tcp", "Color",    IS_COLOR_MODEL ? "T" : "F");
    MDNS.addServiceTxt("ipp", "tcp", "Duplex",   "F");
    MDNS.addServiceTxt("ipp", "tcp", "priority", "50");
    MDNS.addServiceTxt("ipp", "tcp", "printer-type", IS_COLOR_MODEL ? "0x8A80" : "0x8110");
    MDNS.addServiceTxt("ipp", "tcp", "URFConvert", IS_COLOR_MODEL ? "SRGB24" : "W8");

    // UUID without the "urn:uuid:" prefix for mDNS TXT
    char uuidShort[40];
    snprintf(uuidShort, sizeof(uuidShort), "%s", sPrinterUUID + 9); // skip "urn:uuid:"
    MDNS.addServiceTxt("ipp", "tcp", "UUID", (const char*)uuidShort);

    char adminURL[64];
    snprintf(adminURL, sizeof(adminURL), "http://%s:%d/", wifiGetIP(), IPP_PORT);
    MDNS.addServiceTxt("ipp", "tcp", "adminurl", (const char*)adminURL);

    Serial.println("[MDNS] Advertising _ipp._tcp");

    // ── TCP server ──────────────────────────────────────────
    sServer.begin();
    sServerStarted = true;
    Serial.printf("[IPP]  Server listening on port %d\n", IPP_PORT);
    Serial.printf("[IPP]  Printer URI: %s\n", sPrinterURI);
    Serial.printf("[IPP]  UUID: %s\n", sPrinterUUID);
}

void ippServerLoop() {
    if (!sServerStarted) return;

    // Handle deferred new-document notification (after connection closed)
    if (sNewDocPending) {
        sNewDocPending = false;
        navNotifyNewDocument();
    }

    WiFiClient client = sServer.available();
    if (client) {
        handleClient(client);
    }
}
