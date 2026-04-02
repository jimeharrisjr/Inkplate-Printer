#ifndef HARDWARE_H
#define HARDWARE_H

#include <Inkplate.h>
#include "config.h"

// Initialize display hardware. Returns true on success.
bool hwInit();

// Get reference to the Inkplate display object.
Inkplate& hwDisplay();

// Initialize SD card. Returns true on success.
bool hwInitSD();

// Read a touchpad (PAD1, PAD2, or PAD3). Returns true if touched.
bool hwReadTouchpad(uint8_t pad);

// Trigger a full display refresh.
void hwFullRefresh();

// Attempt a partial display refresh. Returns false if unsupported (Color model).
// Only works in 1-bit display mode on mono Inkplate.
bool hwPartialRefresh();

// Switch display mode (mono only). true=1-bit (fast/partial), false=3-bit (quality).
// No-op on Color model.
void hwSetDisplayMode1Bit(bool is1Bit);

// Clear the display framebuffer (does not refresh screen).
void hwClearDisplay();

// Return human-readable model name.
const char* hwModelName();

#endif // HARDWARE_H
