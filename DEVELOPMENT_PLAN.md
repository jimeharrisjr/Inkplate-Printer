# Development Plan: Inkplate IPP Printer

## Overview

Turn an Inkplate 6 into a network printer discoverable by Mac/Windows via IPP (Internet Printing Protocol). The host computer rasterizes documents into PWG Raster format; the Inkplate receives, stores, and displays them.

## Architecture

```
┌─────────────────────────────────────┐
│           inkplate_print.ino        │  Main sketch
├─────────────────────────────────────┤
│  WiFiManager  │  IPPServer  │ UI    │  Core modules
├───────────────┼─────────────┼───────┤
│  SDStorage    │  PWGParser  │ Nav   │  Support modules
├─────────────────────────────────────┤
│  Inkplate + ESP32 Arduino Core      │  Platform
└─────────────────────────────────────┘
```

### File Structure
```
inkplate_print/
├── inkplate_print.ino      # Setup/loop, orchestration
├── wifi_manager.h/.cpp     # AP mode, captive portal, STA connection
├── ipp_server.h/.cpp       # HTTP server on port 631, IPP encoding/decoding
├── ipp_attributes.h        # IPP constants, attribute builders
├── pwg_parser.h/.cpp       # PWG Raster stream parser
├── sd_storage.h/.cpp       # Document/page storage on SD card
├── navigation.h/.cpp       # Touch input, page/document navigation
├── display.h/.cpp          # E-ink rendering from stored pages
├── config.h                # Pin definitions, constants, defaults
└── CLAUDE.md
```

## Phases

---

### Phase 1: Project Skeleton & WiFi Management
**Goal:** Inkplate boots, connects to WiFi or enters AP setup mode.

**Tasks:**
- Create Arduino sketch with `setup()` / `loop()` structure
- Implement `config.h` with hardware pin definitions and constants
- Implement `wifi_manager`:
  - Check NVS (ESP32 Non-Volatile Storage) for stored WiFi credentials
  - Attempt STA connection with stored credentials (timeout 10s)
  - On failure: start AP mode with captive portal
  - Serve a minimal HTML page for SSID/password entry + "continue offline" button
  - Store new credentials to NVS on submission
  - Display WiFi status on e-ink screen (IP address, SSID, or "AP Mode" info)

**Dependencies:** Inkplate library, WiFi.h, ESPmDNS, Preferences.h (NVS)

---

### Phase 2: mDNS Advertisement & HTTP Server
**Goal:** Inkplate appears as a printer in Mac/Windows "Add Printer" dialogs.

**Tasks:**
- Start mDNS with hostname (e.g., `inkplate`)
- Advertise `_ipp._tcp` service on port 631 with required TXT records:
  - `txtvers=1`
  - `qtotal=1`
  - `rp=ipp/print` (resource path)
  - `ty=Inkplate E-Ink Display` (human-readable name)
  - `product=(Inkplate E-Ink Display)`
  - `pdl=image/pwg-raster` (supported format)
  - `Color=F` (or `T` for Color model)
  - `Duplex=F`
  - `URFConvert=...` (if supporting URF)
- Stand up HTTP server on port 631
- Handle `GET /` with a simple status page
- Route `POST /ipp/print` to IPP handler

**Dependencies:** ESPmDNS, WebServer (or raw WiFiServer for more control)

---

### Phase 3: IPP Protocol Core
**Goal:** Respond to printer attribute queries so the OS accepts the printer.

**Tasks:**
- Implement IPP binary encoding/decoding:
  - Parse IPP request header (version, operation-id, request-id)
  - Parse attribute groups (operation-attributes, printer-attributes)
  - Build IPP response with attribute groups
- Handle `Get-Printer-Attributes` (operation 0x000B):
  - Return printer capabilities:
    - `printer-uri-supported`: `ipp://<ip>:631/ipp/print`
    - `document-format-supported`: `image/pwg-raster`
    - `printer-state`: idle/processing
    - `printer-is-accepting-jobs`: true
    - `media-supported`: custom size matching display resolution
    - `copies-supported`: 1
    - `sides-supported`: one-sided
    - `color-supported`: false (for mono Inkplate)
    - `printer-resolution-supported`: match display native resolution
- Handle `Validate-Job` (operation 0x0004): return success
- Handle `Get-Jobs` (operation 0x000A): return job list (can be empty initially)
- Handle `Get-Printer-Attributes` for the `cups` resource path as well (macOS compatibility)

**Key constraint:** IPP requests arrive as HTTP POST with `Content-Type: application/ipp`. The body is binary IPP data, optionally followed by document data (for `Print-Job`).

---

### Phase 4: SD Card Storage
**Goal:** Reliably store and index documents/pages on SD card.

**Tasks:**
- Initialize SD card on boot
- Define storage structure:
  ```
  /print/
  ├── manifest.json          # Document index: [{id, name, pages, timestamp}]
  ├── doc_0001/
  │   ├── meta.json           # Job name, page count, timestamp
  │   ├── page_001.raw        # Raw pixel data (1bpp or 8bpp grayscale)
  │   ├── page_002.raw
  │   └── ...
  ├── doc_0002/
  │   └── ...
  └── ...
  ```
