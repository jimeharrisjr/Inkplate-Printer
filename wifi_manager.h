#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

// Initialize WiFi: try stored credentials, fall back to AP captive portal.
// Blocks until connected or AP mode is running.
void wifiManagerInit();

// Call from loop(). Handles captive portal clients when in AP mode.
void wifiManagerLoop();

// True if connected to a WiFi network in STA mode.
bool wifiIsConnected();

// True if user chose "Continue Offline" from captive portal.
bool wifiIsOffline();

// IP address as dotted-quad string. Only valid when wifiIsConnected().
const char* wifiGetIP();

#endif // WIFI_MANAGER_H
