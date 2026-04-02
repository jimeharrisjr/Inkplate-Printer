#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <stdint.h>

// Maximum tracked documents (in-memory manifest).
#define MAX_DOCUMENTS 200

// Initialize storage subsystem. Creates /print/ if needed, loads manifest.
// Call after hwInitSD() succeeds.
bool sdStorageInit();

// True if storage is ready (SD mounted and /print/ exists).
bool sdStorageReady();

// Create a new document directory. Returns doc ID (>0) or -1 on failure.
int sdCreateDocument(const char* name);

// Open a page file for streaming row writes.
bool sdOpenPageWrite(int docId, int pageNum);

// Write one row of pixel data (1 byte per pixel) to the open page file.
bool sdWritePageRow(const uint8_t* row, int len);

// Close the currently open page file.
void sdClosePageWrite();

// Finalize a document: write meta.txt and add to manifest.
bool sdFinalizeDocument(int docId, int pageCount, int width, int height);

// Number of documents in the manifest.
int sdGetDocumentCount();

// Get the doc ID at a given index (0 = oldest).
int sdGetDocumentId(int index);

// Get the page count for a document (reads meta.txt).
int sdGetPageCount(int docId);

// Get the document name (reads meta.txt).
bool sdGetDocumentName(int docId, char* buf, int maxLen);

// Get the stored dimensions (reads meta.txt).
bool sdGetDocumentDimensions(int docId, int* w, int* h);

// Delete a specific document (files + directory + manifest entry).
bool sdDeleteDocument(int docId);

// Delete the oldest document. Returns true if one was deleted.
bool sdDeleteOldest();

#endif // SD_STORAGE_H
