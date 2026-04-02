#include "pwg_parser.h"
#include "ipp_server.h"
#include "sd_storage.h"
#include "config.h"

#include <Arduino.h>

// ── Format constants ────────────────────────────────────────
#define PWG_SYNC_WORD          0x52615332  // "RaS2"
#define URF_SYNC_WORD          0x554E4952  // "UNIR"

#define PWG_HDR_SIZE           1796
#define URF_PAGE_HDR_SIZE      36

// PWG page header field offsets (big-endian uint32)
#define PWG_OFF_WIDTH          372
#define PWG_OFF_HEIGHT         376
#define PWG_OFF_BITS_PER_COLOR 384
#define PWG_OFF_BITS_PER_PIXEL 388
#define PWG_OFF_BYTES_PER_LINE 392
#define PWG_OFF_COLOR_SPACE    400
#define PWG_OFF_COMPRESSION    404
#define PWG_OFF_HW_RES_X      276
#define PWG_OFF_HW_RES_Y      280

// Document format detected from sync word
enum DocFormat { FMT_PWG, FMT_URF };
static DocFormat sDocFormat = FMT_PWG;

// CUPS color space IDs
#define CSPACE_W               0   // device gray  (0=black 255=white)
#define CSPACE_RGB             1
#define CSPACE_SW              18  // sGray        (0=black 255=white)
#define CSPACE_SRGB            19  // sRGB

// ── Buffers ─────────────────────────────────────────────────
// Sized for the larger model's needs at compile time.

#define MAX_PX_WIDTH           SCREEN_WIDTH
#define MAX_LINE_BYTES         (MAX_PX_WIDTH * 3)  // worst case: RGB

static uint8_t  sLineBuf[MAX_LINE_BYTES];   // decompressed input line
static uint8_t  sOutBuf[MAX_PX_WIDTH];      // dithered output line
static int16_t  sErrCur[MAX_PX_WIDTH * 3];  // current-line error (up to 3 ch)
static int16_t  sErrNxt[MAX_PX_WIDTH * 3];  // next-line error

// ── Stream reading with byte tracking ───────────────────────
// Uses ippReadBody() which handles chunked transfer encoding.

static int sBytesLeft = -1; // document bytes remaining; -1 = unknown

static int streamRead(uint8_t* buf, int n) {
    if (sBytesLeft >= 0 && n > sBytesLeft) n = sBytesLeft;
    if (n <= 0) return 0;
    int got = ippReadBody(buf, n);
    if (sBytesLeft >= 0) sBytesLeft -= got;
    return got;
}

static int streamReadByte() {
    uint8_t b;
    if (streamRead(&b, 1) == 1) return b;
    return -1;
}

// Read a big-endian uint32 from a buffer
static uint32_t readBE32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

// ── PackBits decompression ──────────────────────────────────
//
// Decompresses one scanline of PackBits-encoded data from the stream
// into `output`. Returns true on success.

// Decompress one PackBits-encoded line.
// PWG Raster uses standard PackBits: n<128 = literal(n+1), n>128 = repeat(257-n)
// URF (Apple Raster) uses REVERSED PackBits: n<128 = repeat(n+1), n>128 = literal(257-n)
static bool decompressLine(uint8_t* output, int bytesPerLine) {
    bool reversed = (sDocFormat == FMT_URF);
    int pos = 0;
    while (pos < bytesPerLine) {
        int n = streamReadByte();
        if (n < 0) return false;
        if (n == 128) continue; // no-op

        bool isRepeat;
        int count;
        if (n < 128) {
            count = n + 1;
            isRepeat = reversed;   // URF: repeat; PWG: literal
        } else {
            count = 257 - n;
            isRepeat = !reversed;  // URF: literal; PWG: repeat
        }

        if (pos + count > bytesPerLine) count = bytesPerLine - pos;

        if (isRepeat) {
            int val = streamReadByte();
            if (val < 0) return false;
            memset(output + pos, (uint8_t)val, count);
        } else {
            if (streamRead(output + pos, count) != count) return false;
        }
        pos += count;
    }
    return true;
}

