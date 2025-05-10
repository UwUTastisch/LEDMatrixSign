// main.ino
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "matrix_driver.h"
#include "config.h"
#include "base64.hpp"

// ——— Globals ———
AsyncWebServer server(80);
ConfigReader config;
MatrixDriver *driver;

// Frame‐chain
static const uint8_t MAX_CHAIN = 10;
String imageChain[MAX_CHAIN];
uint8_t currentFrame = 0;
uint16_t frameDuration = 1000 / 24; // default 24 FPS
unsigned long lastUpdate = 0;

// ——— Helpers ———
String generateNewID()
{
    static uint32_t counter = 0;
    File cf = SD.open("/metadata/counter", FILE_READ);
    if (cf)
    {
        counter = cf.parseInt();
        cf.close();
    }
    counter++;
    cf = SD.open("/metadata/counter", FILE_WRITE);
    if (cf)
    {
        cf.print(counter);
        cf.close();
    }
    return String(counter);
}

String readUID(const String &id)
{
    File uf = SD.open("/metadata/" + id + ".uid");
    if (!uf)
        return String();
    String u = uf.readString();
    uf.close();
    return u;
}

// ——— HTTP Handlers ———

// GET /api/img?id=<ID>
void handleGetImage(AsyncWebServerRequest *req)
{
    String id;
    if (req->hasParam("id"))
    {
        id = req->getParam("id")->value();
    }
    else if (req->hasParam("uid"))
    {
        String uid = req->getParam("uid")->value();
        // scan metadata for matching .uid
        File md = SD.open("/metadata");
        File f = md.openNextFile();
        while (f)
        {
            String name = f.name();
            if (name.endsWith(".uid"))
            {
                File uf = SD.open("/metadata/" + name);
                if (uf.readString() == uid)
                {
                    int dot = name.lastIndexOf('.');
                    id = name.substring(1, dot);
                    uf.close();
                    break;
                }
                uf.close();
            }
            f.close();
            f = md.openNextFile();
        }
        md.close();
        if (id.isEmpty())
        {
            req->send(404, "application/json", "{\"error\":\"not found\"}");
            return;
        }
    }
    else
    {
        req->send(400, "application/json", "{\"error\":\"missing id/uid\"}");
        return;
    }

    String path = "/images/" + id + ".bmp";
    if (!SD.exists(path))
    {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    if (driver->drawBMP(path.c_str()))
    {
#if DEBUG_MATRIX
        driver->debugPrintMatrix();
#endif
        req->send(200, "application/json", "{\"status\":\"ok\"}");
    }
    else
    {
        req->send(500, "application/json", "{\"error\":\"render fail\"}");
    }
}

// POST /api/img  { "id":"…", "uid":"…", "img":"<base64-BMP>" }
void handlePostImage(AsyncWebServerRequest *req, uint8_t *data, size_t len)
{
    DynamicJsonDocument doc(4 * 1024);
    if (deserializeJson(doc, data, len))
    {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    String id = doc["id"] | "";
    if (id.isEmpty())
        id = generateNewID();

    String b64 = doc["img"].as<String>();
    unsigned int inLen = b64.length();
    unsigned int decLen = decode_base64_length((const unsigned char *)b64.c_str(), inLen);
    auto *buf = (uint8_t *)malloc(decLen);
    unsigned int got = decode_base64((const unsigned char *)b64.c_str(), inLen, buf);

    File f = SD.open("/images/" + id + ".bmp", FILE_WRITE);
    if (!f)
    {
        free(buf);
        req->send(500, "application/json", "{\"error\":\"fs write\"}");
        return;
    }
    f.write(buf, got);
    f.close();
    free(buf);

    if (doc.containsKey("uid"))
    {
        File uf = SD.open("/metadata/" + id + ".uid", FILE_WRITE);
        uf.print(doc["uid"].as<String>());
        uf.close();
    }

    DynamicJsonDocument out(64);
    out["id"] = id;
    String res;
    serializeJson(out, res);
    req->send(200, "application/json", res);
}

// POST /api/imgchain { "chain":["1","2",…], "fps":12 }
void handlePostImgChain(AsyncWebServerRequest *req, uint8_t *data, size_t len)
{
    DynamicJsonDocument doc(2 * 1024);
    if (deserializeJson(doc, data, len))
    {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    auto arr = doc["chain"].as<JsonArray>();
    memset(imageChain, 0, sizeof(imageChain));
    for (uint8_t i = 0; i < arr.size() && i < MAX_CHAIN; i++)
    {
        imageChain[i] = arr[i].as<String>();
    }
    uint16_t fps = doc["fps"].as<uint16_t>();
    if (fps == 0)
        fps = 24;
    frameDuration = 1000 / fps;
    currentFrame = 0;

    // draw first immediately
    if (imageChain[0].length())
    {
        String p = "/images/" + imageChain[0] + ".bmp";
        if (SD.exists(p))
            driver->drawBMP(p.c_str());
    }

    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

// GET /listimg
void handleListImages(AsyncWebServerRequest *req)
{
    DynamicJsonDocument doc(1024);
    auto arr = doc.createNestedArray("list");
    File dir = SD.open("/images");
    File f = dir.openNextFile();
    while (f)
    {
        String nm = f.name();
        if (nm.endsWith(".bmp"))
        {
            int dot = nm.lastIndexOf('.');
            arr.add(nm.substring(1, dot));
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    String res;
    serializeJson(doc, res);
    req->send(200, "application/json", res);
}

// Serve index.html
void handleGetIndex(AsyncWebServerRequest *req)
{
    if (!SD.exists("/index.html"))
    {
        req->send(404, "text/plain", "no index");
    }
    else
    {
        req->send(SD, "/index.html", "text/html");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(100);

    // — Load JSON config, init Wi-Fi & SD
    if (!config.loadFromSD(CONFIG_PATH))
    {
        Serial.println("❌ Config load failed");
        for (;;)
            delay(1000);
    }
    config.beginWiFi();

    // — Init matrix driver
    driver = new MatrixDriver(config);
    driver->begin();
#if DEBUG_MATRIX
    driver->debugPrintMatrix();
#endif

    // Ensure folders exist
    if (!SD.exists("/images"))
        SD.mkdir("/images");
    if (!SD.exists("/metadata"))
        SD.mkdir("/metadata");

    // — Register API endpoints
    server.on("/api/img", HTTP_GET, handleGetImage);
    server.on("/api/img", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  // Handle pre-processing if needed
              },
              NULL, // No upload handler
              [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              { handlePostImage(request, data, len); });

    // POST /api/imgchain endpoint
    server.on("/api/imgchain", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  // Handle pre-processing if needed
              },
              NULL, // No upload handler
              [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              { handlePostImgChain(request, data, len); });
    server.on("/api/listimg", HTTP_GET, handleListImages);
    server.on("/index.html", HTTP_GET, handleGetIndex);
    server.on("/", HTTP_GET, handleGetIndex);

    server.begin();
}

void loop()
{
    unsigned long now = millis();
    if (now - lastUpdate >= frameDuration)
    {
        lastUpdate = now;
        if (imageChain[currentFrame].length())
        {
            String p = "/images/" + imageChain[currentFrame] + ".bmp";
            if (SD.exists(p))
            {
                driver->drawBMP(p.c_str());
            }
            currentFrame = (currentFrame + 1) % MAX_CHAIN;
        }
    }
}
