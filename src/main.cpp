// main.cpp — LED Matrix Sign 2.0
// Compositor architecture: FrameFactory drives two layers (API overlay +
// primary FS animations) into a single output FrameBuffer that is pushed to
// the panel every loop().
//
// Storage priority: SD card (if present + has config.json) -> LittleFS.
// All FS access goes through activeFS() defined in config.h.

#define FS_MOUNT_DEFINE // instantiate FSMount::source / FSMount::fs here
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "matrix_driver.h"
#include "anim/framefactory.h"
#include "storage.h"
#include "api/api_v2.h"

// ——— Globals ———
AsyncWebServer server(80);
ConfigReader config;
MatrixDriver *driver = nullptr;
FrameFactory factory;
ApiV2 *api = nullptr;

// ====== Captive-portal (AP fallback) ======
// 2.0 drops the bundled web frontend, but we keep a minimal captive portal so
// the device is reachable when it falls back to its own access point.
void setUpAPServer()
{
    server.on("/generate_204", HTTP_ANY, [](AsyncWebServerRequest *req)
              { req->redirect(portalURL); });
    server.onNotFound([](AsyncWebServerRequest *req)
                      { req->send(200, "application/json",
                                  "{\"status\":\"ap\",\"info\":\"LED Matrix Sign 2.0 — use the REST API\"}"); });
}

// ====== setup() ======
void setup()
{
    Serial.begin(115200);
    Serial.println("LED Matrix Sign 2.0 — starting up…");

    // Mount SD (preferred) or LittleFS (fallback), then parse config.json
    if (!config.loadFromSD(CONFIG_PATH))
    {
        Serial.println("❌ Config load failed — halting");
        for (;;)
            delay(1000);
    }

    config.beginWiFi();
    if (WiFi.status() != WL_CONNECTED)
        setUpAPServer();
    else
        Serial.println("Wi-Fi connected: " + WiFi.localIP().toString());

    // Matrix driver
    driver = new MatrixDriver(config);
    driver->begin();

    // Compositor: size both layers + output to the panel dimensions
    factory.init(config.width, config.height);
    factory.setLoader(Storage::loadAnimation); // primary buffer loads FS anims

    // Make sure /anim exists for uploads + persistence
    Storage::ensureBaseDirs();

    // REST API 2.0
    api = new ApiV2(server, driver, factory, config);
    api->begin();

    server.begin();
    Serial.println("HTTP server up. Heap free: " + String(ESP.getFreeHeap()));
}

// ====== loop() ======
void loop()
{
    if (WiFi.getMode() == WIFI_MODE_AP)
        dnsServer.processNextRequest();

    // Advance the compositor timeline and push the composed frame to the panel.
    const unsigned long now = millis();
    factory.render(now);
    driver->showFrameBuffer(factory.out);
}
