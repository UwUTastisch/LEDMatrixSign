// matrix_driver.h
#pragma once

#include <Arduino.h>
#include <SPI.h>

#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <soc/soc_caps.h>
#include <stddef.h>
#include "config.h"
#include "gfx/framebuffer.h"
#define min(a, b) ((a) < (b) ? (a) : (b))

#ifndef __containerof
#define __containerof(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

// ——— Custom RMT encoder: RGB bytes followed by a WS2812 reset (latch) ———
typedef struct
{
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    rmt_symbol_word_t reset_code;
    int state;
} ws2812_rmt_encoder_t;

static size_t IRAM_ATTR ws2812_rmt_encode(rmt_encoder_t *encoder,
                                          rmt_channel_handle_t channel,
                                          const void *primary_data,
                                          size_t data_size,
                                          rmt_encode_state_t *ret_state)
{
    ws2812_rmt_encoder_t *e = __containerof(encoder, ws2812_rmt_encoder_t, base);
    rmt_encoder_handle_t bytes = e->bytes_encoder;
    rmt_encoder_handle_t copy = e->copy_encoder;
    rmt_encode_state_t session = RMT_ENCODING_RESET;
    rmt_encode_state_t out = RMT_ENCODING_RESET;
    size_t n = 0;

    switch (e->state)
    {
    case 0: // stream the pixel bytes
        n += bytes->encode(bytes, channel, primary_data, data_size, &session);
        if (session & RMT_ENCODING_COMPLETE)
            e->state = 1;
        if (session & RMT_ENCODING_MEM_FULL)
        {
            out = (rmt_encode_state_t)(out | RMT_ENCODING_MEM_FULL);
            break;
        }
        // fall through
    case 1: // emit the reset / latch pulse
        n += copy->encode(copy, channel, &e->reset_code,
                          sizeof(e->reset_code), &session);
        if (session & RMT_ENCODING_COMPLETE)
        {
            e->state = RMT_ENCODING_RESET;
            out = (rmt_encode_state_t)(out | RMT_ENCODING_COMPLETE);
        }
        if (session & RMT_ENCODING_MEM_FULL)
            out = (rmt_encode_state_t)(out | RMT_ENCODING_MEM_FULL);
        break;
    }

    *ret_state = out;
    return n;
}

