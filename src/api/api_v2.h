// api_v2.h — 2.0 REST surface.
//
//   /framebuffer/{draw,savetostorage,get,getcomposition,size}
//   /file/{uploadanim,uploadasset,ls,download,fsstatus}
//   /anim/{start,setspeed,stop,status}
//   /display/{brightness,gpiopins}
//
// Handlers are intentionally permissive: malformed input yields a 4xx with a
// short JSON error rather than ever aborting the render loop.
#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>
#include "base64.hpp"
#include "../config.h"
#include "../storage.h"
#include "../matrix_driver.h"
#include "../anim/framefactory.h"

class ApiV2
{
public:
    AsyncWebServer &server;
    MatrixDriver *&driver; // reference to the global pointer
    FrameFactory &factory;
    ConfigReader &config;

    ApiV2(AsyncWebServer &s, MatrixDriver *&d, FrameFactory &f, ConfigReader &c)
        : server(s), driver(d), factory(f), config(c) {}

    // ——— small helpers ———
    using BodyDone = std::function<void(AsyncWebServerRequest *, uint8_t *, size_t)>;

    void onBody(const char *path, WebRequestMethodComposite method, BodyDone done)
    {
        server.on(
            path, method, [](AsyncWebServerRequest *) {}, nullptr,
            [done](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                   size_t index, size_t total)
            {
                if (index == 0)
                {
                    req->_tempObject = new std::vector<uint8_t>();
                    ((std::vector<uint8_t> *)req->_tempObject)->reserve(total);
                }
                auto *buf = (std::vector<uint8_t> *)req->_tempObject;
                buf->insert(buf->end(), data, data + len);
                if (index + len == total)
                {
                    done(req, buf->data(), buf->size());
                    delete buf;
                    req->_tempObject = nullptr;
                }
            });
    }

    static void sendJson(AsyncWebServerRequest *req, int code, const JsonDocument &doc)
    {
        String out;
        serializeJson(doc, out);
        req->send(code, "application/json", out);
    }
    static void err(AsyncWebServerRequest *req, int code, const char *msg)
    {
        req->send(code, "application/json", String("{\"error\":\"") + msg + "\"}");
    }

    void begin()
    {
        registerFramebuffer();
        registerFile();
        registerAnim();
        registerDisplay();
    }

private:
    // ============ /framebuffer ============
    void registerFramebuffer()
    {
        // POST /framebuffer/draw — body is a single anim.json frame → API buffer
        onBody("/framebuffer/draw", HTTP_POST,
               [this](AsyncWebServerRequest *req, uint8_t *data, size_t len)
               {
                   JsonDocument doc;
                   if (deserializeJson(doc, data, len)) { err(req, 400, "bad json"); return; }
                   Frame f = parseFrameObject(doc.as<JsonObjectConst>(), 0);
                   factory.apiApplyFrame(f);
                   req->send(200, "application/json", "{\"status\":\"ok\"}");
               });

        // POST /framebuffer/savetostorage — { filename, animname }
        // Persists the current API-buffer composition as a one-frame anim.json.
        onBody("/framebuffer/savetostorage", HTTP_POST,
               [this](AsyncWebServerRequest *req, uint8_t *data, size_t len)
               {
                   JsonDocument in;
                   if (deserializeJson(in, data, len)) { err(req, 400, "bad json"); return; }
                   String animname = in["animname"] | "";
                   String filename = in["filename"] | "anim.json";
                   if (animname.isEmpty()) { err(req, 400, "missing animname"); return; }

                   JsonDocument out;
                   JsonObject root = out.to<JsonObject>();
                   JsonArray frames = root["frames"].to<JsonArray>();
                   JsonObject fo = frames.add<JsonObject>();
                   for (auto &o : factory.api.live) o->toJson(fo);
                   fo["duration"] = 1000;

                   String text;
                   serializeJsonPretty(out, text);
                   Storage::mkdirs(Storage::animDir(animname));
                   Storage::mkdirs(Storage::assetsDir(animname));
                   String path = Storage::animDir(animname) + "/" + filename;
                   File f = activeFS().open(path, FILE_WRITE);
                   if (!f) { err(req, 500, "fs write"); return; }
                   f.print(text);
                   f.close();
                   req->send(200, "application/json",
                             String("{\"status\":\"ok\",\"path\":\"") + path + "\"}");
               });

        // GET /framebuffer/get — composited output as base64 BGRA8888
        server.on("/framebuffer/get", HTTP_GET, [this](AsyncWebServerRequest *req)
                  {
            const FrameBuffer &fb = factory.out;
            size_t n = (size_t)fb.w * fb.h * 4;
            std::vector<uint8_t> raw(n);
            for (size_t i = 0; i < fb.px.size(); i++) {
                const Rgba &c = fb.px[i];
                raw[i*4+0]=c.b; raw[i*4+1]=c.g; raw[i*4+2]=c.r; raw[i*4+3]=c.a;
            }
            unsigned int encLen = encode_base64_length(n);
            std::vector<unsigned char> enc(encLen + 1);
            unsigned int actual = encode_base64(raw.data(), n, enc.data());
            enc[actual] = '\0';
            JsonDocument doc;
            doc["width"] = fb.w; doc["height"] = fb.h;
            doc["format"] = "BGRA8888";
            doc["data"] = (char*)enc.data();
            sendJson(req, 200, doc); });

        // GET /framebuffer/getcomposition — live object trees of both layers
        server.on("/framebuffer/getcomposition", HTTP_GET,
                  [this](AsyncWebServerRequest *req)
                  {
            JsonDocument doc;
            doc["width"] = factory.out.w; doc["height"] = factory.out.h;
            auto dump = [](JsonArray arr, std::vector<std::shared_ptr<CObj>> &live){
                for (auto &o : live) {
                    JsonObject e = arr.add<JsonObject>();
                    e["type"] = o->type();
                    e["id"] = o->id;
                    if (o->objname.length()) e["objname"] = o->objname;
                }
            };
            dump(doc["primary"].to<JsonArray>(), factory.primary.live);
            dump(doc["api"].to<JsonArray>(), factory.api.live);
            sendJson(req, 200, doc); });

        // GET /framebuffer/size
        server.on("/framebuffer/size", HTTP_GET, [this](AsyncWebServerRequest *req)
                  {
            JsonDocument doc;
            doc["width"] = config.width; doc["height"] = config.height;
            sendJson(req, 200, doc); });
    }

