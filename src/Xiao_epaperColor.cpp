#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <FS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

// Bold free fonts are provided by Seeed_GFX (LOAD_GFXFF in the user setup).

#include "weather_config.h"
// 4bpp icon sprites used by the e-paper renderer (indexed color values).
#include "weather_icons_40x40.h"

#ifdef EPAPER_ENABLE
EPaper epaper;
#endif

namespace
{
constexpr uint8_t kForecastDays = 5;
constexpr uint8_t kMaxLogEntries = 24;
constexpr uint8_t kMaxScanEntries = 16;
constexpr uint16_t kHeaderHeight = 22;
// Tighten the gap so each day card is slightly wider.
constexpr uint16_t kCellGap = 0;
constexpr uint32_t kWifiTimeoutMs = 20000;
constexpr uint32_t kWifiAttemptDelayMs = 1200;
constexpr uint32_t kReconnectCheckIntervalMs = 10000;
constexpr uint32_t kScanCacheTtlMs = 30000;
constexpr uint32_t kHttpTimeoutMs = 15000;
constexpr uint32_t kNtpTimeoutMs = 10000;
// Refresh forecast every 15 minutes to keep data current without aggressive polling.
constexpr uint32_t kRefreshIntervalMs = 15UL * 60UL * 1000UL;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr uint8_t kMaxReconnectFailuresBeforeAp = 3;
constexpr byte kDnsPort = 53;
constexpr const char* kPrefsNamespace = "wifi";
constexpr const char* kApSsid = "XIAO-Weather-Setup";
constexpr const char* kStaHostname = "xiao-weather";

Preferences preferences;
DNSServer dnsServer;
WebServer server(80);

bool apModeActive = false;
bool serverStarted = false;
bool routesConfigured = false;
bool forecastValid = false;
bool wifiEventsRegistered = false;
bool staGotIp = false;
bool hasStoredCredentials = false;
bool usingCompiledDefaults = false;
uint32_t lastRefreshMs = 0;
uint32_t lastReconnectAttemptMs = 0;
uint8_t reconnectFailures = 0;
uint8_t lastDisconnectReason = 0;
uint32_t lastScanCacheMs = 0;
bool scanInProgress = false;

String currentSsid;
String currentPassword;
String logBuffer[kMaxLogEntries];
uint8_t logWriteIndex = 0;

struct ScanCacheEntry
{
  // Raw SSID as reported by the scan; used for display.
  String ssid;
  // Normalized SSID key used for deduplication across multiple BSSIDs.
  String ssidKey;
  int32_t rssi = -127;
  int32_t channel = 0;
  uint8_t bssid[6] = {0};
  bool inUse = false;
};

ScanCacheEntry scanCache[kMaxScanEntries];
uint8_t scanCacheCount = 0;

struct Coordinates
{
  float latitude;
  float longitude;
  const char* label;
  const char* timezone;
};

struct ForecastDay
{
  char isoDate[11];
  char weekday[4];
  int weatherCode;
  int tempMax;
  int tempMin;
  int precipitationProbability;
  int windSpeed;
};

struct ForecastData
{
  char location[32];
  char updatedAt[24];
  char updatedDay[12];
  ForecastDay days[kForecastDays];
};

ForecastData currentForecast = {};

struct StaticIpConfig
{
  bool enabled = false;
  IPAddress ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns1;
  IPAddress dns2;
};

StaticIpConfig staticIpConfig = {};

enum class WeatherVisual
{
  Clear,
  MostlyClear,
  PartlyCloudy,
  Cloudy,
  Fog,
  LightRain,
  HeavyRain,
  Showers,
  Thunderstorm,
  Drizzle,
  Snow,
  MixedRainSnow,
  Sleet,
  FreezingRain,
  Hail,
  Wind,
  WindRain
};

enum class UiLanguage : uint8_t
{
  English = 0,
  German,
  Spanish,
  French
};

struct UiText
{
  const char* code;
  const char* name;
  const char* title;
  const char* statusTitle;
  const char* wifiSetupTitle;
  const char* logsTitle;
  const char* ssidLabel;
  const char* hiddenSsidLabel;
  const char* passwordLabel;
  const char* saveReboot;
  const char* rescan;
  const char* refreshForecast;
  const char* forgetWifi;
  const char* loadingState;
  const char* loadingLogs;
  const char* summaryAp;
  const char* summarySta;
  const char* summaryConnected;
  const char* summaryNotConnected;
  const char* summaryNoForecast;
  const char* statusSsid;
  const char* statusIp;
  const char* statusApIp;
  const char* statusWifiState;
  const char* statusReason;
  const char* statusForecast;
  const char* statusUpdated;
  const char* forecastValid;
  const char* forecastNotLoaded;
  const char* labelUpdated;
  const char* labelWifi;
  const char* setupTitle;
  const char* setupIntro;
  const char* visibleNetworks;
  const char* manualSsid;
  const char* saveAndReboot;
  const char* reloadCaptive;
  const char* openFull;
  const char* accessPoint;
  const char* deviceIp;
  const char* languageLabel;
  const char* staticIpTitle;
  const char* staticIpEnable;
  const char* staticIpAddress;
  const char* staticIpGateway;
  const char* staticIpSubnet;
  const char* staticIpDns1;
  const char* staticIpDns2;
  const char* weekday[7];
  const char* weatherLabel[17];
};

const UiText kUiText[] = {
    {
        "en",
        "English",
        "XIAO Weather Control",
        "Status",
        "Wi-Fi Setup",
        "Logs",
        "SSID",
        "Hidden SSID",
        "Password",
        "Save & Reboot",
        "Rescan",
        "Refresh Forecast",
        "Forget Wi-Fi",
        "Loading device state...",
        "Loading logs...",
        "AP mode",
        "Station mode",
        "Connected",
        "Not connected",
        "No forecast",
        "SSID",
        "IP",
        "AP IP",
        "Wi-Fi State",
        "Reason",
        "Forecast",
        "Updated",
        "Valid",
        "Not loaded",
        "Updated",
        "WiFi",
        "XIAO Weather Setup",
        "Connect this display to your home Wi-Fi. This page is optimized for iPhone, iPad, and Mac captive portal setup.",
        "Visible networks",
        "Hidden or manual SSID",
        "Save and Reboot",
        "Reload captive setup page",
        "Open full control page",
        "Access point",
        "Device IP",
        "Language",
        "Static IP",
        "Enable static IP",
        "IP address",
        "Gateway",
        "Subnet",
        "DNS 1",
        "DNS 2",
        {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"},
        {"SUN", "CLEAR", "PARTLY", "CLOUD", "FOG", "RAIN", "HEAVY", "SHOWERS", "STORM", "DRIZZLE", "SNOW", "MIXED", "SLEET",
         "ICE", "HAIL", "WIND", "WIND+R"},
    },
    {
        "de",
        "Deutsch",
        "XIAO Wetter",
        "Status",
        "WLAN Setup",
        "Logs",
        "SSID",
        "Versteckte SSID",
        "Passwort",
        "Speichern & Neustart",
        "Neu scannen",
        "Vorhersage aktualisieren",
        "WLAN loeschen",
        "Status wird geladen...",
        "Logs werden geladen...",
        "AP Modus",
        "Station Modus",
        "Verbunden",
        "Nicht verbunden",
        "Keine Vorhersage",
        "SSID",
        "IP",
        "AP IP",
        "WLAN Status",
        "Grund",
        "Vorhersage",
        "Aktualisiert",
        "Gueltig",
        "Nicht geladen",
        "Aktualisiert",
        "WLAN",
        "XIAO Wetter Setup",
        "Verbinde das Display mit deinem WLAN. Diese Seite ist fuer Apple Captive Portal optimiert.",
        "Sichtbare Netzwerke",
        "Versteckte oder manuelle SSID",
        "Speichern und Neustart",
        "Captive Seite neu laden",
        "Vollseite oeffnen",
        "Access Point",
        "Geraet IP",
        "Sprache",
        "Statische IP",
        "Statische IP aktivieren",
        "IP-Adresse",
        "Gateway",
        "Subnetz",
        "DNS 1",
        "DNS 2",
        {"SO", "MO", "DI", "MI", "DO", "FR", "SA"},
        {"SONNE", "KLAR", "TEIL", "WOLKIG", "NEBEL", "REGEN", "STARK", "SCHAU", "GEWITER", "NIESEL", "SCHNEE", "MIX",
         "HAGEL", "EIS", "HAGEL", "WIND", "W+R"},
    },
    {
        "es",
        "Espanol",
        "Control del Tiempo",
        "Estado",
        "Configuracion Wi-Fi",
        "Registros",
        "SSID",
        "SSID oculta",
        "Contrasena",
        "Guardar y reiniciar",
        "Reescanear",
        "Actualizar pronostico",
        "Olvidar Wi-Fi",
        "Cargando estado...",
        "Cargando registros...",
        "Modo AP",
        "Modo estacion",
        "Conectado",
        "No conectado",
        "Sin pronostico",
        "SSID",
        "IP",
        "IP AP",
        "Estado Wi-Fi",
        "Motivo",
        "Pronostico",
        "Actualizado",
        "Valido",
        "No cargado",
        "Actualizado",
        "WiFi",
        "XIAO Weather Setup",
        "Conecta la pantalla a tu Wi-Fi. Esta pagina esta optimizada para Apple captive portal.",
        "Redes visibles",
        "SSID oculta o manual",
        "Guardar y reiniciar",
        "Recargar pagina cautiva",
        "Abrir pagina completa",
        "Punto de acceso",
        "IP del dispositivo",
        "Idioma",
        "IP estatica",
        "Habilitar IP estatica",
        "Direccion IP",
        "Gateway",
        "Subred",
        "DNS 1",
        "DNS 2",
        {"DOM", "LUN", "MAR", "MIE", "JUE", "VIE", "SAB"},
        {"SOL", "CLARO", "PARC", "NUBE", "NIEB", "LLUV", "FUERTE", "CHUB", "TORM", "LLOV", "NIEVE", "MIX",
         "SLEET", "HIELO", "GRAN", "VIEN", "V+L"},
    },
    {
        "fr",
        "Francais",
        "Controle Meteo",
        "Statut",
        "Configuration Wi-Fi",
        "Journaux",
        "SSID",
        "SSID masque",
        "Mot de passe",
        "Enregistrer et redemarrer",
        "Re-scanner",
        "Actualiser la prevision",
        "Oublier le Wi-Fi",
        "Chargement de l'etat...",
        "Chargement des journaux...",
        "Mode AP",
        "Mode station",
        "Connecte",
        "Non connecte",
        "Aucune prevision",
        "SSID",
        "IP",
        "IP AP",
        "Etat Wi-Fi",
        "Raison",
        "Prevision",
        "Mis a jour",
        "Valide",
        "Non charge",
        "Mis a jour",
        "WiFi",
        "XIAO Weather Setup",
        "Connectez l'ecran a votre Wi-Fi. Page optimisee pour le captive portal Apple.",
        "Reseaux visibles",
        "SSID masque ou manuel",
        "Enregistrer et redemarrer",
        "Recharger la page captive",
        "Ouvrir la page complete",
        "Point d'acces",
        "IP de l'appareil",
        "Langue",
        "IP statique",
        "Activer IP statique",
        "Adresse IP",
        "Passerelle",
        "Sous-reseau",
        "DNS 1",
        "DNS 2",
        {"DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM"},
        {"SOLEIL", "CLAIR", "PART", "NUAGE", "BROU", "PLUIE", "FORT", "AVERS", "ORAGE", "BRUI", "NEIGE", "MIX",
         "SLEET", "GLACE", "GRELE", "VENT", "V+P"},
    },
};

UiLanguage currentLanguage = UiLanguage::English;

const UiText& ui()
{
  return kUiText[static_cast<uint8_t>(currentLanguage)];
}

UiLanguage languageFromCode(const String& code)
{
  for (uint8_t i = 0; i < (sizeof(kUiText) / sizeof(kUiText[0])); ++i) {
    if (code.equalsIgnoreCase(kUiText[i].code)) {
      return static_cast<UiLanguage>(i);
    }
  }
  return UiLanguage::English;
}

bool isLanguageCodeSupported(const String& code)
{
  for (uint8_t i = 0; i < (sizeof(kUiText) / sizeof(kUiText[0])); ++i) {
    if (code.equalsIgnoreCase(kUiText[i].code)) {
      return true;
    }
  }
  return false;
}

const __FlashStringHelper* wifiStatusLabel(wl_status_t status)
{
  switch (status) {
    case WL_IDLE_STATUS:
      return F("IDLE");
    case WL_NO_SSID_AVAIL:
      return F("NO_SSID");
    case WL_SCAN_COMPLETED:
      return F("SCAN_DONE");
    case WL_CONNECTED:
      return F("CONNECTED");
    case WL_CONNECT_FAILED:
      return F("CONNECT_FAILED");
    case WL_CONNECTION_LOST:
      return F("CONNECTION_LOST");
    case WL_DISCONNECTED:
      return F("DISCONNECTED");
    default:
      return F("UNKNOWN");
  }
}

const __FlashStringHelper* disconnectReasonLabel(uint8_t reason)
{
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
      return F("AUTH_EXPIRE");
    case WIFI_REASON_AUTH_LEAVE:
      return F("AUTH_LEAVE");
    case WIFI_REASON_ASSOC_EXPIRE:
      return F("ASSOC_EXPIRE");
    case WIFI_REASON_ASSOC_TOOMANY:
      return F("ASSOC_TOOMANY");
    case WIFI_REASON_NOT_AUTHED:
      return F("NOT_AUTHED");
    case WIFI_REASON_NOT_ASSOCED:
      return F("NOT_ASSOCED");
    case WIFI_REASON_ASSOC_LEAVE:
      return F("ASSOC_LEAVE");
    case WIFI_REASON_ASSOC_NOT_AUTHED:
      return F("ASSOC_NOT_AUTHED");
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
      return F("PWRCAP_BAD");
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
      return F("SUPCHAN_BAD");
    case WIFI_REASON_IE_INVALID:
      return F("IE_INVALID");
    case WIFI_REASON_MIC_FAILURE:
      return F("MIC_FAILURE");
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return F("4WAY_TIMEOUT");
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
      return F("GROUP_KEY_TIMEOUT");
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
      return F("IE_4WAY_DIFF");
    case WIFI_REASON_GROUP_CIPHER_INVALID:
      return F("GROUP_CIPHER_INVALID");
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
      return F("PAIRWISE_CIPHER_INVALID");
    case WIFI_REASON_AKMP_INVALID:
      return F("AKMP_INVALID");
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
      return F("RSN_VERSION");
    case WIFI_REASON_INVALID_RSN_IE_CAP:
      return F("RSN_CAP");
    case WIFI_REASON_802_1X_AUTH_FAILED:
      return F("8021X_FAILED");
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
      return F("CIPHER_REJECTED");
    case WIFI_REASON_BEACON_TIMEOUT:
      return F("BEACON_TIMEOUT");
    case WIFI_REASON_NO_AP_FOUND:
      return F("NO_AP_FOUND");
    case WIFI_REASON_AUTH_FAIL:
      return F("AUTH_FAIL");
    case WIFI_REASON_ASSOC_FAIL:
      return F("ASSOC_FAIL");
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return F("HANDSHAKE_TIMEOUT");
    default:
      return F("UNKNOWN");
  }
}

void addLog(const String& message)
{
  Serial.println(message);
  logBuffer[logWriteIndex] = message;
  logWriteIndex = (logWriteIndex + 1) % kMaxLogEntries;
}

String canonicalizeSsid(const String& ssid)
{
  // Normalize SSID for dedup: trim, remove non-printables, collapse whitespace, and uppercase.
  // This ensures multiple BSSIDs with the same visible SSID collapse into one list entry.
  String key;
  key.reserve(ssid.length());
  bool lastWasSpace = false;
  for (size_t i = 0; i < ssid.length(); ++i) {
    const char c = ssid[i];
    if (c <= 0x20 || c == 0x7f) {
      if (!lastWasSpace && !key.isEmpty()) {
        key += ' ';
        lastWasSpace = true;
      }
      continue;
    }
    char upper = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    key += upper;
    lastWasSpace = false;
  }
  key.trim();
  return key;
}

void clearScanCache()
{
  // Reset the scan cache so each refresh rebuilds a clean, deduplicated list.
  for (uint8_t i = 0; i < kMaxScanEntries; ++i) {
    scanCache[i].ssid = "";
    scanCache[i].ssidKey = "";
    scanCache[i].rssi = -127;
    scanCache[i].channel = 0;
    memset(scanCache[i].bssid, 0, sizeof(scanCache[i].bssid));
    scanCache[i].inUse = false;
  }
  scanCacheCount = 0;
}

void upsertScanCacheEntry(const String& ssid, int32_t rssi, int32_t channel, const uint8_t* bssid)
{
  if (ssid.isEmpty()) {
    return;
  }

  // Dedup by normalized SSID, keeping only the strongest RSSI entry.
  const String key = canonicalizeSsid(ssid);
  if (key.isEmpty()) {
    return;
  }

  int emptyIndex = -1;
  for (uint8_t i = 0; i < kMaxScanEntries; ++i) {
    if (scanCache[i].inUse && scanCache[i].ssidKey == key) {
      // Keep the strongest entry for this SSID key (best AP candidate).
      if (rssi > scanCache[i].rssi) {
        scanCache[i].rssi = rssi;
        scanCache[i].channel = channel;
        scanCache[i].ssid = ssid;
        if (bssid != nullptr) {
          memcpy(scanCache[i].bssid, bssid, sizeof(scanCache[i].bssid));
        }
      }
      return;
    }
    if (!scanCache[i].inUse && emptyIndex < 0) {
      emptyIndex = static_cast<int>(i);
    }
  }

  if (emptyIndex < 0) {
    return;
  }

  scanCache[emptyIndex].ssid = ssid;
  scanCache[emptyIndex].ssidKey = key;
  scanCache[emptyIndex].rssi = rssi;
  scanCache[emptyIndex].channel = channel;
  if (bssid != nullptr) {
    memcpy(scanCache[emptyIndex].bssid, bssid, sizeof(scanCache[emptyIndex].bssid));
  }
  scanCache[emptyIndex].inUse = true;
  scanCacheCount++;
}

String jsonEscape(const String& input)
{
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '\\' || c == '"') {
      output += '\\';
      output += c;
    } else if (c == '\n') {
      output += "\\n";
    } else if (c >= 0x20) {
      output += c;
    }
  }
  return output;
}

