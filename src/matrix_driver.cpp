#include "matrix_driver.h"
#include <SPI.h>
#include <SD.h>
#include <algorithm>

MatrixDriver::MatrixDriver(ConfigReader &c)
    : cfg(c), strip(c.stripLen, c.pin, NEO_GRB + NEO_KHZ800) {}

void MatrixDriver::begin()
{
    strip.begin();
    strip.show();
}
void MatrixDriver::show() { strip.show(); }

// Map (x,y) → global LED index
int MatrixDriver::xyToIndex(uint16_t x, uint16_t y)
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

void MatrixDriver::setPixel(uint16_t x, uint16_t y,
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
uint32_t MatrixDriver::read32(File &f)
{
    uint32_t b0 = f.read();
    uint32_t b1 = f.read() << 8;
    uint32_t b2 = f.read() << 16;
    uint32_t b3 = f.read() << 24;
    return b0 | b1 | b2 | b3;
}

bool MatrixDriver::drawBMP(uint8_t *buf, size_t len)
{
    struct [[gnu::packed]] BmpFileHeader {
        uint16_t type;
        uint32_t fileSize;
        uint16_t reserved1;
        uint16_t reserved2;
        uint32_t offset;
    };

    struct BmpInfoHeader {
        uint32_t size;
        int32_t width;
        int32_t height;
        uint16_t planes;
        uint16_t bitCount;
        uint32_t compression;
        // we don't care about the rest of the header
    };

    struct Pixel {
        uint8_t g;
        uint8_t b;
        uint8_t r;
    };

    if (len < sizeof(BmpFileHeader) + sizeof(BmpInfoHeader)) {
        Serial.println("❌ Not big enough for BMP header");
        return false;
    }
    BmpFileHeader &fileHeader = *(BmpFileHeader*)buf;
    if (fileHeader.type != ('B' | 'M' << 8))
    {
        Serial.println("❌ Not a BMP");
        return false;
    }
    BmpInfoHeader &infoHeader = *(BmpInfoHeader*)(buf + sizeof(BmpFileHeader));
    if (infoHeader.size < sizeof(BmpInfoHeader))
    {
        Serial.println("❌ Unsupported BMP header");
        return false;
    }
    if (infoHeader.bitCount != 24)
    {
        Serial.printf("❌ Only 24-bpp BMP (got %u)\n", infoHeader.bitCount);
        return false;
    }
    if (infoHeader.compression != 0)
    {
        Serial.println("❌ Compressed BMP not supported");
        return false;
    }
    size_t rowLen = ((uint32_t(infoHeader.width) * 3 + 3) & ~3);
    if (len < fileHeader.offset + rowLen*infoHeader.height)
    {
        Serial.println("❌ Missing image data");
        return false;
    }

    uint32_t absH = std::abs(infoHeader.height);

    strip.clear();

    // Precompute ratios:
    float fy = (float)absH / (float)cfg.height;
    float fx = (float)infoHeader.width / (float)cfg.width;

    // For each destination row
    for (int y = 0; y < cfg.height; y++)
    {
        // map to source row (nearest-neighbor)
        uint32_t srcRow = std::min((uint32_t)(y * fy), absH - 1);
        // account for BMP’s bottom-up storage if bmpH>0
        uint32_t bmpRow = (infoHeader.height > 0) ? (absH - 1 - srcRow) : srcRow;
        // seek & read that one row
        Pixel *pixelRow = (Pixel*)(buf + fileHeader.offset + bmpRow*rowLen);
        for (int x = 0; x < infoHeader.width; x++)
        {
            uint32_t srcCol = std::min((long)(x * fx), infoHeader.width - 1);
            Pixel &pixel = pixelRow[x];
            setPixel(x, y, pixel.r, pixel.g, pixel.b);
        }
    }

#if DEBUG_MATRIX
    debugPrintMatrix();
#endif
    // strip.setBrightness(brightness); // Ensure current brightness is applied
    strip.show();
    return true;
}

// Draw a 24-bpp BMPvoid MatrixDriver::debugPrintMatrix();
// onto the matrix with general nearest-neighbor scaling
bool MatrixDriver::drawBMP(const char *filename)
{
    File f = SD.open(filename, FILE_READ);
    if (!f)
    {
        Serial.printf("❌ Open BMP %s failed\n", filename);
        return false;
    }
    std::vector<uint8_t> buf(f.size());
    f.read(buf.data(), buf.size());
    bool ret = drawBMP(buf.data(), buf.size());
    f.close();
    return ret;
}

// Call this once you’ve filled the strip (e.g. after drawPNG() or show())
void MatrixDriver::debugPrintMatrix()
{
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
            int idx = xyToIndex(x, y);
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
            Serial.printf("%3d ", xyToIndex(x, y));
        }
        Serial.println();
    }

    // 3) Remapping Sequence
    Serial.println(F("\n=== Remapping Sequence (send order 1→N) ==="));
    for (uint16_t y = 0; y < cfg.height; y++)
    {
        for (uint16_t x = 0; x < cfg.width; x++)
        {
            int idx = xyToIndex(x, y);
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
        int send = xyToIndex(x, y);
        Serial.printf("%u->%d", orig, send);
        if (orig + 1 < strip.numPixels())
            Serial.print(", ");
    }
    Serial.println();
}
