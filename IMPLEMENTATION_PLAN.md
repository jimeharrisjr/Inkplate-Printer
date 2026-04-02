# Implementation Plan: Inkplate IPP Printer

## Design Decisions (from Q&A)

| Decision | Choice |
|----------|--------|
| Hardware targets | Inkplate 6 (800x600 mono) and Inkplate 6 Color (600x448, 7-color) |
| Hardware detection | Auto-detect at runtime if possible; fallback to compile-time `#define` |
| Input | 3 capacitive touch pads |
| SD card | FAT32, pre-inserted |
| Printer name | Configurable via `config.h`, default `Inkplate-printer` |
| Security | Open access, no auth |
| Protocol | IPP over HTTP, port 631, unencrypted |
| Resolution | Native display resolution (150 DPI equivalent) |
| Color handling | Floyd-Steinberg dithering to grayscale (mono) or to 7-color palette (Color) |
| New document behavior | Stay on current view, show notification banner |
| Storage overflow | Auto-delete oldest document |
| Navigation cycling | Wrap around from last document to first and vice versa |
| Document deletion | Hold all 3 touch pads ~3 seconds, then confirm with single pad press |
| Power | Battery with hardware toggle; no deep-sleep logic needed |
| Display refresh | Full refresh on document switch; partial during rapid paging; full refresh after 1s idle on a page |

---

## File Structure

```
inkplate_print/
├── inkplate_print.ino          # Main sketch: setup(), loop()
├── config.h                    # Build-time configuration (model, printer name, pins)
├── hardware.h / hardware.cpp   # Hardware abstraction (display, touchpads, SD, model detection)
├── wifi_manager.h / wifi_manager.cpp   # WiFi STA/AP, captive portal, NVS credentials
├── ipp_server.h / ipp_server.cpp       # HTTP listener on port 631, routes requests
├── ipp_protocol.h / ipp_protocol.cpp   # IPP binary encoding/decoding, attribute builders
├── pwg_parser.h / pwg_parser.cpp       # Streaming PWG Raster parser + dithering
├── sd_storage.h / sd_storage.cpp       # Document/page filesystem operations
├── navigation.h / navigation.cpp       # Touch input, debounce, gestures, delete flow
├── display.h / display.cpp             # Page rendering, refresh strategy, notifications
└── CLAUDE.md
```

---

## Phase 1: Skeleton, Config & Hardware Abstraction

**Goal:** The sketch compiles for both Inkplate models, initializes hardware, and shows a boot screen.

### 1.1 — `config.h`

Define compile-time configuration:

```cpp
// Uncomment ONE, or leave both commented for auto-detect
// #define INKPLATE_MODEL_MONO
// #define INKPLATE_MODEL_COLOR

#define PRINTER_NAME      "Inkplate-printer"
#define IPP_PORT          631
#define SD_CS_PIN         15          // Inkplate SD chip-select
#define WIFI_CONNECT_TIMEOUT_MS  10000
#define WIFI_AP_SSID      "Inkplate-Setup"
#define WIFI_AP_PASSWORD  ""          // Open AP for setup
#define MAX_CHUNK_SIZE    4096        // Streaming buffer size (bytes)
```

### 1.2 — `hardware.h / hardware.cpp`

Hardware abstraction layer to hide model differences:

- `HW_Model` enum: `MODEL_MONO`, `MODEL_COLOR`
- `hwDetectModel()` — attempt runtime detection (check board ID register or display response); fall back to `config.h` `#define`
- `hwInit()` — call `Inkplate::begin()` with correct mode (`INKPLATE_1BIT` or `INKPLATE_3BIT` for mono; appropriate mode for Color)
- Expose model-specific constants:
  - `HW_SCREEN_W` / `HW_SCREEN_H` (800x600 or 600x448)
  - `HW_COLOR_CAPABLE` (false or true)
  - `HW_BITS_PER_PIXEL` (1/3 for mono, TBD for color)
