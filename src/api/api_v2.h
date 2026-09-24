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
#include <atomic>
#include <esp_heap_caps.h>
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

    // Largest body we will buffer. An asset upload is base64, so this allows
    // roughly a 48 KB BMP; bigger bodies are refused instead of being
    // collected until the heap runs out.
    static const size_t kMaxBodyBytes = 64 * 1024;

    void onBody(const char *path, WebRequestMethodComposite method, BodyDone done)
    {
        server.on(
            path, method,
            [](AsyncWebServerRequest *req)
            {
                // Reached with no body handler run at all: answer instead of
                // leaving the client waiting for a reply that never comes.
                if (req->contentLength() == 0)
                    req->send(400, "application/json", "{\"error\":\"empty body\"}");
            },
            nullptr,
            [done](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                   size_t index, size_t total)
            {
                // The buffer is plain malloc'd memory on purpose: the server
                // frees _tempObject with free() when the request is destroyed,
                // so an upload cut short by a disconnect is released rather
                // than leaked (a new'd std::vector would leak its contents and
                // be freed with the wrong call).
                if (total > kMaxBodyBytes)
                {
                    if (index == 0)
                        err(req, 413, "body too large");
                    return;
                }
                if (index == 0)
                {
                    req->_tempObject = malloc(total + 1);
                    if (!req->_tempObject) { err(req, 500, "out of memory"); return; }
                }
                if (!req->_tempObject) return; // earlier failure; drop the rest
                if (index + len > total) return;
                memcpy((uint8_t *)req->_tempObject + index, data, len);
                if (index + len == total)
                {
                    done(req, (uint8_t *)req->_tempObject, total);
                    free(req->_tempObject);
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
    // animname/filename become path components, so refuse anything that could
    // climb out of /anim/<id>/.
    static bool safeName(const String &n)
    {
        if (n.isEmpty() || n.length() > 64) return false;
        if (n.indexOf('/') >= 0 || n.indexOf('\\') >= 0) return false;
        if (n == "." || n == ".." || n.indexOf("..") >= 0) return false;
        return true;
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
                   if (!safeName(animname)) { err(req, 400, "bad animname"); return; }

                   JsonDocument out;
                   String text;
                   {
                       // The render loop may be rasterising this layer.
                       CompositorGuard g(factory.lock);
                       JsonObject root = out.to<JsonObject>();
                       JsonArray frames = root["frames"].to<JsonArray>();
                       JsonObject fo = frames.add<JsonObject>();
                       for (auto &o : factory.api.live) o->toJson(fo);
                       fo["duration"] = 1000;
                       serializeJsonPretty(out, text);
                   }
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
            // Snapshot the pixels under the lock (loop() rewrites factory.out
            // every frame) and hand the snapshot to the response.
            //
            // The body is produced by a filler callback, which the server asks
            // for bytes a packet at a time. An AsyncResponseStream would have
            // been simpler, but it collects the *whole* body in a growable
            // cbuf and reallocates as it grows — on a C3 that fails with
            // "cbuf resize(): failed to allocate temporary buffer" long before
            // the frame is sent. With a filler, peak memory is the snapshot
            // plus one TCP buffer.
            // The pixel buffer is malloc'ed, not a std::vector: vector::resize
            // throws std::bad_alloc when the heap is short, and an uncaught
            // exception is abort() + reboot. malloc just returns nullptr and
            // the request gets a 503 instead.
            struct Pixels
            {
                uint8_t *p = nullptr;
                size_t n = 0;
                Pixels() = default;
                Pixels(const Pixels &) = delete;
                Pixels &operator=(const Pixels &) = delete;
                ~Pixels() { free(p); }
                bool alloc(size_t len)
                {
                    p = (uint8_t *)malloc(len);
                    n = p ? len : 0;
                    return p != nullptr;
                }
                size_t size() const { return n; }
                uint8_t &operator[](size_t i) { return p[i]; }
            };
            // Load shedding. Every snapshot holds ~18 KB until its ~25 KB
            // response has been sent, which over the AP takes a while — a
            // client polling in a tight loop (watch -n 0 curl …) stacks
            // requests until the heap is empty and Wi-Fi itself can't
            // allocate any more. So: at most kMaxInFlight snapshots at once,
            // and only if the heap keeps kHeapReserve free afterwards for
            // Wi-Fi / lwIP / AsyncTCP / LittleFS. Otherwise answer 503 right
            // away, which costs almost nothing.
            static std::atomic<int> inFlight{0};
            static constexpr int kMaxInFlight = 2;
            static constexpr size_t kHeapReserve = 32 * 1024;

            struct Shot
            {
                int w = 0, h = 0;
                String head;
                Pixels px; // BGRA, exactly as sent
                Shot() { inFlight++; }
                // Runs when the response is done *or* the client vanished,
                // since the filler lambda holding the last reference is
                // destroyed with the response.
                ~Shot() { inFlight--; }
            };

            const size_t need = (size_t)factory.out.w * factory.out.h * 4;
            if (inFlight.load() >= kMaxInFlight)
            {
                err(req, 503, "busy: another frame is still being sent, retry shortly");
                return;
            }
            if (ESP.getFreeHeap() < need + kHeapReserve ||
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < need)
            {
                err(req, 503, "low memory: frame snapshot refused, retry shortly");
                return;
            }
            auto shot = std::make_shared<Shot>();
            {
                CompositorGuard g(factory.lock);
                const FrameBuffer &fb = factory.out;
                shot->w = fb.w;
                shot->h = fb.h;
                shot->px.alloc((size_t)fb.w * fb.h * 4);
                if (shot->px.size() == (size_t)fb.w * fb.h * 4)
                    for (size_t i = 0; i < fb.px.size(); i++)
                    {
                        const Rgba &c = fb.px[i];
                        shot->px[i * 4 + 0] = c.b;
                        shot->px[i * 4 + 1] = c.g;
                        shot->px[i * 4 + 2] = c.r;
                        shot->px[i * 4 + 3] = c.a;
                    }
            }
            if (shot->px.size() != (size_t)shot->w * shot->h * 4)
            {
                err(req, 503, "out of memory for a frame snapshot");
                return;
            }

            shot->head = String("{\"width\":") + shot->w + ",\"height\":" + shot->h +
                         ",\"format\":\"BGRA8888\",\"data\":\"";
            const size_t headLen = shot->head.length();
            const size_t b64Len = 4 * ((shot->px.size() + 2) / 3);
            const size_t tailLen = 2; // closing quote and brace
            const size_t total = headLen + b64Len + tailLen;

            req->send(req->beginResponse(
                "application/json", total,
                [shot, headLen, b64Len, total](uint8_t *buf, size_t maxLen, size_t index) -> size_t
                {
                    static const char *b64 =
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    size_t written = 0;
                    size_t pos = index;
                    while (written < maxLen && pos < total)
                    {
                        if (pos < headLen)
                        {
                            buf[written++] = (uint8_t)shot->head[pos++];
                            continue;
                        }
                        if (pos < headLen + b64Len)
                        {
                            // Every base64 character is a pure function of its
                            // position, so the callback needs no cursor state.
                            size_t c = pos - headLen;
                            size_t group = c / 4, k = c % 4;
                            size_t i = group * 3;
                            size_t have = shot->px.size() - i;
                            uint32_t v = (uint32_t)shot->px[i] << 16;
                            if (have > 1) v |= (uint32_t)shot->px[i + 1] << 8;
                            if (have > 2) v |= shot->px[i + 2];
                            char ch;
                            if (k == 0)      ch = b64[(v >> 18) & 63];
                            else if (k == 1) ch = b64[(v >> 12) & 63];
                            else if (k == 2) ch = have > 1 ? b64[(v >> 6) & 63] : '=';
                            else             ch = have > 2 ? b64[v & 63] : '=';
                            buf[written++] = (uint8_t)ch;
                            pos++;
                            continue;
                        }
                        buf[written++] = (uint8_t)(pos == total - 2 ? '"' : '}');
                        pos++;
                    }
                    return written;
                })); });

        // GET /framebuffer/getcomposition — live object trees of both layers
        server.on("/framebuffer/getcomposition", HTTP_GET,
                  [this](AsyncWebServerRequest *req)
                  {
            JsonDocument doc;
            CompositorGuard g(factory.lock);
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
                   if (!safeName(animname)) { err(req, 400, "bad animname"); return; }
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
                   if (!safeName(animname) || !safeName(filename))
                   { err(req, 400, "bad animname/filename"); return; }
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
                               ParamValue::fromJson(kv.value());

                   {
                       CompositorGuard g(factory.lock);
                       factory.player.start(anim, overrides, cycles, factory.primary);
                   }
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
                   {
                       CompositorGuard g(factory.lock);
                       factory.player.setSpeedScale(speed);
                   }
                   req->send(200, "application/json",
                             String("{\"status\":\"ok\",\"speed\":") + String(speed, 3) + "}");
               });

        // POST /anim/stop
        server.on("/anim/stop", HTTP_POST, [this](AsyncWebServerRequest *req)
                  {
            { CompositorGuard g(factory.lock); factory.player.stop(factory.primary); }
            req->send(200, "application/json", "{\"status\":\"stopped\"}"); });

        // POST /anim/pause — hold the timeline where it is
        server.on("/anim/pause", HTTP_POST, [this](AsyncWebServerRequest *req)
                  {
            bool running;
            {
                CompositorGuard g(factory.lock);
                running = factory.player.running();
                if (running) factory.player.pause();
            }
            if (!running) { err(req, 409, "nothing playing"); return; }
            req->send(200, "application/json", "{\"status\":\"paused\"}"); });

        // POST /anim/resume
        server.on("/anim/resume", HTTP_POST, [this](AsyncWebServerRequest *req)
                  {
            bool running;
            {
                CompositorGuard g(factory.lock);
                running = factory.player.running();
                if (running) factory.player.resume();
            }
            if (!running) { err(req, 409, "nothing playing"); return; }
            req->send(200, "application/json", "{\"status\":\"running\"}"); });

        // GET /anim/status
        server.on("/anim/status", HTTP_GET, [this](AsyncWebServerRequest *req)
                  {
            JsonDocument doc;
            CompositorGuard g(factory.lock);
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
            doc["brightness"] = driver->brightness;       // as requested
            doc["applied"] = driver->appliedBri;          // after scale-bri + limiter
            doc["pwr"] = driver->estimatedMilliamps;      // estimated mA (WLED: info.leds.pwr)
            doc["maxpwr"] = config.maxMilliamps;          // budget, 0 = off
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
