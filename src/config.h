// config.h
#pragma once

#if !DEBUG_MATRIX
#define DEBUG_MATRIX 0
#endif

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <vector>

// ====== Wi-Fi and Captive Portal Settings ======
constexpr char *fallbackSSID = "ESP32_AP";     // Default SSID for captive portal
constexpr char *fallbackPassword = "test1234"; // Default password for testing
#define MAX_CLIENTS 4
#define WIFI_CHANNEL 6

const IPAddress portalIP(4, 3, 2, 1);
const IPAddress gatewayIP(4, 3, 2, 1);
const IPAddress subnetMask(255, 255, 255, 0);
const String portalURL = "http://4.3.2.1/index.html";

// Path to JSON configuration on SD card
constexpr char CONFIG_PATH[] = "/config.json";
#if !SD_CS
#define SD_CS 22
#endif

#if !SD_MOSI
#define SD_MOSI MOSI
#endif
#if !SD_MISO
#define SD_MISO MISO
#endif
#if !SD_SCK
#define SD_SCK SCK
#endif

// ——— Per-panel layout using WLED flags ———
struct PanelConfig
{
    uint16_t x, y;    // panel origin in the big matrix
    uint16_t w, h;    // panel dimensions
    bool bottomFirst; // 'b' start at bottom edge
    bool rightFirst;  // 'r' start at right edge
    bool vertical;    // 'v' run strips vertically (else horizontally)
    bool serpentine;  // 's' zig-zag every other strip
};

class ConfigReader
{
public:
    // LEDs + matrix
    uint16_t totalLEDs, startLED, stripLen, skipLEDs;
    uint8_t pin, order;
    bool reverse;
    uint16_t width, height;
    std::vector<PanelConfig> panels;

    // Wi-Fi
    String wifiSsid;
    String wifiPassword;

    // Captive portal
    String apSSID;
    String apPassword;
    uint8_t apChannel = WIFI_CHANNEL;
    bool apHidden = false;

    bool loadFromSD(const char *path);
    void beginWiFi(DNSServer &dnsServer);

private:
    void startSoftAP();
    void setUpDNSServer(DNSServer &dnsServer);
    void parseDocument(JsonDocument &doc);
};
