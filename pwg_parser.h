#ifndef PWG_PARSER_H
#define PWG_PARSER_H

// Parse a PWG Raster document from the request body and store pages to SD.
// Reads document data via ippReadBody() (handles chunked encoding).
//
// docId:          SD storage document ID (from sdCreateDocument)
// bytesAvailable: document body bytes remaining (-1 if unknown/chunked)
//
// Returns the number of pages successfully parsed and stored, or -1 on error.
int pwgParseDocument(int docId, int bytesAvailable);

#endif // PWG_PARSER_H