- Wrap touchpad reads: `hwReadTouchpad(int pad)` → `bool` (pads 1, 2, 3)
- Wrap SD init: `hwInitSD()` → `bool`

### 1.3 — `inkplate_print.ino` (initial)

```
setup():
  Serial.begin(115200)
  hwInit()           → detect model, init display
  hwInitSD()         → mount SD card
  display boot screen with model name, "Starting..."
  wifiManagerInit()  → attempt WiFi (Phase 2)

loop():
  wifiManagerLoop()
  ippServerLoop()
  navigationLoop()
  displayLoop()
```

### 1.4 — Deliverable

Sketch compiles in Arduino IDE for both models. On boot, screen shows "Inkplate Printer — Mono 800x600" (or Color variant). SD card mounts successfully. Serial output confirms hardware detection.

---

## Phase 2: WiFi Management

**Goal:** Connect to stored WiFi, or launch AP with captive portal for setup.

### 2.1 — Credential Storage (NVS)

- Use ESP32 `Preferences` library (namespace `"inkplate"`)
- Store: `ssid` (string, max 32 chars), `password` (string, max 64 chars)
- Functions: `wifiLoadCredentials()`, `wifiSaveCredentials(ssid, pass)`, `wifiClearCredentials()`

### 2.2 — STA Mode Connection

```
wifiManagerInit():
  if credentials exist in NVS:
    WiFi.mode(WIFI_STA)
    WiFi.begin(ssid, password)
    wait up to WIFI_CONNECT_TIMEOUT_MS
    if connected:
      display IP address on screen
      start mDNS + IPP server (Phase 3)
      return
  // Fall through to AP mode
  startAPMode()
```

### 2.3 — AP Mode + Captive Portal

- `WiFi.mode(WIFI_AP)` with SSID `Inkplate-Setup`
- Start `WebServer` on port 80
- Serve a single HTML page at all routes (captive portal behavior):
  - Show form: SSID input (text), Password input (password), Submit button
  - Show "Continue Offline" button
- On form submit (`POST /save`):
  - Save credentials to NVS
  - Display "Credentials saved, restarting..." on e-ink
  - `ESP.restart()`
- On "Continue Offline" (`POST /offline`):
  - Skip network setup
  - Enter offline navigation mode (display stored documents only)
- Display on e-ink: "Connect to WiFi: Inkplate-Setup" and "Open http://192.168.4.1 in browser"

### 2.4 — Deliverable

Inkplate boots, connects to WiFi (showing IP on screen), or enters AP mode with functional captive portal. Credentials persist across reboots.

---

## Phase 3: mDNS & IPP Server Scaffolding

**Goal:** Inkplate appears in "Add Printer" dialogs on Mac and Windows.

### 3.1 — mDNS Service Advertisement

```cpp
MDNS.begin(PRINTER_NAME);     // e.g., "Inkplate-printer"
MDNS.addService("ipp", "tcp", 631);
MDNS.addServiceTxt("ipp", "tcp", "txtvers", "1");
MDNS.addServiceTxt("ipp", "tcp", "qtotal", "1");
MDNS.addServiceTxt("ipp", "tcp", "rp", "ipp/print");
MDNS.addServiceTxt("ipp", "tcp", "ty", PRINTER_NAME);
MDNS.addServiceTxt("ipp", "tcp", "product", "(Inkplate E-Ink Display)");
MDNS.addServiceTxt("ipp", "tcp", "pdl", "image/pwg-raster");
MDNS.addServiceTxt("ipp", "tcp", "Color", hwIsColor() ? "T" : "F");
MDNS.addServiceTxt("ipp", "tcp", "Duplex", "F");
MDNS.addServiceTxt("ipp", "tcp", "priority", "50");
MDNS.addServiceTxt("ipp", "tcp", "adminurl", "http://...:631/");
```

### 3.2 — HTTP Server on Port 631

Use raw `WiFiServer` (not `WebServer`) for more control over streaming large POST bodies:

