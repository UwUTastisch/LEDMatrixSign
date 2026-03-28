// matrix_driver.h
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"

// ——— Drives WS2812 strip & renders BMPs ———
class MatrixDriver
{
public:
    ConfigReader &cfg;
    Adafruit_NeoPixel strip;
    int brightness = 255;

    MatrixDriver(ConfigReader &c);
    void begin();
    void show();

    // Map (x,y) → global LED index
    int xyToIndex(uint16_t x, uint16_t y);

    void setPixel(uint16_t x, uint16_t y,
                  uint8_t r, uint8_t g, uint8_t b);

    // Read little-endian 32-bit
    static uint32_t read32(File &f);

    // Draw a 24-bpp BMP onto the matrix with general nearest-neighbor scaling
    bool drawBMP(uint8_t *buf, size_t len);

    // Draw a 24-bpp BMP onto the matrix with general nearest-neighbor scaling
    bool drawBMP(const char *filename);

    // Call this once you’ve filled the strip (e.g. after drawPNG() or show())
    void debugPrintMatrix();
};
