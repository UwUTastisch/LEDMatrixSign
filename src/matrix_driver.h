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
        int panelIdx = -1;
        uint32_t offset = 0;

        for (size_t i = 0; i < cfg.panels.size(); i++)
        {
            auto &P = cfg.panels[i];
            if (!P.enabled)
            {
                offset += uint32_t(P.w) * P.h;
                continue;
            }
            if (x >= P.x && x < P.x + P.w &&
                y >= P.y && y < P.y + P.h)
            {
                panelIdx = i;
                break;
            }
            offset += uint32_t(P.w) * P.h;
        }
        if (panelIdx < 0)
            return -1;

        auto &P = cfg.panels[panelIdx];
        int lx = x - P.x, ly = y - P.y;
        // rotate 90° CW
        if (P.rotate)
        {
            int ox = lx;
            lx = P.h - 1 - ly;
            ly = ox;
        }
        // vertical flip
        if (P.flipV)
        {
            ly = P.h - 1 - ly;
        }

        uint32_t idxInPanel;
        switch (cfg.order)
        {
        case 0:
            idxInPanel = ly * P.w + lx;
            break;
        case 1:
            idxInPanel = lx * P.h + ly;
            break;
        case 2: // serpentine rows
            if (ly & 1)
                idxInPanel = ly * P.w + (P.w - 1 - lx);
            else
                idxInPanel = ly * P.w + lx;
            break;
        case 3: // serpentine cols
            if (lx & 1)
                idxInPanel = lx * P.h + (P.h - 1 - ly);
            else
                idxInPanel = lx * P.h + ly;
            break;
        default:
            idxInPanel = ly * P.w + lx;
        }

        if (cfg.reverse)
        {
            idxInPanel = (cfg.stripLen - 1) - (offset + idxInPanel);
        }
        uint32_t globalIdx = cfg.startLED + cfg.skipLEDs + offset + idxInPanel;
        if (globalIdx >= cfg.startLED + cfg.stripLen)
            return -1;
        return int(globalIdx);
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

    // Draw a 24-bpp BMP onto the matrix
    bool drawBMP(const char *filename)
    {
        File f = SD.open(filename, FILE_READ);
        if (!f)
        {
            Serial.printf("❌ Open BMP %s failed\n", filename);
            return false;
        }
        // “BM”?
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

        uint32_t rowSize = ((uint32_t(bmpW) * 3 + 3) & ~3);
        strip.clear();

        for (int row = 0; row < abs(bmpH); row++)
        {
            int srcRow = (bmpH > 0) ? (bmpH - 1 - row) : row;
            f.seek(dataOffset + uint32_t(srcRow) * rowSize);
            for (int col = 0; col < bmpW; col++)
            {
                uint8_t b = f.read();
                uint8_t g = f.read();
                uint8_t r = f.read();
                int x = min(int((col * cfg.width) / bmpW), cfg.width - 1);
                int y = min(int((row * cfg.height) / abs(bmpH)), cfg.height - 1);
                setPixel(x, y, r, g, b);
            }
        }
        f.close();
        strip.show();
        return true;
    }

    // Optional mesh-dump over Serial
    void debugPrintMatrix()
    {
        Serial.println(F("=== Matrix indices ==="));
        for (uint16_t y = 0; y < cfg.height; y++)
        {
            for (uint16_t x = 0; x < cfg.width; x++)
            {
                Serial.printf("%4d", xyToIndex(x, y));
            }
            Serial.println();
        }
    }
};