- Listen on port 631
- Accept connections in `ippServerLoop()`
- Parse HTTP request line and headers
- Route:
  - `POST /ipp/print` → IPP handler (Phase 4)
  - `GET /` → HTML status page (printer name, document count, free space, IP)
- Must handle chunked transfer encoding (some IPP clients use it)
- Must handle `Expect: 100-continue` header (respond with `100 Continue`)

### 3.3 — Deliverable

After connecting to WiFi, the Inkplate appears in macOS "Printers & Scanners" and Windows "Add Printer" lists. Clicking it fails (IPP not yet implemented), but discovery works. `GET /` in a browser returns a status page.

---

## Phase 4: IPP Protocol Implementation

**Goal:** Mac/Windows can successfully "add" the printer and send print jobs.

### 4.1 — IPP Binary Format Handling

**Request parsing** (read from TCP stream):
```
Bytes 0-1:   version (0x0101 for IPP 1.1, 0x0200 for IPP 2.0)
Bytes 2-3:   operation-id (request) or status-code (response)
Bytes 4-7:   request-id
Then:        attribute groups until end-of-attributes tag (0x03)
Then:        document data (for Print-Job)
```

**Attribute group structure:**
```
1 byte:      group tag (0x01=operation, 0x04=printer, 0x05=unsupported)
For each attribute:
  1 byte:    value tag (0x41=textWithoutLanguage, 0x44=keyword, 0x45=uri, etc.)
  2 bytes:   name length
  N bytes:   name
  2 bytes:   value length
  N bytes:   value
```

Implement:
- `ippParseRequest(stream)` → `IppRequest` struct (operation, request-id, attributes map)
- `ippWriteResponse(client, statusCode, requestId, attributes)` — serialize IPP response

### 4.2 — Operation Handlers

**`Get-Printer-Attributes` (0x000B):**

Return comprehensive attribute set. Critical attributes that Mac/Windows check:

| Attribute | Value |
|-----------|-------|
| `printer-uri-supported` | `ipp://<ip>:631/ipp/print` |
| `uri-security-supported` | `none` |
| `uri-authentication-supported` | `none` |
| `printer-name` | from `config.h` |
| `printer-state` | `3` (idle) or `4` (processing) |
| `printer-state-reasons` | `none` |
| `ipp-versions-supported` | `1.1`, `2.0` |
| `operations-supported` | `0x0002` (Print-Job), `0x0004` (Validate-Job), `0x0008` (Cancel-Job), `0x000A` (Get-Jobs), `0x000B` (Get-Printer-Attributes) |
| `document-format-supported` | `image/pwg-raster`, `application/octet-stream` |
| `document-format-default` | `image/pwg-raster` |
| `media-supported` | Custom media name matching display dimensions |
| `media-default` | Same |
| `media-col-supported` | (media size collection with exact pixel dimensions) |
| `printer-resolution-supported` | `150dpi` (or calculated to match native pixels) |
| `color-supported` | `false` / `true` per model |
| `copies-supported` | `1-1` |
| `sides-supported` | `one-sided` |
| `printer-is-accepting-jobs` | `true` |
| `queued-job-count` | `0` |
| `pdl-override-supported` | `attempted` |
| `printer-make-and-model` | `Inkplate E-Ink Display` |
| `printer-info` | `Inkplate E-Ink Display` |
| `pwg-raster-document-resolution-supported` | `150dpi` |
| `pwg-raster-document-type-supported` | `sgray_8` (mono) or `srgb_8` (color) |

**`Validate-Job` (0x0004):** Return `successful-ok` (0x0000). No-op.

**`Print-Job` (0x0002):**
1. Parse IPP attributes (job-name, document-format, etc.)
2. Assign job ID (incrementing counter)
3. Return IPP response with `job-id`, `job-state=processing`
4. After end-of-attributes tag, remaining TCP stream = PWG Raster data
5. Pass stream to PWG parser (Phase 5) for storage
6. Update job state to `completed`

**`Get-Jobs` (0x000A):** Return list of recent jobs (or empty).

