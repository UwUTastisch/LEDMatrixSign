// matrix_driver.h
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"

// ——— Drives WS2812 strip & renders BMPs ———
class MatrixDriver
{
public:
    ConfigReader &cfg;
    Adafruit_NeoPixel strip;

    MatrixDriver(ConfigReader &c)
        : cfg(c), strip(c.stripLen, c.pin, NEO_GRB + NEO_KHZ800) {}

    void debugPrintMatrix();

    void begin()
    {
        strip.begin();
        strip.show();
    }
    void show() { strip.show(); }

    // Map (x,y) → global LED index
    int xyToIndex(uint16_t x, uint16_t y)
    {
        if (x >= cfg.width || y >= cfg.height)
            return -1;

        // find which panel this (x,y) lives in and accumulate offset
        uint32_t offset = 0;
        int panelIdx = -1;
        for (size_t i = 0; i < cfg.panels.size(); i++)
        {
            const auto &P = cfg.panels[i];
            uint32_t panelSize = P.w * P.h;
            if (x >= P.x && x < P.x + P.w && y >= P.y && y < P.y + P.h)
            {
                panelIdx = i;
                break;
            }
            offset += panelSize;
        }
        if (panelIdx < 0)
            return -1;
        const auto &P = cfg.panels[panelIdx];

        // local coords in panel
        uint16_t lx = x - P.x;
        uint16_t ly = y - P.y;

        // flip for rightFirst / bottomFirst
        uint16_t xp = P.rightFirst ? (P.w - 1 - lx) : lx;
        uint16_t yp = P.bottomFirst ? (P.h - 1 - ly) : ly;

        // choose strip index and position along strip
        uint32_t stripIndex, posInStrip, stripLength;
        if (P.vertical)
        {
            stripIndex = xp;
            posInStrip = yp;
            stripLength = P.h;
        }
        else
        {
            stripIndex = yp;
            posInStrip = xp;
            stripLength = P.w;
        }

        // serpentine every other strip
        if (P.serpentine && (stripIndex & 1))
        {
            posInStrip = stripLength - 1 - posInStrip;
        }

        // combine into panel-local index
        uint32_t idxInPanel = stripIndex * stripLength + posInStrip;
        uint32_t pixelIndex = offset + idxInPanel;
        if (pixelIndex >= cfg.stripLen)
            return -1;

        // global reverse flag
        if (cfg.reverse)
        {
            pixelIndex = (cfg.stripLen - 1) - pixelIndex;
        }

        return int(cfg.startLED + cfg.skipLEDs + pixelIndex);
    }

    void setPixel(uint16_t x, uint16_t y,
                  uint8_t r, uint8_t g, uint8_t b)
    {
        int i = xyToIndex(x, y);
        if (i >= 0)
            strip.setPixelColor(i, strip.Color(r, g, b));
    }
    // Read little-endian 32-bit
    static uint32_t read32(File &f)
    {
        uint32_t b0 = f.read();
        uint32_t b1 = f.read() << 8;
        uint32_t b2 = f.read() << 16;
        uint32_t b3 = f.read() << 24;
        return b0 | b1 | b2 | b3;
    }