// ── Dithering: grayscale → 3-bit (Inkplate 6 mono) ─────────
//
// Input:  sLineBuf — 8-bit grayscale (0=black, 255=white), `width` pixels
// Output: sOutBuf  — values 0-7 (0=black, 7=white)
// Error buffers sErrCur / sErrNxt are used for Floyd-Steinberg diffusion.

static void ditherGrayscaleLine(int width) {
    for (int x = 0; x < width; x++) {
        int val = (int)sLineBuf[x] + sErrCur[x];
        if (val < 0) val = 0;
        if (val > 255) val = 255;

        // Quantize to 8 levels (0-7)
        int level = (val * 7 + 128) / 255;
        if (level > 7) level = 7;

        // Reconstruct quantized value in 0-255 space for error calc
        int quant = level * 255 / 7;
        int err   = val - quant;

        sOutBuf[x] = (uint8_t)level;

        // Floyd-Steinberg error distribution
        if (x + 1 < width) sErrCur[x + 1] += err * 7 / 16;
        if (x > 0)         sErrNxt[x - 1] += err * 3 / 16;
                            sErrNxt[x]     += err * 5 / 16;
        if (x + 1 < width) sErrNxt[x + 1] += err * 1 / 16;
    }
}

// ── Dithering: RGB → 7-color palette (Inkplate 6 Color) ────
//
// Input:  sLineBuf — 24-bit sRGB (R,G,B), `width` pixels
// Output: sOutBuf  — palette index 0-6

#ifdef INKPLATE_MODEL_COLOR

static const int16_t sPalette[7][3] = {
    {  0,   0,   0},  // 0 = black
    {255, 255, 255},  // 1 = white
    {  0, 128,   0},  // 2 = green
    {  0,   0, 255},  // 3 = blue
    {255,   0,   0},  // 4 = red
    {255, 255,   0},  // 5 = yellow
    {255, 128,   0},  // 6 = orange
};

static int nearestPaletteColor(int r, int g, int b) {
    int best = 0;
    int bestDist = 0x7FFFFFFF;
    for (int i = 0; i < 7; i++) {
        int dr = r - sPalette[i][0];
        int dg = g - sPalette[i][1];
        int db = b - sPalette[i][2];
        int dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist) { bestDist = dist; best = i; }
    }
    return best;
}

static void ditherColorLine(int width) {
    for (int x = 0; x < width; x++) {
        int r = (int)sLineBuf[x * 3 + 0] + sErrCur[x * 3 + 0];
        int g = (int)sLineBuf[x * 3 + 1] + sErrCur[x * 3 + 1];
        int b = (int)sLineBuf[x * 3 + 2] + sErrCur[x * 3 + 2];
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;

        int idx = nearestPaletteColor(r, g, b);
        sOutBuf[x] = (uint8_t)idx;

        int errR = r - sPalette[idx][0];
        int errG = g - sPalette[idx][1];
        int errB = b - sPalette[idx][2];

        // Distribute error for each channel
        for (int c = 0; c < 3; c++) {
            int e = (c == 0) ? errR : (c == 1) ? errG : errB;
            if (x + 1 < width) sErrCur[(x+1)*3+c] += e * 7 / 16;
            if (x > 0)         sErrNxt[(x-1)*3+c] += e * 3 / 16;
                                sErrNxt[ x   *3+c] += e * 5 / 16;
            if (x + 1 < width) sErrNxt[(x+1)*3+c] += e * 1 / 16;
        }
    }
}

#endif // INKPLATE_MODEL_COLOR

// ── RGB → grayscale conversion (in-place) ───────────────────

