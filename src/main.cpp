// main.h
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "matrix_driver.h"
#include "base64.hpp"
#include <vector>
#include <SD.h>
#include <WiFi.h>

// ——— Globals ———
AsyncWebServer server(80);
ConfigReader config;
MatrixDriver *driver;
DNSServer dnsServer;

// Frame‐chain
static const uint8_t MAX_CHAIN = 100;                      // TODO: make dynamic by config file
static const uint16_t MAX_IMG_CHAIN_STRING_LENGTH = 10000; // somehow more than 500 characters causes crashes
String imageChain[MAX_CHAIN];
uint8_t chainLength = 0;
uint8_t currentFrame = 0;
uint16_t frameDuration = 1000 / 24; // default 24 FPS
unsigned long lastUpdate = 0;

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

void handlePostBrightness(AsyncWebServerRequest *req, uint8_t *data, size_t len)
{
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, data, len))
    {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    uint8_t newBrightness = doc["brightness"] | 255;
    if (newBrightness > 255)
        newBrightness = 255;

    // Update the brightness
    driver->brightness = newBrightness;

    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

// ——— POST /api/img { "file":"…", "img":"<base64-BMP>" } ———
void handlePostImageComplete(AsyncWebServerRequest *req, const String &body)
{
    // 1. Allocate a JSON document just big enough (with a little wiggle room)
    size_t capacity = body.length() * 15 / 10 + 512;
    DynamicJsonDocument doc(capacity);

    // 2. Parse
    auto err = deserializeJson(doc, body);
    if (err)
    {
        Serial.printf("❌ JSON parse failed: %s\n", err.c_str());
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    // 3. Extract fields
    String filename = doc["file"] | "";
    String b64data = doc["img"] | "";
    if (filename.isEmpty() || b64data.isEmpty())
    {
        req->send(400, "application/json", "{\"error\":\"missing file or img\"}");
        return;
    }

    // 4. Decode Base64
    unsigned int expectedLen = decode_base64_length((const unsigned char *)b64data.c_str(), b64data.length());
    auto *buf = (uint8_t *)malloc(expectedLen);
    if (!buf)
    {
        req->send(500, "application/json", "{\"error\":\"memory alloc failed\"}");
        return;
    }
    unsigned int actualLen = decode_base64((const unsigned char *)b64data.c_str(), b64data.length(), buf);

    // 5. Write to SD
    String path = "/images/" + filename;
    File f = SD.open(path, FILE_WRITE);
    if (!f)
    {
        free(buf);
        req->send(500, "application/json", "{\"error\":\"fs write\"}");
        return;
    }
    f.write(buf, actualLen);
    f.close();
    free(buf);

    // 6. Success response
    String resp = String("{\"file\":\"") + filename + "\"}";
    req->send(200, "application/json", resp);
}

// POST /api/imgchain { "chain":["1","2",…], "fps":12.5, ?"num":1 }
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

    int chainNum = -1;

    if (doc.containsKey("num"))
    {
        chainNum = doc["num"].as<int>();
    }
    else
    {
        // Store as csv in /imgchain/<number>.chain as: "frame_duration"\n"frame1.bmp"\n"frame2.bmp"\n…
        File dir = SD.open("/imgchain");

        String nm = dir.getNextFileName();
        int maxNum = 0;

        while (nm && nm != "")
        {
            String lowerNm = nm + "";
            lowerNm.toLowerCase();
            if (lowerNm.endsWith(".chain"))
            {
                String base = lowerNm.substring(0, lowerNm.lastIndexOf('.'));
                int num = base.toInt();
                if (num > maxNum)
                    maxNum = num;
            }
            nm = dir.getNextFileName();
        }
        dir.close();
        chainNum = maxNum + 1;
    }

    String chainPath = "/imgchain/" + String(chainNum) + ".chain";

    if (SD.exists(chainPath))
    {
        SD.rename(chainPath, chainPath + ".bak");
    }

    File f = SD.open(chainPath, FILE_WRITE);
    if (!f)
    {
        req->send(500, "application/json", "{\"error\":\"fs write chain\"}");
        return;
    }
    f.printf("%u\n", frameDuration);
    for (uint8_t i = 0; i < chainLength; i++)
    {
        f.printf("%s\n", imageChain[i].c_str());
    }

    f.close();

    req->send(200, "application/json", "{\"status\":\"ok\", \"chainNum\":\"" + String(chainNum) + "\"}");
}

// POST /api/imgchain?num=<NUMBER> // this returns the file content list -> { "chain":["1","2",…], "fps":12.5, "num":1 }
void handleGetImgChain(AsyncWebServerRequest *req)
{
    if (!req->hasParam("num"))
    {
        req->send(400, "application/json", "{\"error\":\"missing num\"}");
        return;
    }
    String numStr = req->getParam("num")->value();
    String path = "/imgchain/" + numStr + ".chain";

    if (!SD.exists(path))
    {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    File f = SD.open(path, FILE_READ);
    if (!f)
    {
        req->send(500, "application/json", "{\"error\":\"fs read chain\"}");
        return;
    }

    // Read first line: frameDuration
    String line = f.readStringUntil('\n');
    line.trim();
    uint16_t duration = line.toInt();
    float fps = duration > 0 ? 1000.0f / duration : 1.0f;

    // Read rest: image filenames
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("chain");
    while (f.available())
    {
        String img = f.readStringUntil('\n');
        img.trim();
        if (img.length() > 0)
        {
            // Add ".bmp" if not present
            if (!img.endsWith(".bmp"))
                img += ".bmp";
            arr.add(img);
        }
    }
    f.close();

    doc["fps"] = fps;
    doc["num"] = numStr.toInt();

    String res;
    serializeJson(doc, res);

    req->send(200, "text/plain", res);
}

// GET /api/listimg?contains=<FILTER>
void handleListImages(AsyncWebServerRequest *req)
{
    File dir = SD.open("/images");
    if (!dir)
    {
        req->send(500, "application/json", "{\"error\":\"failed to open /images\"}");
        return;
    }

    String containsFilter = "";
    if (req->hasParam("contains"))
    {
        containsFilter = req->getParam("contains")->value();
    }

    int countTotalLength = 0;
    bool first = true;

    String res;
    res.reserve(MAX_IMG_CHAIN_STRING_LENGTH + MAX_CHAIN * 4 + 40);
    res = "{\"list\":[";

    String f = dir.getNextFileName();
    int i = 0;
    while (f && f != "")
    {
        String nm = f;
        int p = nm.lastIndexOf('/');
        if (p >= 0)
            nm = nm.substring(p + 1);

        String lowerNm = nm + "";
        nm.trim();
        lowerNm.toLowerCase();
        containsFilter.toLowerCase();

        if (lowerNm.endsWith(".bmp") &&
            (containsFilter.isEmpty() || lowerNm.indexOf(containsFilter) != -1))
        {
            countTotalLength += nm.length();
            if (countTotalLength > MAX_IMG_CHAIN_STRING_LENGTH)
            {
                Serial.println("Max image list length reached, stopping");
                break;
            }

            if (!first)
            {
                res += ',';
            }
            first = false;

            res += '"';
            res += nm;
            res += '"';

            // Serial.printf("Found image file: %s (total length so far: %d)\n", nm.c_str(), countTotalLength);
        }

        f = dir.getNextFileName();
    }

    dir.close();

    res += "]}";

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
    // Brightness Slider
    server.on("/api/brightness", HTTP_POST, [](AsyncWebServerRequest *request) {},
              NULL, // No upload handler
              [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              { handlePostBrightness(request, data, len); });
    // Matrix/Image API Handlers on same server
    server.on("/api/img", HTTP_GET, handleGetImage);
    server.on("/api/img", HTTP_POST,
              /* onRequest  */ [](AsyncWebServerRequest *req)
              {
                  // Nothing to do here; we wait for the body callback
              },
              /* onUpload   */ nullptr,
              /* onBody     */ [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total)
              {
        static String bodyBuffer;

        // Start of a new upload?
        if (index == 0)
        {
            bodyBuffer = "";
        }

        // Append this chunk
        bodyBuffer += String((char *)data).substring(0, len);

        // If this is the last chunk…
        if (index + len == total)
        {
            handlePostImageComplete(req, bodyBuffer);
        } });
    server.on("/api/display", HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              nullptr,
              [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              {
                  static std::vector<uint8_t> buffer;
                  if (total > 6966) // this is the size of a 48x48 24bit color bmp as produced by ffmpeg
                  {
                      return;
                  }
                  buffer.resize(total);
                  std::copy_n(data, len, buffer.data()+index);
                  if (index+len == total) {
                      driver->drawBMP(buffer.data(), buffer.size());
                      buffer.clear();
                      request->send(204);
                  }
              });
    server.on("/api/imgchain", HTTP_GET, handleGetImgChain);
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
    Serial.println("Starting up…");
    // — Load JSON config, init Wi-Fi & SD
    if (!config.loadFromSD(CONFIG_PATH))
    {
        Serial.println("❌ Config load failed");
        for (;;)
            delay(1000);
    }
    config.beginWiFi(dnsServer);

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
    if (!SD.exists("/imgchain"))
        SD.mkdir("/imgchain");

    setUpAPIServer();
}

// ====== loop() ======
void loop()
{
    if (WiFi.getMode() == WIFI_MODE_AP)
    {
#ifdef DEBUG
        Serial.println("AP mode, processing DNS requests");
#endif
        dnsServer.processNextRequest();
    }

    if (chainLength == 0)
        return;

    unsigned long now = millis();
    if (now - lastUpdate >= frameDuration)
    {
        lastUpdate = now;
#ifdef DEBUG
        Serial.printf("Frame %u of %u\n", currentFrame + 1, chainLength);
#endif

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