int weekdayIndex(int year, int month, int day)
{
  static constexpr int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) {
    year -= 1;
  }
  return (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
}

void isoDateToWeekday(const char* isoDate, char* output, size_t outputSize)
{
  int year = 0;
  int month = 0;
  int day = 0;
  sscanf(isoDate, "%4d-%2d-%2d", &year, &month, &day);
  const int idx = weekdayIndex(year, month, day);
  snprintf(output, outputSize, "%s", ui().weekday[idx]);
}

// Map Open-Meteo weather codes to the icon set used on the display.
WeatherVisual classifyWeather(const ForecastDay& day)
{
  const int weatherCode = day.weatherCode;
  if (weatherCode == 0) {
    return WeatherVisual::Clear;
  }
  if (weatherCode == 1 || weatherCode == 2) {
    return (weatherCode == 1) ? WeatherVisual::MostlyClear : WeatherVisual::PartlyCloudy;
  }
  if (weatherCode == 3) {
    return WeatherVisual::Cloudy;
  }
  if (weatherCode == 45 || weatherCode == 48) {
    return WeatherVisual::Fog;
  }
  if (weatherCode >= 51 && weatherCode <= 55) {
    return WeatherVisual::Drizzle;
  }
  if (weatherCode == 56 || weatherCode == 57) {
    return WeatherVisual::FreezingRain;
  }
  if (weatherCode >= 61 && weatherCode <= 65) {
    return (weatherCode >= 63) ? WeatherVisual::HeavyRain : WeatherVisual::LightRain;
  }
  if (weatherCode == 66 || weatherCode == 67) {
    return WeatherVisual::FreezingRain;
  }
  if (weatherCode >= 71 && weatherCode <= 75) {
    return WeatherVisual::Snow;
  }
  if (weatherCode == 77) {
    return WeatherVisual::Hail;
  }
  if (weatherCode >= 80 && weatherCode <= 82) {
    return WeatherVisual::Showers;
  }
  if (weatherCode == 85 || weatherCode == 86) {
    return WeatherVisual::MixedRainSnow;
  }
  if (weatherCode >= 95) {
    return WeatherVisual::Thunderstorm;
  }
  return WeatherVisual::Cloudy;
}