static esp_err_t ws2812_rmt_encoder_reset(rmt_encoder_t *encoder)
{
    ws2812_rmt_encoder_t *e = __containerof(encoder, ws2812_rmt_encoder_t, base);
    rmt_encoder_reset(e->bytes_encoder);
    rmt_encoder_reset(e->copy_encoder);
    e->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t ws2812_rmt_encoder_del(rmt_encoder_t *encoder)
{
    ws2812_rmt_encoder_t *e = __containerof(encoder, ws2812_rmt_encoder_t, base);
    rmt_del_encoder(e->bytes_encoder);
    rmt_del_encoder(e->copy_encoder);
    free(e);
    return ESP_OK;
}

// ——— Drop-in replacement for the slice of Adafruit_NeoPixel we use ———
class Ws2812Rmt
{
public:
    Ws2812Rmt(uint16_t count, uint8_t pin) : _count(count), _pin(pin) {}

    void begin()
    {
        if (_buf)
            return;
        _buf = (uint8_t *)calloc(size_t(_count) * 3, 1); // GRB wire order
        if (!_buf)
        {
            Serial.println("❌ WS2812: out of memory for pixel buffer");
            return;
        }

        rmt_tx_channel_config_t ch = {};
        ch.gpio_num = (gpio_num_t)_pin;
        ch.clk_src = RMT_CLK_SRC_DEFAULT;
        ch.resolution_hz = 10 * 1000 * 1000; // 10 MHz -> 0.1 µs / tick
        // Two hardware blocks: small (~hundreds of bytes) but enough ISR slack
        // to keep refilling while Wi-Fi / AsyncWebServer interrupts run.
        ch.mem_block_symbols = 2 * SOC_RMT_MEM_WORDS_PER_CHANNEL;
        ch.trans_queue_depth = 4;
        if (rmt_new_tx_channel(&ch, &_chan) != ESP_OK)
        {
            Serial.println("❌ WS2812: rmt_new_tx_channel failed");
            return;
        }

        ws2812_rmt_encoder_t *e =
            (ws2812_rmt_encoder_t *)calloc(1, sizeof(ws2812_rmt_encoder_t));
        e->base.encode = ws2812_rmt_encode;
        e->base.del = ws2812_rmt_encoder_del;
        e->base.reset = ws2812_rmt_encoder_reset;

        rmt_bytes_encoder_config_t bcfg = {};
        // WS2812B bit timings @ 10 MHz (ticks of 0.1 µs):
        //   '0' = 0.3 µs high, 0.9 µs low   '1' = 0.9 µs high, 0.3 µs low
        bcfg.bit0.level0 = 1;
        bcfg.bit0.duration0 = 3;
        bcfg.bit0.level1 = 0;
        bcfg.bit0.duration1 = 9;
        bcfg.bit1.level0 = 1;
        bcfg.bit1.duration0 = 9;
        bcfg.bit1.level1 = 0;
        bcfg.bit1.duration1 = 3;
        bcfg.flags.msb_first = 1; // WS2812 expects MSB first
        rmt_new_bytes_encoder(&bcfg, &e->bytes_encoder);

        rmt_copy_encoder_config_t ccfg = {};
        rmt_new_copy_encoder(&ccfg, &e->copy_encoder);

        // ~300 µs low = latch (safe for newer WS2812B that want >280 µs)
        e->reset_code.level0 = 0;
        e->reset_code.duration0 = 1500;
        e->reset_code.level1 = 0;
        e->reset_code.duration1 = 1500;
        e->state = RMT_ENCODING_RESET;

        _encoder = &e->base;
        rmt_enable(_chan);
    }

    void show()
    {
        if (!_buf || !_chan || !_encoder)
            return;
        rmt_transmit_config_t tx = {};
        tx.loop_count = 0; // single shot
        rmt_transmit(_chan, _encoder, _buf, size_t(_count) * 3, &tx);
        rmt_tx_wait_all_done(_chan, portMAX_DELAY);
    }

    void clear()
    {
        if (_buf)
            memset(_buf, 0, size_t(_count) * 3);
    }

    static uint32_t Color(uint8_t r, uint8_t g, uint8_t b)
    {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    void setPixelColor(uint16_t i, uint32_t c)
    {
        if (i >= _count || !_buf)
            return;
        uint8_t *p = _buf + size_t(i) * 3;
        p[0] = (c >> 8) & 0xFF;  // G
        p[1] = (c >> 16) & 0xFF; // R
        p[2] = c & 0xFF;         // B
    }

    uint32_t getPixelColor(uint16_t i) const
    {
        if (i >= _count || !_buf)
            return 0;
        uint8_t *p = _buf + size_t(i) * 3;
        return ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 8) | p[2];
    }

    // Dimming is applied in MatrixDriver::setPixel, so this stays a no-op
    // (matches the previously-commented strip.setBrightness call).
    void setBrightness(uint8_t) {}
    uint16_t numPixels() const { return _count; }

private:
    uint16_t _count;
    uint8_t _pin;
    uint8_t *_buf = nullptr;
    rmt_channel_handle_t _chan = nullptr;
    rmt_encoder_handle_t _encoder = nullptr;
};

// ——— Drives WS2812 strip & renders BMPs ———
class MatrixDriver
{
public:
    ConfigReader &cfg;
    Ws2812Rmt strip;
    int brightness = 255;

    MatrixDriver(ConfigReader &c)
        : cfg(c), strip(c.stripLen, c.pin) {}

    void debugPrintMatrix();

    void begin()
    {
        strip.begin();
        strip.show();
    }
    void show() { strip.show(); }

    // ——— 2.0: push a composited matrix-sized FrameBuffer to the LEDs ———
    // The FrameFactory hands us the already-composited RGBA buffer at matrix
    // resolution; we map each (x,y) through the panel layout and apply global
    // brightness. Transparent pixels (alpha 0) read as black.
    void showFrameBuffer(const FrameBuffer &fb)
    {
        if (fb.w != cfg.width || fb.h != cfg.height)
            return;
        strip.clear();
        for (uint16_t y = 0; y < cfg.height; y++)
            for (uint16_t x = 0; x < cfg.width; x++)
            {
                const Rgba &c = fb.atRef(x, y);
                setPixel(x, y, c.r, c.g, c.b);
            }
        strip.show();
    }

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
        {
            r = (r * brightness) / 255;
            g = (g * brightness) / 255;
            b = (b * brightness) / 255;
            strip.setPixelColor(i, strip.Color(r, g, b));
        }
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
    bool drawBMP(File f)
    {
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
        if (DEBUG_MATRIX > 1)
            debugPrintMatrix(*this);
#endif
        // strip.setBrightness(brightness); // Ensure current brightness is applied
        strip.show();
        return true;
    }

    // Draw a 24-bpp BMP onto the matrix with general nearest-neighbor scaling
    bool drawBMP(const char *filename)
    {
        File f = activeFS().open(filename, FILE_READ);
        if (!f)
        {
            Serial.printf("❌ Open BMP %s failed\n", filename);
            return false;
        }
        return drawBMP(f);
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