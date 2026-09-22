// config.h
#pragma once

#if !DEBUG_MATRIX
#define DEBUG_MATRIX 0
#endif

// ——— Storage: SD preferred, LittleFS fallback ———
// Both headers must be included so both filesystems can be probed at runtime.
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
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

// Path to JSON configuration (same path on both SD and LittleFS)
constexpr char CONFIG_PATH[] = "/config.json";

// SD SPI pin defaults (override via build_flags: -D SD_CS=4 etc.)
#if !defined(SD_CS)
#define SD_CS 22
#endif
#if !defined(SD_MOSI)
#define SD_MOSI MOSI
#endif
#if !defined(SD_MISO)
#define SD_MISO MISO
#endif
#if !defined(SD_SCK)
#define SD_SCK SCK
#endif

// ——— Global active filesystem ———
// After initFS() this points to whichever FS is actually in use.
// All runtime code (main.cpp, matrix_driver.h) uses `activeFS` exclusively.
namespace FSMount
{
    enum Source
    {
        NONE,
        SD_CARD,
        LITTLEFS
    };
    extern Source source; // defined in config.h (inline via first TU that includes it)
    extern fs::FS *fs;
}

// Only one translation unit should define these; guard with a macro set in main.cpp.
#ifdef FS_MOUNT_DEFINE
namespace FSMount
{
    Source source = NONE;
    fs::FS *fs = nullptr;
}
#endif

// Convenience accessor — use this everywhere instead of SD / LittleFS directly.
inline fs::FS &activeFS() { return *FSMount::fs; }

// ——— Try to mount SD, then LittleFS.  Returns true on success. ———
inline bool initFS()
{
    // — Try SD first —
    Serial.println("🔍 Trying SD card…");
#if DEBUG_MATRIX
    Serial.printf("   SD pins: CS=%d MOSI=%d MISO=%d SCK=%d\n",
                  SD_CS, SD_MOSI, SD_MISO, SD_SCK);
#endif
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
    if (SD.begin(SD_CS, SPI, 4000000))
    {
        // Make sure config.json is actually on the card
        if (SD.exists(CONFIG_PATH))
        {
            Serial.println("✅ SD card mounted — using SD");
            FSMount::source = FSMount::SD_CARD;
            FSMount::fs = &SD;
            return true;
        }
        Serial.println("⚠️  SD mounted but config.json not found — falling back to LittleFS");
        SD.end(); // release SPI bus cleanly
    }
    else
    {
        Serial.println("⚠️  SD init failed — falling back to LittleFS");
    }

    // — Fall back to LittleFS —
    Serial.println("🔍 Trying LittleFS…");
    if (!LittleFS.begin(true)) // true = format on first use
    {
        Serial.println("❌ LittleFS mount failed!");
        return false;
    }
    if (!LittleFS.exists(CONFIG_PATH))
    {
        Serial.println("❌ LittleFS mounted but config.json not found!");
        Serial.println("ℹ️  Flash a filesystem image with: pio run --target uploadfs");
        return false;
    }
    Serial.println("✅ LittleFS mounted — using LittleFS");
    FSMount::source = FSMount::LITTLEFS;
    FSMount::fs = &LittleFS;
    return true;
}

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

    // Wi-Fi supervision (see maintainWiFi)
    static constexpr unsigned long kBootConnectMs = 30000;     // was 10 s
    static constexpr unsigned long kRetryMs = 15000;           // first retry
    static constexpr unsigned long kMaxRetryMs = 120000;       // backoff cap
    static constexpr unsigned long kRestartMs = 10UL * 60000;  // give up → reboot
    bool apFallback = false;
    bool wasConnected = true;
    unsigned long lastConnectedMs = 0;
    unsigned long lastAttemptMs = 0;
    unsigned long retryDelayMs = kRetryMs;

    // Log why the link went away — the reason code is the single most useful
    // clue next time it drops (e.g. 200 BEACON_TIMEOUT, 8 ASSOC_LEAVE,
    // 2/15 auth expired / 4-way handshake timeout).
    static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
    {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
            Serial.printf("⚠️ Wi-Fi disconnected, reason=%d t=%lus heap=%u maxblk=%u\n",
                          (int)info.wifi_sta_disconnected.reason, millis() / 1000,
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
            Serial.printf("📶 got IP %s\n", WiFi.localIP().toString().c_str());
    }

    // Call this once — mounts SD or LittleFS and reads config.json.
    bool loadConfig(const char *path)
    {
        if (!initFS())
            return false;

        File f = activeFS().open(path, FILE_READ);
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

    // Legacy shim — main.cpp calls loadFromSD(); redirect to loadConfig().
    bool loadFromSD(const char *path) { return loadConfig(path); }

    void beginWiFi()
    {
        if (wifiSsid.isEmpty())
        {
            Serial.println("⚠️ No Wi-Fi creds in config");
            return;
        }
        // Settings that keep the station link stable on a busy network:
        //  - no modem sleep: with it on, the radio naps between beacons and
        //    busy APs / heavy ISR load (the RMT LED refill) make it miss
        //    them until the AP drops us.
        //  - don't write credentials to NVS on every begin() (flash wear).
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        WiFi.onEvent(onWiFiEvent);

        Serial.printf("Connecting to Wi-Fi \"%s\" …\n", wifiSsid.c_str());
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < kBootConnectMs)
        {
            delay(500);
            Serial.print('.');
        }
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("❌ Wi-Fi failed, starting captive AP (still retrying STA)…");
            startSoftAP();
            setUpDNSServer();
            apFallback = true;
        }
        Serial.println();
        Serial.print("📶 IP Address: ");
        if (apFallback)
            Serial.println(WiFi.softAPIP());
        else
            Serial.println(WiFi.localIP());
        lastConnectedMs = millis();
        lastAttemptMs = millis();
    }

    // Call every loop(). Keeps the station connected to the configured
    // network:
    //  - if the link has been down for kRetryMs, force a fresh begin()
    //    (the core's auto-reconnect gives up on some disconnect reasons);
    //  - if it has been down for kRestartMs despite that, reboot;
    //  - while in AP fallback, keep trying the real network in the
    //    background and reboot into normal STA mode once it's reachable.
    void maintainWiFi(unsigned long now)
    {
        if (wifiSsid.isEmpty())
            return;

        if (WiFi.status() == WL_CONNECTED)
        {
            if (apFallback)
            {
                // Captive-portal routes + DNS are still registered; a clean
                // reboot is the simplest way back to plain STA mode.
                Serial.println("📶 Home Wi-Fi reachable again — rebooting out of AP fallback");
                Serial.flush();
                delay(100);
                ESP.restart();
            }
            if (!wasConnected)
            {
                Serial.printf("📶 Wi-Fi back: %s RSSI=%d\n",
                              WiFi.localIP().toString().c_str(), WiFi.RSSI());
                wasConnected = true;
            }
            lastConnectedMs = now;
            retryDelayMs = kRetryMs;
            return;
        }

        if (wasConnected)
        {
            Serial.println("⚠️ Wi-Fi link down");
            wasConnected = false;
        }

        if (now - lastAttemptMs >= retryDelayMs)
        {
            Serial.printf("🔁 Wi-Fi reconnect attempt (down %lus, status=%d)\n",
                          (now - lastConnectedMs) / 1000, (int)WiFi.status());
            lastAttemptMs = now;
            WiFi.disconnect(false, false); // keep the radio + any AP up
            WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
            // Back off so a long outage doesn't hammer the AP (and, in AP
            // fallback, doesn't keep interrupting portal clients' channel).
            retryDelayMs = min<unsigned long>(retryDelayMs * 2, kMaxRetryMs);
        }

        if (!apFallback && now - lastConnectedMs >= kRestartMs)
        {
            Serial.println("❌ Wi-Fi down too long — rebooting");
            Serial.flush();
            delay(100);
            ESP.restart();
        }
    }

    bool inApFallback() const { return apFallback; }

