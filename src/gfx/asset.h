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

        int absH = abs(bmpH);
        bool bottomUp = (bmpH > 0);
        a.w = bmpW;
        a.h = absH;
        a.bgra.assign((size_t)bmpW * absH * 4, 0);
        uint32_t rowSize = (uint32_t)bmpW * 4; // 32bpp is inherently 4-aligned

        std::vector<uint8_t> row(rowSize);
        for (int srcY = 0; srcY < absH; srcY++)
        {
            int dstY = bottomUp ? (absH - 1 - srcY) : srcY;
            f.seek(dataOffset + (uint32_t)srcY * rowSize);
            if (f.read(row.data(), rowSize) != (int)rowSize) break;
            // file order is B,G,R,A already — copy straight through
            memcpy(a.bgra.data() + (size_t)dstY * rowSize, row.data(), rowSize);
        }
        f.close();
        a.ok = true;
        return a;
    }
}