**`Cancel-Job` (0x0008):** Acknowledge; cancel in-progress job if possible.

### 4.3 — Deliverable

macOS and Windows can add "Inkplate-printer" as a printer without errors. Printing a test page reaches the `Print-Job` handler and the IPP response is correct. Document data is received (but not yet parsed/stored).

---

## Phase 5: PWG Raster Parser & Storage Pipeline

**Goal:** Incoming print data is parsed, dithered, and stored as displayable page images on SD.

### 5.1 — PWG Raster Stream Parser

**File header:** 4-byte sync word `RaS2` (0x52615332)

**Page header:** 1796 bytes, key fields at fixed offsets:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 64 | MediaColor (string) |
| 64 | 64 | MediaType (string) |
| 256 | 4 | CupsWidth (pixels) |
| 260 | 4 | CupsHeight (pixels) |
| 272 | 4 | CupsBitsPerColor |
| 276 | 4 | CupsBitsPerPixel |
| 280 | 4 | CupsBytesPerLine |
| 296 | 4 | CupsColorSpace |
| 300 | 4 | CupsCompression (0=uncompressed, 1=PackBits-like) |
| 388 | 4 | HWResolution[0] (X DPI) |
| 392 | 4 | HWResolution[1] (Y DPI) |

All integers are big-endian (network byte order).

**Streaming parse algorithm:**
```
readSyncWord()  // validate "RaS2"
while (more data):
  readPageHeader(1796 bytes into stack buffer)
  extract width, height, bpp, compression
  docId = sdCreateDocument(jobName) // on first page
  for each scanline (0..height-1):
    if compressed:
      decompressPackBitsLine(stream, lineBuffer, bytesPerLine)
    else:
      readExact(stream, lineBuffer, bytesPerLine)
    convertAndDither(lineBuffer, outputBuffer)  // see 5.2
    sdWritePageChunk(docId, pageNum, outputBuffer, outputBytesPerLine)
  pageNum++
```

**Memory budget:** One scanline buffer = `CupsBytesPerLine`. At 800px wide, 8bpp grayscale = 800 bytes. At 24bpp RGB = 2400 bytes. Well within limits. Allocate on stack or with a small fixed buffer.

### 5.2 — Floyd-Steinberg Dithering

For **Inkplate 6 (mono/3-bit grayscale):**
- Input: 8-bit grayscale scanline (or convert RGB→grayscale first: `0.299R + 0.587G + 0.114B`)
- Output: 3-bit grayscale (8 levels, values 0-7) — or 1-bit if using 1-bit display mode
- Floyd-Steinberg requires current scanline + error buffer for next scanline
- Memory: 2 × width × sizeof(int16_t) = 2 × 800 × 2 = 3.2KB — fine

For **Inkplate 6 Color (7-color):**
- Input: RGB scanline
- Output: nearest color from 7-color palette (black, white, green, blue, red, yellow, orange)
- Floyd-Steinberg in RGB space, snapping to nearest palette color
- Error diffusion in 3 channels: 3 × 2 × width × sizeof(int16_t) = 3 × 2 × 600 × 2 = 7.2KB — fine

Dithering is applied per-scanline as data streams in, writing the dithered output directly to SD.

### 5.3 — SD Storage Format

Each page stored as a raw file optimized for fast display rendering:

**Inkplate 6 mono (3-bit grayscale mode):**
- File: `/print/doc_NNNN/page_NNN.raw`
- Format: packed pixel data matching display format. 800×600 at 3bpp ≈ 180KB per page.

**Inkplate 6 Color:**
- File: `/print/doc_NNNN/page_NNN.raw`
- Format: packed pixel data matching display format. 600×448 at 3-4bpp ≈ 134KB per page.

**Metadata:** `/print/doc_NNNN/meta.txt` — plain text, one key=value per line:
```
name=My Document
pages=3
timestamp=1711929600
width=800
height=600
```

**Manifest:** `/print/manifest.txt` — one document ID per line, newest last:
```
doc_0001
doc_0003
doc_0007
```

