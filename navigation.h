#ifndef NAVIGATION_H
#define NAVIGATION_H

// Initialize navigation. Shows the newest document or the empty screen.
void navigationInit();

// Poll touchpads and handle input. Call from loop().
void navigationLoop();

// Notify that a new document was received (call from IPP server).
void navNotifyNewDocument();

#endif // NAVIGATION_H