// Short label for the current weather icon; kept small to avoid overlap.
String weatherLabel(const ForecastDay& day)
{
  const uint8_t index = static_cast<uint8_t>(classifyWeather(day));
  if (index < 17) {
    return ui().weatherLabel[index];
  }
  return F("N/A");
}

Coordinates resolveCoordinates()
{
  return {weather_config::kDefaultLatitude, weather_config::kDefaultLongitude, weather_config::kLocationLabel,
          weather_config::kDefaultTimezone};
}

bool updateClock(const char* timezone, ForecastData& forecast)
{
  configTzTime(timezone, "pool.ntp.org", "time.nist.gov");
  struct tm timeInfo = {};
  const uint32_t startedAt = millis();
  while (!getLocalTime(&timeInfo, 250) && (millis() - startedAt) < kNtpTimeoutMs) {
    delay(100);
  }

  if (!getLocalTime(&timeInfo, 100)) {
    snprintf(forecast.updatedAt, sizeof(forecast.updatedAt), "%s", "--:--");
    snprintf(forecast.updatedDay, sizeof(forecast.updatedDay), "%s", "TIME N/A");
    return false;
  }

  strftime(forecast.updatedAt, sizeof(forecast.updatedAt), "%H:%M", &timeInfo);
  // Localized weekday + numeric date avoids month-name localization issues on embedded C locale.
  snprintf(forecast.updatedDay,
           sizeof(forecast.updatedDay),
           "%s %02d/%02d",
           ui().weekday[timeInfo.tm_wday],
           timeInfo.tm_mday,
           timeInfo.tm_mon + 1);
  return true;
}

String buildForecastUrl(const Coordinates& coordinates)
{
  String url = F("https://api.open-meteo.com/v1/forecast?");
  url += F("latitude=");
  url += String(coordinates.latitude, 4);
  url += F("&longitude=");
  url += String(coordinates.longitude, 4);
  url += F("&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,wind_speed_10m_max");
  url += F("&forecast_days=5");
  url += F("&temperature_unit=celsius");
  url += F("&wind_speed_unit=kmh");
  url += F("&timezone=");
  url += coordinates.timezone;
  return url;
}

bool parseForecastPayload(const String& payload, const Coordinates& coordinates, ForecastData& forecast)
{
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    addLog(String("JSON parse failed: ") + error.c_str());
    return false;
  }

  const JsonObject daily = doc["daily"];
  if (daily.isNull()) {
    addLog("Missing daily forecast object.");
    return false;
  }

  const JsonArray times = daily["time"].as<JsonArray>();
  const JsonArray weatherCodes = daily["weather_code"].as<JsonArray>();
  const JsonArray tempMax = daily["temperature_2m_max"].as<JsonArray>();
  const JsonArray tempMin = daily["temperature_2m_min"].as<JsonArray>();
  const JsonArray precipitation = daily["precipitation_probability_max"].as<JsonArray>();
  const JsonArray windSpeed = daily["wind_speed_10m_max"].as<JsonArray>();

  if (times.size() < kForecastDays || weatherCodes.size() < kForecastDays || tempMax.size() < kForecastDays ||
      tempMin.size() < kForecastDays || precipitation.size() < kForecastDays || windSpeed.size() < kForecastDays) {
    addLog("Forecast arrays shorter than expected.");
    return false;
  }

  snprintf(forecast.location, sizeof(forecast.location), "%s", coordinates.label);
  for (uint8_t i = 0; i < kForecastDays; ++i) {
    snprintf(forecast.days[i].isoDate, sizeof(forecast.days[i].isoDate), "%s", times[i].as<const char*>());
    isoDateToWeekday(forecast.days[i].isoDate, forecast.days[i].weekday, sizeof(forecast.days[i].weekday));
    forecast.days[i].weatherCode = weatherCodes[i].as<int>();
    forecast.days[i].tempMax = static_cast<int>(lroundf(tempMax[i].as<float>()));
    forecast.days[i].tempMin = static_cast<int>(lroundf(tempMin[i].as<float>()));
    forecast.days[i].precipitationProbability = precipitation[i].isNull() ? 0 : precipitation[i].as<int>();
    forecast.days[i].windSpeed = static_cast<int>(lroundf(windSpeed[i].as<float>()));
  }
  updateClock(coordinates.timezone, forecast);
  return true;
}

bool fetchForecast(ForecastData& forecast)
{
  const Coordinates coordinates = resolveCoordinates();
  const String url = buildForecastUrl(coordinates);

  addLog("Requesting Open-Meteo forecast.");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(client, url)) {
    addLog("HTTP begin failed.");
    return false;
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    addLog(String("HTTP GET failed: ") + httpCode);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();
  if (!parseForecastPayload(payload, coordinates, forecast)) {
    return false;
  }

  lastRefreshMs = millis();
  addLog(String("Forecast updated for ") + forecast.location);
  return true;
}

// Draws a 40x40 weather icon using 4bpp palette indices from the sprite sheet.
// The icon data targets the default Seeed_GFX 4-bit palette (white/black/red/yellow).
void drawWeatherIcon(const ForecastDay& day, int16_t centerX, int16_t topY)
{
  // Icons are ordered left-to-right, top-to-bottom in the 6x3 sprite:
  // clear, mostly_clear, partly_cloudy, cloudy, fog, light_rain,
  // heavy_rain, showers, thunderstorm, drizzle, snow, mixed_rain_snow,
  // sleet, freezing_rain, hail, wind, wind_rain.
  static const uint8_t* const kWeatherIcon40[] = {
      kWeatherIcon40Clear,
      kWeatherIcon40MostlyClear,
      kWeatherIcon40PartlyCloudy,
      kWeatherIcon40Cloudy,
      kWeatherIcon40Fog,
      kWeatherIcon40LightRain,
      kWeatherIcon40HeavyRain,
      kWeatherIcon40Showers,
      kWeatherIcon40Thunderstorm,
      kWeatherIcon40Drizzle,
      kWeatherIcon40Snow,
      kWeatherIcon40MixedRainSnow,
      kWeatherIcon40Sleet,
      kWeatherIcon40FreezingRain,
      kWeatherIcon40Hail,
      kWeatherIcon40Wind,
      kWeatherIcon40WindRain
  };

  uint8_t iconIndex = 3; // default: cloudy
  switch (classifyWeather(day)) {
    case WeatherVisual::Clear:
      iconIndex = 1;
      break;
    case WeatherVisual::MostlyClear:
      iconIndex = 0;
      break;
    case WeatherVisual::PartlyCloudy:
      iconIndex = 2;
      break;
    case WeatherVisual::Cloudy:
      iconIndex = 3;
      break;
    case WeatherVisual::Fog:
      iconIndex = 4;
      break;
    case WeatherVisual::LightRain:
      iconIndex = 5;
      break;
    case WeatherVisual::HeavyRain:
      iconIndex = 6;
      break;
    case WeatherVisual::Showers:
      iconIndex = 7;
      break;
    case WeatherVisual::Thunderstorm:
      iconIndex = 8;
      break;
    case WeatherVisual::Drizzle:
      iconIndex = 9;
      break;
    case WeatherVisual::Snow:
      iconIndex = 10;
      break;
    case WeatherVisual::MixedRainSnow:
      iconIndex = 11;
      break;
    case WeatherVisual::Sleet:
      iconIndex = 12;
      break;
    case WeatherVisual::FreezingRain:
      iconIndex = 13;
      break;
    case WeatherVisual::Hail:
      iconIndex = 14;
      break;
    case WeatherVisual::Wind:
      iconIndex = 15;
      break;
    case WeatherVisual::WindRain:
      iconIndex = 16;
      break;
  }

  const int16_t x = centerX - (kWeatherIcon40Width / 2);
  // Keep a white icon background so the glyphs remain visible.
  epaper.fillRect(x, topY, kWeatherIcon40Width, kWeatherIcon40Height, TFT_WHITE);
  // Icon data is 4bpp palette indices. Push into the 4bpp e-paper sprite.
  const uint8_t* icon = kWeatherIcon40[iconIndex];
  epaper.pushImage(
      x,
      topY,
      kWeatherIcon40Width,
      kWeatherIcon40Height,
      const_cast<uint16_t*>(reinterpret_cast<const uint16_t*>(icon)),
      4);
}

void drawCenteredText(const String& text, int32_t x, int32_t y, uint8_t font, uint16_t fg, uint16_t bg)
{
  epaper.setTextColor(fg, bg);
  epaper.drawCentreString(text, x, y, font);
}