(Using plain text instead of JSON to avoid the ArduinoJson dependency for simple data.)

### 5.4 — Storage Overflow Handling

Before starting a new document:
```
while (sdFreeSpaceMB() < estimatedDocSizeMB):
  sdDeleteOldestDocument()   // delete first entry in manifest
```

### 5.5 — Deliverable

Printing from Mac/Windows results in pages stored on the SD card as raw image files. Serial output logs page dimensions, scanline progress, and file sizes. Dithering is applied. Storage overflow auto-deletes oldest documents.

---

## Phase 6: Display Rendering & Navigation

**Goal:** User can view stored documents and navigate between pages/documents.

### 6.1 — Display Rendering

**`displayShowPage(docId, pageNum)`:**
1. Read `/print/<docId>/meta.txt` for dimensions
2. Open `/print/<docId>/page_<NNN>.raw`
3. Read in chunks (4KB) and write to display framebuffer using `Inkplate::drawPixel()` or direct framebuffer access
4. Trigger refresh (full or partial per strategy)

**Refresh strategy state machine:**
```
State: IDLE
  On page change:
    → if different document: do FULL refresh, go to IDLE
    → if same document: do PARTIAL refresh, start 1-second timer, go to RAPID_NAV

State: RAPID_NAV
  On page change within 1 second:
    → do PARTIAL refresh, restart 1-second timer
  On 1-second timer expiry:
    → do FULL refresh, go to IDLE
```

**Notification banner** (new document received while viewing):
- After print job completes, set flag `newDocPending = true`
- On next display refresh, draw a small banner at the bottom: "New document received"
- Banner clears on next navigation action

### 6.2 — Navigation (Touch Pads)

**Pad assignment:**
| Pad | Short press | Hold 3s (all three) |
|-----|-------------|----------------------|
| Pad 1 (left) | Previous page | — |
| Pad 2 (center) | Next page | — |
| Pad 3 (right) | Next document (page 1) | — |
| All three | — | Initiate delete |

**Debounce & input handling:**
- Poll touch pads every 50ms in `navigationLoop()`
- Require pad to be held for 50ms minimum (debounce)
- Detect release for "short press" action
- Track simultaneous hold duration for delete gesture

**Navigation logic:**
```
Previous page:
  if currentPage > 1: currentPage--
  else: wrap to previous document's last page (cycle)

Next page:
  if currentPage < totalPages: currentPage++
  else: wrap to next document's first page (cycle)

Next document:
  advance to next document (cycle), page 1
```

**Edge cases:**
- No documents stored: display "No documents. Print something!" with WiFi IP
- Single document, single page: all navigation is no-op (show "1/1")
- Current document deleted externally (SD removed): fall back to "No documents" screen

### 6.3 — Document Deletion Flow

1. User holds all 3 touch pads for 3 seconds
2. Screen shows: "Delete [document name]? Press Pad 2 to confirm, Pad 1 to cancel"
3. On Pad 2 press: delete document directory and update manifest; navigate to next document (or "No documents" screen)
4. On Pad 1 press or 5-second timeout: cancel, return to normal view

### 6.4 — Status Bar

Small status area at bottom of screen (below document content):
```
┌──────────────────────────────┐
│                              │
│      [Document content]      │
│                              │
├──────────────────────────────┤
│ Doc 2/5  Page 3/12    WiFi ✓ │
└──────────────────────────────┘
```

- Reserve bottom 16-20 pixels for status
- Show: document position, page position, WiFi connected indicator
- Update with each page render (negligible overhead)

### 6.5 — Deliverable

User can navigate through stored documents with touch pads. Display updates use the correct refresh strategy. New documents trigger a notification. Documents can be deleted with the 3-pad hold gesture.

---

## Phase 7: Integration, Testing & Polish

**Goal:** Reliable end-to-end experience.

### 7.1 — End-to-End Testing Matrix