    // ============ /file ============
    void registerFile()
    {
        // POST /file/uploadanim — { animname, anim:{...full anim.json...} }
        onBody("/file/uploadanim", HTTP_POST,
               [this](AsyncWebServerRequest *req, uint8_t *data, size_t len)
               {
                   JsonDocument in;
                   if (deserializeJson(in, data, len)) { err(req, 400, "bad json"); return; }
                   String animname = in["animname"] | "";
                   if (animname.isEmpty()) { err(req, 400, "missing animname"); return; }
                   JsonVariantConst anim = in["anim"];
                   if (anim.isNull()) { err(req, 400, "missing anim"); return; }
                   String text;
                   serializeJson(anim, text);
                   if (!Storage::saveAnimationJson(animname,
                                                   (const uint8_t *)text.c_str(),
                                                   text.length()))
                   { err(req, 500, "fs write"); return; }
                   req->send(200, "application/json",
                             String("{\"status\":\"ok\",\"animname\":\"") + animname + "\"}");
               });

        // POST /file/uploadasset — { animname, filename, data:"<base64 BMP>" }
        onBody("/file/uploadasset", HTTP_POST,
               [this](AsyncWebServerRequest *req, uint8_t *data, size_t len)
               {
                   JsonDocument in;
                   if (deserializeJson(in, data, len)) { err(req, 400, "bad json"); return; }
                   String animname = in["animname"] | "";
                   String filename = in["filename"] | "";
                   String b64 = in["data"] | "";
                   if (animname.isEmpty() || filename.isEmpty() || b64.isEmpty())
                   { err(req, 400, "missing animname/filename/data"); return; }
                   unsigned int outLen = decode_base64_length(
                       (const unsigned char *)b64.c_str(), b64.length());
                   std::vector<uint8_t> bytes(outLen);
                   unsigned int actual = decode_base64(
                       (const unsigned char *)b64.c_str(), b64.length(), bytes.data());
                   if (!Storage::saveAsset(animname, filename, bytes.data(), actual))
                   { err(req, 500, "fs write"); return; }
                   req->send(200, "application/json",
                             String("{\"status\":\"ok\",\"bytes\":") + actual + "}");
               });

        // GET /file/ls — animation + asset tree
        server.on("/file/ls", HTTP_GET, [](AsyncWebServerRequest *req)
                  {
            JsonDocument doc;
            JsonArray anims = doc["anims"].to<JsonArray>();
            Storage::listAnimations([&](const String &id){
                JsonObject a = anims.add<JsonObject>();
                a["id"] = id;
                JsonArray assets = a["assets"].to<JsonArray>();
                Storage::listAssets(id, [&](const String &f){ assets.add(f); });
            });
            sendJson(req, 200, doc); });

        // GET /file/download?path=/anim/<id>/anim.json
        server.on("/file/download", HTTP_GET, [](AsyncWebServerRequest *req)
                  {
            if (!req->hasParam("path")) { err(req, 400, "missing path"); return; }
            String path = req->getParam("path")->value();
            if (!path.startsWith("/")) path = "/" + path;
            // restrict to known roots
            if (!(path.startsWith("/anim/") || path == "/config.json"))
            { err(req, 403, "forbidden"); return; }
            if (!activeFS().exists(path)) { err(req, 404, "not found"); return; }
            const char *ct = path.endsWith(".json") ? "application/json"
                              : path.endsWith(".bmp") ? "image/bmp"
                                                      : "application/octet-stream";
            req->send(activeFS(), path, ct); });

        // GET /file/fsstatus
        server.on("/file/fsstatus", HTTP_GET, [](AsyncWebServerRequest *req)
                  {
            Storage::FsStat s = Storage::fsStatus();
            JsonDocument doc;
            doc["fs"] = s.source;
            doc["total"] = s.total; doc["used"] = s.used;
            doc["free"] = (s.total > s.used) ? (s.total - s.used) : 0;
            sendJson(req, 200, doc); });
    }