// Draws a slightly bolder centered string by overprinting with a 1px offset.
// This improves readability for temperatures without changing fonts.
void drawBoldCenteredText(const String& text, int32_t x, int32_t y, uint8_t font, uint16_t fg, uint16_t bg)
{
  epaper.setTextColor(fg, bg);
  epaper.drawCentreString(text, x, y, font);
  epaper.drawCentreString(text, x + 1, y, font);
}

// Draw a bold GFX free font string centered at (x,y).
// Free fonts improve legibility at small sizes on e-paper.
void drawFreeFontCentered(const String& text, int32_t x, int32_t y, const GFXfont* font, uint16_t fg, uint16_t bg)
{
  epaper.setTextColor(fg, bg);
  epaper.setFreeFont(font);
  epaper.setTextDatum(MC_DATUM);
  epaper.drawString(text, x, y);
  epaper.setTextDatum(TL_DATUM);
  epaper.setFreeFont(nullptr);
}

// Draw a small degree symbol as a circle. Keeps output consistent across fonts that lack "°".
void drawDegreeSymbol(int16_t x, int16_t y, uint8_t radius, uint16_t color)
{
  epaper.drawCircle(x, y, radius, color);
}

// Draw a centered temperature value with a manually drawn degree symbol.
// This avoids relying on the "°" glyph, which is missing in some GFX fonts.
void drawFreeFontCenteredTemp(int value, int32_t x, int32_t y, const GFXfont* font, uint16_t fg, uint16_t bg)
{
  const String text = String(value);
  epaper.setTextColor(fg, bg);
  epaper.setFreeFont(font);
  epaper.setTextDatum(MC_DATUM);
  epaper.drawString(text, x, y);

  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  epaper.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  const int16_t left = x - static_cast<int16_t>(w / 2) - x1;
  const int16_t top = y - static_cast<int16_t>(h / 2) - y1;
  drawDegreeSymbol(left + w + 2, top + 2, 2, fg);

  epaper.setTextDatum(TL_DATUM);
  epaper.setFreeFont(nullptr);
}

// Draw a bold GFX free font string left-aligned at (x,y).
void drawFreeFontLeft(const String& text, int32_t x, int32_t y, const GFXfont* font, uint16_t fg, uint16_t bg)
{
  epaper.setTextColor(fg, bg);
  epaper.setFreeFont(font);
  epaper.setTextDatum(TL_DATUM);
  epaper.drawString(text, x, y);
  epaper.setFreeFont(nullptr);
}

// Clamps text to a maximum pixel width, appending "..." when needed.
// This prevents header strings (SSID, updated time) from overlapping other labels.
String clampTextToWidth(const String& text, uint16_t maxWidth, uint8_t font)
{
  if (maxWidth == 0) {
    return "";
  }
  if (epaper.textWidth(text, font) <= maxWidth) {
    return text;
  }
  String clipped = text;
  while (clipped.length() > 0 && epaper.textWidth(clipped + "...", font) > maxWidth) {
    clipped.remove(clipped.length() - 1);
  }
  return clipped.isEmpty() ? "" : clipped + "...";
}

void renderSetupScreen(const String& title, const String& line1, const String& line2)
{
  epaper.setRotation(1);
  epaper.setTextWrap(false, false);
  epaper.fillScreen(TFT_WHITE);
  epaper.fillRect(0, 0, epaper.width(), 24, TFT_RED);
  epaper.setTextColor(TFT_WHITE, TFT_RED);
  epaper.drawString(title, 6, 4, 2);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.drawString(line1, 8, 42, 2);
  epaper.drawString(line2, 8, 66, 1);
  epaper.update();
}

// Draw the fixed top banner: city on the left, Wi-Fi and updated time right-aligned.
// Keeping the banner black preserves contrast for the red/yellow/white text colors.
void renderHeader(const ForecastData& forecast)
{
  const uint16_t displayWidth = epaper.width();
  epaper.fillRect(0, 0, displayWidth, kHeaderHeight, TFT_BLACK);
  epaper.drawFastHLine(0, kHeaderHeight - 1, displayWidth, TFT_RED);
  // Use a bold free font for the city name to improve legibility.
  drawFreeFontLeft(forecast.location, 4, 2, &FreeSansBold9pt7b, TFT_YELLOW, TFT_BLACK);

  // Right-align Wi-Fi status on the first header line to avoid overlap with the title.
  const String wifiName = currentSsid.isEmpty() ? String("AP") : currentSsid;
  const String ipText = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("--.--.--.--");
  const String wifiLabel = String(ui().labelWifi) + ": " + wifiName + " " + ipText;
  const uint16_t titleWidth = epaper.textWidth(forecast.location, 2);
  const uint16_t wifiMaxWidth = (displayWidth > titleWidth + 12) ? (displayWidth - titleWidth - 12) : 0;
  // this is where the SSID is drawn, so use yellow to match the city name and stand out against the black banner.
  epaper.setTextColor(TFT_YELLOW, TFT_BLACK);
  epaper.drawRightString(clampTextToWidth(wifiLabel, wifiMaxWidth, 1), displayWidth - 4, 2, 1);

  // Keep the location on the left and right-align the updated timestamp on the second line.
  const String updatedLabel = String(ui().labelUpdated) + ": " + forecast.updatedDay + " " + forecast.updatedAt;
  const uint16_t locationWidth = epaper.textWidth(forecast.location, 1);
  const uint16_t updatedMaxWidth = (displayWidth > locationWidth + 12) ? (displayWidth - locationWidth - 12) : 0;
  epaper.setTextColor(TFT_WHITE, TFT_BLACK);
  epaper.drawRightString(clampTextToWidth(updatedLabel, updatedMaxWidth, 1), displayWidth - 4, 13, 1);
}

// Draw a single forecast day card. Layout is tuned to avoid text overlap.
void renderDayCard(const ForecastDay& day, uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
  // Revert to white card background for better icon visibility.
  epaper.fillRect(x, y, width, height, TFT_WHITE);
  epaper.drawRect(x, y, width, height, TFT_BLACK);

  // Layout is spaced to prevent overlap between label, icon, and temperatures.
  const uint16_t dividerY = y + 16;
  // Position the icon two pixels below the day divider for a tighter layout.
  const uint16_t iconTop = dividerY - 1;
  // The label is below the icon with a small gap, and the temperature is below that.
  const uint16_t labelY = y + 53;
  const uint16_t tempY = y + 70;

  epaper.drawFastHLine(x, dividerY, width, TFT_YELLOW);
  drawCenteredText(day.weekday, x + width / 2, y + 2, 2, TFT_BLACK, TFT_WHITE);
  drawWeatherIcon(day, x + width / 2, iconTop);

  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.drawCentreString(weatherLabel(day), x + width / 2, labelY, 1);
  epaper.setTextColor(TFT_RED, TFT_WHITE);
  drawFreeFontCenteredTemp(day.tempMax, x + width / 2, tempY, &FreeSansBold12pt7b, TFT_RED, TFT_WHITE);
}

// Render the compact footer metrics. Single-letter legends save horizontal space.
void renderFooterMetrics(const ForecastDay& day, uint16_t x, uint16_t y, uint16_t width)
{
  // Bottom line shows low temp and precipitation chance for readability.
  // Using left/right alignment keeps text within the card and avoids overlap.
  const String lowLabel = String("L") + String(day.tempMin);
  const String popLabel = String("P") + String(day.precipitationProbability) + "%";
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.drawString(lowLabel, x + 4, y, 1);
  const int16_t degreeX = x + 4 + epaper.textWidth(lowLabel, 1) + 2;
  const int16_t degreeY = y + 2;
  drawDegreeSymbol(degreeX, degreeY, 1, TFT_BLACK);
  epaper.drawRightString(popLabel, x + width - 4, y, 1);
}

// Full-screen forecast render. This redraws the full frame (not partial refresh).
void renderForecast(const ForecastData& forecast)
{
  epaper.setRotation(1);
  epaper.setTextWrap(false, false);
  const uint16_t displayWidth = epaper.width();
  const uint16_t displayHeight = epaper.height();
  epaper.fillScreen(TFT_WHITE);
  renderHeader(forecast);

  const uint16_t usableTop = kHeaderHeight + 4;
  const uint16_t cardHeight = displayHeight - usableTop - 4;
  const uint16_t totalGap = kCellGap * (kForecastDays - 1);
  const uint16_t cardWidth = (displayWidth - totalGap) / kForecastDays;

  for (uint8_t i = 0; i < kForecastDays; ++i) {
    const uint16_t cardX = i * (cardWidth + kCellGap);
    renderDayCard(forecast.days[i], cardX, usableTop, cardWidth, cardHeight);
    renderFooterMetrics(forecast.days[i], cardX, usableTop + cardHeight - 12, cardWidth);
  }
  epaper.update();
}

void renderErrorScreen(const String& title, const String& detail)
{
  epaper.setRotation(1);
  epaper.setTextWrap(false, false);
  epaper.fillScreen(TFT_WHITE);
  epaper.fillRect(0, 0, epaper.width(), 22, TFT_RED);
  epaper.setTextColor(TFT_WHITE, TFT_RED);
  epaper.drawString("WEATHER ERROR", 6, 3, 2);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.drawString(title, 8, 38, 2);
  epaper.drawString(detail, 8, 62, 1);
  epaper.update();
}

void loadStoredCredentials()
{
  currentSsid = "";
  currentPassword = "";
  hasStoredCredentials = false;
  usingCompiledDefaults = false;

  // First boot has no namespace yet. Open read-write so Preferences can create it.
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed. Continuing with compile-time Wi-Fi defaults.");
  } else {
    if (preferences.isKey("ssid")) {
      currentSsid = preferences.getString("ssid", "");
      hasStoredCredentials = !currentSsid.isEmpty();
    }
    if (preferences.isKey("password")) {
      currentPassword = preferences.getString("password", "");
    }
    preferences.end();
  }

  if (currentSsid.isEmpty() && strlen(weather_config::kWifiSsid) > 0) {
    currentSsid = weather_config::kWifiSsid;
    currentPassword = weather_config::kWifiPassword;
    usingCompiledDefaults = true;
  }
}

