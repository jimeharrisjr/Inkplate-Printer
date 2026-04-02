#ifndef IPP_PROTOCOL_H
#define IPP_PROTOCOL_H

#include <stdint.h>
#include <string.h>

// ── IPP Operation Codes ─────────────────────────────────────
#define IPP_OP_PRINT_JOB           0x0002
#define IPP_OP_VALIDATE_JOB        0x0004
#define IPP_OP_CANCEL_JOB          0x0008
#define IPP_OP_GET_JOB_ATTRS       0x0009
#define IPP_OP_GET_JOBS            0x000A
#define IPP_OP_GET_PRINTER_ATTRS   0x000B

// ── IPP Status Codes ────────────────────────────────────────
#define IPP_STATUS_OK              0x0000
#define IPP_STATUS_BAD_REQUEST     0x0400
#define IPP_STATUS_NOT_FOUND       0x0406
#define IPP_STATUS_NOT_POSSIBLE    0x0410
#define IPP_STATUS_INTERNAL_ERROR  0x0500
#define IPP_STATUS_OP_NOT_SUPPORTED 0x0501
#define IPP_STATUS_SERVER_BUSY     0x0503

// ── IPP Delimiter Tags ─────────────────────────────────────
#define IPP_TAG_OPERATION          0x01
#define IPP_TAG_JOB                0x02
#define IPP_TAG_END                0x03
#define IPP_TAG_PRINTER            0x04
#define IPP_TAG_UNSUPPORTED_GROUP  0x05

// ── IPP Value Tags ──────────────────────────────────────────
#define IPP_TAG_INTEGER            0x21
#define IPP_TAG_BOOLEAN            0x22
#define IPP_TAG_ENUM               0x23
#define IPP_TAG_RANGE              0x33
#define IPP_TAG_RESOLUTION         0x32
#define IPP_TAG_BEG_COLLECTION     0x34
#define IPP_TAG_END_COLLECTION     0x37
#define IPP_TAG_TEXT               0x41
#define IPP_TAG_NAME               0x42
#define IPP_TAG_KEYWORD            0x44
#define IPP_TAG_URI                0x45
#define IPP_TAG_CHARSET            0x47
#define IPP_TAG_LANGUAGE           0x48
#define IPP_TAG_MIME_TYPE          0x49
#define IPP_TAG_MEMBER_NAME        0x4A

// ── IPP Printer States ──────────────────────────────────────
#define IPP_PSTATE_IDLE            3
#define IPP_PSTATE_PROCESSING      4
#define IPP_PSTATE_STOPPED         5

// ── IPP Job States ──────────────────────────────────────────
#define IPP_JSTATE_PENDING         3
#define IPP_JSTATE_PROCESSING      5
#define IPP_JSTATE_CANCELED        7
#define IPP_JSTATE_ABORTED         8
#define IPP_JSTATE_COMPLETED       9

// ── Resolution units ────────────────────────────────────────
#define IPP_RES_PER_INCH           3

// ────────────────────────────────────────────────────────────
// IppWriter — builds an IPP response into a fixed buffer.
// ────────────────────────────────────────────────────────────
class IppWriter {
public:
    IppWriter(uint8_t* buf, int capacity);

    // Header & structure
    void writeHeader(uint8_t vMajor, uint8_t vMinor,
                     uint16_t status, uint32_t reqId);
    void beginGroup(uint8_t groupTag);
    void endAttributes();

    // String-type attributes (charset, language, text, name, keyword, uri, mime)
    void addCharset(const char* name, const char* value);
    void addLanguage(const char* name, const char* value);
    void addText(const char* name, const char* value);
    void addName(const char* name, const char* value);
    void addKeyword(const char* name, const char* value);
    void addUri(const char* name, const char* value);
    void addMimeType(const char* name, const char* value);

    // Numeric attributes
    void addInteger(const char* name, int32_t value);
    void addEnum(const char* name, int32_t value);
    void addBoolean(const char* name, bool value);
    void addRange(const char* name, int32_t lower, int32_t upper);
    void addResolution(const char* name, int32_t xRes, int32_t yRes,
                       uint8_t units);

    // Additional values for multi-valued attributes (name-length = 0)
    void addKeywordExtra(const char* value);
    void addMimeTypeExtra(const char* value);
    void addEnumExtra(int32_t value);
    void addIntegerExtra(int32_t value);
    void addResolutionExtra(int32_t xRes, int32_t yRes, uint8_t units);

    // Collection support
    void beginCollection(const char* name);
    void beginCollectionExtra();
    void memberName(const char* name);
    void memberInteger(int32_t value);
    void endCollection();

    int          size()     const { return _pos; }
    const uint8_t* data()  const { return _buf; }
    bool         overflow() const { return _overflow; }

private:
    uint8_t* _buf;
    int      _cap;
    int      _pos;
    bool     _overflow;

    void writeByte(uint8_t b);
    void writeShort(uint16_t v);
    void writeInt(uint32_t v);
    void writeRaw(const void* src, int len);
    void writeStringAttr(uint8_t tag, const char* name, const char* value);
    void writeNoNameStringAttr(uint8_t tag, const char* value);
    void writeIntAttr(uint8_t tag, const char* name, int32_t value);
    void writeNoNameIntAttr(uint8_t tag, int32_t value);
};

#endif // IPP_PROTOCOL_H
