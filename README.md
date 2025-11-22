# Project Title

**ESP32 LED Matrix Controller with SD-Card Configuration**

A flexible LED matrix driver for ESP32-based controllers that reads its entire configuration (LED & panel layout, SPI/SD settings, and Wi‑Fi credentials) from a JSON file stored on an SD card. Designed for easy reconfiguration without reflashing firmware.

## Features

- **SD‑Card Configuration**: Load hardware and network settings from `/config.json` on SD card
- **LED Matrix Layout**: Define multiple panels, their positions, sizes, and orientation parameters (rotate, flip, serpentine)
- **Flexible LED Strip Mapping**: Support for skipped LEDs, multiple input segments, reversing order, and custom SPI pins
- **Wi‑Fi Autoconnect**: Automatically connect to Wi‑Fi using credentials in the JSON config
- **Lightweight JSON**: Powered by ArduinoJson for fast parsing on constrained devices

## Hardware Requirements

- ESP32 development board (or equivalent)
- MicroSD card module wired to SPI pins
- SD card (formatted FAT32)
- LED strip (e.g., WS2812b) or matrix panels
- Power supply rated for your LED count

## Software Requirements

- PlatformIO

## Wiring

```text
ESP32        SD     LED Strip
-----        --     ---------
3V3  (or 5V) VCC    (LED 5V)
GND          GND    (LED GND)
GPIO22 (CS)  CS
GPIO18 (SCK) SCK
GPIO23 (MOSI) MOSI
GPIO19 (MISO) MISO  (optional)
GPIO<order>  WS2812B   (configured pin in JSON)
```

> Adjust pins in the JSON under `hw.led.ins[0].pin[0]` and `hw.led.ins[0].pin[1]` as needed.

## Configuration File (`/config.json`)

Place a JSON file named `config.json` at the root of your SD card. The file should define two top‑level objects: `hw` and `wifi`.

```json
{
  "hw": {
    "led": {
      "total": 512,
      "ins": [
        {
          "start": 0,
          "len": 512,
          "skip": 0,
          "pin": [5],
          "order": 1,
          "rev": false
        }
      ],
      "matrix": {
        "panels": [
          {
            "b": true,
            "x": 0,
            "y": 0,
            "w": 16,
            "h": 16,
            "r": false,
            "v": false,
            "s": true
          },
          {
            "b": true,
            "x": 16,
            "y": 0,
            "w": 16,
            "h": 16,
            "r": false,
            "v": false,
            "s": true
          }
        ]
      }
    }
  },
  "wifi": {
    "ssid": "YourNetworkSSID",
    "password": "YourNetworkPassword"
  }
}
```

| Key                    | Description                                  |
| ---------------------- | -------------------------------------------- |
| `hw.led.total`         | Total number of LEDs across all panels       |
| `hw.led.ins[].start`   | Starting LED index for this input segment    |
| `hw.led.ins[].len`     | Number of LEDs in this segment               |
| `hw.led.ins[].skip`    | Number of LEDs to skip at the start          |
| `hw.led.ins[].pin`     | SPI / data pin number(s)                     |
| `hw.led.ins[].order`   | Color order enum (e.g., GRB = 1)             |
| `hw.led.ins[].rev`     | Reverse LED strand direction                 |
| `hw.led.matrix.panels` | Array of panel layout objects                |
| `panels[].b`           | Panel enabled (boolean)                      |
| `panels[].x`, `y`      | Top‑left corner of panel in the virtual grid |
| `panels[].w`, `h`      | Width and height of each panel in LEDs       |
| `panels[].r`           | Flip panel horizontal  (right start led)     |
| `panels[].v`           | Flip panel vertically                        |
| `panels[].s`           | Override serpentine wiring                   |
| `wifi.ssid`            | Wi‑Fi network SSID                           |
| `wifi.password`        | Wi‑Fi network password                       |

This should be mostly compatible with WLED-Config.

## Usage

1. Copy `config.json` to the root of your SD card.
2. (Optional) put a folder `./images/` at root of Sd card and put your bitmaps `*.bmp` inside. `convert "./input.png" -strip -colorspace sRGB -type TrueColor "BPP24:output-image.bmp"`
3. Insert the SD card into the Esp-Sd-Cardreader.
4. Install the Project with Platform.io to your ESP.
5. The firmware will parse hardware settings and Wi‑Fi credentials at startup.

## Troubleshooting

- **SD init failed**: Check CS pin wiring and SD card formated FAT.
- **JSON parse error**: Verify that `config.json` is valid JSON (e.g., via [JSONLint](https://jsonlint.com/)).
- **Wi‑Fi not connecting**: Confirm SSID/password are correct and in range.

## License

All rights reserved.&#x20;