// Load the UI language preference from NVS. Defaults to English on first boot.
void loadLanguagePreference()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading language. Defaulting to English.");
    currentLanguage = UiLanguage::English;
    return;
  }
  const String langCode = preferences.getString("lang", "en");
  preferences.end();
  currentLanguage = languageFromCode(langCode);
}

// Persist the UI language preference and update the in-memory selection.
void saveLanguagePreference(const String& langCode)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving language preference.");
    return;
  }
  preferences.putString("lang", langCode);
  preferences.end();
  currentLanguage = languageFromCode(langCode);
}

String buildLanguageOptions()
{
  String options;
  for (uint8_t i = 0; i < (sizeof(kUiText) / sizeof(kUiText[0])); ++i) {
    options += "<option value='";
    options += kUiText[i].code;
    options += "'";
    if (static_cast<uint8_t>(currentLanguage) == i) {
      options += " selected";
    }
    options += ">";
    options += kUiText[i].name;
    options += "</option>";
  }
  return options;
}

void saveCredentials(const String& ssid, const String& password)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving Wi-Fi credentials.");
    return;
  }
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
}

bool parseIpString(const String& text, IPAddress& out)
{
  if (text.isEmpty()) {
    return false;
  }
  return out.fromString(text);
}

void loadStaticIpConfig()
{
  staticIpConfig = {};
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading static IP config.");
    return;
  }

  staticIpConfig.enabled = preferences.getBool("static_enabled", false);
  String ip = preferences.getString("static_ip", "");
  String gateway = preferences.getString("static_gw", "");
  String subnet = preferences.getString("static_subnet", "");
  String dns1 = preferences.getString("static_dns1", "");
  String dns2 = preferences.getString("static_dns2", "");
  preferences.end();

  parseIpString(ip, staticIpConfig.ip);
  parseIpString(gateway, staticIpConfig.gateway);
  parseIpString(subnet, staticIpConfig.subnet);
  parseIpString(dns1, staticIpConfig.dns1);
  parseIpString(dns2, staticIpConfig.dns2);
}

void saveStaticIpConfig(bool enabled,
                        const String& ip,
                        const String& gateway,
                        const String& subnet,
                        const String& dns1,
                        const String& dns2)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving static IP config.");
    return;
  }
  preferences.putBool("static_enabled", enabled);
  preferences.putString("static_ip", ip);
  preferences.putString("static_gw", gateway);
  preferences.putString("static_subnet", subnet);
  preferences.putString("static_dns1", dns1);
  preferences.putString("static_dns2", dns2);
  preferences.end();

  staticIpConfig.enabled = enabled;
  parseIpString(ip, staticIpConfig.ip);
  parseIpString(gateway, staticIpConfig.gateway);
  parseIpString(subnet, staticIpConfig.subnet);
  parseIpString(dns1, staticIpConfig.dns1);
  parseIpString(dns2, staticIpConfig.dns2);
}

bool applyStaticIpConfig()
{
  if (!staticIpConfig.enabled) {
    return WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }
  if (!staticIpConfig.ip || !staticIpConfig.gateway || !staticIpConfig.subnet) {
    addLog("Static IP enabled but IP/gateway/subnet is invalid.");
    return false;
  }
  if (staticIpConfig.dns1 || staticIpConfig.dns2) {
    return WiFi.config(staticIpConfig.ip,
                       staticIpConfig.gateway,
                       staticIpConfig.subnet,
                       staticIpConfig.dns1,
                       staticIpConfig.dns2);
  }
  return WiFi.config(staticIpConfig.ip, staticIpConfig.gateway, staticIpConfig.subnet);
}

void clearCredentials()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while clearing Wi-Fi credentials.");
    return;
  }
  preferences.remove("ssid");
  preferences.remove("password");
  preferences.end();
  currentSsid = "";
  currentPassword = "";
}

void stopApMode();

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      addLog("Wi-Fi STA started.");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      addLog("Wi-Fi associated with router.");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staGotIp = true;
      reconnectFailures = 0;
      addLog(String("Wi-Fi got IP: ") + WiFi.localIP().toString());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      staGotIp = false;
      lastDisconnectReason = info.wifi_sta_disconnected.reason;
      addLog(String("Wi-Fi disconnected, reason=") + disconnectReasonLabel(lastDisconnectReason));
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      addLog("SoftAP started.");
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      addLog("SoftAP stopped.");
      break;
    default:
      break;
  }
}

void registerWiFiEvents()
{
  if (wifiEventsRegistered) {
    return;
  }
  WiFi.onEvent(onWiFiEvent);
  wifiEventsRegistered = true;
}

void rebuildScanCacheFromDriver(int count)
{
  // Rebuild a deduplicated scan cache from the driver scan results.
  clearScanCache();
  for (int i = 0; i < count; ++i) {
    upsertScanCacheEntry(WiFi.SSID(i), WiFi.RSSI(i), WiFi.channel(i), WiFi.BSSID(i));
  }
  lastScanCacheMs = millis();
  addLog(String("Wi-Fi scan cache refreshed with ") + scanCacheCount + " unique SSIDs.");
}

bool refreshScanCacheSync()
{
  // Synchronous scan for explicit rescan requests.
  addLog("Refreshing Wi-Fi scan cache.");
  const int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    addLog("Wi-Fi scan failed.");
    return false;
  }
  rebuildScanCacheFromDriver(count);
  WiFi.scanDelete();
  scanInProgress = false;
  return true;
}

void startScanCacheRefreshAsync()
{
  // Asynchronous scan to keep the captive portal responsive.
  if (scanInProgress) {
    return;
  }
  const int status = WiFi.scanComplete();
  if (status == WIFI_SCAN_RUNNING) {
    scanInProgress = true;
    return;
  }
  if (status >= 0) {
    WiFi.scanDelete();
  }
  if (WiFi.scanNetworks(true, true) == WIFI_SCAN_FAILED) {
    addLog("Async Wi-Fi scan start failed.");
    return;
  }
  scanInProgress = true;
}

void pollScanCache()
{
  // Complete async scans without blocking the web UI.
  if (!scanInProgress) {
    return;
  }

  const int status = WiFi.scanComplete();
  if (status == WIFI_SCAN_RUNNING) {
    return;
  }
  if (status < 0) {
    scanInProgress = false;
    return;
  }

  rebuildScanCacheFromDriver(status);
  WiFi.scanDelete();
  scanInProgress = false;
}

const ScanCacheEntry* findBestNetworkForSsid(const String& ssid)
{
  bool found = false;
  // Compare against normalized key to avoid duplicates like "MBUmain" vs "MBUmain ".
  const String key = canonicalizeSsid(ssid);
  if (key.isEmpty()) {
    return nullptr;
  }
  const ScanCacheEntry* best = nullptr;
  for (uint8_t i = 0; i < kMaxScanEntries; ++i) {
    if (scanCache[i].inUse && scanCache[i].ssidKey == key) {
      found = true;
      if (best == nullptr || scanCache[i].rssi > best->rssi) {
        best = &scanCache[i];
      }
    }
  }
  if (!found) {
    return nullptr;
  }
  return best;
}

void logTargetNetworkInfo(const String& ssid)
{
  const ScanCacheEntry* network = findBestNetworkForSsid(ssid);
  if (network == nullptr) {
    addLog(String("Target SSID not visible in scan: ") + ssid);
    return;
  }
  addLog(String("Target SSID visible, best RSSI=") + network->rssi + " dBm, channel=" + network->channel);
}

void prepareStaMode()
{
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(kStaHostname);
  applyStaticIpConfig();
  WiFi.mode(WIFI_STA);
}

bool waitForStaConnection(uint32_t timeoutMs)
{
  wl_status_t previousStatus = static_cast<wl_status_t>(-1);
  const uint32_t startedAt = millis();
  while ((millis() - startedAt) < timeoutMs) {
    const wl_status_t status = WiFi.status();
    if (status != previousStatus) {
      addLog(String("Wi-Fi status -> ") + wifiStatusLabel(status));
      previousStatus = status;
    }
    if (status == WL_CONNECTED && staGotIp) {
      return true;
    }
    delay(250);
  }
  return false;
}