- Implement `sd_storage`:
  - `initStorage()` — mount SD, create `/print/` if missing, load manifest
  - `createDocument(name)` — create new doc directory, return doc ID
  - `writePage(docId, pageNum, pixelData, size)` — write raw page to SD
  - `readPage(docId, pageNum, buffer, size)` — read page into buffer
  - `getDocumentCount()` / `getPageCount(docId)`
  - `deleteOldest()` — free space when needed
  - `updateManifest()` — persist document index

**Key constraint:** ESP32 has ~160KB free heap with WiFi active. Pages must be streamed to SD in chunks, not buffered in RAM. For 800x600 at 1bpp = 60KB per page; at 8bpp grayscale = 480KB per page. Use chunked writes (4-8KB buffers).

---

### Phase 5: PWG Raster Parser
**Goal:** Parse incoming PWG Raster stream and extract page images.

**Tasks:**
- Implement streaming PWG Raster parser:
  - Validate file sync word (`RaS2`)
  - Parse page header (1796 bytes):
    - Extract: width, height, bits-per-color, color-space, resolution
    - Extract: compression type (uncompressed or PackBits)
  - Stream-decode pixel data:
    - PackBits decompression (run-length encoding variant)
    - Convert color data to grayscale/1-bit as needed
    - Write decoded scanlines directly to SD card in chunks
  - Detect end of page / next page header
- Wire into IPP `Print-Job` handler:
  - Read IPP header and attributes from POST body
  - After attributes-end tag, remaining body is the PWG Raster document
  - Stream-parse and store pages as they arrive

**Key constraint:** The entire document arrives in a single HTTP POST. We must parse and write to SD as data arrives over TCP — no buffering the entire document in RAM. Use the WiFiClient stream directly.

---

### Phase 6: Display Rendering & Navigation
**Goal:** Show stored pages on the e-ink display with touch navigation.

**Tasks:**
- Implement `display`:
  - `showPage(docId, pageNum)` — read page data from SD, render to e-ink
  - Handle resolution/format differences between stored data and display
  - Show a status bar or overlay (optional): doc X/Y, page N/M
  - Show idle screen when no documents exist
- Implement `navigation`:
  - Read touch pad states (Inkplate `readTouchpad()` API)
  - Debounce input (avoid double-triggers)
  - Touch 1: previous page (wrap to previous document's last page)
  - Touch 2: next page (wrap to next document's first page)
  - Touch 3: next document (jump to page 1 of next document)
  - Long-press Touch 3 (optional): enter WiFi setup / settings
- Auto-display: when a new print job completes, automatically display page 1 of the new document

---

### Phase 7: Integration & Polish
**Goal:** Reliable end-to-end printing from Mac/Windows.

**Tasks:**
- End-to-end testing with macOS print dialog
- End-to-end testing with Windows print dialog
- Handle edge cases:
  - Print job while another is in progress (queue or reject)
  - Network disconnection during print job
  - SD card full
  - Malformed IPP requests
  - Very large documents (many pages)
- Display a "Receiving..." indicator during print jobs
- Status page on `GET /` showing current state, stored documents, free space
- Optional: USB serial debug output for troubleshooting

---

## Library Dependencies

| Library | Purpose | Source |
|---------|---------|--------|
| `Inkplate` | Display driver, touchpads | Soldered Electronics |
| `WiFi` | STA/AP mode | ESP32 Arduino Core |
| `WebServer` | HTTP server | ESP32 Arduino Core |
| `ESPmDNS` | mDNS/Bonjour | ESP32 Arduino Core |
| `Preferences` | NVS credential storage | ESP32 Arduino Core |
| `SD` | SD card access | ESP32 Arduino Core |
| `ArduinoJson` | Manifest/metadata files | bblanchon/ArduinoJson |

## Key Technical Risks

1. **Memory pressure:** WiFi + HTTP server + PWG parsing + SD writes competing for ~160KB heap. Mitigation: strict streaming, small buffers, no dynamic allocation during print jobs.

2. **IPP compliance:** Mac/Windows may expect specific IPP attributes or behavior not covered by minimal implementation. Mitigation: capture real IPP traffic with Wireshark from a working printer, replicate key attributes.

3. **PWG Raster complexity:** PackBits decompression on ESP32 at TCP speeds. Mitigation: the data rate is bounded by WiFi throughput (~2-5 MB/s realistic), and PackBits is simple run-length encoding — CPU should keep up.

4. **E-ink refresh time:** Full refresh takes ~2 seconds. Partial refresh is faster but causes ghosting. User experience tradeoff to discuss.

## Development Order Rationale

Phases are ordered by dependency chain:
- WiFi (Phase 1) is needed for everything network
- mDNS + HTTP (Phase 2) is needed before IPP
- IPP attributes (Phase 3) must work before print jobs will be sent
- SD storage (Phase 4) is needed before we can receive documents
- PWG parsing (Phase 5) connects IPP to storage
- Display (Phase 6) makes it useful
- Polish (Phase 7) makes it reliable
