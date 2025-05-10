// config.h
#pragma once

#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <vector>

// Path to JSON configuration on SD card
constexpr char CONFIG_PATH[] = "/config.json";
#define SD_CS 22

// ——— Per-panel layout ———
struct PanelConfig
{
    bool enabled;  // panel on/off
    uint16_t x, y; // panel origin
    uint16_t w, h; // panel size
    bool rotate;   // 90° CW?
    bool flipV;    // vertical flip?
    bool serp;     // serpentine override?
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

    // ——— Load entire config.json from SD (incl. Wi-Fi) ———
    bool loadFromSD(const char *path)
    {
        // init SPI+SD
        SPI.begin();
        if (!SD.begin(SD_CS))
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

    // ——— Connect Wi-Fi using loaded creds ———
    void beginWiFi()
    {
        if (wifiSsid.isEmpty())
        {
            Serial.println("⚠️ No Wi-Fi creds in config");
            return;
        }
        Serial.printf("Connecting to Wi-Fi \"%s\" …\n", wifiSsid.c_str());
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        while (WiFi.status() != WL_CONNECTED)
        {
            delay(500);
            Serial.print('.');
        }
        Serial.println();
        Serial.print("📶 IP Address: ");
        Serial.println(WiFi.localIP());
    }

private:
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

        auto panelsArr = hwLed["matrix"]["panels"].as<JsonArray>();
        width = height = 0;
        panels.clear();
        for (auto p : panelsArr)
        {
            PanelConfig pc;
            pc.enabled = p["b"].as<bool>();
            pc.x = p["x"].as<uint16_t>();
            pc.y = p["y"].as<uint16_t>();
            pc.w = p["w"].as<uint16_t>();
            pc.h = p["h"].as<uint16_t>();
            pc.rotate = p["r"].as<bool>();
            pc.flipV = p["v"].as<bool>();
            pc.serp = p["s"].as<bool>();
            panels.push_back(pc);
            width = max<uint16_t>(width, pc.x + pc.w);
            height = max<uint16_t>(height, pc.y + pc.h);
        }

        // — Parse Wi-Fi section —
        auto wifi = doc["wifi"].as<JsonObject>();
        wifiSsid = wifi["ssid"].as<const char *>();
        wifiPassword = wifi["password"].as<const char *>();
    }
};