| Test | Mac | Windows |
|------|-----|---------|
| Discover printer via mDNS | | |
| Add printer | | |
| Print single-page document | | |
| Print multi-page document | | |
| Print while viewing another document | | |
| Cancel print mid-job | | |
| Print when SD card is nearly full (triggers auto-delete) | | |
| Navigate pages (forward/back) | | |
| Navigate documents (cycle) | | |
| Delete document (3-pad hold) | | |
| WiFi AP setup flow | | |
| Continue offline | | |
| Reconnect after WiFi loss | | |

### 7.2 — Error Handling

| Scenario | Behavior |
|----------|----------|
| SD card not inserted | Display "No SD card" on boot; disable printing; allow WiFi setup |
| SD card full (after auto-delete fails) | Return IPP error `server-error-busy`; display warning |
| WiFi disconnects mid-print | Abort job; discard partial document; display "Print failed" |
| Malformed IPP request | Return `client-error-bad-request` (0x0400) |
| PWG parse error | Abort job; discard partial document; log to Serial |
| Second print job while one is active | Queue it (single-slot queue) or return `server-error-busy` |

### 7.3 — Concurrent Print Handling

- Only one print job at a time (ESP32 memory constraint)
- If a job arrives while one is processing: return `server-error-busy` (0x0503)
- After job completes, immediately ready for next

### 7.4 — Serial Debug Output

All phases should output to Serial for debugging in Arduino Serial Monitor:
```
[BOOT] Model: Inkplate 6 Mono (800x600)
[WIFI] Connecting to "MyNetwork"... OK (192.168.1.42)
[MDNS] Advertising _ipp._tcp on port 631
[IPP]  Get-Printer-Attributes from 192.168.1.10
[IPP]  Print-Job "test.pdf" from 192.168.1.10
[PWG]  Page 1: 800x600, sgray_8, compressed
[PWG]  Page 1: dithered, 180KB written to /print/doc_0012/page_001.raw
[PWG]  Page 2: 800x600, sgray_8, compressed
[PWG]  Page 2: dithered, 180KB written to /print/doc_0012/page_002.raw
[JOB]  Complete: 2 pages stored
[NAV]  Pad 2 pressed: next page (doc_0012 page 2/2)
```

### 7.5 — Deliverable

Complete, tested system. Printing works from both Mac and Windows. Navigation is responsive. Error cases are handled gracefully. Serial output enables debugging.

---

## Implementation Order & Dependencies

```
Phase 1 ──→ Phase 2 ──→ Phase 3 ──→ Phase 4 ──┐
  (hw)        (wifi)      (mdns+http)  (ipp)    │
                                                 ├──→ Phase 7
Phase 1 ──→ Phase 5 ──────────────────→ Phase 5 │    (integration)
  (hw)        (sd storage)              (pwg+sd) │
                                                 │
Phase 1 ──→ Phase 6 ─────────────────────────────┘
  (hw)        (display+nav)
```

Phases 4 and 5 can be partially developed in parallel with Phase 6, since storage and display are loosely coupled (they share the SD file format contract).

---

## Memory Budget (Estimated)

| Component | RAM Usage |
|-----------|-----------|
| WiFi stack | ~100 KB |
| TCP/HTTP buffers | ~8 KB |
| IPP parse buffer | ~2 KB |
| PWG scanline buffer | ~2.4 KB (worst case: 800px × 24bpp) |
| Floyd-Steinberg error buffers | ~7.2 KB (worst case: color, 3-channel) |
| SD write buffer | ~4 KB |
| Display framebuffer | managed by Inkplate library (~60 KB for mono) |
| Navigation state | ~0.1 KB |
| **Total estimated** | **~184 KB** |
| **Typical ESP32 free heap** | **~200-220 KB** |
| **Headroom** | **~16-36 KB** |

Memory is tight. Key rules:
- No dynamic allocation (`new`/`malloc`) during print jobs — use stack and static buffers
- No String class — use `char[]` buffers throughout
- No JSON library — plain text for metadata files
- Release HTTP connection resources promptly after each request
