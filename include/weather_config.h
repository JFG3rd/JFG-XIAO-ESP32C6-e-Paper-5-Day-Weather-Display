#pragma once

// Arduino.h provides the D4/D5 pin aliases used by the battery gauge config.
#include <Arduino.h>

namespace weather_config
{
// Compile-time Wi-Fi defaults. Leave empty to force AP-mode onboarding.
// If you do set these, keep them out of version control (e.g. use a
// gitignored local_config.h that overrides these values).
inline constexpr char kWifiSsid[] = "";
inline constexpr char kWifiPassword[] = "";

// Fixed Berlin defaults for this hardware profile.
inline constexpr float kDefaultLatitude = 52.5200f;
inline constexpr float kDefaultLongitude = 13.4050f;
inline constexpr char kDefaultTimezone[] = "Europe/Berlin";
inline constexpr char kLocationLabel[] = "Berlin";

// Wi-Fi geolocation is possible, but it requires a third-party AP database API.
// Keep the display on fixed coordinates unless you intentionally integrate one.
inline constexpr bool kUseWifiGeolocation = false;

// --- Battery gauge (Adafruit LC709203F) --------------------------------------
// The ePaper driver board never routes BAT_4V2 to the XIAO and the ESP32-C6 has
// no free ADC pin (D0/D1/D2 are the panel's RST/CS/BUSY), so battery state comes
// from an I2C fuel gauge on the free D4/D5 pins instead. See docs/wire-diagram.md
// for the wiring. Set kBatteryGaugeEnabled to false to build without it.
inline constexpr bool kBatteryGaugeEnabled = true;

// I2C pins. D4/D5 are the XIAO's default SDA/SCL and are otherwise unused here.
inline constexpr int8_t kBatterySdaPin = D4;
inline constexpr int8_t kBatterySclPin = D5;

// Nominal pack capacity in mAh, used to pick the gauge's APA profile. Must be
// one of 100, 200, 500, 1000, 2000, 3000 - the values the LC709203F supports.
// The cell shipped with the driver board is 1000 mAh.
inline constexpr uint16_t kBatteryPackMah = 1000;

// Thermistor B-constant. 0 = no thermistor on the pack, so the gauge is told to
// use its I2C temperature register instead. Adafruit's 10K NTC is B = 3950.
inline constexpr uint16_t kBatteryThermistorB = 0;

// Warn threshold (percent) below which the header glyph turns red.
inline constexpr uint8_t kBatteryLowPercent = 20;
}  // namespace weather_config
