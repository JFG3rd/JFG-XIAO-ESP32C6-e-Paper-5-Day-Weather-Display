# 2.9" BWRY E-Paper on XIAO ESP32-C6

This document captures the verified working setup for the following hardware and software combination:

- MCU board: Seeed Studio XIAO ESP32-C6
- E-paper board: Seeed Studio XIAO ePaper driver board / XIAO ePaper expansion board V2 pinout
- Panel: 2.9" BWRY e-paper, 128x296
- Library stack: Seeed_GFX
- Verified combo selection: `BOARD_SCREEN_COMBO 512`
- Verified controller path: `JD79667_DRIVER`

This guide exists because the panel was initially driven with the wrong controller family. The custom `JD79661` path compiled and ran, but the panel showed no physical activity. The correct Seeed_GFX setup for this panel is combo `512`, which maps to `JD79667_DRIVER`.

## Root Cause Summary

The original failure was not in PlatformIO, the ESP32-C6 board definition, or the SPI wiring once the XIAO variant was corrected.

The root cause was a display-driver mismatch:

- The custom code assumed a `JD79661`-style init/update contract.
- The official Seeed_GFX source maps the 2.9" BWRY panel to `JD79667_DRIVER`.
- With the wrong controller path, the firmware ran and logged progress, but the panel never entered a valid update cycle.

Verified source locations in a typical project:

- `platformio.ini`
- `include/driver.h`
- `src/Xiao_epaperColor.cpp`
- `lib/Seeed_GFX/User_Setups/Setup512_Seeed_XIAO_EPaper_2inch9_BWRY.h`
- `lib/Seeed_GFX/User_Setups/Dynamic_Setup.h`
- `lib/Seeed_GFX/User_Setups/EPaper_Board_Pins_Setups.h`

## Verified PlatformIO Setup

Use the pioarduino Espressif platform and the real XIAO ESP32-C6 board definition:

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

Current project file:

- `platformio.ini`

### Why these defines matter

- `BOARD_SCREEN_COMBO=512` selects `Setup512_Seeed_XIAO_EPaper_2inch9_BWRY.h`
- That setup enables `JD79667_DRIVER`
- `USE_XIAO_EPAPER_DRIVER_BOARD` selects the pin map that matches the working hardware in this project

## `driver.h`

The project also carries a local driver-selection header:

```cpp
#pragma once

#define BOARD_SCREEN_COMBO 512
#define USE_XIAO_EPAPER_DRIVER_BOARD
```

Current file:

- `include/driver.h`

The build flags already define these macros globally. Keeping them in `driver.h` makes the setup self-describing for sketches and for the library's `User_Setup_Select.h` logic.

## Board and Pin Mapping

The working logical pin assignment is the Seeed XIAO e-paper mapping:

- `RST = D0`
- `CS = D1`
- `BUSY = D2`
- `DC = D3`
- `SCK = D8`
- `MOSI = D10`

On XIAO ESP32-C6, these resolve to:

- `D0 -> GPIO0`
- `D1 -> GPIO1`
- `D2 -> GPIO2`
- `D3 -> GPIO21`
- `D8 -> GPIO19`
- `D10 -> GPIO18`

Why this matters:

- Earlier code used stale raw GPIO numbers from a temporary custom variant workaround.
- The official XIAO C6 variant maps `D3`, `D8`, and `D10` differently than those old raw values.
- Using symbolic `D` pins or the official Seeed setup avoids silent pin-map drift.

## Important Board-Macro Detail

In the official Seeed_GFX source:

- `USE_XIAO_EPAPER_DRIVER_BOARD` uses `BUSY = D2`
- `USE_XIAO_EPAPER_BREAKOUT_BOARD` uses `BUSY = D5`

For this working setup, `USE_XIAO_EPAPER_DRIVER_BOARD` is the correct macro because it matches the actual working pin map for this hardware path.

Reference:

- `lib/Seeed_GFX/User_Setups/EPaper_Board_Pins_Setups.h`

## Library Setup

The project vendors Seeed_GFX locally:

- [lib/Seeed_GFX](/Users/jessegreene/Documents/PlatformIO/Projects/Xiao_epaperColor/lib/Seeed_GFX)

This avoids relying on manual edits inside the global PlatformIO package directory.

The library resolves the panel like this:

