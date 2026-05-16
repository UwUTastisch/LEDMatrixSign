# LEDMatrixSign — SD-first / LittleFS-fallback fork

**ESP32 LED Matrix Controller with automatic storage selection**

This is a fork of [UwUTastisch/LEDMatrixSign](https://github.com/UwUTastisch/LEDMatrixSign).

At boot the firmware probes for an SD card first. If the card is present **and** contains `/config.json` it is used. Otherwise the firmware falls back to LittleFS (internal flash). No recompile is needed to switch — just insert or remove the card.

## Storage selection logic

```
boot
 └─ try SD.begin() + check /config.json on SD
      ├─ ✅ found  →  use SD for everything
      └─ ❌ missing / init failed
           └─ mount LittleFS + check /config.json
                ├─ ✅ found  →  use LittleFS for everything
                └─ ❌ missing  →  halt (upload a filesystem image)
```

You can confirm which storage is active at runtime via the new endpoint:

```
GET /api/fssource   →  { "fs": "sd" }  or  { "fs": "littlefs" }
```

## What changed vs. the original

| Area              | Original                  | This fork                                                 |
| ----------------- | ------------------------- | --------------------------------------------------------- |
| Storage           | SD only                   | SD preferred, LittleFS fallback                           |
| FS abstraction    | `SD.open()` etc. directly | `activeFS().open()` etc. everywhere                       |
| Directory listing | `File::getNextFileName()` | `File::openNextFile()` (works on both)                    |
| `platformio.ini`  | `greiman/SdFat` only      | `greiman/SdFat` **+** `board_build.filesystem = littlefs` |
| LittleFS data     | —                         | `data/` folder; flash with `uploadfs`                     |
| New API endpoint  | —                         | `GET /api/fssource`                                       |

## Wiring

SD card is **optional**. If you don't use one, skip the SD wiring entirely.

```
ESP32          SD module      LED Strip
-----          ---------      ---------
3V3 / 5V       VCC            VCC (5 V)
GND            GND            GND
GPIO SD_CS     CS
GPIO SD_SCK    SCK
GPIO SD_MOSI   MOSI
GPIO SD_MISO   MISO (opt.)
GPIO<pin>                     Data (set in config.json)
```

Default SD pins per environment (override with `-D SD_CS=…` in `build_flags`):

| env                | CS  | MOSI | MISO | SCK |
| ------------------ | --- | ---- | ---- | --- |
| esp32-c3-devkitm-1 | 4   | 3    | 1    | 2   |
| esp32dev           | 22  | 23   | 19   | 18  |

## Getting started

### Option A — SD card

1. Format the card as FAT32.
2. Copy `data/config.json` (and any BMP images) to the card root.
3. Insert the card and flash the firmware: `pio run --target upload`

### Option B — LittleFS (no SD card)

1. Edit `data/config.json` with your settings.
2. Place BMP images in `data/images/`.
3. Flash firmware **and** filesystem:
   ```
   pio run --target upload
   pio run --target uploadfs
   ```

### Option C — SD primary, LittleFS emergency fallback

Do both: keep the SD card inserted for normal use, and flash a LittleFS image as a backup. If you ever boot without the card the firmware will use LittleFS automatically.

## Configuration file (`/config.json`)

Same schema as the upstream project — see the [original README](https://github.com/UwUTastisch/LEDMatrixSign) for full field documentation.

## License — MIT

Copyright © 2026 UwUTastisch — same terms as the upstream project.
