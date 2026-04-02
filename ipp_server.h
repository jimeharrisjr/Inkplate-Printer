#ifndef IPP_SERVER_H
#define IPP_SERVER_H

#include <stdint.h>

// Start mDNS advertisement and TCP server on port 631.
void ippServerInit();

// Accept and handle incoming connections. Call from loop().
void ippServerLoop();

// Read request body bytes (handles both Content-Length and chunked encoding).
// Used by ipp_server internals and by pwg_parser.
// Returns number of bytes read, or 0 at end of body.
int ippReadBody(uint8_t* buf, int n);

#endif // IPP_SERVER_H