1. `User_Setup_Select.h` includes `driver.h`
2. `Dynamic_Setup.h` maps `BOARD_SCREEN_COMBO 512`
3. `Setup512_Seeed_XIAO_EPaper_2inch9_BWRY.h` enables:
   - `JD79667_DRIVER`
   - `EPAPER_ENABLE`
   - e-paper geometry `128x296`

## Verified Application Pattern

The current working sketch uses the official `EPaper` API:

- `src/Xiao_epaperColor.cpp`

Core usage pattern:

```cpp
#include <Arduino.h>
#include <TFT_eSPI.h>

#ifdef EPAPER_ENABLE
EPaper epaper;
#endif

void setup() {
  Serial.begin(115200);
  delay(2000);

#ifdef EPAPER_ENABLE
  epaper.begin();
  epaper.fillScreen(TFT_WHITE);
  epaper.drawString("Hello ePaper", 10, 10);
  epaper.update();
  epaper.sleep();
#endif
}

void loop() {}
```

### Update model

For this library and panel:

1. Draw into the e-paper buffer with normal drawing APIs
2. Call `epaper.update()` to transfer and refresh the panel
3. Call `epaper.sleep()` when finished if the display will remain static

This is the most important usage rule. Drawing calls alone do not change the physical panel.

## Supported Color Model

For this 2.9" panel, the practical display colors are:

- `TFT_WHITE`
- `TFT_BLACK`
- `TFT_YELLOW`
- `TFT_RED`

These are the colors used in the verified working sketch and in the Seeed colorful e-paper examples.

## 4bpp Icon Workflow (Sprites)

For compact UI graphics, this project uses 40x40 icons stored as **4bpp indexed color** sprites and drawn with `pushImage(..., 4)`. This keeps assets small and fast to render while still using the full BWRY palette.

Key points:

- **Palette is not RGB**. The 4-bit indices map to Seeed_GFX’s e-paper palette.
- For the JD79667 BWRY combo, the mapping used in this project is:
  - `white = 0x00`
  - `black = 0x0F`
  - `yellow = 0x0B`
  - `red = 0x06`
- Always clear the icon background to **white** before pushing the icon, otherwise old pixels stay visible.

Minimal draw pattern used in this project:

```cpp
// Draw a 40x40 icon from a 4bpp sprite.
epaper.fillRect(x, y, 40, 40, TFT_WHITE);
epaper.pushImage(
    x,
    y,
    40,
    40,
    const_cast<uint16_t*>(reinterpret_cast<const uint16_t*>(kWeatherIcon40Clear)),
    4);
```

If colors look swapped on the panel (red/yellow mixed), correct the **palette indices** in the generated asset header. The display itself is fine; the issue is almost always the index mapping.

## Font Notes (Bold Text)

This project uses `FreeSansBold9pt7b` and `FreeSansBold12pt7b` via `setFreeFont()`.

Important:

- **Do not include** the `FreeSansBold*.h` headers directly when using Seeed_GFX.  
  The fonts are already included by `gfxfont.h` and duplicate includes cause compile errors.
- Enable `LOAD_GFXFF` in the Seeed_GFX setup if you use free fonts.

Example:

```cpp
epaper.setTextColor(TFT_RED, TFT_WHITE);
epaper.setFreeFont(&FreeSansBold12pt7b);
epaper.setTextDatum(MC_DATUM);
epaper.drawString("14", x, y);
epaper.setFreeFont(nullptr);
```

## Degree Symbol (°) Workaround

Some free fonts do **not** include the `°` glyph. If you see missing or corrupted degree symbols, draw it manually as a small circle.

Example (used in this project):

```cpp
// Draw a small degree symbol next to a temperature value.
void drawDegreeSymbol(int16_t x, int16_t y, uint8_t radius, uint16_t color) {
  epaper.drawCircle(x, y, radius, color);
}
```

Then append it after the numeric value instead of relying on `"°"`:

```cpp
epaper.drawString("14", x, y);
drawDegreeSymbol(x + 18, y + 2, 2, TFT_BLACK);
```

## Minimal Examples

### 1. Solid full-screen colors

```cpp
epaper.fillScreen(TFT_WHITE);
epaper.update();

epaper.fillScreen(TFT_BLACK);
epaper.update();

epaper.fillScreen(TFT_YELLOW);
epaper.update();

epaper.fillScreen(TFT_RED);
epaper.update();
```

### 2. Mixed-color blocks

