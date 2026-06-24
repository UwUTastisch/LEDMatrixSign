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

// ====== Static frontend ======
// 2.0 has no bundled UI compiled in, but if an index.html is present on the
// active filesystem we serve it (plus any other static assets it references).
void serveIndex(AsyncWebServerRequest *req)
{
    if (activeFS().exists("/index.html"))
        req->send(activeFS(), "/index.html", "text/html");
    else
        req->send(200, "application/json",
                  "{\"status\":\"ok\",\"info\":\"LED Matrix Sign 2.0 — REST API only "
                  "(no index.html on filesystem)\"}");
}

void setUpStaticRoutes()
{
    server.on("/", HTTP_GET, serveIndex);
    server.on("/index.html", HTTP_GET, serveIndex);
}

// ====== Captive-portal (AP fallback) ======
// Redirect unknown requests to the portal so the control panel pops up when a
// client joins the device's own access point.
void setUpAPServer()
{
    server.on("/generate_204", HTTP_ANY, [](AsyncWebServerRequest *req)
              { req->redirect(portalURL); });
    server.onNotFound([](AsyncWebServerRequest *req)
                      { req->redirect(portalURL); });
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

    // Static frontend (index.html) if present on the filesystem
    setUpStaticRoutes();

    server.begin();
    Serial.println("HTTP server up. Heap free: " + String(ESP.getFreeHeap()));
}

// ====== loop() ======
// IMPORTANT: the Arduino loop runs as a FreeRTOS task sharing the core with the
// Wi-Fi / TCP / idle tasks. It MUST yield every iteration, or the task watchdog
// fires (rst:0x8 TG1WDT_SYS_RST). We also cap rendering to a fixed frame rate so
// we don't pointlessly re-rasterise and re-clock the LED strip thousands of
// times per second.
static const uint16_t kTargetFps = 60;
static const unsigned long kFrameIntervalMs = 1000UL / kTargetFps; // ~16 ms

void loop()
{
    if (WiFi.getMode() == WIFI_MODE_AP)
        dnsServer.processNextRequest();

    static unsigned long lastFrame = 0;
    const unsigned long now = millis();
    if (now - lastFrame >= kFrameIntervalMs)
    {
        lastFrame = now;
        factory.render(now);                 // advance timeline + composite
        driver->showFrameBuffer(factory.out); // push to the panel
    }

    // Hand the CPU back to the scheduler (feeds the watchdog, lets Wi-Fi +
    // AsyncTCP + DNS run). Without this the device resets within seconds.
    delay(1);
}
