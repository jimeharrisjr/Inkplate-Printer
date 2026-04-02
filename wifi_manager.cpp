#include "wifi_manager.h"
#include "hardware.h"
#include "config.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ── State ───────────────────────────────────────────────────
static bool     sConnected  = false;
static bool     sOffline    = false;
static bool     sAPMode     = false;
static char     sIPBuf[16]  = "";
static char     sSSID[33]   = "";
static char     sPass[65]   = "";

static WebServer  sWebServer(80);
static DNSServer  sDNSServer;
static Preferences sPrefs;

// ── Captive-portal HTML ─────────────────────────────────────
static const char SETUP_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Inkplate Printer Setup</title>
<style>
body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px}
h1{color:#333}
input[type=text],input[type=password]{width:100%;padding:10px;margin:8px 0;box-sizing:border-box}
button{width:100%;padding:12px;margin:8px 0;background:#333;color:#fff;border:none;cursor:pointer;font-size:16px}
button:hover{background:#555}
.offline{background:#888}
</style>
</head>
<body>
<h1>Inkplate Printer Setup</h1>
<form action="/save" method="POST">
<label>WiFi Network (SSID):</label>
<input type="text" name="ssid" required maxlength="32">
<label>Password:</label>
<input type="password" name="password" maxlength="64">
<button type="submit">Connect</button>
</form>
<form action="/offline" method="POST">
<button type="submit" class="offline">Continue Offline</button>
</form>
</body>
</html>
)rawliteral";

static const char SAVED_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title></head>
<body style="font-family:sans-serif;text-align:center;margin-top:60px">
<h1>Credentials Saved</h1>
<p>The Inkplate is restarting and will connect to your network.</p>
</body>
</html>
)rawliteral";

// ── NVS helpers ─────────────────────────────────────────────
static bool loadCredentials() {
    sPrefs.begin("inkplate", true);
    size_t ssidLen = sPrefs.getString("ssid", sSSID, sizeof(sSSID));
    sPrefs.getString("pass", sPass, sizeof(sPass));
    sPrefs.end();
    return ssidLen > 0;
}

static void saveCredentials(const char* ssid, const char* pass) {
    sPrefs.begin("inkplate", false);
    sPrefs.putString("ssid", ssid);
    sPrefs.putString("pass", pass);
    sPrefs.end();
}

// ── Display helpers ─────────────────────────────────────────
static void showConnecting() {
    Inkplate& d = hwDisplay();
    hwClearDisplay();
    d.setTextColor(COLOR_BLACK, COLOR_WHITE);
    d.setTextSize(3);
    d.setCursor(10, 10);
    d.print("Inkplate Printer");
    d.setTextSize(2);
    d.setCursor(10, 55);
    d.print("Connecting to WiFi...");
    d.setCursor(10, 85);
    d.print("SSID: ");
    d.print(sSSID);
    hwFullRefresh();
}

static void showConnected() {
    Inkplate& d = hwDisplay();
    hwClearDisplay();
    d.setTextColor(COLOR_BLACK, COLOR_WHITE);
    d.setTextSize(3);
    d.setCursor(10, 10);
    d.print("Inkplate Printer");
    d.setTextSize(2);
    d.setCursor(10, 55);
    d.print("WiFi Connected");
    d.setCursor(10, 85);
    d.print("SSID: ");
    d.print(sSSID);
    d.setCursor(10, 115);
    d.print("IP:   ");
    d.print(sIPBuf);
    d.setCursor(10, 155);
    d.print("Printer: ");
    d.print(PRINTER_NAME);
    hwFullRefresh();
}

static void showAPMode() {
    Inkplate& d = hwDisplay();
    hwClearDisplay();
    d.setTextColor(COLOR_BLACK, COLOR_WHITE);
    d.setTextSize(3);
    d.setCursor(10, 10);
    d.print("WiFi Setup");
    d.setTextSize(2);
    d.setCursor(10, 55);
    d.print("1. Connect your phone or");
    d.setCursor(10, 80);
    d.print("   computer to WiFi network:");
    d.setTextSize(2);
    d.setCursor(10, 115);
    d.print("   ");
    d.print(WIFI_AP_SSID);
    d.setCursor(10, 155);
    d.print("2. Open in browser:");
    d.setCursor(10, 185);
    d.print("   http://192.168.4.1");
    hwFullRefresh();
}

static void showSaved() {
    Inkplate& d = hwDisplay();
    hwClearDisplay();
    d.setTextColor(COLOR_BLACK, COLOR_WHITE);
    d.setTextSize(3);
    d.setCursor(10, 10);
    d.print("Credentials Saved");
    d.setTextSize(2);
    d.setCursor(10, 55);
    d.print("Restarting...");
    hwFullRefresh();
}

static void showOffline() {
    Inkplate& d = hwDisplay();
    hwClearDisplay();
    d.setTextColor(COLOR_BLACK, COLOR_WHITE);
    d.setTextSize(3);
    d.setCursor(10, 10);
    d.print("Inkplate Printer");
    d.setTextSize(2);
    d.setCursor(10, 55);
    d.print("Offline Mode");
    d.setCursor(10, 85);
    d.print("Browsing stored documents only.");
    hwFullRefresh();
}

static void showConnectionFailed() {
    Inkplate& d = hwDisplay();
    hwClearDisplay();
    d.setTextColor(COLOR_BLACK, COLOR_WHITE);
    d.setTextSize(3);
    d.setCursor(10, 10);
    d.print("Connection Failed");
    d.setTextSize(2);
    d.setCursor(10, 55);
    d.print("Could not connect to:");
    d.setCursor(10, 80);
    d.print(sSSID);
    d.setCursor(10, 120);
    d.print("Starting setup mode...");
    hwFullRefresh();
    delay(2000);
}

// ── AP mode web handlers ────────────────────────────────────
static void handleRoot() {
    sWebServer.send_P(200, "text/html", SETUP_PAGE);
}

static void handleSave() {
    if (!sWebServer.hasArg("ssid") || sWebServer.arg("ssid").length() == 0) {
        sWebServer.send(400, "text/plain", "SSID is required");
        return;
    }
    const String& ssid = sWebServer.arg("ssid");
    const String& pass = sWebServer.arg("password");

    char ssidBuf[33] = {};
    char passBuf[65] = {};
    ssid.toCharArray(ssidBuf, sizeof(ssidBuf));
    pass.toCharArray(passBuf, sizeof(passBuf));

    saveCredentials(ssidBuf, passBuf);
    Serial.print("[WIFI] Saved credentials for: ");
    Serial.println(ssidBuf);

    sWebServer.send_P(200, "text/html", SAVED_PAGE);
    showSaved();
    delay(1500);
    ESP.restart();
}

static void handleOffline() {
    sWebServer.send(200, "text/html",
        "<html><body style='font-family:sans-serif;text-align:center;margin-top:60px'>"
        "<h1>Offline Mode</h1><p>You may disconnect.</p></body></html>");

    Serial.println("[WIFI] User chose offline mode");
    sOffline = true;
    sAPMode  = false;

    // Shut down AP and web server
    sWebServer.stop();
    sDNSServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    showOffline();
}

// ── AP mode startup ─────────────────────────────────────────
static void startAPMode() {
    Serial.println("[WIFI] Starting AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    delay(100); // let AP stabilize

    IPAddress apIP = WiFi.softAPIP();
    Serial.print("[WIFI] AP IP: ");
    Serial.println(apIP);

    // DNS server: redirect all domains to our IP (captive portal)
    sDNSServer.start(53, "*", apIP);

    // Web server routes
    sWebServer.on("/",        HTTP_GET,  handleRoot);
    sWebServer.on("/save",    HTTP_POST, handleSave);
    sWebServer.on("/offline", HTTP_POST, handleOffline);
    sWebServer.onNotFound(handleRoot);  // all unknown routes → setup page
    sWebServer.begin();

    sAPMode = true;
    showAPMode();
}

// ── Public API ──────────────────────────────────────────────
void wifiManagerInit() {
    if (loadCredentials()) {
        Serial.print("[WIFI] Stored SSID: ");
        Serial.println(sSSID);
        showConnecting();

        WiFi.mode(WIFI_STA);
        WiFi.begin(sSSID, sPass);

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED &&
               millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            WiFi.localIP().toString().toCharArray(sIPBuf, sizeof(sIPBuf));
            sConnected = true;
            Serial.print("[WIFI] Connected, IP: ");
            Serial.println(sIPBuf);
            showConnected();
            return;
        }

        Serial.println("[WIFI] Connection failed");
        showConnectionFailed();
    } else {
        Serial.println("[WIFI] No stored credentials");
    }

    startAPMode();
}

void wifiManagerLoop() {
    if (sAPMode) {
        sDNSServer.processNextRequest();
        sWebServer.handleClient();
    }
}

bool wifiIsConnected() {
    return sConnected;
}

bool wifiIsOffline() {
    return sOffline;
}

const char* wifiGetIP() {
    return sIPBuf;
}