bool connectToWifi(const String& ssid, const String& password, bool allowScanInfo = true)
{
  if (ssid.isEmpty()) {
    addLog("No Wi-Fi SSID available for station mode.");
    return false;
  }

  stopApMode();
  prepareStaMode();
  if ((millis() - lastScanCacheMs) > kScanCacheTtlMs || scanCacheCount == 0) {
    // Seed the cache so we can pick the strongest AP before associating.
    refreshScanCacheSync();
  }

  const ScanCacheEntry* targetNetwork = findBestNetworkForSsid(ssid);
  if (targetNetwork != nullptr) {
    // Prefer the strongest BSSID for a multi-AP SSID (fixes random/weak associations).
    addLog(String("Using strongest AP candidate for SSID ") + ssid + ": RSSI " + targetNetwork->rssi +
           " dBm on channel " + targetNetwork->channel);
  }

  for (uint8_t attempt = 1; attempt <= kWifiConnectAttempts; ++attempt) {
    staGotIp = false;
    addLog(String("Connecting to Wi-Fi SSID: ") + ssid + " (attempt " + attempt + "/" + kWifiConnectAttempts + ")");
    WiFi.disconnect(true, true);
    delay(120);
    if (targetNetwork != nullptr && targetNetwork->channel > 0) {
      // Use channel + BSSID pinning when available to avoid hopping across APs.
      WiFi.begin(ssid.c_str(), password.c_str(), targetNetwork->channel, targetNetwork->bssid, true);
    } else {
      WiFi.begin(ssid.c_str(), password.c_str());
    }

    if (waitForStaConnection(kWifiTimeoutMs)) {
      currentSsid = ssid;
      currentPassword = password;
      lastReconnectAttemptMs = millis();
      reconnectFailures = 0;
      addLog(String("Wi-Fi connected. IP: ") + WiFi.localIP().toString());
      return true;
    }

    const wl_status_t status = WiFi.status();
    addLog(String("Wi-Fi connect attempt failed with status ") + wifiStatusLabel(status));
    if (allowScanInfo && (status == WL_NO_SSID_AVAIL || lastDisconnectReason == WIFI_REASON_NO_AP_FOUND ||
                          lastDisconnectReason == WIFI_REASON_AUTH_FAIL ||
                          lastDisconnectReason == WIFI_REASON_HANDSHAKE_TIMEOUT)) {
      // Refresh cache when a scan-related failure occurs to improve the next attempt.
      refreshScanCacheSync();
      targetNetwork = findBestNetworkForSsid(ssid);
      logTargetNetworkInfo(ssid);
      addLog(String("Disconnect reason detail: ") + disconnectReasonLabel(lastDisconnectReason));
    }

    if (status == WL_CONNECT_FAILED || lastDisconnectReason == WIFI_REASON_AUTH_FAIL ||
        lastDisconnectReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || lastDisconnectReason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
      addLog("Authentication or passphrase failure suspected.");
      break;
    }

    delay(kWifiAttemptDelayMs);
  }

  addLog(String("Wi-Fi connection failed for SSID: ") + ssid);
  return false;
}

void stopApMode()
{
  if (!apModeActive) {
    return;
  }
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  apModeActive = false;
  addLog("AP mode stopped.");
}

void startApMode()
{
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(kApSsid)) {
    addLog("Failed to start AP mode.");
    renderErrorScreen("AP failed", "Restart device");
    return;
  }

  apModeActive = true;
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  // Kick off an async scan so the captive portal can render quickly.
  startScanCacheRefreshAsync();
  if (!serverStarted) {
    server.begin();
    serverStarted = true;
    addLog("HTTP control server started in AP mode.");
  }
  addLog(String("AP mode active. Connect to SSID: ") + kApSsid);
  addLog(String("AP IP: ") + WiFi.softAPIP().toString());
  renderSetupScreen(ui().wifiSetupTitle,
                    String(ui().accessPoint) + ": " + kApSsid,
                    String(ui().deviceIp) + ": " + WiFi.softAPIP().toString());
}

String buildLogsJson()
{
  String response = "[";
  bool first = true;
  for (uint8_t i = 0; i < kMaxLogEntries; ++i) {
    const uint8_t index = (logWriteIndex + i) % kMaxLogEntries;
    if (logBuffer[index].isEmpty()) {
      continue;
    }
    if (!first) {
      response += ',';
    }
    first = false;
    response += '"';
    response += jsonEscape(logBuffer[index]);
    response += '"';
  }
  response += ']';
  return response;
}

String buildStatusJson()
{
  String json = "{";
  json += "\"apMode\":";
  json += apModeActive ? "true" : "false";
  json += ",\"wifiConnected\":";
  json += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
  json += ",\"ssid\":\"";
  json += jsonEscape(currentSsid);
  json += "\",\"ip\":\"";
  json += jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(""));
  json += "\",\"apIp\":\"";
  json += jsonEscape(apModeActive ? WiFi.softAPIP().toString() : String(""));
  json += "\",\"wifiStatus\":\"";
  json += jsonEscape(String(wifiStatusLabel(WiFi.status())));
  json += "\",\"disconnectReason\":\"";
  json += jsonEscape(String(disconnectReasonLabel(lastDisconnectReason)));
  json += "\",\"forecastValid\":";
  json += forecastValid ? "true" : "false";
  json += ",\"location\":\"";
  json += jsonEscape(currentForecast.location);
  json += "\",\"updated\":\"";
  json += jsonEscape(String(currentForecast.updatedDay) + " " + currentForecast.updatedAt);
  json += "\",\"language\":\"";
  json += kUiText[static_cast<uint8_t>(currentLanguage)].code;
  json += "\"}";
  return json;
}

String buildScanJson()
{
  String json = "[";
  bool first = true;
  for (uint8_t i = 0; i < kMaxScanEntries; ++i) {
    if (!scanCache[i].inUse) {
      continue;
    }
    if (!first) {
      json += ',';
    }
    first = false;
    json += "{\"ssid\":\"";
    json += jsonEscape(scanCache[i].ssid);
    json += "\",\"rssi\":";
    json += String(scanCache[i].rssi);
    json += ",\"channel\":";
    json += String(scanCache[i].channel);
    json += "}";
  }
  json += "]";
  return json;
}