```cpp
epaper.fillScreen(TFT_WHITE);
epaper.fillRect(0, 0, 64, 148, TFT_BLACK);
epaper.fillRect(64, 0, 64, 148, TFT_WHITE);
epaper.fillRect(0, 148, 64, 148, TFT_YELLOW);
epaper.fillRect(64, 148, 64, 148, TFT_RED);
epaper.update();
```

### 3. Color bars

```cpp
epaper.fillScreen(TFT_WHITE);
epaper.fillRect(0, 0, 32, 296, TFT_BLACK);
epaper.fillRect(32, 0, 32, 296, TFT_WHITE);
epaper.fillRect(64, 0, 32, 296, TFT_YELLOW);
epaper.fillRect(96, 0, 32, 296, TFT_RED);
epaper.update();
```

### 4. Lines in mixed colors

```cpp
epaper.fillScreen(TFT_WHITE);
for (int y = 0; y < 296; y += 24) {
  epaper.drawLine(0, y, 127, y, TFT_BLACK);
  if (y + 8 < 296)  epaper.drawLine(0, y + 8, 127, y + 8, TFT_YELLOW);
  if (y + 16 < 296) epaper.drawLine(0, y + 16, 127, y + 16, TFT_RED);
}
epaper.update();
```

## Text Rendering

### 1. Simple built-in text

```cpp
epaper.fillScreen(TFT_WHITE);
epaper.setTextColor(TFT_BLACK);
epaper.setTextSize(1);
epaper.drawString("Hello ePaper", 6, 10);
epaper.drawString("Black text", 6, 30);
epaper.update();
```

### 2. Same text in different colors

```cpp
epaper.fillScreen(TFT_WHITE);

epaper.setTextColor(TFT_BLACK);
epaper.drawString("BLACK", 6, 10);

epaper.setTextColor(TFT_RED);
epaper.drawString("RED", 6, 30);

epaper.setTextColor(TFT_YELLOW);
epaper.drawString("YELLOW", 6, 50);

epaper.update();
```

### 3. Text on colored panels

```cpp
epaper.fillScreen(TFT_YELLOW);
epaper.setTextColor(TFT_BLACK);
epaper.drawString("Alert", 10, 20);
epaper.update();
```

### 4. Larger text

```cpp
epaper.fillScreen(TFT_WHITE);
epaper.setTextColor(TFT_RED);
epaper.setTextSize(2);
epaper.drawString("Status", 10, 20);
epaper.update();
```

### 5. Mixed text colors in one layout

```cpp
epaper.fillScreen(TFT_WHITE);
epaper.setTextSize(1);

epaper.setTextColor(TFT_BLACK);
epaper.drawString("Device:", 6, 10);

epaper.setTextColor(TFT_RED);
epaper.drawString("ALARM", 58, 10);

epaper.setTextColor(TFT_YELLOW);
epaper.drawString("Battery low", 6, 30);

epaper.update();
```

## Icons and Small Graphics

For simple icons, use primitives first. This keeps assets small and avoids conversion steps.

### 1. Status icon using primitives

```cpp
epaper.fillScreen(TFT_WHITE);

epaper.fillCircle(20, 20, 10, TFT_RED);
epaper.drawRect(40, 10, 20, 20, TFT_BLACK);
epaper.fillTriangle(80, 30, 100, 30, 90, 10, TFT_YELLOW);

epaper.update();
```

### 2. Mixed-color tiled icon strip

```cpp
epaper.fillScreen(TFT_WHITE);

for (int x = 0; x < 128; x += 16) {
  uint16_t color = TFT_BLACK;
  if ((x / 16) % 4 == 1) color = TFT_WHITE;
  if ((x / 16) % 4 == 2) color = TFT_YELLOW;
  if ((x / 16) % 4 == 3) color = TFT_RED;
  epaper.fillRect(x, 0, 16, 16, color);
}

epaper.update();
```

## Displaying Full Images

The official Seeed examples for this panel use:

```cpp
epaper.pushImage(0, 0, 128, 296, (uint16_t *)gImage_2inch9_BWRY);
epaper.update();
```

Reference example:

- `lib/Seeed_GFX/examples/ePaper/Colorful/Bitmap/Bitmap_02inch90_BWRY/Bitmap_02inch90_BWRY.ino`
- `lib/Seeed_GFX/examples/ePaper/Colorful/Bitmap/Bitmap_02inch90_BWRY/image.h`

