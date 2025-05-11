// main.h
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
uint8_t chainLength = 0;
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

// ——— Helper to slurp a file into a byte buffer ———
bool readFileToBuffer(const String &path, std::vector<uint8_t> &outBuf)
{
    outBuf.clear();
    File f = SD.open(path, FILE_READ);
    if (!f)
        return false;
    while (f.available())
    {
        outBuf.push_back(f.read());
    }
    f.close();
    return true;
}

// ——— HTTP Handlers ———

// ——— GET /api/img?file=<FILENAME> ———
void handleGetImage(AsyncWebServerRequest *req)
{
    if (!req->hasParam("file"))
    {
        req->send(400, "application/json", "{\"error\":\"missing file\"}");
        return;
    }
    String filename = req->getParam("file")->value();
    String path = "/images/" + filename;

    if (!SD.exists(path))
    {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    // Read raw bytes
    std::vector<uint8_t> buf;
    if (!readFileToBuffer(path, buf))
    {
        req->send(500, "application/json", "{\"error\":\"fs read\"}");
        return;
    }

    // Base64-encode
    unsigned int encLen = encode_base64_length(buf.size());
    unsigned char *outB = (unsigned char *)malloc(encLen + 1);
    unsigned int actual = encode_base64(buf.data(), buf.size(), outB);
    outB[actual] = '\0';
    String b64((char *)outB);
    free(outB);

    // Build JSON response
    DynamicJsonDocument doc(2048);
    doc["img"] = b64;
    doc["file"] = filename;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

// ——— POST /api/img { "file":"…", "img":"<base64-BMP>" } ———
void handlePostImage(AsyncWebServerRequest *req, uint8_t *data, size_t len)
{
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, data, len))
    {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    String file = doc["file"] | "";
    if (file.isEmpty())
    {
        req->send(400, "application/json", "{\"error\":\"missing file\"}");
        return;
    }

    String b64 = doc["img"].as<String>();
    unsigned int decLen = decode_base64_length((const unsigned char *)b64.c_str(), b64.length());
    auto *buf = (uint8_t *)malloc(decLen);
    unsigned int got = decode_base64((const unsigned char *)b64.c_str(), b64.length(), buf);

    String path = "/images/" + file;
    File f = SD.open(path, FILE_WRITE);
    if (!f)
    {
        free(buf);
        req->send(500, "application/json", "{\"error\":\"fs write\"}");
        return;
    }
    f.write(buf, got);
    f.close();
    free(buf);

    DynamicJsonDocument out(256);
    out["file"] = file;
    String res;
    serializeJson(out, res);
    req->send(200, "application/json", res);
}

// POST /api/imgchain { "chain":["1","2",…], "fps":12.5 }
void handlePostImgChain(AsyncWebServerRequest *req, uint8_t *data, size_t len)
{
    DynamicJsonDocument doc(2 * 1024);
    if (deserializeJson(doc, data, len))
    {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    if (!doc.containsKey("chain") || !doc["chain"].is<JsonArray>())
    {
        req->send(400, "application/json", "{\"error\":\"missing or invalid chain\"}");
        return;
    }
    auto arr = doc["chain"].as<JsonArray>();
    if (arr.size() == 0)
    {
        req->send(400, "application/json", "{\"error\":\"empty chain\"}");
        return;
    }

    // Clear out any old entries
    for (uint8_t i = 0; i < MAX_CHAIN; i++)
    {
        imageChain[i].clear();
    }

    // Fill in the new chain, up to MAX_CHAIN
    chainLength = 0;
    for (uint8_t i = 0; i < arr.size() && i < MAX_CHAIN; i++)
    {
        String fn = arr[i].as<String>();
        String path = "/images/" + fn;
        if (!SD.exists(path))
        {
            req->send(404, "application/json",
                      "{\"error\":\"file not found: " + fn + "\"}");
            return;
        }
        // strip “.bmp” so we can re‐add it later in loop()
        imageChain[i] = fn.substring(0, fn.lastIndexOf('.'));
        chainLength++;
    }

    // Compute our per‐frame delay
    float fps = doc["fps"].is<float>() ? doc["fps"].as<float>() : 1.0;
    if (fps <= 0)
    {
        req->send(400, "application/json", "{\"error\":\"invalid fps\"}");
        return;
    }
    frameDuration = static_cast<uint16_t>(1000.0 / fps);

#ifdef DEBUG
    Serial.printf("FPS as String: %s\n", doc["fps"].as<String>().c_str());
    Serial.printf("FPS: %.2f\n", fps);
    Serial.printf("frameDuration: %u ms\n", frameDuration);
#endif

    // Reset playback
    currentFrame = 0;
    lastUpdate = millis();

    // Draw the first frame immediately
    if (chainLength)
    {
        String p = "/images/" + imageChain[0] + ".bmp";
        driver->drawBMP(p.c_str());
    }

    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

// GET /api/listimg
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
            arr.add(nm.substring(0)); // Include full file name with extension
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

// GET /api/imgspec
void handleGetSpec(AsyncWebServerRequest *req)
{
    DynamicJsonDocument doc(512);
    doc["format"] = "BMP";
    doc["colorspace"] = "sRGB";
    doc["width"] = config.width;
    doc["height"] = config.height;
    doc["bitDepth"] = 24;
    doc["compression"] = "none";
    doc["maxSizeKB"] = 450;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ====== AP-mode Server Setup ======
void setUpAPServer()
{
    server.on("/generate_204", [](AsyncWebServerRequest *req)
              { req->redirect(portalURL); });
    server.onNotFound([](AsyncWebServerRequest *req)
                      { req->redirect(portalURL); });
}

void setUpAPIServer()
{
    // Redirect common captive portal checks to index.html
    server.on("/index.html", HTTP_ANY, [](AsyncWebServerRequest *req)
              {
      if (SD.exists("/index.html")) {
        req->send(SD, "/index.html", "text/html");
      } else {
        req->send(404, "text/plain", "Missing index.html");
      } });
    // Matrix/Image API Handlers on same server
    server.on("/api/img", HTTP_GET, handleGetImage);
    server.on("/api/img", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  // Handle pre-processing if needed
              },
              NULL, // No upload handler
              [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              { handlePostImage(request, data, len); });
    server.on("/api/imgchain", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  // Handle pre-processing if needed
              },
              NULL, // No upload handler
              [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              { handlePostImgChain(request, data, len); });
    server.on("/api/listimg", HTTP_GET, handleListImages);
    server.on("/api/imgspec", HTTP_GET, handleGetSpec);
    server.on("/", HTTP_GET, handleGetIndex);
    server.begin();
}

// ====== setup() ======
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

    // Check if we are connected to Wi-Fi
    if (WiFi.status() != WL_CONNECTED)
    {
        setUpAPServer();
    }
    else
    {
        Serial.println("\nWi-Fi connected: " + WiFi.localIP().toString());
    }

    driver = new MatrixDriver(config);
    driver->begin();
    if (!SD.exists("/images"))
        SD.mkdir("/images");
    if (!SD.exists("/metadata"))
        SD.mkdir("/metadata");

    setUpAPIServer();
}

// ====== loop() ======
void loop()
{
    if (WiFi.getMode() == WIFI_MODE_AP)
    {
        dnsServer.processNextRequest();
    }

    if (chainLength == 0)
        return;

    unsigned long now = millis();
    if (now - lastUpdate >= frameDuration)
    {
        lastUpdate = now;

        // draw current frame
        String fn = imageChain[currentFrame];
        if (fn.length())
        {
            String path = "/images/" + fn + ".bmp";
            if (SD.exists(path))
            {
                driver->drawBMP(path.c_str());
            }
            else
            {
                Serial.printf("❌ File not found: %s\n", path.c_str());
            }
        }

        // step to next, wrap at chainLength
        currentFrame = (currentFrame + 1) % chainLength;
    }
}