String buildMainPage()
{
  const UiText& t = ui();
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{TITLE}}</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; background: #101416; color: #eef2f4; }
    header { background: #0b0f10; padding: 16px; border-bottom: 3px solid #e0b400; }
    .lang-row { margin-top: 10px; max-width: 280px; }
    h1 { margin: 0 0 8px 0; font-size: 22px; }
    main { padding: 16px; display: grid; gap: 16px; }
    .card { background: #182024; border: 1px solid #2f3c43; border-radius: 10px; padding: 16px; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 12px; }
    label { display: block; margin-top: 10px; margin-bottom: 4px; }
    input, select, button { width: 100%; box-sizing: border-box; padding: 10px; border-radius: 8px; border: 1px solid #4a5c65; }
    input, select { background: #f5f7f8; color: #111; }
    button { background: #e0b400; color: #111; font-weight: bold; cursor: pointer; }
    button.secondary { background: #2f3c43; color: #eef2f4; }
    pre { margin: 0; white-space: pre-wrap; word-break: break-word; font-size: 12px; line-height: 1.4; max-height: 320px; overflow-y: auto; }
    .mono { font-family: monospace; }
    .status { display: grid; grid-template-columns: max-content 1fr; gap: 6px 10px; }
  </style>
</head>
<body>
  <header>
    <h1>{{TITLE}}</h1>
    <div id="summary">{{LOADING_STATE}}</div>
    <div class="lang-row">
      <label for="language">{{LANGUAGE_LABEL}}</label>
      <select id="language">{{LANGUAGE_OPTIONS}}</select>
    </div>
  </header>
  <main>
    <section class="grid">
      <div class="card">
        <h2>{{STATUS_TITLE}}</h2>
        <div id="status" class="status mono"></div>
      </div>
      <div class="card">
        <h2>{{WIFI_SETUP_TITLE}}</h2>
        <label for="ssid">{{SSID_LABEL}}</label>
        <select id="ssid"></select>
        <label for="manual_ssid">{{HIDDEN_SSID_LABEL}}</label>
        <input id="manual_ssid" type="text" placeholder="Enter SSID manually">
        <label for="password">{{PASSWORD_LABEL}}</label>
        <input id="password" type="password" placeholder="WLAN password">
        <h3 style="margin-top:16px;">{{STATIC_IP_TITLE}}</h3>
        <label>
          <input id="static_enabled" type="checkbox" style="width:auto; margin-right:8px;" {{STATIC_IP_CHECKED}}>
          {{STATIC_IP_ENABLE}}
        </label>
        <label for="static_ip">{{STATIC_IP_ADDRESS}}</label>
        <input id="static_ip" type="text" placeholder="192.168.1.50" value="{{STATIC_IP_VALUE}}">
        <label for="static_gw">{{STATIC_IP_GATEWAY}}</label>
        <input id="static_gw" type="text" placeholder="192.168.1.1" value="{{STATIC_GW_VALUE}}">
        <label for="static_subnet">{{STATIC_IP_SUBNET}}</label>
        <input id="static_subnet" type="text" placeholder="255.255.255.0" value="{{STATIC_SUBNET_VALUE}}">
        <label for="static_dns1">{{STATIC_IP_DNS1}}</label>
        <input id="static_dns1" type="text" placeholder="1.1.1.1" value="{{STATIC_DNS1_VALUE}}">
        <label for="static_dns2">{{STATIC_IP_DNS2}}</label>
        <input id="static_dns2" type="text" placeholder="8.8.8.8" value="{{STATIC_DNS2_VALUE}}">
        <div class="grid">
          <button onclick="saveWiFi()">{{SAVE_REBOOT}}</button>
          <button class="secondary" onclick="scanNetworks()">{{RESCAN}}</button>
        </div>
        <div class="grid" style="margin-top:10px;">
          <button class="secondary" onclick="refreshForecast()">{{REFRESH_FORECAST}}</button>
          <button class="secondary" onclick="forgetWiFi()">{{FORGET_WIFI}}</button>
        </div>
      </div>
    </section>
    <section class="card">
      <h2>{{LOGS_TITLE}}</h2>
      <pre id="logs">{{LOADING_LOGS}}</pre>
    </section>
  </main>
  <script>
    const ui = {
      summaryAp: '{{SUMMARY_AP}}',
      summarySta: '{{SUMMARY_STA}}',
      summaryConnected: '{{SUMMARY_CONNECTED}}',
      summaryNotConnected: '{{SUMMARY_NOT_CONNECTED}}',
      summaryNoForecast: '{{SUMMARY_NO_FORECAST}}',
      statusSsid: '{{STATUS_SSID}}',
      statusIp: '{{STATUS_IP}}',
      statusApIp: '{{STATUS_AP_IP}}',
      statusWifiState: '{{STATUS_WIFI_STATE}}',
      statusReason: '{{STATUS_REASON}}',
      statusForecast: '{{STATUS_FORECAST}}',
      statusUpdated: '{{STATUS_UPDATED}}',
      forecastValid: '{{FORECAST_VALID}}',
      forecastMissing: '{{FORECAST_MISSING}}'
    };
    async function fetchJson(path, options) {
      const response = await fetch(path, options);
      if (!response.ok) throw new Error(await response.text());
      return response.json();
    }
    async function scanNetworks() {
      const select = document.getElementById('ssid');
      select.innerHTML = '<option>Scanning...</option>';
      const networks = await fetchJson('/scan');
      select.innerHTML = '';
      if (!networks.length) {
        select.innerHTML = '<option value="">No networks found</option>';
        return;
      }
      networks.forEach(n => {
        const option = document.createElement('option');
        option.value = n.ssid;
        option.textContent = `${n.ssid} (${n.rssi} dBm)`;
        select.appendChild(option);
      });
    }
    async function updateStatus() {
      const status = await fetchJson('/status');
      document.getElementById('summary').textContent =
        `${status.apMode ? ui.summaryAp : ui.summarySta} | ${status.wifiConnected ? ui.summaryConnected : ui.summaryNotConnected} | ${status.location || ui.summaryNoForecast}`;
      document.getElementById('status').innerHTML = `
        <div>${ui.statusSsid}</div><div>${status.ssid || '-'}</div>
        <div>${ui.statusIp}</div><div>${status.ip || '-'}</div>
        <div>${ui.statusApIp}</div><div>${status.apIp || '-'}</div>
        <div>${ui.statusWifiState}</div><div>${status.wifiStatus || '-'}</div>
        <div>${ui.statusReason}</div><div>${status.disconnectReason || '-'}</div>
        <div>${ui.statusForecast}</div><div>${status.forecastValid ? ui.forecastValid : ui.forecastMissing}</div>
        <div>${ui.statusUpdated}</div><div>${status.updated || '-'}</div>`;
    }
    async function updateLogs() {
      const logs = await fetchJson('/logs');
      document.getElementById('logs').textContent = logs.join('\n');
    }
    async function saveWiFi() {
      const manual = document.getElementById('manual_ssid').value.trim();
      const ssid = manual || document.getElementById('ssid').value;
      const password = document.getElementById('password').value;
      const staticEnabled = document.getElementById('static_enabled').checked;
      const staticIp = document.getElementById('static_ip').value.trim();
      const staticGw = document.getElementById('static_gw').value.trim();
      const staticSubnet = document.getElementById('static_subnet').value.trim();
      const staticDns1 = document.getElementById('static_dns1').value.trim();
      const staticDns2 = document.getElementById('static_dns2').value.trim();
      if (!ssid) { alert('SSID is required'); return; }
      const response = await fetch('/saveWiFi', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          ssid,
          password,
          staticEnabled,
          staticIp,
          staticGw,
          staticSubnet,
          staticDns1,
          staticDns2
        })
      });
      alert(await response.text());
    }
    async function refreshForecast() {
      const response = await fetch('/refresh', {method: 'POST'});
      alert(await response.text());
      await updateStatus();
      await updateLogs();
    }
    async function forgetWiFi() {
      const response = await fetch('/forgetWiFi', {method: 'POST'});
      alert(await response.text());
    }
    async function setLanguage() {
      const lang = document.getElementById('language').value;
      await fetch('/language', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({language: lang})
      });
      location.reload();
    }
    const languageSelect = document.getElementById('language');
    if (languageSelect) {
      languageSelect.addEventListener('change', setLanguage);
    }
    document.getElementById('manual_ssid').addEventListener('input', e => {
      if (e.target.value.trim()) {
        document.getElementById('ssid').value = e.target.value.trim();
      }
    });
    scanNetworks().then(updateStatus).then(updateLogs);
    setInterval(updateStatus, 8000);
    setInterval(updateLogs, 5000);
  </script>
</body>
</html>
)rawliteral";

  page.replace("{{TITLE}}", t.title);
  page.replace("{{LOADING_STATE}}", t.loadingState);
  page.replace("{{STATUS_TITLE}}", t.statusTitle);
  page.replace("{{WIFI_SETUP_TITLE}}", t.wifiSetupTitle);
  page.replace("{{LOGS_TITLE}}", t.logsTitle);
  page.replace("{{SSID_LABEL}}", t.ssidLabel);
  page.replace("{{HIDDEN_SSID_LABEL}}", t.hiddenSsidLabel);
  page.replace("{{PASSWORD_LABEL}}", t.passwordLabel);
  page.replace("{{SAVE_REBOOT}}", t.saveReboot);
  page.replace("{{RESCAN}}", t.rescan);
  page.replace("{{REFRESH_FORECAST}}", t.refreshForecast);
  page.replace("{{FORGET_WIFI}}", t.forgetWifi);
  page.replace("{{LOADING_LOGS}}", t.loadingLogs);
  page.replace("{{STATIC_IP_TITLE}}", t.staticIpTitle);
  page.replace("{{STATIC_IP_ENABLE}}", t.staticIpEnable);
  page.replace("{{STATIC_IP_ADDRESS}}", t.staticIpAddress);
  page.replace("{{STATIC_IP_GATEWAY}}", t.staticIpGateway);
  page.replace("{{STATIC_IP_SUBNET}}", t.staticIpSubnet);
  page.replace("{{STATIC_IP_DNS1}}", t.staticIpDns1);
  page.replace("{{STATIC_IP_DNS2}}", t.staticIpDns2);
  page.replace("{{STATIC_IP_VALUE}}", staticIpConfig.ip.toString());
  page.replace("{{STATIC_GW_VALUE}}", staticIpConfig.gateway.toString());
  page.replace("{{STATIC_SUBNET_VALUE}}", staticIpConfig.subnet.toString());
  page.replace("{{STATIC_DNS1_VALUE}}", staticIpConfig.dns1.toString());
  page.replace("{{STATIC_DNS2_VALUE}}", staticIpConfig.dns2.toString());
  page.replace("{{SUMMARY_AP}}", t.summaryAp);
  page.replace("{{SUMMARY_STA}}", t.summarySta);
  page.replace("{{SUMMARY_CONNECTED}}", t.summaryConnected);
  page.replace("{{SUMMARY_NOT_CONNECTED}}", t.summaryNotConnected);
  page.replace("{{SUMMARY_NO_FORECAST}}", t.summaryNoForecast);
  page.replace("{{STATUS_SSID}}", t.statusSsid);
  page.replace("{{STATUS_IP}}", t.statusIp);
  page.replace("{{STATUS_AP_IP}}", t.statusApIp);
  page.replace("{{STATUS_WIFI_STATE}}", t.statusWifiState);
  page.replace("{{STATUS_REASON}}", t.statusReason);
  page.replace("{{STATUS_FORECAST}}", t.statusForecast);
  page.replace("{{STATUS_UPDATED}}", t.statusUpdated);
  page.replace("{{FORECAST_VALID}}", t.forecastValid);
  page.replace("{{FORECAST_MISSING}}", t.forecastNotLoaded);
  page.replace("{{LANGUAGE_LABEL}}", t.languageLabel);
  page.replace("{{LANGUAGE_OPTIONS}}", buildLanguageOptions());
  page.replace("{{STATIC_IP_CHECKED}}", staticIpConfig.enabled ? "checked" : "");
  return page;
}

String buildCaptiveScanOptions()
{
  String options;
  if (scanCacheCount == 0) {
    return "<option value=''>No networks found</option>";
  }

  for (uint8_t i = 0; i < kMaxScanEntries; ++i) {
    if (!scanCache[i].inUse || scanCache[i].ssid.isEmpty()) {
      continue;
    }
    options += "<option value='";
    options += jsonEscape(scanCache[i].ssid);
    options += "'>";
    options += jsonEscape(scanCache[i].ssid);
    options += " (";
    options += scanCache[i].rssi;
    options += " dBm)</option>";
  }
  return options.isEmpty() ? "<option value=''>No networks found</option>" : options;
}

