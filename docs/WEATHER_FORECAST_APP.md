# 5-Day Weather Forecast App

This project now includes a working weather forecast application for the Seeed Studio XIAO ESP32-C6 with the Seeed XIAO ePaper Driver Board V2 and the 2.9" BWRY panel.

## Verified Hardware and Driver Path

- MCU: Seeed Studio XIAO ESP32-C6
- Driver board: Seeed XIAO ePaper Driver Board V2
- Panel: 2.9" 128x296 BWRY
- Seeed_GFX combo: `512`
- Controller path selected by Seeed_GFX: `JD79667_DRIVER`

## Open-Meteo API

The application requests 5 daily forecast entries from Open-Meteo with:

- `weather_code`
- `temperature_2m_max`
- `temperature_2m_min`
- `precipitation_probability_max`
- `wind_speed_10m_max`

Reference:
- https://open-meteo.com/en/docs

## Configuration

Edit [weather_config.h](/Users/jessegreene/Documents/PlatformIO/Projects/Xiao_epaperColor/include/weather_config.h):

```cpp
inline constexpr char kWifiSsid[] = "YOUR_WIFI";
inline constexpr char kWifiPassword[] = "YOUR_PASSWORD";

inline constexpr float kDefaultLatitude = 52.5200f;
inline constexpr float kDefaultLongitude = 13.4050f;
inline constexpr char kDefaultTimezone[] = "Europe/Berlin";
inline constexpr char kLocationLabel[] = "Berlin";
```

## Current Behavior

On boot (and on every deep-sleep wake) the firmware:

1. Connects to Wi-Fi
2. Requests a 5-day Open-Meteo forecast for the configured coordinates
3. Parses the daily arrays
4. Rotates the panel into landscape mode
5. Draws a 5-column forecast layout, with a battery symbol in the header
6. Updates the e-paper panel once
7. Stays awake ~2 minutes so the web UI is reachable (extended by any HTTP request)
8. Powers down Wi-Fi, puts the panel to sleep, and deep sleeps until the next 30-minute mark

A failed fetch shortens the cycle to 5 minutes. AP/captive-portal mode never sleeps, and the
web UI has a toggle (persisted in NVS) to disable deep sleep entirely.

A cold boot (power-on or reset) holds the device awake for 5 minutes instead of 2, so the web UI
is reachable right after you switch the device on.

## Battery Sensing

Battery state comes from an Adafruit LC709203F I2C fuel gauge on D4/D5 - the driver board never
routes `BAT_4V2` to the XIAO and the ESP32-C6 has no free ADC pin. Wiring and bring-up steps are in
[wire-diagram.md](/Users/jessegreene/Documents/PlatformIO/Projects/Xiao_epaperColor/docs/wire-diagram.md);
the `kBattery*` constants live in
[weather_config.h](/Users/jessegreene/Documents/PlatformIO/Projects/Xiao_epaperColor/include/weather_config.h).

`setupBatteryGauge()` configures the gauge once per wake cycle and `readBattery()` is the single
read seam - it returns "no sensor" when the gauge is absent or the reading is out of range, which
is what the display and web UI render until the part is fitted.

## Wi-Fi Geolocation

Wi-Fi geolocation is possible in principle, but it is not implemented as an active provider in this project.

Reason:
- ESP32 can scan nearby access points
- latitude/longitude still require a third-party geolocation database API
- those providers typically require an API key and upload visible BSSIDs

The current code therefore uses fixed coordinates. This keeps the weather app deterministic and avoids a hidden dependency on an external location vendor.
