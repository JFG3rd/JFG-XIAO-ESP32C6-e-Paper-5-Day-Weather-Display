#pragma once

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
}  // namespace weather_config
