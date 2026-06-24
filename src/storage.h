// storage.h — filesystem layout + (de)serialisation for the 2.0 structure.
//
//   /config.json
//   /anim/<id>/anim.json
//   /anim/<id>/assets/<file>.bmp
//
// Provides the AnimLoader used by the Player and the read/write helpers used by
// the file/* and anim/* endpoints.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include "config.h"
#include "anim/composition.h"

namespace Storage
{
    inline String animDir(const String &id) { return "/anim/" + id; }
    inline String animJsonPath(const String &id) { return animDir(id) + "/anim.json"; }
    inline String assetsDir(const String &id) { return animDir(id) + "/assets"; }

    inline void ensureBaseDirs()
    {
        if (!activeFS().exists("/anim")) activeFS().mkdir("/anim");
    }

    // mkdir -p for a directory path (creates each missing level).
    inline void mkdirs(const String &dir)
    {
        int from = 1; // skip leading '/'
        while (true)
        {
            int slash = dir.indexOf('/', from);
            String sub = (slash < 0) ? dir : dir.substring(0, slash);
            if (sub.length() && !activeFS().exists(sub)) activeFS().mkdir(sub);
            if (slash < 0) break;
            from = slash + 1;
        }
    }

    // Parse /anim/<id>/anim.json into `out`. Returns false if missing/invalid.
    inline bool loadAnimation(const String &id, Animation &out)
    {
        String path = animJsonPath(id);
        if (!activeFS().exists(path)) return false;
        File f = activeFS().open(path, FILE_READ);
        if (!f) return false;
        JsonDocument doc; // v7 elastic document
        DeserializationError e = deserializeJson(doc, f);
        f.close();
        if (e)
        {
            Serial.printf("❌ anim.json parse (%s): %s\n", id.c_str(), e.c_str());
            return false;
        }
        return out.parse(doc.as<JsonObjectConst>(), animDir(id));
    }

    // Write raw anim.json text for an animation id.
    inline bool saveAnimationJson(const String &id, const uint8_t *data, size_t len)
    {
        mkdirs(animDir(id));
        mkdirs(assetsDir(id));
        File f = activeFS().open(animJsonPath(id), FILE_WRITE);
        if (!f) return false;
        f.write(data, len);
        f.close();
        return true;
    }

    // Write an asset (already-decoded bytes) under an animation's assets dir.
    inline bool saveAsset(const String &id, const String &filename,
                          const uint8_t *data, size_t len)
    {
        mkdirs(assetsDir(id));
        String path = assetsDir(id) + "/" + filename;
        File f = activeFS().open(path, FILE_WRITE);
        if (!f) return false;
        f.write(data, len);
        f.close();
        return true;
    }

    // List animation ids (top-level dirs under /anim).
    inline void listAnimations(std::function<void(const String &)> cb)
    {
        File dir = activeFS().open("/anim");
        if (!dir || !dir.isDirectory()) return;
        File e = dir.openNextFile();
        while (e)
        {
            if (e.isDirectory())
            {
                String n = e.name();
                int slash = n.lastIndexOf('/');
                cb(slash >= 0 ? n.substring(slash + 1) : n);
            }
            e = dir.openNextFile();
        }
        dir.close();
    }

    inline void listAssets(const String &id, std::function<void(const String &)> cb)
    {
        File dir = activeFS().open(assetsDir(id));
        if (!dir || !dir.isDirectory()) return;
        File e = dir.openNextFile();
        while (e)
        {
            if (!e.isDirectory())
            {
                String n = e.name();
                int slash = n.lastIndexOf('/');
                cb(slash >= 0 ? n.substring(slash + 1) : n);
            }
            e = dir.openNextFile();
        }
        dir.close();
    }

    struct FsStat
    {
        const char *source;
        uint64_t total = 0, used = 0;
    };

    inline FsStat fsStatus()
    {
        FsStat s;
        if (FSMount::source == FSMount::SD_CARD)
        {
            s.source = "sd";
            s.total = SD.totalBytes();
            s.used = SD.usedBytes();
        }
        else if (FSMount::source == FSMount::LITTLEFS)
        {
            s.source = "littlefs";
            s.total = LittleFS.totalBytes();
            s.used = LittleFS.usedBytes();
        }
        else
        {
            s.source = "none";
        }
        return s;
    }
}