    // Draw a 24-bpp BMP onto the matrix with general nearest-neighbor scaling
    bool drawBMP(const char *filename)
    {
        File f = SD.open(filename, FILE_READ);
        if (!f)
        {
            Serial.printf("❌ Open BMP %s failed\n", filename);
            return false;
        }
        // — Header check —
        if (f.read() != 'B' || f.read() != 'M')
        {
            Serial.println("❌ Not a BMP");
            f.close();
            return false;
        }
        f.seek(10);
        uint32_t dataOffset = read32(f);
        uint32_t dibSize = read32(f);
        if (dibSize < 40)
        {
            Serial.println("❌ Unsupported BMP header");
            f.close();
            return false;
        }
        int32_t bmpW = int32_t(read32(f));
        int32_t bmpH = int32_t(read32(f));
        f.seek(2, SeekCur);
        uint16_t bpp = f.read() | (f.read() << 8);
        if (bpp != 24)
        {
            Serial.printf("❌ Only 24-bpp BMP (got %u)\n", bpp);
            f.close();
            return false;
        }
        if (read32(f) != 0)
        {
            Serial.println("❌ Compressed BMP not supported");
            f.close();
            return false;
        }

        // — Prep for scaling —
        int absH = abs(bmpH);
        // rowSize padded to 4-byte boundary:
        uint32_t rowSize = ((uint32_t(bmpW) * 3 + 3) & ~3);

        // buffer one source row of pixels
        struct Pixel
        {
            uint8_t r, g, b;
        };
        Pixel *rowBuf = (Pixel *)malloc(sizeof(Pixel) * bmpW);
        if (!rowBuf)
        {
            Serial.println("❌ Out of memory");
            f.close();
            return false;
        }

        // clear your matrix
        strip.clear();

        // Precompute ratios:
        float fy = float(absH) / float(cfg.height);
        float fx = float(bmpW) / float(cfg.width);

        // For each destination row
        for (int y = 0; y < cfg.height; y++)
        {
            // map to source row (nearest-neighbor)
            int srcRow = min(int(y * fy), absH - 1);
            // account for BMP’s bottom-up storage if bmpH>0
            int bmpRow = (bmpH > 0) ? (absH - 1 - srcRow) : srcRow;
            // seek & read that one row
            f.seek(dataOffset + uint32_t(bmpRow) * rowSize);
            for (int x = 0; x < bmpW; x++)
            {
                uint8_t bb = f.read();
                uint8_t gg = f.read();
                uint8_t rr = f.read();
                rowBuf[x] = {rr, gg, bb};
            }

            // now map each destination X → srcCol, and setPixel
            for (int x = 0; x < cfg.width; x++)
            {
                int srcCol = min(int(x * fx), bmpW - 1);
                auto &p = rowBuf[srcCol];
                setPixel(x, y, p.r, p.g, p.b);
            }
        }

        free(rowBuf);
        f.close();
#if DEBUG_MATRIX
        debugPrintMatrix(*this);
#endif
        strip.show();
        return true;
    }

    // Call this once you’ve filled the strip (e.g. after drawPNG() or show())
    void debugPrintMatrix(MatrixDriver &driver)
    {
        auto &strip = driver.strip;
        auto &cfg = driver.cfg;

        const char *RESET = "\x1b[0m";
        auto printColor = [&](uint32_t c)
        {
            uint8_t r = (c >> 16) & 0xFF;
            uint8_t g = (c >> 8) & 0xFF;
            uint8_t b = c & 0xFF;
            Serial.printf("\x1b[38;2;%u;%u;%um#%06X%s ",
                          r, g, b, c, RESET);
        };

        // 1) Original Img Mesh
        Serial.println(F("=== Original Img Mesh (hex colors) ==="));
        for (uint16_t y = 0; y < cfg.height; y++)
        {
            for (uint16_t x = 0; x < cfg.width; x++)
            {
                int idx = driver.xyToIndex(x, y);
                uint32_t c = (idx >= 0)
                                 ? (strip.getPixelColor(idx) & 0xFFFFFF)
                                 : 0;
                printColor(c);
            }
            Serial.println();
        }

        // 2) Remapping Mesh
        Serial.println(F("\n=== Remapping Mesh (LED indices) ==="));
        for (uint16_t y = 0; y < cfg.height; y++)
        {
            for (uint16_t x = 0; x < cfg.width; x++)
            {
                Serial.printf("%3d ", driver.xyToIndex(x, y));
            }
            Serial.println();
        }

        // 3) Remapping Sequence
        Serial.println(F("\n=== Remapping Sequence (send order 1→N) ==="));
        for (uint16_t y = 0; y < cfg.height; y++)
        {
            for (uint16_t x = 0; x < cfg.width; x++)
            {
                int idx = driver.xyToIndex(x, y);
                Serial.printf("%3u ",
                              (idx >= 0) ? (idx + 1) : 0);
            }
            Serial.println();
        }

        // 4) Origin → Destination Map
        Serial.println(F("\n=== Origin → Destination Map (origIdx -> sendIdx) ==="));
        // flat, comma-separated
        for (uint16_t orig = 0; orig < strip.numPixels(); orig++)
        {
            uint16_t x = orig % cfg.width;
            uint16_t y = orig / cfg.width;
            int send = driver.xyToIndex(x, y);
            Serial.printf("%u->%d", orig, send);
            if (orig + 1 < strip.numPixels())
                Serial.print(", ");
        }
        Serial.println();
    }
};