private:
    void startSoftAP()
    {
        // AP+STA so maintainWiFi() can keep trying the real network.
        WiFi.mode(WIFI_MODE_APSTA);
        WiFi.softAPConfig(portalIP, gatewayIP, subnetMask);

        if (apSSID.isEmpty())
        {
            WiFi.softAP(fallbackSSID, fallbackPassword, WIFI_CHANNEL, 0, MAX_CLIENTS);
            Serial.printf("⚠️ Using fallback SSID \"%s\"\n", fallbackSSID);
        }
        else
        {
            WiFi.softAP(apSSID.c_str(), apPassword.c_str(), apChannel, apHidden, MAX_CLIENTS);
            Serial.printf("📡 SoftAP: SSID=%s Channel=%d Hidden=%d\n",
                          apSSID.c_str(), apChannel, apHidden);
        }

        // NOTE: an earlier "disable AMPDU RX" workaround used to run here —
        // esp_wifi_stop()/deinit()/init()/start() AFTER softAP(). On current
        // ESP32 cores that tore down the AP we'd just configured (IP came up as
        // 0.0.0.0) and tripped the task watchdog (TG1WDT_SYS_RST) during setup.
        // It's removed: the AP now stays up with its configured IP. If you ever
        // truly need AMPDU-RX disabled for an Android captive-portal quirk, do
        // it BEFORE WiFi.mode()/softAP(), not after — never deinit a live AP.
    }

    void setUpDNSServer()
    {
        dnsServer.setTTL(300);
        dnsServer.start(53, "*", portalIP);
    }

    void parseDocument(JsonDocument &doc)
    {
        // — LED / matrix —
        auto hwLed = doc["hw"]["led"].as<JsonObject>();
        totalLEDs = hwLed["total"].as<uint16_t>();
        auto ins0 = hwLed["ins"][0].as<JsonObject>();
        startLED = ins0["start"].as<uint16_t>();
        stripLen = ins0["len"].as<uint16_t>();
        skipLEDs = ins0["skip"].as<uint16_t>();
        pin = ins0["pin"][0].as<uint8_t>();
        order = ins0["order"].as<uint8_t>();
        reverse = ins0["rev"].as<bool>();

        Serial.printf("LEDs: total=%d start=%d len=%d skip=%d pin=%d order=%d rev=%d\n",
                      totalLEDs, startLED, stripLen, skipLEDs, pin, order, reverse);

        // — Panels —
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
        Serial.printf("Matrix: %dx%d panels=%zu\n", width, height, panels.size());

        // — Wi-Fi —
        auto wifi = doc["wifi"].as<JsonObject>();
        wifiSsid = wifi["ssid"].as<const char *>();
        wifiPassword = wifi["password"].as<const char *>();
        Serial.printf("Wi-Fi SSID: %s\n", wifiSsid.c_str());

        // — AP —
        auto ap = doc["ap"].as<JsonObject>();
        if (ap.containsKey("ssid"))
            apSSID = ap["ssid"].as<const char *>();
        if (ap.containsKey("password"))
            apPassword = ap["password"].as<const char *>();
        if (ap.containsKey("chan"))
            apChannel = ap["chan"].as<uint8_t>();
        if (ap.containsKey("hide"))
            apHidden = ap["hide"].as<bool>();
        Serial.printf("AP: SSID=%s Channel=%d Hidden=%d\n",
                      apSSID.c_str(), apChannel, apHidden);
    }
};