static void rgbToGray(uint8_t* buf, int width) {
    for (int x = 0; x < width; x++) {
        int r = buf[x * 3 + 0];
        int g = buf[x * 3 + 1];
        int b = buf[x * 3 + 2];
        // ITU-R BT.601 luma
        buf[x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }
}

// ── Process one page ────────────────────────────────────────

static bool parsePage(int docId, int pageNum) {
    uint32_t width, height, bitsPerPixel, bytesPerLine, colorSpace, compression;

    if (sDocFormat == FMT_PWG) {
        // PWG Raster: 1796-byte page header
        uint8_t hdr[PWG_HDR_SIZE];
        if (streamRead(hdr, PWG_HDR_SIZE) != PWG_HDR_SIZE) {
            Serial.println("[PWG]  Failed to read page header");
            return false;
        }
        width        = readBE32(hdr + PWG_OFF_WIDTH);
        height       = readBE32(hdr + PWG_OFF_HEIGHT);
        bitsPerPixel = readBE32(hdr + PWG_OFF_BITS_PER_PIXEL);
        bytesPerLine = readBE32(hdr + PWG_OFF_BYTES_PER_LINE);
        colorSpace   = readBE32(hdr + PWG_OFF_COLOR_SPACE);
        compression  = readBE32(hdr + PWG_OFF_COMPRESSION);
    } else {
        // URF (Apple Raster): 32-byte page header
        uint8_t hdr[URF_PAGE_HDR_SIZE];
        if (streamRead(hdr, URF_PAGE_HDR_SIZE) != URF_PAGE_HDR_SIZE) {
            Serial.println("[URF]  Failed to read page header");
            return false;
        }
        bitsPerPixel = hdr[4];  // 8=gray, 24=RGB, 32=RGBA
        uint8_t urfCS = hdr[5]; // 0=sGray, 1=sRGB
        colorSpace   = (urfCS == 0) ? CSPACE_SW : CSPACE_SRGB;
        width        = readBE32(hdr + 16);
        height       = readBE32(hdr + 20);
        bytesPerLine = width * (bitsPerPixel / 8);
        compression  = 1; // URF always uses PackBits
    }

    Serial.printf("[%s]  Page %d: %ux%u, %ubpp, cs=%u, bpl=%u, comp=%u\n",
                  sDocFormat == FMT_URF ? "URF" : "PWG",
                  pageNum, (unsigned)width, (unsigned)height,
                  (unsigned)bitsPerPixel, (unsigned)colorSpace,
                  (unsigned)bytesPerLine, (unsigned)compression);

    // Validate dimensions
    int outWidth  = (int)width;
    int outHeight = (int)height;
    if (outWidth > MAX_PX_WIDTH) outWidth = MAX_PX_WIDTH;
    if (outWidth <= 0 || outHeight <= 0 || bytesPerLine == 0) {
        Serial.println("[PWG]  Invalid page dimensions");
        return false;
    }
    if ((int)bytesPerLine > MAX_LINE_BYTES) {
        Serial.println("[PWG]  Line too wide for buffer");
        return false;
    }

    // Determine processing mode
    bool isRGB = (colorSpace == CSPACE_RGB || colorSpace == CSPACE_SRGB);
    bool usePackBits = (compression != 0);

    // Open page file
    if (!sdOpenPageWrite(docId, pageNum)) return false;

    // Clear error buffers
    memset(sErrCur, 0, sizeof(sErrCur));
    memset(sErrNxt, 0, sizeof(sErrNxt));

    // Process scanlines
    int linesOutput = 0;
    while (linesOutput < outHeight) {
        // Read line repeat count
        int repeatCount = streamReadByte();
        if (repeatCount < 0) {
            Serial.printf("[PWG]  Stream ended at line %d/%d\n",
                          linesOutput, outHeight);
            break;
        }
        // Read one line: PackBits compressed or raw uncompressed
        bool lineOK;
        if (usePackBits) {
            lineOK = decompressLine(sLineBuf, (int)bytesPerLine);
        } else {
            lineOK = (streamRead(sLineBuf, (int)bytesPerLine) == (int)bytesPerLine);
        }
        if (!lineOK) {
            Serial.printf("[PWG]  Read failed at line %d (comp=%d)\n",
                          linesOutput, (int)compression);
            break;
        }

        // Output this line (repeatCount + 1) times
        for (int rep = 0; rep <= repeatCount && linesOutput < outHeight;
             rep++, linesOutput++) {
            // Convert/dither
#ifdef INKPLATE_MODEL_COLOR
            if (isRGB) {
                ditherColorLine(outWidth);
            } else {
                // Grayscale input on color display: treat as gray
                // Convert each gray pixel to RGB (R=G=B=gray) then dither
                for (int x = outWidth - 1; x >= 0; x--) {
                    sLineBuf[x * 3 + 0] = sLineBuf[x];
                    sLineBuf[x * 3 + 1] = sLineBuf[x];
                    sLineBuf[x * 3 + 2] = sLineBuf[x];
                }
                ditherColorLine(outWidth);
            }
#else
            if (isRGB) {
                rgbToGray(sLineBuf, outWidth);
            }
            ditherGrayscaleLine(outWidth);
#endif

            // Write dithered row to SD
            sdWritePageRow(sOutBuf, outWidth);

            // Swap error buffers: next becomes current, clear next
            // Mono always dithers in 1 channel; Color always in 3 channels
#ifdef INKPLATE_MODEL_COLOR
            int errChans = 3;
#else
            int errChans = 1;
#endif
            memcpy(sErrCur, sErrNxt, sizeof(int16_t) * outWidth * errChans);
            memset(sErrNxt, 0, sizeof(int16_t) * outWidth * errChans);
        }
    }

    sdClosePageWrite();

    Serial.printf("[PWG]  Page %d: %d lines written\n", pageNum, linesOutput);
    return linesOutput > 0;
}

// ── Public API ──────────────────────────────────────────────

int pwgParseDocument(int docId, int bytesAvailable) {
    sBytesLeft = bytesAvailable;

    // Read sync word to detect format
    uint8_t sync[4];
    if (streamRead(sync, 4) != 4) {
        Serial.println("[DOC]  Failed to read sync word");
        return -1;
    }
    Serial.printf("[DOC]  Sync: %02X %02X %02X %02X ('%c%c%c%c')\n",
                  sync[0], sync[1], sync[2], sync[3],
                  sync[0]>31?sync[0]:'.', sync[1]>31?sync[1]:'.',
                  sync[2]>31?sync[2]:'.', sync[3]>31?sync[3]:'.');

    uint32_t syncWord = readBE32(sync);
    if (syncWord == URF_SYNC_WORD) {
        sDocFormat = FMT_URF;
        // URF file header is "UNIRAST\0" — 8 bytes total.
        // We already read "UNIR" (4); skip "AST\0" (4 more).
        uint8_t urfHdr[4];
        if (streamRead(urfHdr, 4) != 4) {
            Serial.println("[URF]  Failed to read file header");
            return -1;
        }
        Serial.println("[URF]  Apple Raster detected");
    } else if (syncWord == PWG_SYNC_WORD) {
        sDocFormat = FMT_PWG;
        Serial.println("[PWG]  PWG Raster detected");
    } else {
        Serial.printf("[DOC]  Unknown sync: 0x%08X\n", (unsigned)syncWord);
        return -1;
    }

    Serial.println("[PWG]  Sync word OK — parsing pages");

    int pageCount = 0;
    int minPageHdr = (sDocFormat == FMT_URF) ? URF_PAGE_HDR_SIZE : PWG_HDR_SIZE;

    // Parse pages until stream ends
    while (true) {
        if (sBytesLeft == 0) break;
        if (sBytesLeft > 0 && sBytesLeft < minPageHdr) break;

        if (!parsePage(docId, pageCount + 1)) break;
        pageCount++;
    }

    // Drain any remaining document bytes the parser didn't consume
    if (sBytesLeft > 0) {
        Serial.printf("[PWG]  Draining %d remaining bytes\n", sBytesLeft);
        uint8_t tmp[256];
        while (sBytesLeft > 0) {
            int toRead = sBytesLeft < (int)sizeof(tmp) ? sBytesLeft : (int)sizeof(tmp);
            int r = streamRead(tmp, toRead);
            if (r <= 0) break;
        }
    }

    if (pageCount > 0) {
        int w = SCREEN_WIDTH;
        int h = SCREEN_HEIGHT;
        sdFinalizeDocument(docId, pageCount, w, h);
        Serial.printf("[PWG]  Document complete: %d pages\n", pageCount);
    }

    return pageCount;
}
