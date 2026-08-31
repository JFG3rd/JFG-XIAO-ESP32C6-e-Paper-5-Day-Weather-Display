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
// 500 is the nearest profile for the 400 mAh cell in use.
inline constexpr uint16_t kBatteryPackMah = 500;

// --- Modeled charge estimate (used when no gauge is fitted) ------------------
// With nothing to measure, the firmware still knows exactly how long it spent
// awake and asleep, so it integrates that against these assumed currents to
// estimate remaining charge. The result is always shown with a "?" to mark it
// as a guess rather than a measurement.
//
// Actual pack capacity in mAh. Unlike kBatteryPackMah this is not restricted to
// the gauge's profile steps, so set it to the true value.
inline constexpr uint16_t kBatteryCapacityMah = 400;
// Average draw while awake: MCU + Wi-Fi associated, plus the panel refresh.
inline constexpr uint16_t kEstimatedActiveMa = 90;
// Whole-board draw in deep sleep. This is the least certain number here - the
// driver board's boost converter, not the MCU, dominates it. Measure once with a
// multimeter in series with the pack and correct this if the estimate drifts.
inline constexpr uint16_t kEstimatedSleepUa = 200;

// Thermistor B-constant. 0 = no thermistor on the pack, so the gauge is told to
// use its I2C temperature register instead. Adafruit's 10K NTC is B = 3950.
inline constexpr uint16_t kBatteryThermistorB = 0;

// Which discharge curve the gauge maps voltage onto.
//   0 = 3.7 V nominal / 4.2 V charged  <- ordinary LiPo, including Adafruit's
//   1 = 3.8 V nominal / 4.35 V charged <- high-voltage cells only
// This must be set explicitly. Adafruit_LC709203F::begin() writes 1 under a
// comment claiming it is the 4.2 V profile, so a standard cell left on the
// library default is read against a curve for a pack that charges 150 mV higher:
// it never reaches 100% and reads low across the whole range.
inline constexpr uint16_t kBatteryProfile = 0;

// Warn threshold (percent) below which the header glyph turns red.
inline constexpr uint8_t kBatteryLowPercent = 20;

// --- Derived weather warnings ------------------------------------------------
// Thresholds for the device's own assessment of today's forecast, used when the
// warning source is set to "derived". These are our judgement, not official
// meteorological warnings - the "dwd" source exists for those.
//
// All are evaluated against data the daily forecast request already returns, so
// a derived warning costs no extra HTTP request and no extra awake time.
inline constexpr int kWarnWindKmh = 60;
inline constexpr int kWarnPrecipitationPercent = 80;
inline constexpr int kWarnFrostCelsius = 0;
inline constexpr int kWarnHeatCelsius = 32;

// Official DWD alerts, served by Bright Sky. Plain HTTP works (verified: returns
// 200 with no redirect to HTTPS), so this costs no TLS heap. Germany only - the
// alerts array simply comes back empty elsewhere.
inline constexpr char kAlertsHost[] = "http://api.brightsky.dev/alerts";
}  // namespace weather_config