### Example

```cpp
#include <TFT_eSPI.h>
#include "image.h"

EPaper epaper;

void setup() {
  epaper.begin();
  epaper.fillScreen(TFT_WHITE);
  epaper.update();
  delay(1000);

  epaper.pushImage(0, 0, 128, 296, (uint16_t *)gImage_2inch9_BWRY);
  epaper.update();
  epaper.sleep();
}
```

### Notes on image assets

- The Seeed example asset for this panel is stored as a color image array and pushed with `pushImage()`
- The library maps supported colors to the panel’s BWRY palette
- For full-screen art, using the library’s own example format is the safest starting point

## Creating Custom Image Assets

The library includes a conversion tool:

- `lib/Seeed_GFX/Tools/bmp2array4bit/bmp2array4bit.py`

Recommended workflow:

1. Prepare artwork at exactly `128x296`
2. Restrict the palette to:
   - white
   - black
   - yellow
   - red
3. Convert the image to an array format compatible with your selected rendering path
4. Store the generated array in a project header, for example `include/image.h`
5. Render with `epaper.pushImage()`

For first implementations, copy the style used by the official example in:

- `lib/Seeed_GFX/examples/ePaper/Colorful/Bitmap/Bitmap_02inch90_BWRY`

## Application Patterns

### 1. Dashboard / status screen

Use a white background and reserve:

- black for labels and outlines
- red for alarms / critical state
- yellow for warnings / low priority attention

Example:

```cpp
epaper.fillScreen(TFT_WHITE);

epaper.setTextColor(TFT_BLACK);
epaper.drawString("Doorbell", 6, 10);

epaper.fillRect(6, 30, 116, 24, TFT_YELLOW);
epaper.setTextColor(TFT_BLACK);
epaper.drawString("Motion detected", 10, 36);

epaper.fillCircle(110, 100, 12, TFT_RED);

epaper.update();
```

### 2. Badge / sign

```cpp
epaper.fillScreen(TFT_RED);
epaper.setTextColor(TFT_WHITE);
epaper.setTextSize(2);
epaper.drawString("STOP", 28, 120);
epaper.update();
```

### 3. Mixed icon + text card

```cpp
epaper.fillScreen(TFT_WHITE);
epaper.fillCircle(18, 18, 10, TFT_YELLOW);
epaper.setTextColor(TFT_BLACK);
epaper.drawString("Weather", 36, 10);
epaper.drawString("Clear, 21C", 36, 30);
epaper.update();
```

## Refresh Strategy

### Use this pattern unless you have separately validated something else

- draw complete frame
- call `epaper.update()`
- sleep if the screen is static

This is the verified full-refresh path for this hardware combo.

### Partial refresh

The library contains partial-refresh examples for some e-paper workflows, but partial behavior for this exact 2.9" BWRY panel has not been validated in this project.

Do not assume partial refresh is safe or artifact-free on this panel until you validate it separately.

Reference example location:

- `lib/Seeed_GFX/examples/ePaper/Partial/HelloWorld`

## Things That Are Now Known To Be Correct

- PlatformIO environment selection
- XIAO ESP32-C6 board definition
- Arduino framework path
- Seeed_GFX combo selection
- controller family selection
- working pin map
- full-screen color fills
- mixed-color geometry
- text rendering
- official image rendering path via `pushImage()`

## Things To Avoid

Avoid these failure modes in future projects:

- Do not assume the controller family from panel size alone
- Do not carry raw GPIO numbers forward from temporary board-variant hacks
- Do not treat a successful serial log as proof the panel accepted the command stream
- Do not invent a custom e-paper init sequence if the vendor already publishes a combo/setup mapping

## Future Project Checklist

For any new project using this exact hardware:

1. Copy `platformio.ini`
2. Copy `include/driver.h`
3. Reuse the vendored `lib/Seeed_GFX` or install the same version intentionally
4. Start from `src/Xiao_epaperColor.cpp`
5. Use `epaper.begin()`, drawing calls, `epaper.update()`, then `epaper.sleep()`
6. Keep artwork and UI colors inside the BWRY palette
7. Prefer full-frame redraws until a partial-update workflow is separately validated

## Official References

- Seeed wiki:
  https://wiki.seeedstudio.com/xiao_eink_expansion_board_v2/
- Seeed_GFX repository:
  https://github.com/Seeed-Studio/Seeed_GFX
