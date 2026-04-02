#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// Hardware Model Selection
// ============================================================
// Uncomment exactly ONE of these lines for your Inkplate model.
// If neither is defined, defaults to INKPLATE_MODEL_MONO.

// #define INKPLATE_MODEL_MONO    // Inkplate 6 (800x600, 3-bit grayscale)
// #define INKPLATE_MODEL_COLOR   // Inkplate 6 Color (600x448, 7-color)

#if !defined(INKPLATE_MODEL_MONO) && !defined(INKPLATE_MODEL_COLOR)
  #define INKPLATE_MODEL_MONO
#endif

// ============================================================
// Printer Configuration
// ============================================================
#define PRINTER_NAME            "Inkplate-printer"
#define IPP_PORT                631

// ============================================================
// WiFi Configuration
// ============================================================
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_AP_SSID            "Inkplate-Setup"
#define WIFI_AP_PASSWORD        ""

// ============================================================
// Streaming / Buffer Sizes
// ============================================================
#define MAX_CHUNK_SIZE          4096

// ============================================================
// Derived Display & Media Constants (do not edit)
// ============================================================
#define PRINTER_DPI             150

#ifdef INKPLATE_MODEL_MONO
  #define SCREEN_WIDTH          800
  #define SCREEN_HEIGHT         600
  #define IS_COLOR_MODEL        false
  #define COLOR_BLACK           0
  #define COLOR_WHITE           7
  // 800/150 DPI = 5.333" = 135.47mm; 600/150 = 4.000" = 101.60mm
  #define MEDIA_NAME            "custom_inkplate6_135.47x101.60mm"
  #define MEDIA_WIDTH_HMM       13547   // hundredths of mm
  #define MEDIA_HEIGHT_HMM      10160
#else
  #define SCREEN_WIDTH          600
  #define SCREEN_HEIGHT         448
  #define IS_COLOR_MODEL        true
  #define COLOR_BLACK           0
  #define COLOR_WHITE           1
  // 600/150 DPI = 4.000" = 101.60mm; 448/150 = 2.987" = 75.86mm
  #define MEDIA_NAME            "custom_inkplate6color_101.60x75.86mm"
  #define MEDIA_WIDTH_HMM       10160
  #define MEDIA_HEIGHT_HMM       7586
#endif

#endif // CONFIG_H
