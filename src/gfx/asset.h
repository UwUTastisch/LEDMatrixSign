// asset.h — load 32-bit BGRA8888 uncompressed BMP assets from the active FS.
//
// Spec: "assets stored as 32-bit BGRA8888 uncompressed BMP". BMP rows are
// 4-byte aligned (already true at 32bpp) and stored bottom-up when height > 0.
// We decode into a flat top-down BGRA buffer that FrameBuffer::blitBGRA wants.
#pragma once
#include <Arduino.h>
#include <vector>
#include "../config.h"

struct Asset
{
    int w = 0, h = 0;
    std::vector<uint8_t> bgra; // top-down, 4 bytes/px (B,G,R,A)
    bool ok = false;
};

namespace AssetLoader
{
    // Sanity limits for an untrusted BMP header. 512 KB of decoded pixels is
    // already far more than any panel needs and comfortably beyond what the
    // heap can spare on an ESP32.
    static const int kMaxDim = 4096;
    static const size_t kMaxBytes = 512 * 1024;

    inline uint32_t rd32(File &f)
    {
        uint8_t b[4];
        f.read(b, 4);
        return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
               ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    }
    inline uint16_t rd16(File &f)
    {
        uint8_t b[2];
        f.read(b, 2);
        return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    }

    // Load `path` (absolute, on activeFS). Returns Asset with ok=false on any
    // problem — callers then draw the missing-pattern instead of throwing.
    inline Asset load(const String &path)
    {
        Asset a;
        if (!activeFS().exists(path)) return a;
        File f = activeFS().open(path, FILE_READ);
        if (!f) return a;

        if (f.read() != 'B' || f.read() != 'M') { f.close(); return a; }
        f.seek(10);
        uint32_t dataOffset = rd32(f);
        uint32_t dibSize = rd32(f);
        if (dibSize < 40) { f.close(); return a; }
        int32_t bmpW = (int32_t)rd32(f);
        int32_t bmpH = (int32_t)rd32(f);
        rd16(f);                 // planes
        uint16_t bpp = rd16(f);  // must be 32
        uint32_t compression = rd32(f); // 0 = BI_RGB, 3 = BI_BITFIELDS
        if (bpp != 32 || (compression != 0 && compression != 3))
        {
            Serial.printf("⚠️ asset %s: need 32bpp uncompressed (bpp=%u comp=%u)\n",
                          path.c_str(), bpp, compression);
            f.close();
            return a;
        }

        // The header is untrusted: /file/uploadasset stores whatever bytes it
        // is given, and a truncated upload can leave a plausible-looking but
        // wrong header behind. Without these checks a negative or huge width
        // made the allocation below throw std::length_error — which is an
        // abort() and a panic reset on the device.
        if (bmpW <= 0 || bmpH == 0 || bmpW > kMaxDim ||
            bmpH > kMaxDim || bmpH < -kMaxDim)
        {
            Serial.printf("⚠️ asset %s: implausible size %dx%d (max %d)\n",
                          path.c_str(), (int)bmpW, (int)bmpH, kMaxDim);
            f.close();
            return a;
        }
        int absH = (bmpH > 0) ? bmpH : -bmpH;
        size_t need = (size_t)bmpW * (size_t)absH * 4;
        if (need > kMaxBytes)
        {
            Serial.printf("⚠️ asset %s: %ux%u needs %u bytes, over the %u-byte limit\n",
                          path.c_str(), (unsigned)bmpW, (unsigned)absH,
                          (unsigned)need, (unsigned)kMaxBytes);
            f.close();
            return a;
        }

        bool bottomUp = (bmpH > 0);
        a.w = bmpW;
        a.h = absH;
        a.bgra.assign(need, 0);
        if (a.bgra.size() != need) { f.close(); return a; } // allocation failed
        uint32_t rowSize = (uint32_t)bmpW * 4; // 32bpp is inherently 4-aligned

        std::vector<uint8_t> row(rowSize);
        bool complete = true;
        for (int srcY = 0; srcY < absH; srcY++)
        {
            int dstY = bottomUp ? (absH - 1 - srcY) : srcY;
            f.seek(dataOffset + (uint32_t)srcY * rowSize);
            if (f.read(row.data(), rowSize) != (int)rowSize) { complete = false; break; }
            // file order is B,G,R,A already — copy straight through
            memcpy(a.bgra.data() + (size_t)dstY * rowSize, row.data(), rowSize);
        }
        f.close();
        // A short file used to report ok with the missing rows left black;
        // treating it as broken shows the missing-asset marker instead.
        if (!complete)
        {
            Serial.printf("⚠️ asset %s: pixel data ends early\n", path.c_str());
            a.bgra.clear();
            a.w = a.h = 0;
            return a;
        }
        a.ok = true;
        return a;
    }
}
