// config.h
#pragma once

#if !DEBUG_MATRIX
#define DEBUG_MATRIX 0
#endif

#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <vector>

// ====== Wi-Fi and Captive Portal Settings ======
const char *fallbackSSID = "ESP32_AP";     // Default SSID for captive portal
const char *fallbackPassword = "test1234"; // Default password for testing
#define MAX_CLIENTS 4
#define WIFI_CHANNEL 6

const IPAddress portalIP(4, 3, 2, 1);
const IPAddress gatewayIP(4, 3, 2, 1);
const IPAddress subnetMask(255, 255, 255, 0);
const String portalURL = "http://4.3.2.1/index.html";

DNSServer dnsServer;

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

    bool loadFromSD(const char *path)
    {
        // Serial.printf("Checking SD card at Pins: CS=%d, MOSI=%d, MISO=%d, SCK=%d\n", SD_CS, SD_MOSI, SD_MISO, -1);
#if DEBUG_MATRIX
        delay(1000);
        Serial.printf("Checking SD card at Pins: CS=%d, MOSI=%d, MISO=%d, SCK=%d\n", SD_CS, SD_MOSI, SD_MISO, -1);
#endif

        SPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
        if (!SD.begin(SD_CS, SPI, 4000000))
        {
            Serial.println("❌ SD init failed!");
            return false;
        }
        File f = SD.open(path);
        if (!f)
        {
            Serial.printf("❌ Failed to open %s\n", path);
            return false;
        }

        DynamicJsonDocument doc(64 * 1024);
        auto err = deserializeJson(doc, f);
        f.close();
        if (err)
        {
            Serial.printf("❌ JSON parse error: %s\n", err.c_str());
            return false;
        }
        parseDocument(doc);
        return true;
    }

    void beginWiFi()
    {
        if (wifiSsid.isEmpty())
        {
            Serial.println("⚠️ No Wi-Fi creds in config");
            return;
        }
        Serial.printf("Connecting to Wi-Fi \"%s\" …\n", wifiSsid.c_str());
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
        {
            delay(500);
            Serial.print('.');
        }
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("❌ Wi-Fi failed, starting captive AP…");
            startSoftAP();
            setUpDNSServer();
        }
        Serial.println();
        Serial.print("📶 IP Address: ");
        Serial.println(WiFi.localIP());
    }

private:
    void startSoftAP()
    {
        WiFi.mode(WIFI_MODE_AP);
        WiFi.softAPConfig(portalIP, gatewayIP, subnetMask);

        if (apSSID.isEmpty())
        {
            WiFi.softAP(fallbackSSID, fallbackPassword, WIFI_CHANNEL, 0, MAX_CLIENTS);
            Serial.printf("⚠️ No AP SSID in config, using fallback SSID: \"%s\"\n", fallbackSSID);
            Serial.printf("⚠️ No AP Password in config, using fallback Password: \"%s\"\n", fallbackPassword);
        }
        else
        {
            WiFi.softAP(apSSID.c_str(), apPassword.c_str(), apChannel, apHidden, MAX_CLIENTS);
            Serial.printf("📡 SoftAP started: SSID=%s, Channel=%d, Hidden=%d\n", apSSID.c_str(), apChannel, apHidden);
        }

        // disable AMPDU RX bug on Android
        esp_wifi_stop();
        esp_wifi_deinit();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.ampdu_rx_enable = false;
        esp_wifi_init(&cfg);
        esp_wifi_start();
    }

    void setUpDNSServer()
    {
        dnsServer.setTTL(300);
        dnsServer.start(53, "*", portalIP);
    }

    void parseDocument(JsonDocument &doc)
    {
        // — Parse LED/matrix section —
        auto hwLed = doc["hw"]["led"].as<JsonObject>();
        totalLEDs = hwLed["total"].as<uint16_t>();
        auto ins0 = hwLed["ins"][0].as<JsonObject>();
        startLED = ins0["start"].as<uint16_t>();
        stripLen = ins0["len"].as<uint16_t>();
        skipLEDs = ins0["skip"].as<uint16_t>();
        pin = ins0["pin"][0].as<uint8_t>();
        order = ins0["order"].as<uint8_t>();
        reverse = ins0["rev"].as<bool>();

        Serial.printf("LEDs: total=%d, start=%d, len=%d, skip=%d, pin=%d, order=%d, reverse=%d\n",
                      totalLEDs, startLED, stripLen, skipLEDs, pin, order, reverse);

        // — Parse panels using WLED flags —
        auto panelsArr = hwLed["matrix"]["panels"].as<JsonArray>();
        width = height = 0;
        panels.clear();
        for (auto p : panelsArr)
        {
            PanelConfig pc;
            pc.x = p["x"].as<uint16_t>();
            pc.y = p["y"].as<uint16_t>();
            pc.w = p["w"].as<uint16_t>();
            pc.h = p["h"].as<uint16_t>();
            pc.bottomFirst = p["b"].as<bool>();
            pc.rightFirst = p["r"].as<bool>();
            pc.vertical = p["v"].as<bool>();
            pc.serpentine = p["s"].as<bool>();
            panels.push_back(pc);
            width = max<uint16_t>(width, pc.x + pc.w);
            height = max<uint16_t>(height, pc.y + pc.h);
        }

        Serial.printf("Matrix: width=%d, height=%d, panels=%zu\n", width, height, panels.size());

        // — Parse Wi-Fi section —
        auto wifi = doc["wifi"].as<JsonObject>();
        wifiSsid = wifi["ssid"].as<const char *>();
        wifiPassword = wifi["password"].as<const char *>();

        Serial.printf("Wi-Fi: SSID=%s\n", wifiSsid.c_str());

        auto ap = doc["ap"].as<JsonObject>();
        if (ap.containsKey("ssid"))
            apSSID = ap["ssid"].as<const char *>();
        if (ap.containsKey("password"))
            apPassword = ap["password"].as<const char *>();
        if (ap.containsKey("chan"))
            apChannel = ap["chan"].as<uint8_t>();
        if (ap.containsKey("hide"))
            apHidden = ap["hide"].as<bool>();

        Serial.printf("AP: SSID=%s, Channel=%d, Hidden=%d\n", apSSID.c_str(), apChannel, apHidden);
    }
};
