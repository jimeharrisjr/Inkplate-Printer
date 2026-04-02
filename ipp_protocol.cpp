#include "ipp_protocol.h"

// ── Constructor ─────────────────────────────────────────────

IppWriter::IppWriter(uint8_t* buf, int capacity)
    : _buf(buf), _cap(capacity), _pos(0), _overflow(false) {}

// ── Primitive writes ────────────────────────────────────────

void IppWriter::writeByte(uint8_t b) {
    if (_pos < _cap) _buf[_pos++] = b;
    else _overflow = true;
}

void IppWriter::writeShort(uint16_t v) {
    writeByte(v >> 8);
    writeByte(v & 0xFF);
}

void IppWriter::writeInt(uint32_t v) {
    writeByte((v >> 24) & 0xFF);
    writeByte((v >> 16) & 0xFF);
    writeByte((v >>  8) & 0xFF);
    writeByte( v        & 0xFF);
}

void IppWriter::writeRaw(const void* src, int len) {
    const uint8_t* s = (const uint8_t*)src;
    for (int i = 0; i < len; i++) writeByte(s[i]);
}

// ── Internal attribute helpers ──────────────────────────────

void IppWriter::writeStringAttr(uint8_t tag, const char* name,
                                const char* value) {
    uint16_t nLen = (uint16_t)strlen(name);
    uint16_t vLen = (uint16_t)strlen(value);
    writeByte(tag);
    writeShort(nLen);
    writeRaw(name, nLen);
    writeShort(vLen);
    writeRaw(value, vLen);
}

void IppWriter::writeNoNameStringAttr(uint8_t tag, const char* value) {
    uint16_t vLen = (uint16_t)strlen(value);
    writeByte(tag);
    writeShort(0);
    writeShort(vLen);
    writeRaw(value, vLen);
}

void IppWriter::writeIntAttr(uint8_t tag, const char* name, int32_t value) {
    uint16_t nLen = (uint16_t)strlen(name);
    writeByte(tag);
    writeShort(nLen);
    writeRaw(name, nLen);
    writeShort(4);
    writeInt((uint32_t)value);
}

void IppWriter::writeNoNameIntAttr(uint8_t tag, int32_t value) {
    writeByte(tag);
    writeShort(0);
    writeShort(4);
    writeInt((uint32_t)value);
}

// ── Header & structure ──────────────────────────────────────

void IppWriter::writeHeader(uint8_t vMajor, uint8_t vMinor,
                            uint16_t status, uint32_t reqId) {
    writeByte(vMajor);
    writeByte(vMinor);
    writeShort(status);
    writeInt(reqId);
}

void IppWriter::beginGroup(uint8_t groupTag) {
    writeByte(groupTag);
}

void IppWriter::endAttributes() {
    writeByte(IPP_TAG_END);
}

// ── String-type attributes ──────────────────────────────────

void IppWriter::addCharset(const char* n, const char* v)  { writeStringAttr(IPP_TAG_CHARSET,  n, v); }
void IppWriter::addLanguage(const char* n, const char* v) { writeStringAttr(IPP_TAG_LANGUAGE, n, v); }
void IppWriter::addText(const char* n, const char* v)     { writeStringAttr(IPP_TAG_TEXT,     n, v); }
void IppWriter::addName(const char* n, const char* v)     { writeStringAttr(IPP_TAG_NAME,     n, v); }
void IppWriter::addKeyword(const char* n, const char* v)  { writeStringAttr(IPP_TAG_KEYWORD,  n, v); }
void IppWriter::addUri(const char* n, const char* v)      { writeStringAttr(IPP_TAG_URI,      n, v); }
void IppWriter::addMimeType(const char* n, const char* v) { writeStringAttr(IPP_TAG_MIME_TYPE, n, v); }

// ── Numeric attributes ──────────────────────────────────────

void IppWriter::addInteger(const char* n, int32_t v) { writeIntAttr(IPP_TAG_INTEGER, n, v); }
void IppWriter::addEnum(const char* n, int32_t v)    { writeIntAttr(IPP_TAG_ENUM, n, v); }

void IppWriter::addBoolean(const char* name, bool value) {
    uint16_t nLen = (uint16_t)strlen(name);
    writeByte(IPP_TAG_BOOLEAN);
    writeShort(nLen);
    writeRaw(name, nLen);
    writeShort(1);
    writeByte(value ? 1 : 0);
}

void IppWriter::addRange(const char* name, int32_t lower, int32_t upper) {
    uint16_t nLen = (uint16_t)strlen(name);
    writeByte(IPP_TAG_RANGE);
    writeShort(nLen);
    writeRaw(name, nLen);
    writeShort(8);
    writeInt((uint32_t)lower);
    writeInt((uint32_t)upper);
}

void IppWriter::addResolution(const char* name, int32_t xRes, int32_t yRes,
                              uint8_t units) {
    uint16_t nLen = (uint16_t)strlen(name);
    writeByte(IPP_TAG_RESOLUTION);
    writeShort(nLen);
    writeRaw(name, nLen);
    writeShort(9);
    writeInt((uint32_t)xRes);
    writeInt((uint32_t)yRes);
    writeByte(units);
}

// ── Multi-value extras (name-length = 0) ────────────────────

void IppWriter::addKeywordExtra(const char* v)  { writeNoNameStringAttr(IPP_TAG_KEYWORD, v); }
void IppWriter::addMimeTypeExtra(const char* v) { writeNoNameStringAttr(IPP_TAG_MIME_TYPE, v); }
void IppWriter::addEnumExtra(int32_t v)          { writeNoNameIntAttr(IPP_TAG_ENUM, v); }
void IppWriter::addIntegerExtra(int32_t v)       { writeNoNameIntAttr(IPP_TAG_INTEGER, v); }

void IppWriter::addResolutionExtra(int32_t xRes, int32_t yRes, uint8_t units) {
    writeByte(IPP_TAG_RESOLUTION);
    writeShort(0);
    writeShort(9);
    writeInt((uint32_t)xRes);
    writeInt((uint32_t)yRes);
    writeByte(units);
}

// ── Collection support ──────────────────────────────────────

void IppWriter::beginCollection(const char* name) {
    uint16_t nLen = (uint16_t)strlen(name);
    writeByte(IPP_TAG_BEG_COLLECTION);
    writeShort(nLen);
    writeRaw(name, nLen);
    writeShort(0); // value-length always 0 for beginCollection
}

void IppWriter::beginCollectionExtra() {
    writeByte(IPP_TAG_BEG_COLLECTION);
    writeShort(0);
    writeShort(0);
}

void IppWriter::memberName(const char* name) {
    uint16_t nLen = (uint16_t)strlen(name);
    writeByte(IPP_TAG_MEMBER_NAME);
    writeShort(0);           // attribute name is empty
    writeShort(nLen);        // value is the member name
    writeRaw(name, nLen);
}

void IppWriter::memberInteger(int32_t value) {
    writeByte(IPP_TAG_INTEGER);
    writeShort(0);
    writeShort(4);
    writeInt((uint32_t)value);
}

void IppWriter::endCollection() {
    writeByte(IPP_TAG_END_COLLECTION);
    writeShort(0);
    writeShort(0);
}