String buildCaptivePage()
{
  const UiText& t = ui();
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{SETUP_TITLE}}</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, Helvetica, Arial, sans-serif; margin: 0; background: #f5f5f7; color: #111; }
    main { max-width: 560px; margin: 0 auto; padding: 18px; }
    h1 { font-size: 24px; margin: 0 0 8px; }
    p { line-height: 1.45; }
    .card { background: #fff; border-radius: 14px; padding: 16px; box-shadow: 0 1px 4px rgba(0,0,0,0.12); margin-top: 14px; }
    label { display: block; margin: 12px 0 6px; font-weight: 600; }
    input, select, button { width: 100%; box-sizing: border-box; padding: 12px; border-radius: 10px; font-size: 16px; }
    input, select { border: 1px solid #c7c7cc; }
    button { margin-top: 14px; border: 0; background: #007aff; color: #fff; font-weight: 600; }
    .hint { color: #555; font-size: 14px; }
    .mono { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
    a { color: #007aff; text-decoration: none; }
  </style>
</head>
<body>
  <main>
    <h1>{{SETUP_TITLE}}</h1>
    <p>{{SETUP_INTRO}}</p>
    <div class="card">
      <form method="POST" action="/saveWiFiForm">
        <label for="language">{{LANGUAGE_LABEL}}</label>
        <select id="language" name="language">
{{LANGUAGE_OPTIONS}}
        </select>
        <label for="ssid_select">{{VISIBLE_NETWORKS}}</label>
        <select id="ssid_select" name="ssid_select">
)rawliteral";
  page += buildCaptiveScanOptions();
  page += R"rawliteral(
        </select>
        <label for="ssid_manual">{{MANUAL_SSID}}</label>
        <input id="ssid_manual" name="ssid_manual" type="text" autocapitalize="none" autocorrect="off" placeholder="Enter SSID manually if needed">
        <label for="password">{{PASSWORD_LABEL}}</label>
        <input id="password" name="password" type="password" autocapitalize="none" autocorrect="off" placeholder="Router password">
        <button type="submit">{{SAVE_AND_REBOOT}}</button>
      </form>
    </div>
    <div class="card">
      <p class="hint"><a href="/hotspot-detect.html">{{RELOAD_CAPTIVE}}</a></p>
      <p class="hint"><a href="/">{{OPEN_FULL}}</a></p>
      <p class="hint">{{ACCESS_POINT}}: <span class="mono">XIAO-Weather-Setup</span></p>
      <p class="hint">{{DEVICE_IP}}: <span class="mono">192.168.4.1</span></p>
    </div>
  </main>
</body>
</html>
)rawliteral";
  page.replace("{{SETUP_TITLE}}", t.setupTitle);
  page.replace("{{SETUP_INTRO}}", t.setupIntro);
  page.replace("{{VISIBLE_NETWORKS}}", t.visibleNetworks);
  page.replace("{{MANUAL_SSID}}", t.manualSsid);
  page.replace("{{PASSWORD_LABEL}}", t.passwordLabel);
  page.replace("{{SAVE_AND_REBOOT}}", t.saveAndReboot);
  page.replace("{{RELOAD_CAPTIVE}}", t.reloadCaptive);
  page.replace("{{OPEN_FULL}}", t.openFull);
  page.replace("{{ACCESS_POINT}}", t.accessPoint);
  page.replace("{{DEVICE_IP}}", t.deviceIp);
  page.replace("{{LANGUAGE_LABEL}}", t.languageLabel);
  page.replace("{{LANGUAGE_OPTIONS}}", buildLanguageOptions());
  return page;
}

bool isAppleCaptiveRequest()
{
  const String userAgent = server.header("User-Agent");
  return userAgent.indexOf("CaptiveNetworkSupport") >= 0 || userAgent.indexOf("wispr") >= 0;
}

void handleRoot()
{
  server.sendHeader("Cache-Control", "no-store");
  if (apModeActive && isAppleCaptiveRequest()) {
    server.send(200, "text/html", buildCaptivePage());
    return;
  }
  server.send(200, "text/html", buildMainPage());
}

void handleLogs()
{
  server.send(200, "application/json", buildLogsJson());
}

void handleStatus()
{
  server.send(200, "application/json", buildStatusJson());
}

void handleScan()
{
  // Explicit rescan for the full control UI.
  refreshScanCacheSync();
  server.send(200, "application/json", buildScanJson());
}

void handleSaveWiFi()
{
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  const String ssid = doc["ssid"].as<String>();
  const String password = doc["password"].as<String>();
  const String lang = doc["language"].as<String>();
  if (ssid.isEmpty()) {
    server.send(400, "text/plain", "SSID cannot be empty");
    return;
  }

  if (!lang.isEmpty() && isLanguageCodeSupported(lang)) {
    saveLanguagePreference(lang);
    addLog(String("Language set to ") + lang + " via setup API.");
  }

  saveCredentials(ssid, password);
  addLog(String("Saved Wi-Fi credentials for SSID: ") + ssid);
  server.send(200, "text/plain", "Wi-Fi credentials saved. Device will reboot.");
  delay(500);
  ESP.restart();
}

void handleSaveWiFiForm()
{
  const String ssid = server.arg("ssid_manual").length() ? server.arg("ssid_manual") : server.arg("ssid_select");
  const String password = server.arg("password");
  const String lang = server.arg("language");
  if (ssid.isEmpty()) {
    server.send(400, "text/plain", "SSID cannot be empty");
    return;
  }

  if (!lang.isEmpty() && isLanguageCodeSupported(lang)) {
    saveLanguagePreference(lang);
    addLog(String("Language set to ") + lang + " via captive form.");
  }

  saveCredentials(ssid, password);
  addLog(String("Saved Wi-Fi credentials from captive form for SSID: ") + ssid);
  server.send(200, "text/html",
              "<!DOCTYPE html><html><body style='font-family:-apple-system,Helvetica,Arial,sans-serif;padding:24px;'>"
              "<h2>Wi-Fi saved</h2><p>The device will reboot and join your router.</p></body></html>");
  delay(500);
  ESP.restart();
}

void handleLanguage()
{
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  const String lang = doc["language"].as<String>();
  if (lang.isEmpty() || !isLanguageCodeSupported(lang)) {
    server.send(400, "text/plain", "Unsupported language");
    return;
  }

  saveLanguagePreference(lang);
  addLog(String("Language set to ") + lang + ".");
  if (forecastValid) {
    // Recompute the updated timestamp so localized weekday strings refresh too.
    updateClock(resolveCoordinates().timezone, currentForecast);
    renderForecast(currentForecast);
  }
  server.send(200, "text/plain", "Language updated.");
}

void handleForgetWiFi()
{
  clearCredentials();
  addLog("Stored Wi-Fi credentials erased.");
  server.send(200, "text/plain", "Wi-Fi credentials erased. Device will reboot.");
  delay(500);
  ESP.restart();
}

bool refreshForecastAndDisplay()
{
  if (WiFi.status() != WL_CONNECTED) {
    forecastValid = false;
    renderErrorScreen("Wi-Fi disconnected", "Forecast refresh unavailable");
    return false;
  }

  ForecastData nextForecast = {};
  if (!fetchForecast(nextForecast)) {
    forecastValid = false;
    renderErrorScreen("Forecast failed", "Check logs via web UI");
    return false;
  }

  currentForecast = nextForecast;
  forecastValid = true;
  renderForecast(currentForecast);
  return true;
}

void handleRefresh()
{
  if (refreshForecastAndDisplay()) {
    server.send(200, "text/plain", "Forecast refreshed.");
  } else {
    server.send(500, "text/plain", "Forecast refresh failed.");
  }
}

void handleReboot()
{
  server.send(200, "text/plain", "Rebooting.");
  delay(300);
  ESP.restart();
}

void handleNotFound()
{
  if (apModeActive) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", buildCaptivePage());
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void handleAppleCaptiveProbe()
{
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", buildCaptivePage());
}

void handleConnectTestTxt()
{
  if (apModeActive) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", "XIAO Weather setup required");
    return;
  }
  server.send(200, "text/plain", "Microsoft Connect Test");
}

void handleGenerate204()
{
  if (apModeActive) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", buildCaptivePage());
    return;
  }
  server.send(204, "text/plain", "");
}

void setupWebServer()
{
  if (routesConfigured) {
    return;
  }
  const char* headers[] = {"User-Agent", "X-Requested-With"};
  server.collectHeaders(headers, 2);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleAppleCaptiveProbe);
  server.on("/canonical.html", HTTP_GET, handleAppleCaptiveProbe);
  server.on("/library/test/success.html", HTTP_GET, handleAppleCaptiveProbe);
  server.on("/success.txt", HTTP_GET, handleAppleCaptiveProbe);
  server.on("/generate_204", HTTP_GET, handleGenerate204);
  server.on("/gen_204", HTTP_GET, handleGenerate204);
  server.on("/connecttest.txt", HTTP_GET, handleConnectTestTxt);
  server.on("/ncsi.txt", HTTP_GET, handleConnectTestTxt);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/saveWiFi", HTTP_POST, handleSaveWiFi);
  server.on("/saveWiFiForm", HTTP_POST, handleSaveWiFiForm);
  server.on("/language", HTTP_POST, handleLanguage);
  server.on("/forgetWiFi", HTTP_POST, handleForgetWiFi);
  server.on("/refresh", HTTP_POST, handleRefresh);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.onNotFound(handleNotFound);
  routesConfigured = true;
}

void setupRuntime()
{
  registerWiFiEvents();
  loadStoredCredentials();
  loadLanguagePreference();
  setupWebServer();

  // If no credentials were previously saved, prefer fast AP onboarding for Apple CNA.
  if (!hasStoredCredentials && usingCompiledDefaults) {
    // Avoid long STA timeouts on first boot so the captive portal appears fast.
    addLog("No stored Wi-Fi credentials. Starting AP setup immediately.");
    startApMode();
    return;
  }

  if (connectToWifi(currentSsid, currentPassword)) {
    if (!serverStarted) {
      server.begin();
      serverStarted = true;
      addLog("HTTP control server started in station mode.");
    }
    stopApMode();
    refreshForecastAndDisplay();
    return;
  }

  addLog("Falling back to AP mode for WLAN onboarding.");
  startApMode();
}
}  // namespace

void setup()
{
  Serial.begin(115200);
  delay(1000);

#ifdef EPAPER_ENABLE
  epaper.begin();
#endif

  addLog("XIAO weather app booting.");
  setupRuntime();
}

void loop()
{
  if (serverStarted) {
    server.handleClient();
  }
  if (apModeActive) {
    dnsServer.processNextRequest();
    // Keep the scan cache fresh without blocking CNA.
    pollScanCache();
    if (!scanInProgress && (millis() - lastScanCacheMs) > kScanCacheTtlMs) {
      startScanCacheRefreshAsync();
    }
  }

  if (!apModeActive && WiFi.status() == WL_CONNECTED && forecastValid &&
      (millis() - lastRefreshMs) > kRefreshIntervalMs) {
    refreshForecastAndDisplay();
  }

  if (!apModeActive && !currentSsid.isEmpty() && WiFi.status() != WL_CONNECTED &&
      (millis() - lastReconnectAttemptMs) > kReconnectCheckIntervalMs) {
    lastReconnectAttemptMs = millis();
    reconnectFailures++;
    addLog(String("Wi-Fi reconnect check triggered (failure count ") + reconnectFailures + ").");

    if (connectToWifi(currentSsid, currentPassword, reconnectFailures == 1)) {
      if (!forecastValid) {
        refreshForecastAndDisplay();
      }
    } else if (reconnectFailures >= kMaxReconnectFailuresBeforeAp) {
      addLog("Repeated STA reconnect failures. Keeping saved credentials and continuing retry loop.");
    }
  }
}
