# XIAO 2.9" BWRY E‑Paper Weather Display

Build a crisp, low‑power 4‑color (black/white/yellow/red) weather dashboard on the **[Seeed Studio XIAO ESP32‑C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/)** with the **2.9" 128×296 BWRY e‑paper panel** and the **[Seeed XIAO ePaper driver board](https://wiki.seeedstudio.com/xiao_eink_expansion_board_v2/)**.  
This project focuses on a reliable, repeatable workflow: correct controller selection, correct pin mapping, and a proven render path.

![XIAO e‑paper weather display](assets/readme-display.png)

## Why This Is Worth Building

- **Always‑on, almost‑no‑power**: e‑paper only consumes power while refreshing.
- **Legible at a glance**: high‑contrast layout, bold temperature, weather icons.
- **Self‑contained onboarding**: Wi‑Fi captive portal for iPhone/iPad/Mac.
- **Maker‑friendly**: PlatformIO project, no manual SDK installs.

---

## Hardware

- **MCU**: Seeed Studio XIAO ESP32‑C6  
- **Driver Board**: Seeed Studio XIAO ePaper driver board / expansion board V2  
- **Panel**: 2.9" BWRY e‑paper, 128×296  

## Software Stack

- PlatformIO
- Arduino framework
- Seeed_GFX (vendored in `lib/Seeed_GFX`)

---

## Quick Start

### 1) Install PlatformIO

Install PlatformIO Core or use the PlatformIO IDE extension.

### 2) Build & Flash

```bash
platformio run -t upload -e seeed_xiao_esp32c6
```

### 3) First Boot Setup

On first boot, the device starts a captive portal:

- Connect to **XIAO-Weather-Setup**
- Your phone should open the setup page automatically
- Select Wi‑Fi, enter password, and save

The device reboots and fetches the forecast.

---

## PlatformIO Configuration

From `platformio.ini`:

```ini
[env:seeed_xiao_esp32c6]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = seeed_xiao_esp32c6
framework = arduino
monitor_speed = 115200

build_flags =
    -DARDUINO_SEEED_XIAO_ESP32C6
    -DBOARD_SCREEN_COMBO=512
    -DUSE_XIAO_EPAPER_DRIVER_BOARD
```

The key here is **`BOARD_SCREEN_COMBO=512`** which selects the correct **JD79667** controller path for this panel.

---

## Wiring and Pin Mapping

This project uses the **Seeed XIAO ePaper driver board** pin map:

- `RST = D0`
- `CS = D1`
- `BUSY = D2`
- `DC = D3`
- `SCK = D8`
- `MOSI = D10`

The Seeed_GFX combo `512` + `USE_XIAO_EPAPER_DRIVER_BOARD` matches this exactly.

---

## On‑Device UI Features

- 5‑day forecast layout
- Bold max temperature
- Compact legend (L = low, P = precipitation)
- Weather icons (40×40 sprites, 4bpp palette)
- Auto refresh every 15 minutes

---

## Web UI Features

Open the control page at `http://<device-ip>/`:

- Wi‑Fi setup
- Rescan networks
- Manual SSID
- Static IP configuration
- Language selector (EN/DE/ES/FR)
- Logs and status

## Language Selection

The **same language setting** controls both the **web UI** and the **e‑paper display** (labels, weekdays, and status text).

How to change it:

1. Open the web UI at `http://<device-ip>/`
2. Use the **Language** dropdown in the header
3. The page reloads and the e‑paper display updates immediately

Available languages: **English, German, Spanish, French**.

---

## Static IP Setup

In the web UI, enable **Static IP** and enter:

- IP address
- Gateway
- Subnet
- DNS 1 / DNS 2

The configuration is stored in NVS and applied on each connection.

---

## Weather Icons (4bpp)

Icons are stored as 4‑bit indexed sprites and rendered with:

```cpp
epaper.fillRect(x, y, 40, 40, TFT_WHITE);
epaper.pushImage(x, y, 40, 40,
                 const_cast<uint16_t*>(reinterpret_cast<const uint16_t*>(kWeatherIcon40Clear)),
                 4);
```

Palette indices used by this project:

- `white = 0x00`
- `black = 0x0F`
- `yellow = 0x0B`
- `red = 0x06`

If red/yellow look swapped, fix the **indices** in the sprite header.

---

## Degree Symbol Fix

Some free fonts don’t include the `°` glyph. This project draws it manually:

```cpp
void drawDegreeSymbol(int16_t x, int16_t y, uint8_t radius, uint16_t color) {
  epaper.drawCircle(x, y, radius, color);
}
```

---

## Project Structure

```
.
├── src/                     # main app
├── include/                 # icon headers, config
├── lib/Seeed_GFX/           # graphics + driver stack
├── docs/                    # detailed guides
├── assets/                  # source images for icons
├── platformio.ini
└── partitions_singleapp.csv
```

---

## Deep‑Dive Docs

- `docs/EPAPER_2IN9_BWRY_XIAO_GUIDE.md`
- `docs/WEATHER_FORECAST_APP.md`

---

## Common Pitfalls (Already Solved Here)

- Wrong controller family (JD79661 vs JD79667)
- Incorrect XIAO pin mapping
- Expecting drawing calls without `epaper.update()`
- Using partial refresh without validation

---

## License

This project is licensed under the **Apache License 2.0**. See `LICENSE`.
