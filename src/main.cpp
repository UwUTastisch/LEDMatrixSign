// main.cpp
// Storage priority: SD card (if present + has config.json) → LittleFS fallback.
// All FS access goes through activeFS() defined in config.h.

#define FS_MOUNT_DEFINE // instantiate FSMount::source / FSMount::fs here
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "matrix_driver.h"
#include "base64.hpp"
#include <vector>
#include "virtual_file.h"

// ——— Globals ———
AsyncWebServer server(80);
ConfigReader config;
MatrixDriver *driver;

// Frame-chain
static const uint8_t MAX_CHAIN = 100;
static const uint16_t MAX_IMG_CHAIN_STRING_LENGTH = 10000;
String imageChain[MAX_CHAIN];
uint8_t chainLength = 0;
uint8_t currentFrame = 0;
uint16_t frameDuration = 1000 / 24; // default 24 FPS
unsigned long lastUpdate = 0;

// ——— Helper: directory listing via openNextFile() ———
// Works with both SD (which never had openDir) and LittleFS (which supports both).
// We open the directory as a File and iterate with openNextFile().
static void listDirFiles(const char *dirPath,
                         std::function<void(const String &name)> cb)
{
    File dir = activeFS().open(dirPath);
    if (!dir || !dir.isDirectory())
        return;
    File entry = dir.openNextFile();
    while (entry)
    {
        if (!entry.isDirectory())
        {
            // entry.name() returns just the filename on LittleFS,
            // but the full path on SD — normalise to bare filename.
            String fullName = entry.name();
            int slash = fullName.lastIndexOf('/');
            String bare = (slash >= 0) ? fullName.substring(slash + 1) : fullName;
            cb(bare);
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

// ——— Helper: slurp a file into a byte buffer ———
bool readFileToBuffer(const String &path, std::vector<uint8_t> &outBuf)
{
    outBuf.clear();
    File f = activeFS().open(path, FILE_READ);
    if (!f)
        return false;
    while (f.available())
        outBuf.push_back(f.read());
    f.close();
    return true;
}

// ——— HTTP Handlers ———

// GET /api/img?file=<FILENAME>
void handleGetImage(AsyncWebServerRequest *req)
{
    if (!req->hasParam("file"))
    {
        req->send(400, "application/json", "{\"error\":\"missing file\"}");
        return;
    }
    String filename = req->getParam("file")->value();
    String path = "/images/" + filename;

    if (!activeFS().exists(path))
    {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    std::vector<uint8_t> buf;
    if (!readFileToBuffer(path, buf))
    {
        req->send(500, "application/json", "{\"error\":\"fs read\"}");
        return;
    }

    unsigned int encLen = encode_base64_length(buf.size());
    unsigned char *outB = (unsigned char *)malloc(encLen + 1);
    unsigned int actual = encode_base64(buf.data(), buf.size(), outB);
    outB[actual] = '\0';
    String b64((char *)outB);
    free(outB);

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
    driver->brightness = newBrightness;
    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

// POST /api/img { "file":"…", "img":"<base64-BMP>" }
void handlePostImageComplete(AsyncWebServerRequest *req, const String &body)
{
    size_t capacity = body.length() * 15 / 10 + 512;
    DynamicJsonDocument doc(capacity);

    auto err = deserializeJson(doc, body);
    if (err)
    {
        Serial.printf("❌ JSON parse failed: %s\n", err.c_str());
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    String filename = doc["file"] | "";
    String b64data = doc["img"] | "";
    if (filename.isEmpty() || b64data.isEmpty())
    {
        req->send(400, "application/json", "{\"error\":\"missing file or img\"}");
        return;
    }

    unsigned int expectedLen = decode_base64_length(
        (const unsigned char *)b64data.c_str(), b64data.length());
    auto *buf = (uint8_t *)malloc(expectedLen);
    if (!buf)
    {
        req->send(500, "application/json", "{\"error\":\"memory alloc failed\"}");
        return;
    }
    unsigned int actualLen = decode_base64(
        (const unsigned char *)b64data.c_str(), b64data.length(), buf);

    String path = "/images/" + filename;
    File f = activeFS().open(path, FILE_WRITE);
    if (!f)
    {
        free(buf);
        req->send(500, "application/json", "{\"error\":\"fs write\"}");
        return;
    }
    f.write(buf, actualLen);
    f.close();
    free(buf);

    req->send(200, "application/json",
              String("{\"file\":\"") + filename + "\"}");
}

// POST /api/imgchain { "chain":["1","2",…], "fps":12.5, ?"num":1 }
void handlePostImgChain(AsyncWebServerRequest *req, uint8_t *data, size_t len)
{
    const String &body = String((const char *)data, len);
#if DEBUG
    Serial.printf("Received imgchain POST: %s\n", body.c_str());
#endif

    JsonDocument doc;
    if (deserializeJson(doc, body))
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

    for (uint8_t i = 0; i < MAX_CHAIN; i++)
        imageChain[i].clear();
    chainLength = 0;

    for (uint8_t i = 0; i < arr.size() && i < MAX_CHAIN; i++)
    {
        String fn = arr[i].as<String>();
        imageChain[i] = fn.substring(0, fn.lastIndexOf('.'));
        chainLength++;
        Serial.printf("Added to chain[%u]: %s\n", i, imageChain[i].c_str());
    }

    float fps = doc["fps"].is<float>() ? doc["fps"].as<float>() : 1.0f;
    if (fps <= 0)
    {
        req->send(400, "application/json", "{\"error\":\"invalid fps\"}");
        return;
    }
    frameDuration = static_cast<uint16_t>(1000.0f / fps);
    Serial.printf("FPS: %.2f  frameDuration: %u ms\n", fps, frameDuration);

    currentFrame = 0;
    lastUpdate = millis();

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
        // Find the highest existing .chain number in /imgchain/
        // listDirFiles works on both SD and LittleFS.
        int maxNum = 0;
        listDirFiles("/imgchain", [&](const String &nm)
                     {
            String lower = nm;
            lower.toLowerCase();
            if (lower.endsWith(".chain"))
            {
                String base = lower.substring(0, lower.lastIndexOf('.'));
                int    num  = base.toInt();
                if (num > maxNum) maxNum = num;
            } });
#if DEBUG
        Serial.printf("Max existing chain number: %d\n", maxNum);
#endif
        chainNum = maxNum + 1;
    }

    String chainPath = "/imgchain/" + String(chainNum) + ".chain";
    if (activeFS().exists(chainPath))
        activeFS().rename(chainPath, chainPath + ".bak");

    File f = activeFS().open(chainPath, FILE_WRITE);
    if (!f)
    {
        req->send(500, "application/json", "{\"error\":\"fs write chain\"}");
        return;
    }
    f.printf("%u\n", frameDuration);
    for (uint8_t i = 0; i < chainLength; i++)
        f.printf("%s\n", imageChain[i].c_str());
    f.close();

    req->send(200, "application/json",
              "{\"status\":\"ok\", \"chainNum\":\"" + String(chainNum) + "\"}");
}

// GET /api/imgchain?num=<NUMBER>
void handleGetImgChain(AsyncWebServerRequest *req)
{
    if (!req->hasParam("num"))
    {
        req->send(400, "application/json", "{\"error\":\"missing num\"}");
        return;
    }
    String numStr = req->getParam("num")->value();
    String path = "/imgchain/" + numStr + ".chain";

    if (!activeFS().exists(path))
    {
        req->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    File f = activeFS().open(path, FILE_READ);
    if (!f)
    {
        req->send(500, "application/json", "{\"error\":\"fs read chain\"}");
        return;
    }

    String line = f.readStringUntil('\n');
    line.trim();
    uint16_t duration = line.toInt();
    float fps = duration > 0 ? 1000.0f / duration : 1.0f;

    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("chain");
    while (f.available())
    {
        String img = f.readStringUntil('\n');
        img.trim();
        if (img.length() > 0)
        {
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
    String containsFilter = "";
    if (req->hasParam("contains"))
        containsFilter = req->getParam("contains")->value();
    containsFilter.toLowerCase();

    int countTotalLength = 0;
    bool first = true;
    String res;
    res.reserve(MAX_IMG_CHAIN_STRING_LENGTH + MAX_CHAIN * 4 + 40);
    res = "{\"list\":[";

    listDirFiles("/images", [&](const String &nm)
                 {
        if (countTotalLength > MAX_IMG_CHAIN_STRING_LENGTH) return;

        String lower = nm;
        lower.toLowerCase();

        if (!lower.endsWith(".bmp")) return;
        if (!containsFilter.isEmpty() && lower.indexOf(containsFilter) == -1) return;

        countTotalLength += nm.length();
        if (countTotalLength > MAX_IMG_CHAIN_STRING_LENGTH)
        {
            Serial.println("Max image list length reached, stopping");
            return;
        }

        if (!first) res += ',';
        first = false;
        res += '"';
        res += nm;
        res += '"'; });

    res += "]}";
    req->send(200, "application/json", res);
}

// Serve index.html
void handleGetIndex(AsyncWebServerRequest *req)
{
    if (!activeFS().exists("/index.html"))
        req->send(404, "text/plain", "no index");
    else
        req->send(activeFS(), "/index.html", "text/html");
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

// GET /api/fssource — reports which storage is active (handy for debugging)
void handleGetFSSource(AsyncWebServerRequest *req)
{
    const char *src = (FSMount::source == FSMount::SD_CARD)    ? "sd"
                      : (FSMount::source == FSMount::LITTLEFS) ? "littlefs"
                                                               : "none";
    req->send(200, "application/json",
              String("{\"fs\":\"") + src + "\"}");
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
    server.on("/index.html", HTTP_ANY, [](AsyncWebServerRequest *req)
              {
        if (activeFS().exists("/index.html"))
            req->send(activeFS(), "/index.html", "text/html");
        else
            req->send(404, "text/plain", "Missing index.html"); });

    server.on("/api/brightness", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              { handlePostBrightness(request, data, len); });

    server.on("/api/img", HTTP_GET, handleGetImage);
    server.on("/api/img", HTTP_POST, [](AsyncWebServerRequest *req) {}, nullptr, [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total)
              {
                  static String bodyBuffer;
                  if (index == 0) bodyBuffer = "";
                  bodyBuffer += String((char *)data).substring(0, len);
                  if (index + len == total)
                      handlePostImageComplete(req, bodyBuffer); });

    server.on("/api/display", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              {
                  static std::vector<uint8_t> buffer;
                  if (total > 6966) return;
                  buffer.resize(total);
                  std::copy_n(data, len, buffer.data() + index);
                  if (index + len == total)
                  {
                      driver->drawBMP(make_virtual_file(buffer.data(), buffer.size()));
                      buffer.clear();
                      request->send(204);
                  } });

    server.on("/api/imgchain", HTTP_GET, handleGetImgChain);
    server.on("/api/imgchain", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
              {
                  static std::vector<uint8_t> bodyBuffer;
                  if (index == 0) { bodyBuffer.clear(); bodyBuffer.reserve(total); }
                  bodyBuffer.insert(bodyBuffer.end(), data, data + len);
                  if (index + len == total)
                  {
                      handlePostImgChain(request, bodyBuffer.data(), bodyBuffer.size());
                      bodyBuffer.clear();
                  } });

    server.on("/api/listimg", HTTP_GET, handleListImages);
    server.on("/api/imgspec", HTTP_GET, handleGetSpec);
    server.on("/api/fssource", HTTP_GET, handleGetFSSource);
    server.on("/", HTTP_GET, handleGetIndex);
    server.begin();
}

// ====== setup() ======
void setup()
{
    Serial.begin(115200);
    Serial.println("Starting up…");

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

    driver = new MatrixDriver(config);
    driver->begin();

    // Ensure required directories exist on whichever FS is active
    if (!activeFS().exists("/images"))
        activeFS().mkdir("/images");
    if (!activeFS().exists("/imgchain"))
        activeFS().mkdir("/imgchain");

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
        String fn = imageChain[currentFrame];
        if (fn.length())
        {
            String path = "/images/" + fn + ".bmp";
            if (activeFS().exists(path))
                driver->drawBMP(path.c_str());
            else
                Serial.printf("❌ File not found: %s\n", path.c_str());
        }
        currentFrame = (currentFrame + 1) % chainLength;
    }
}