    // ============ /anim ============
    void registerAnim()
    {
        // POST /anim/start — { animname, params:{}, cycles? }
        onBody("/anim/start", HTTP_POST,
               [this](AsyncWebServerRequest *req, uint8_t *data, size_t len)
               {
                   JsonDocument in;
                   if (deserializeJson(in, data, len)) { err(req, 400, "bad json"); return; }
                   String animname = in["animname"] | "";
                   if (animname.isEmpty()) { err(req, 400, "missing animname"); return; }
                   int cycles = in["cycles"] | 0;

                   auto anim = std::make_shared<Animation>();
                   if (!Storage::loadAnimation(animname, *anim))
                   { err(req, 404, "anim not found"); return; }

                   Params overrides;
                   JsonObjectConst p = in["params"];
                   if (!p.isNull())
                       for (JsonPairConst kv : p)
                           overrides[String(kv.key().c_str())] =
                               kv.value().is<const char *>()
                                   ? String(kv.value().as<const char *>())
                                   : String(kv.value().as<float>());

                   factory.player.start(anim, overrides, cycles, factory.primary);
                   req->send(200, "application/json",
                             String("{\"status\":\"started\",\"animname\":\"") + animname + "\"}");
               });

        // POST /anim/setspeed — { speed }
        onBody("/anim/setspeed", HTTP_POST,
               [this](AsyncWebServerRequest *req, uint8_t *data, size_t len)
               {
                   JsonDocument in;
                   if (deserializeJson(in, data, len)) { err(req, 400, "bad json"); return; }
                   float speed = in["speed"] | 1.0f;
                   factory.player.setSpeedScale(speed);
                   req->send(200, "application/json",
                             String("{\"status\":\"ok\",\"speed\":") + String(speed, 3) + "}");
               });

        // POST /anim/stop
        server.on("/anim/stop", HTTP_POST, [this](AsyncWebServerRequest *req)
                  {
            factory.player.stop(factory.primary);
            req->send(200, "application/json", "{\"status\":\"stopped\"}"); });

        // GET /anim/status
        server.on("/anim/status", HTTP_GET, [this](AsyncWebServerRequest *req)
                  {
            JsonDocument doc;
            doc["running"] = factory.player.running();
            doc["paused"] = factory.player.isPaused();
            doc["speed"] = factory.player.speedScale();
            doc["animname"] = factory.player.currentId();
            sendJson(req, 200, doc); });
    }

    // ============ /display ============
    void registerDisplay()
    {
        // POST /display/brightness — { brightness }
        onBody("/display/brightness", HTTP_POST,
               [this](AsyncWebServerRequest *req, uint8_t *data, size_t len)
               {
                   JsonDocument in;
                   if (deserializeJson(in, data, len)) { err(req, 400, "bad json"); return; }
                   int b = in["brightness"] | 255;
                   if (b < 0) b = 0;
                   if (b > 255) b = 255;
                   driver->brightness = b;
                   req->send(200, "application/json",
                             String("{\"status\":\"ok\",\"brightness\":") + b + "}");
               });

        // GET /display/brightness
        server.on("/display/brightness", HTTP_GET, [this](AsyncWebServerRequest *req)
                  {
            JsonDocument doc;
            doc["brightness"] = driver->brightness;
            sendJson(req, 200, doc); });

        // GET /display/gpiopins
        server.on("/display/gpiopins", HTTP_GET, [this](AsyncWebServerRequest *req)
                  {
            JsonDocument doc;
            doc["data_pin"] = config.pin;
            JsonObject sd = doc["sd"].to<JsonObject>();
            sd["cs"] = SD_CS; sd["mosi"] = SD_MOSI; sd["miso"] = SD_MISO; sd["sck"] = SD_SCK;
            sendJson(req, 200, doc); });
    }
};
