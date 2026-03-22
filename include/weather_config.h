#pragma once

namespace weather_config
{
// Fill in your local Wi-Fi credentials before flashing.
inline constexpr char kWifiSsid[] = "MBUmain";
inline constexpr char kWifiPassword[] = "84351504002357674640";

// Fixed Berlin defaults for this hardware profile.
inline constexpr float kDefaultLatitude = 52.5200f;
inline constexpr float kDefaultLongitude = 13.4050f;
inline constexpr char kDefaultTimezone[] = "Europe/Berlin";
inline constexpr char kLocationLabel[] = "Berlin";

// Wi-Fi geolocation is possible, but it requires a third-party AP database API.
// Keep the display on fixed coordinates unless you intentionally integrate one.
inline constexpr bool kUseWifiGeolocation = false;
}  // namespace weather_config
