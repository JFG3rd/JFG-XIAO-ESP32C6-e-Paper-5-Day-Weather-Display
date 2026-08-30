#include <Adafruit_LC709203F.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <FS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Update.h>
#include <Wire.h>
#include <time.h>

// Bold free fonts are provided by Seeed_GFX (LOAD_GFXFF in the user setup).

#include "weather_config.h"
// 4bpp icon sprites used by the e-paper renderer (indexed color values).
#include "weather_icons_40x40.h"

#ifdef EPAPER_ENABLE
EPaper epaper;
#endif

// Latched by the patched CHECK_BUSY() in JD79667_Defines.h when the panel fails
// to raise BUSY within EPD_BUSY_TIMEOUT_MS. Must live at global scope with C++
// linkage matching the extern declaration in that header.
volatile bool epdBusyTimedOut = false;

// Survives deep sleep (but not a power cycle or a hard reset). Counts wake
// cycles and lets /status report how this boot started. Defined here rather than
// beside setup() so buildStatusJson() can read it.
RTC_DATA_ATTR uint32_t rtcWakeCount = 0;
// How long the driver waits on BUSY before giving up. This is a hang guard, not
// a deadline: a real BWRY full refresh takes 12-16 s, so the operating value must
// be comfortably above that or the driver powers the panel down mid-refresh and
// leaves a blank or ghosted image. Init uses a much tighter value (see setupPanel)
// because a healthy panel answers within milliseconds of a reset.
constexpr uint32_t kBusyTimeoutRefreshMs = 30000;
constexpr uint32_t kBusyTimeoutInitMs = 5000;
volatile uint32_t epdBusyTimeoutMs = kBusyTimeoutRefreshMs;

namespace
{
constexpr uint8_t kForecastDays = 5;
constexpr uint8_t kMaxLogEntries = 24;
constexpr uint8_t kMaxScanEntries = 16;
// 30 px. The banner absorbs both the dead margin that used to sit below the
// cards and the 4 px white gap that used to sit between banner and cards, so the
// rule now lands flush on the cards. cardHeight is unchanged at 98 px either way
// (128 - 30), and the extra rows give the 25 px tall 18 pt digits room to breathe.
constexpr uint16_t kHeaderHeight = 30;
// Battery glyph drawn in the top-right of the header (body + terminal nub).
constexpr uint16_t kBatteryIconWidth = 30;
// Horizontal space reserved for the glyph, including the margin around it.
constexpr uint16_t kBatteryIconMargin = kBatteryIconWidth + 8;
// Tighten the gap so each day card is slightly wider.
constexpr uint16_t kCellGap = 0;
constexpr uint32_t kWifiTimeoutMs = 12000;
constexpr uint32_t kWifiAttemptDelayMs = 1200;
constexpr uint32_t kReconnectCheckIntervalMs = 10000;
constexpr uint32_t kScanCacheTtlMs = 30000;
constexpr uint32_t kHttpTimeoutMs = 15000;
constexpr uint32_t kNtpTimeoutMs = 10000;
// Refresh interval is user-configurable (web UI, persisted in NVS). Each cycle
// ends in deep sleep, so this is also the wall-clock wake period - boot and fetch
// time are subtracted before sleeping to keep the cadence honest.
constexpr uint16_t kDefaultRefreshMinutes = 60;
constexpr uint16_t kRefreshMinutesOptions[] = {15, 30, 60, 120, 240};
// After every render the device stays awake this long so the web UI is reachable.
// Any handled HTTP request pushes the deadline forward, so an open browser tab
// keeps the device awake (up to kMaxAwakeMs). This window is the single biggest
// lever on battery life: at ~90 mA awake, every extra minute per cycle costs
// ~1.5 mAh, which is a meaningful slice of a 400 mAh pack.
constexpr uint32_t kAwakeWindowMs = 30UL * 1000UL;
// A cold boot (power-on or reset) holds the device awake for longer: that is
// when someone is most likely to be reaching for the web UI, and a 5 minute
// floor means they never have to race the sleep timer.
constexpr uint32_t kColdBootAwakeMs = 5UL * 60UL * 1000UL;
// Shorter cycle when the forecast could not be fetched, so a transient outage
// does not leave stale data on screen for a full half hour.
constexpr uint32_t kRetrySleepMs = 5UL * 60UL * 1000UL;
// Absolute ceiling on how long one wake cycle may stay awake, regardless of web
// traffic. Without it, any HTTP request postpones sleep by another two minutes
// forever - a forgotten browser tab, or a router probing port 80 to build its
// device list, quietly keeps the radio up and drains the pack. Must stay above
// kColdBootAwakeMs or a cold boot would sleep before its hold expires.
constexpr uint32_t kMaxAwakeMs = 10UL * 60UL * 1000UL;
// A request arriving after this much silence is logged with its client IP, to
// identify whatever is re-arming the awake window.
constexpr uint32_t kWebQuietLogThresholdMs = 60UL * 1000UL;
constexpr uint8_t kWifiConnectAttempts = 2;
constexpr uint8_t kMaxReconnectFailuresBeforeAp = 3;
constexpr byte kDnsPort = 53;
constexpr const char* kPrefsNamespace = "wifi";
constexpr const char* kApSsid = "JFG-PaperCast-Setup";
constexpr const char* kStaHostname = "jfg-papercast";

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
// Deadline (millis) until which the device stays awake. Extended by web activity.
uint32_t awakeUntilMs = 0;
// Handled web requests this wake cycle. Exposed in /status so it is possible to
// tell "a browser tab is holding the device awake" apart from "sleep is broken".
uint32_t webRequestCount = 0;
uint32_t lastWebRequestMs = 0;
// Deep sleep can be disabled from the web UI to keep the device permanently
// reachable for debugging. Persisted in NVS so it survives the sleep/reboot cycle.
bool deepSleepEnabled = true;
// A panel refresh takes ~20 s and blocks the single-threaded web server for all
// of it. Doing that inline in a settings handler meant the next POST in a burst
// (the Settings card sends five) arrived while the device could not accept a
// connection, so those settings were silently dropped - the "changes do not
// stick" bug. Handlers now request a refresh and return immediately; loop()
// performs one refresh once the burst has settled.
bool refreshPending = false;
uint32_t refreshRequestedMs = 0;
constexpr uint32_t kRefreshDebounceMs = 2500;

// True while an OTA upload is in flight. Deep sleep is suppressed entirely for
// the duration: sleeping mid-flash would leave a half-written app partition.
bool otaInProgress = false;
// Timed debug hold. RAM only, deliberately: a keep-awake that survived a reboot
// would be the same footgun as the permanent sleep toggle it complements.
uint32_t keepAwakeUntilMs = 0;
// A USB *host* is attached (SOF frames seen). A plain wall charger supplies VBUS
// without enumerating, so this means "on the bench", not "charging".
bool usbHostConnected = false;
// Inferred from the cell voltage trend, so it also catches a wall charger.
bool batteryCharging = false;
// Minutes between forecast refreshes, set from the web UI and persisted in NVS.
uint16_t refreshIntervalMinutes = kDefaultRefreshMinutes;
// Battery pack capacity in mAh, chosen in the web UI so this firmware is not tied
// to one builder's cell. Drives both the gauge's profile and the modelled estimate.
uint16_t batteryPackMah = weather_config::kBatteryCapacityMah;
// Location and timezone, all web-configurable and NVS-backed. Defaults come from
// weather_config.h so a fresh device behaves exactly as before.
float currentLatitude = weather_config::kDefaultLatitude;
float currentLongitude = weather_config::kDefaultLongitude;
String currentLocationLabel = weather_config::kLocationLabel;
String currentTimezone = weather_config::kDefaultTimezone;
// Display units.
bool useFahrenheit = false;
bool useMph = false;
// Quiet hours: skip refreshes overnight. Removing ~8 of 24 daily wake cycles is
// the single largest battery saving available after the awake window itself.
bool quietHoursEnabled = false;
uint8_t quietStartHour = 23;
uint8_t quietEndHour = 6;
// Low-battery behaviour: warn on the panel and stretch the refresh interval.
bool batteryWarnEnabled = true;
uint8_t batteryWarnPercent = 20;

// Defined further down with the other power helpers; the header renderer needs
// it before that point to draw the low-battery strip.
bool batteryIsLow();
// Defined with the other text helpers; the alert fetcher above it needs to fold
// API text to ASCII before it reaches the panel.
String toDisplayAscii(const String& text);

// Where header warnings come from: "off", "derived" (our own thresholds on the
// forecast we already fetch) or "dwd" (official DWD alerts via Bright Sky).
String warningSource = "derived";
// What the header's first line shows when there is no warning and no new IP:
// "now" / "rain" / "sun" / "wind". See buildHeaderPreset().
String headerMode = "now";
// The header's second line: another preset, or "off" to leave it blank.
String headerMode2 = "sun";
// Which element occupies the left of the banner: "temp" / "location" / "both" /
// "info" (nothing, giving the whole width to the info lines).
String headerLayout = "temp";
// Which whole-panel design to draw: "classic" (five equal cards) or "modern"
// (today as a hero, the rest compact with precipitation bars).
String panelDesign = "classic";
// Text of the active warning, empty when none. Recomputed on each refresh.
String activeWarning;
// Exactly what was last drawn on the header's first line, exposed in /status so
// the panel can be verified without standing in front of it.
String lastHeaderLine;
String lastHeaderLine2;
// How this boot started, exposed in /status: distinguishing a timer wake from a
// cold boot from the outside is otherwise impossible once the 24-entry log ring
// has scrolled past the boot lines.
bool bootWasCold = true;
// True when the header should show SSID/IP instead of weather: set on a cold
// boot, or when the IP differs from the one last seen. The address is only worth
// screen space when it is new information.
bool showNetworkInfo = true;
uint8_t reconnectFailures = 0;
uint8_t lastDisconnectReason = 0;
uint32_t lastScanCacheMs = 0;
bool scanInProgress = false;
// Pre-built captive portal HTML. Rebuilt when the AP starts, language changes,
// or the user triggers a rescan. Serving this cached string avoids ~100ms of
// String.replace() work per request, which would starve DNS processing and
// cause Apple CNA to time out.
String cachedCaptivePage;

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
  // IANA name, for the weather API's timezone parameter.
  const char* timezone;
  // POSIX TZ string, for configTzTime(). See kTimeZones for why these differ.
  const char* posixTz;
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
  // Header extras. Only today's values are ever shown, so these are stored once
  // rather than per day.
  bool currentValid = false;
  int currentTemp = 0;
  int feelsLike = 0;
  int uvIndexMax = 0;
  int precipitationMm = 0;
  int windGusts = 0;
  char sunrise[6] = "--:--";
  char sunset[6] = "--:--";
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

struct BatteryStatus
{
  // false when neither a measurement nor an estimate is available.
  bool valid = false;
  // true when the percentage is modeled from run time rather than measured by a
  // fuel gauge. Such values are rendered with a trailing "?".
  bool estimated = false;
  float volts = 0.0f;
  uint8_t percent = 0;
};

BatteryStatus currentBattery = {};

// I2C LiPo fuel gauge on D4/D5. See docs/wire-diagram.md.
Adafruit_LC709203F batteryGauge;
bool batteryGaugeReady = false;
// False when the panel failed to answer during init; rendering is then skipped
// so the rest of the firmware (Wi-Fi, web UI) still comes up.
bool panelReady = false;

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
  const char* statusBattery;
  const char* statusSleepIn;
  const char* refreshIntervalLabel;
  const char* batteryChargedButton;
  const char* batteryEstimatedNote;
  const char* labelFeelsLike;
  const char* labelRain;
  const char* labelSun;
  const char* labelWind;
  const char* labelGust;
  const char* headerModeLabel;
  const char* headerMode2Label;
  const char* designLabel;
  const char* designClassic;
  const char* designModern;
  const char* layoutLabel;
  const char* layoutTemp;
  const char* layoutLocation;
  const char* layoutBoth;
  const char* layoutInfo;
  const char* previewTitle;
  const char* previewUnsaved;
  const char* previewApplying;
  const char* confirmSave;
  const char* creditPrefix;
  const char* modeOff;
  const char* headerModeNow;
  const char* headerModeRain;
  const char* headerModeSun;
  const char* headerModeWind;
  const char* keepAwakeLabel;
  const char* warnStorm;
  const char* warnWind;
  const char* warnRain;
  const char* warnFrost;
  const char* warnHeat;
  const char* warningSourceLabel;
  const char* warnSourceOff;
  const char* warnSourceDerived;
  const char* warnSourceDwd;
  const char* settingsTitle;
  const char* locationLabelText;
  const char* latitudeLabel;
  const char* longitudeLabel;
  const char* timezoneLabel;
  const char* unitsLabel;
  const char* quietHoursLabel;
  const char* quietFromLabel;
  const char* quietToLabel;
  const char* batteryWarnLabel;
  const char* batteryPackLabel;
  const char* saveButton;
  const char* otaTitle;
  const char* otaHint;
  const char* otaButton;
  const char* batteryUnknown;
  const char* sleepModeLabel;
  const char* sleepModeHint;
  const char* weekday[7];
  const char* weatherLabel[17];
};

const UiText kUiText[] = {
    {
        "en",
        "English",
        "JFG PaperCast",
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
        "Battery",
        "Sleeps in",
        "Refresh every",
        "Battery charged",
        "estimated",
        "Feels",
        "Rain",
        "Sun",
        "Wind",
        "gust",
        "Header line",
        "Second line",
        "Design",
        "Classic - five equal days",
        "Modern - today large",
        "Layout",
        "Temperature",
        "Location",
        "Temperature + location",
        "Info only",
        "Display",
        "Preview of unsaved settings - press Save to apply",
        "Saved - the panel is redrawing",
        "Do you want to change the settings?",
        "Weather data by",
        "Off",
        "Feels-like + UV",
        "Rain in mm",
        "Sunrise / sunset",
        "Wind + gusts",
        "Keep awake (debug)",
        "STORM",
        "WIND",
        "RAIN",
        "FROST",
        "HEAT",
        "Weather warnings",
        "Off",
        "From forecast",
        "Official (DWD)",
        "Settings",
        "Location name",
        "Latitude",
        "Longitude",
        "Time zone",
        "Units",
        "Quiet hours (skip night refreshes)",
        "From",
        "To",
        "Warn and conserve below",
        "Battery size",
        "Save",
        "Firmware Update",
        "Upload a firmware .bin built for this board. The device reboots when it verifies.",
        "Upload firmware",
        "No sensor",
        "Deep sleep between updates",
        "Off keeps this page always reachable.",
        {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"},
        {"SUN", "CLEAR", "PARTLY", "CLOUD", "FOG", "RAIN", "HEAVY", "SHOWERS", "STORM", "DRIZZLE", "SNOW", "MIXED", "SLEET",
         "ICE", "HAIL", "WIND", "WIND+R"},
    },
    {
        "de",
        "Deutsch",
        "JFG PaperCast",
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
        "JFG PaperCast Setup",
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
        "Batterie",
        "Schlaeft in",
        "Aktualisieren alle",
        "Batterie geladen",
        "geschaetzt",
        "Gefuehlt",
        "Regen",
        "Sonne",
        "Wind",
        "Boe",
        "Kopfzeile",
        "Zweite Zeile",
        "Design",
        "Klassisch - fuenf gleiche Tage",
        "Modern - heute gross",
        "Layout",
        "Temperatur",
        "Ort",
        "Temperatur + Ort",
        "Nur Infos",
        "Anzeige",
        "Vorschau nicht gespeicherter Einstellungen - Speichern zum Uebernehmen",
        "Gespeichert - Anzeige wird neu gezeichnet",
        "Moechten Sie die Einstellungen aendern?",
        "Wetterdaten von",
        "Aus",
        "Gefuehlt + UV",
        "Regen in mm",
        "Sonnenauf/-untergang",
        "Wind + Boeen",
        "Wach halten (Debug)",
        "GEWITTER",
        "STURM",
        "REGEN",
        "FROST",
        "HITZE",
        "Wetterwarnungen",
        "Aus",
        "Aus Vorhersage",
        "Amtlich (DWD)",
        "Einstellungen",
        "Ortsname",
        "Breitengrad",
        "Laengengrad",
        "Zeitzone",
        "Einheiten",
        "Ruhezeiten (keine Updates nachts)",
        "Von",
        "Bis",
        "Warnen und sparen unter",
        "Batteriegroesse",
        "Speichern",
        "Firmware-Update",
        "Firmware .bin fuer dieses Board hochladen. Das Geraet startet nach der Pruefung neu.",
        "Firmware hochladen",
        "Kein Sensor",
        "Tiefschlaf zwischen Updates",
        "Aus haelt diese Seite dauerhaft erreichbar.",
        {"SO", "MO", "DI", "MI", "DO", "FR", "SA"},
        {"SONNE", "KLAR", "TEIL", "WOLKIG", "NEBEL", "REGEN", "STARK", "SCHAUER", "GEWITER", "NIESEL", "SCHNEE", "MIX",
         "GRAUPEL", "EIS", "HAGEL", "WIND", "W+R"},
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
        "JFG PaperCast Setup",
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
        "Bateria",
        "Duerme en",
        "Actualizar cada",
        "Bateria cargada",
        "estimado",
        "Sens",
        "Lluvia",
        "Sol",
        "Viento",
        "racha",
        "Linea superior",
        "Segunda linea",
        "Diseno",
        "Clasico - cinco dias iguales",
        "Moderno - hoy grande",
        "Distribucion",
        "Temperatura",
        "Lugar",
        "Temperatura + lugar",
        "Solo info",
        "Pantalla",
        "Vista previa sin guardar - pulse Guardar para aplicar",
        "Guardado - la pantalla se esta redibujando",
        "Desea cambiar los ajustes?",
        "Datos meteorologicos de",
        "Desactivado",
        "Sensacion + UV",
        "Lluvia en mm",
        "Amanecer / ocaso",
        "Viento + rachas",
        "Mantener despierto",
        "TORMENTA",
        "VIENTO",
        "LLUVIA",
        "HELADA",
        "CALOR",
        "Avisos meteorologicos",
        "Desactivado",
        "De la prevision",
        "Oficial (DWD)",
        "Ajustes",
        "Nombre del lugar",
        "Latitud",
        "Longitud",
        "Zona horaria",
        "Unidades",
        "Horas de silencio (sin refrescos de noche)",
        "Desde",
        "Hasta",
        "Avisar y ahorrar por debajo de",
        "Tamano de bateria",
        "Guardar",
        "Actualizacion de firmware",
        "Suba un .bin compilado para esta placa. El dispositivo se reinicia al verificarlo.",
        "Subir firmware",
        "Sin sensor",
        "Suspension entre actualizaciones",
        "Desactivado mantiene esta pagina siempre accesible.",
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
        "JFG PaperCast Setup",
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
        "Batterie",
        "Veille dans",
        "Actualiser toutes les",
        "Batterie chargee",
        "estime",
        "Ressenti",
        "Pluie",
        "Soleil",
        "Vent",
        "rafale",
        "Ligne den-tete",
        "Deuxieme ligne",
        "Design",
        "Classique - cinq jours egaux",
        "Moderne - aujourdhui en grand",
        "Disposition",
        "Temperature",
        "Lieu",
        "Temperature + lieu",
        "Infos seules",
        "Affichage",
        "Apercu non enregistre - appuyez sur Enregistrer",
        "Enregistre - le panneau se redessine",
        "Voulez-vous modifier les parametres?",
        "Donnees meteo de",
        "Desactive",
        "Ressenti + UV",
        "Pluie en mm",
        "Lever / coucher",
        "Vent + rafales",
        "Garder eveille",
        "ORAGE",
        "VENT",
        "PLUIE",
        "GEL",
        "CANICULE",
        "Alertes meteo",
        "Desactive",
        "De la prevision",
        "Officiel (DWD)",
        "Parametres",
        "Nom du lieu",
        "Latitude",
        "Longitude",
        "Fuseau horaire",
        "Unites",
        "Heures calmes (pas de mise a jour la nuit)",
        "De",
        "A",
        "Alerter et economiser sous",
        "Taille de la batterie",
        "Enregistrer",
        "Mise a jour du firmware",
        "Televersez un .bin compile pour cette carte. Lappareil redemarre apres verification.",
        "Televerser le firmware",
        "Pas de capteur",
        "Veille profonde entre les mises a jour",
        "Desactive garde cette page toujours accessible.",
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
  uint32_t sec = millis() / 1000;
  uint8_t h = (sec / 3600) % 100;
  uint8_t m = (sec % 3600) / 60;
  uint8_t s = sec % 60;
  char ts[10];
  snprintf(ts, sizeof(ts), "%02u:%02u:%02u ", h, m, s);
  String stamped = String(ts) + message;
  Serial.println(stamped);
  logBuffer[logWriteIndex] = stamped;
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

// Escape for safe embedding in both JSON strings and HTML attribute values.
// Without HTML entity escaping, a malicious SSID like <script>alert(1)</script>
// would execute in the captive portal page (built via buildCaptiveScanOptions).
String jsonEscape(const String& input)
{
  String output;
  output.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '\\' || c == '"') {
      output += '\\';
      output += c;
    } else if (c == '\n') {
      output += "\\n";
    } else if (c == '<') {
      output += "&lt;";
    } else if (c == '>') {
      output += "&gt;";
    } else if (c == '&') {
      output += "&amp;";
    } else if (c == '\'') {
      output += "&#39;";
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

// A timezone needs two different strings, and using the wrong one is the bug
// this table exists to prevent:
//
//   * Open-Meteo's &timezone= parameter wants the IANA name ("Europe/Berlin").
//   * configTzTime() ends up in newlib's tzset(), which only parses POSIX TZ
//     strings ("CET-1CEST,M3.5.0,M10.5.0/3"). There is no tzdata on the device,
//     so handing it an IANA name silently yields UTC - which is exactly what the
//     firmware did before this table existed, displaying time two hours behind
//     local in a European summer.
struct TimeZoneOption
{
  const char* label;
  const char* iana;
  const char* posix;
};

const TimeZoneOption kTimeZones[] = {
    {"UTC", "UTC", "UTC0"},
    {"Europe/London", "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Berlin", "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris", "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Moscow", "Europe/Moscow", "MSK-3"},
    {"America/New_York", "America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "America/Phoenix", "MST7"},
    {"America/Los_Angeles", "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Asia/Kolkata", "Asia/Kolkata", "IST-5:30"},
    {"Asia/Shanghai", "Asia/Shanghai", "CST-8"},
    {"Asia/Tokyo", "Asia/Tokyo", "JST-9"},
    {"Australia/Sydney", "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
constexpr uint8_t kTimeZoneCount = sizeof(kTimeZones) / sizeof(kTimeZones[0]);

// Look a zone up by IANA name. Persisting the name rather than an index means
// reordering this table can never silently move someone to another timezone.
const TimeZoneOption& timeZoneByIana(const String& iana)
{
  for (uint8_t i = 0; i < kTimeZoneCount; ++i) {
    if (iana == kTimeZones[i].iana) {
      return kTimeZones[i];
    }
  }
  // Fall back to the compile-time default, then to UTC.
  for (uint8_t i = 0; i < kTimeZoneCount; ++i) {
    if (String(weather_config::kDefaultTimezone) == kTimeZones[i].iana) {
      return kTimeZones[i];
    }
  }
  return kTimeZones[0];
}

Coordinates resolveCoordinates()
{
  const TimeZoneOption& zone = timeZoneByIana(currentTimezone);
  return {currentLatitude, currentLongitude, currentLocationLabel.c_str(), zone.iana, zone.posix};
}

// posixTz must be a POSIX TZ string, not an IANA name - newlib's tzset() cannot
// resolve "Europe/Berlin" and silently falls back to UTC if given one.
bool updateClock(const char* posixTz, ForecastData& forecast)
{
  configTzTime(posixTz, "pool.ntp.org", "time.nist.gov");
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
  String url = F("http://api.open-meteo.com/v1/forecast?");
  url += F("latitude=");
  url += String(coordinates.latitude, 4);
  url += F("&longitude=");
  url += String(coordinates.longitude, 4);
  url += F("&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,wind_speed_10m_max");
  // Header preset data. Requested unconditionally so switching presets never
  // needs a refetch, and there is only one URL shape to reason about.
  url += F(",uv_index_max,sunrise,sunset,precipitation_sum,wind_gusts_10m_max");
  url += F("&current=temperature_2m,apparent_temperature");
  url += F("&forecast_days=5");
  url += useFahrenheit ? F("&temperature_unit=fahrenheit") : F("&temperature_unit=celsius");
  url += useMph ? F("&wind_speed_unit=mph") : F("&wind_speed_unit=kmh");
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
  // Header extras. All optional: a missing field leaves the default, and the
  // renderer falls back to the city name / "--:--" rather than showing nonsense.
  const JsonArray uvIndex = daily["uv_index_max"].as<JsonArray>();
  if (!uvIndex.isNull() && uvIndex.size() > 0 && !uvIndex[0].isNull()) {
    forecast.uvIndexMax = static_cast<int>(lroundf(uvIndex[0].as<float>()));
  }
  const JsonArray precipitationSum = daily["precipitation_sum"].as<JsonArray>();
  if (!precipitationSum.isNull() && precipitationSum.size() > 0 && !precipitationSum[0].isNull()) {
    // Tenths of a millimetre, so 0.4 mm does not round away to "0mm".
    forecast.precipitationMm = static_cast<int>(lroundf(precipitationSum[0].as<float>() * 10.0f));
  }
  const JsonArray gusts = daily["wind_gusts_10m_max"].as<JsonArray>();
  if (!gusts.isNull() && gusts.size() > 0 && !gusts[0].isNull()) {
    forecast.windGusts = static_cast<int>(lroundf(gusts[0].as<float>()));
  }
  // Sunrise/sunset arrive as "2026-08-28T06:12"; keep the HH:MM tail.
  const JsonArray sunrise = daily["sunrise"].as<JsonArray>();
  if (!sunrise.isNull() && sunrise.size() > 0 && sunrise[0].as<const char*>() != nullptr) {
    const char* iso = sunrise[0].as<const char*>();
    if (strlen(iso) >= 16) {
      snprintf(forecast.sunrise, sizeof(forecast.sunrise), "%.5s", iso + 11);
    }
  }
  const JsonArray sunset = daily["sunset"].as<JsonArray>();
  if (!sunset.isNull() && sunset.size() > 0 && sunset[0].as<const char*>() != nullptr) {
    const char* iso = sunset[0].as<const char*>();
    if (strlen(iso) >= 16) {
      snprintf(forecast.sunset, sizeof(forecast.sunset), "%.5s", iso + 11);
    }
  }

  const JsonObject current = doc["current"];
  if (!current.isNull() && !current["temperature_2m"].isNull()) {
    forecast.currentTemp = static_cast<int>(lroundf(current["temperature_2m"].as<float>()));
    forecast.feelsLike = current["apparent_temperature"].isNull()
                             ? forecast.currentTemp
                             : static_cast<int>(lroundf(current["apparent_temperature"].as<float>()));
    forecast.currentValid = true;
  }

  updateClock(coordinates.posixTz, forecast);
  return true;
}

// Our own read of today's forecast. Deliberately conservative: one line of text,
// most severe condition wins. These are not official warnings and the labels say
// so - kAlertsHost / the "dwd" source exists for authoritative ones.
String deriveWarning(const ForecastDay& today)
{
  // Thunderstorm codes outrank everything else.
  if (today.weatherCode == 95 || today.weatherCode == 96 || today.weatherCode == 99) {
    return ui().warnStorm;
  }
  if (today.windSpeed >= weather_config::kWarnWindKmh) {
    return String(ui().warnWind) + " " + today.windSpeed;
  }
  // Temperature thresholds are compared in Celsius, so skip them when the user
  // asked for Fahrenheit rather than silently comparing against the wrong scale.
  if (!useFahrenheit && today.tempMin <= weather_config::kWarnFrostCelsius) {
    return ui().warnFrost;
  }
  if (!useFahrenheit && today.tempMax >= weather_config::kWarnHeatCelsius) {
    return ui().warnHeat;
  }
  if (today.precipitationProbability >= weather_config::kWarnPrecipitationPercent) {
    return String(ui().warnRain) + " " + today.precipitationProbability + "%";
  }
  return String();
}

uint8_t alertSeverityRank(const char* severity)
{
  if (severity == nullptr) {
    return 0;
  }
  if (strcmp(severity, "extreme") == 0) return 4;
  if (strcmp(severity, "severe") == 0) return 3;
  if (strcmp(severity, "moderate") == 0) return 2;
  if (strcmp(severity, "minor") == 0) return 1;
  return 0;
}

// Official DWD warnings via Bright Sky. Plain HTTP, so no TLS heap. Returns an
// empty string outside DWD coverage or when nothing is active, which the caller
// treats as "no warning" rather than an error.
String fetchDwdWarning()
{
  String url = weather_config::kAlertsHost;
  url += "?lat=";
  url += String(currentLatitude, 4);
  url += "&lon=";
  url += String(currentLongitude, 4);

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(client, url)) {
    addLog("Alerts HTTP begin failed.");
    return String();
  }
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    addLog(String("Alerts request failed: HTTP ") + httpCode);
    http.end();
    return String();
  }

  // Each alert carries long description_* and instruction_* strings; a busy
  // weather day would blow the heap if the whole document were materialised.
  // The filter admits only the two fields actually rendered.
  JsonDocument filter;
  JsonObject alertFilter = filter["alerts"].add<JsonObject>();
  alertFilter["severity"] = true;
  alertFilter["event_en"] = true;
  alertFilter["event_de"] = true;

  JsonDocument doc;
  const DeserializationError error =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    addLog(String("Alerts JSON parse failed: ") + error.c_str());
    return String();
  }

  const JsonArray alerts = doc["alerts"].as<JsonArray>();
  if (alerts.isNull() || alerts.size() == 0) {
    return String();
  }

  // Most severe wins; Bright Sky offers only English and German event names, so
  // Spanish and French fall back to English.
  const bool german = (currentLanguage == UiLanguage::German);
  uint8_t bestRank = 0;
  String bestEvent;
  for (JsonObject alert : alerts) {
    const uint8_t rank = alertSeverityRank(alert["severity"].as<const char*>());
    if (rank < bestRank) {
      continue;
    }
    const char* event = german ? alert["event_de"].as<const char*>() : alert["event_en"].as<const char*>();
    if (event == nullptr) {
      event = alert["event_en"].as<const char*>();
    }
    if (event != nullptr) {
      bestRank = rank;
      bestEvent = event;
    }
  }
  if (!bestEvent.isEmpty()) {
    addLog(String("DWD alert: ") + bestEvent);
  }
  // Alert names come from DWD in German and routinely contain umlauts.
  return toDisplayAscii(bestEvent);
}

// Recompute the header warning for this refresh, per the configured source.
void updateActiveWarning(const ForecastData& forecast)
{
  activeWarning = "";
  if (warningSource == "off") {
    return;
  }
  if (warningSource == "dwd") {
    activeWarning = fetchDwdWarning();
    return;
  }
  activeWarning = deriveWarning(forecast.days[0]);
}

bool fetchForecast(ForecastData& forecast)
{
  const Coordinates coordinates = resolveCoordinates();
  const String url = buildForecastUrl(coordinates);

  addLog("Requesting Open-Meteo forecast.");

  // Use plain HTTP for the public Open-Meteo API. The previous code used HTTPS
  // with setInsecure() which disabled certificate verification entirely —
  // providing no actual security while wasting ~40KB of heap for the TLS stack.
  // Open-Meteo returns only public weather data (no auth tokens or secrets),
  // so plain HTTP is acceptable and more reliable on constrained devices.
  WiFiClient client;
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
// The 4bpp sprite for a day's conditions. Shared by the full-size and reduced
// icon renderers so the mapping lives in one place.
const uint8_t* weatherIconData(const ForecastDay& day)
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

  return kWeatherIcon40[iconIndex];
}

void drawWeatherIcon(const ForecastDay& day, int16_t centerX, int16_t topY)
{
  const uint8_t* icon = weatherIconData(day);
  if (icon == nullptr) {
    return;
  }
  const int16_t x = centerX - (kWeatherIcon40Width / 2);
  // Keep a white icon background so the glyphs remain visible.
  epaper.fillRect(x, topY, kWeatherIcon40Width, kWeatherIcon40Height, TFT_WHITE);
  // Icon data is 4bpp palette indices. Push into the 4bpp e-paper sprite.
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

  // textWidth works with the currently selected free font.
  const uint16_t w = epaper.textWidth(text);
  const uint16_t h = (font != nullptr) ? font->yAdvance : 12;
  const int16_t left = x - static_cast<int16_t>(w / 2);
  const int16_t top = y - static_cast<int16_t>(h / 2);
  drawDegreeSymbol(left + w + 5, top + 9, 2, fg);

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

// Fold UTF-8 accented Latin characters down to ASCII for the panel.
//
// TFT_eSPI decodes UTF-8 into a code point, but the GLCD font is a 256-glyph
// CP437-style set: U+00D6 lands on a box-drawing character, not "OE". Every
// string this project authors already avoids umlauts for exactly that reason
// ("Laengengrad", "Schlaeft in"), but text we do not control - DWD alert names
// like "STURMBOEEN", a location label someone types - has to be folded here or
// it renders as garbage.
String toDisplayAscii(const String& text)
{
  struct Mapping
  {
    uint16_t code;
    const char* ascii;
  };
  static const Mapping kMappings[] = {
      {0x00C4, "AE"}, {0x00D6, "OE"}, {0x00DC, "UE"}, {0x00E4, "ae"}, {0x00F6, "oe"},
      {0x00FC, "ue"}, {0x00DF, "ss"}, {0x00C0, "A"},  {0x00C1, "A"},  {0x00C2, "A"},
      {0x00C8, "E"},  {0x00C9, "E"},  {0x00CA, "E"},  {0x00CD, "I"},  {0x00D3, "O"},
      {0x00D4, "O"},  {0x00DA, "U"},  {0x00E0, "a"},  {0x00E1, "a"},  {0x00E2, "a"},
      {0x00E7, "c"},  {0x00E8, "e"},  {0x00E9, "e"},  {0x00EA, "e"},  {0x00ED, "i"},
      {0x00F3, "o"},  {0x00F4, "o"},  {0x00F1, "n"},  {0x00FA, "u"},
  };
  constexpr uint8_t kMappingCount = sizeof(kMappings) / sizeof(kMappings[0]);

  String out;
  out.reserve(text.length());
  for (uint16_t i = 0; i < text.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (c < 0x80) {
      out += static_cast<char>(c);
      continue;
    }
    // Two-byte UTF-8 covers the whole Latin-1 supplement, which is all the
    // accented text these APIs produce.
    if ((c & 0xE0) == 0xC0 && (i + 1) < text.length()) {
      const uint16_t code = ((c & 0x1F) << 6) | (static_cast<uint8_t>(text[i + 1]) & 0x3F);
      ++i;
      bool mapped = false;
      for (uint8_t m = 0; m < kMappingCount; ++m) {
        if (kMappings[m].code == code) {
          out += kMappings[m].ascii;
          mapped = true;
          break;
        }
      }
      if (!mapped) {
        out += '?';
      }
      continue;
    }
    // Longer sequences (and stray continuation bytes) have no ASCII equivalent.
    if ((c & 0xF0) == 0xE0) {
      i += 2;
    } else if ((c & 0xF8) == 0xF0) {
      i += 3;
    }
    out += '?';
  }
  return out;
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

// Push the framebuffer to the panel and report whether the panel actually
// completed the refresh. Distinguishes "the image was drawn" from "the driver
// gave up waiting on BUSY", which look identical from the outside: both return,
// and a panel that cannot drive its high-voltage rails stays blank either way.
void panelUpdate(const char* what)
{
  if (!panelReady) {
    return;
  }
  const uint32_t start = millis();
  epdBusyTimedOut = false;
  epaper.update();
  const uint32_t elapsed = millis() - start;
  if (epdBusyTimedOut) {
    addLog(String("Panel BUSY timed out during ") + what + " after " + elapsed + " ms; image NOT updated.");
  } else {
    addLog(String("Panel refresh (") + what + ") completed in " + elapsed + " ms.");
  }
}

void renderSetupScreen(const String& title, const String& line1, const String& line2)
{
  if (!panelReady) {
    return;
  }
  epaper.setRotation(1);
  epaper.setTextWrap(false, false);
  // fillSprite, not fillScreen: fillScreen is bounded by the sprite's unrotated
  // 128 px width, so in landscape it clears only the left 128 columns and leaves
  // the rest of the previous frame behind. The classic layout hid that by
  // repainting every pixel; any layout with white space does not.
  epaper.fillSprite(TFT_WHITE);
  epaper.fillRect(0, 0, epaper.width(), 24, TFT_RED);
  epaper.setTextColor(TFT_WHITE, TFT_RED);
  epaper.drawString(title, 6, 4, 2);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.drawString(line1, 8, 42, 2);
  epaper.drawString(line2, 8, 66, 1);
  panelUpdate("setup screen");
}

#ifdef EPAPER_ENABLE
// Reset the panel and wait for it to report ready, with a bound on the wait.
//
// This deliberately duplicates what the driver's init does, because the driver's
// CHECK_BUSY() spins forever: calling epaper.begin() on an unresponsive panel
// hangs setup() before Wi-Fi starts, leaving the device blank AND unreachable.
// Probing first means we only enter the driver when the panel is actually
// answering. Returns false if BUSY never goes high.
// Probe the BUSY line's electrical behaviour. A panel that is connected but busy
// actively drives the line low, so it reads low with the internal pull-up on. A
// line with no connection at all (unseated FPC, broken trace) floats, so it reads
// low bare but high with the pull-up. This distinguishes a stuck panel from a
// panel that is not electrically there.
void logBusyLineState()
{
  pinMode(TFT_BUSY, INPUT);
  delay(5);
  const int floating = digitalRead(TFT_BUSY);
  pinMode(TFT_BUSY, INPUT_PULLUP);
  delay(5);
  const int pulledUp = digitalRead(TFT_BUSY);
  pinMode(TFT_BUSY, INPUT);
  addLog(String("BUSY line: floating=") + floating + " pullup=" + pulledUp +
         (pulledUp && !floating ? " (line not driven - check FPC seating)"
                                : (!pulledUp ? " (panel actively holding BUSY low)" : "")));
}

bool resetPanelAndWaitReady(uint32_t resetLowMs, uint32_t timeoutMs)
{
  pinMode(TFT_BUSY, INPUT);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  delay(resetLowMs);
  digitalWrite(TFT_RST, HIGH);
  delay(50);

  const uint32_t start = millis();
  while (!digitalRead(TFT_BUSY)) {
    if ((millis() - start) > timeoutMs) {
      return false;
    }
    delay(1);
  }
  return true;
}
#endif

// Bring up the e-paper panel, tolerating a panel that will not respond.
//
// The panel is left in DSLP by EPaper::update() and the MCU then deep-sleeps, so
// on every wake it needs a hardware reset to answer at all. If it will not answer,
// booting continues headless: a dead panel must not cost us Wi-Fi and the web UI,
// which are the only means left to diagnose it.
void setupPanel(bool coldBoot)
{
#ifdef EPAPER_ENABLE
  (void)coldBoot;
  panelReady = false;

  // Release the sleep-time hold before touching RST, otherwise the reset pulse
  // cannot drive the line. Drive the pin to the level it was held at first:
  // releasing a hold on a pin whose output register disagrees glitches the line
  // (see the gpio_hold_dis documentation).
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  gpio_hold_dis(static_cast<gpio_num_t>(TFT_RST));
  delay(20);

  // Park the bus in its idle state before touching the panel. Resetting it with
  // a floating chip select is unreliable - the panel can come out of reset with
  // the bus mid-transaction and never raise BUSY. The driver's own init does
  // this first too, which is why calling begin() directly works where a bare
  // reset-then-probe does not.
  // A healthy panel raises BUSY within milliseconds of a reset, so keep the init
  // guard tight - it bounds how long a dead panel delays the boot.
  epdBusyTimeoutMs = kBusyTimeoutInitMs;

  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_RST, HIGH);
  delay(10);

  // begin() performs its own reset and full register init; the bounded
  // CHECK_BUSY() means it can no longer hang forever. Retry once behind a longer
  // reset pulse, which a panel climbing out of DSLP sometimes needs.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (attempt == 1) {
      digitalWrite(TFT_RST, LOW);
      delay(200);
      digitalWrite(TFT_RST, HIGH);
      delay(200);
    }
    epdBusyTimedOut = false;
    epaper.begin();
    if (!epdBusyTimedOut) {
      panelReady = true;
      epdBusyTimeoutMs = kBusyTimeoutRefreshMs;
      addLog(String("Panel ready (init attempt ") + (attempt + 1) + ").");
      return;
    }
    addLog(String("Panel BUSY timed out during init attempt ") + (attempt + 1) + ".");
  }

  epdBusyTimeoutMs = kBusyTimeoutRefreshMs;
  logBusyLineState();
  addLog("Panel not responding; continuing headless so the web UI stays reachable.");
#else
  (void)coldBoot;
#endif
}

// Map a real pack capacity onto the gauge's APA profile. The LC709203F only
// offers these six steps, so pick the nearest rather than rejecting anything that
// is not an exact match - a maker with a 400 mAh cell should be able to enter 400
// and get the closest profile, not a silent fallback to 1000.
lc709203_adjustment_t batteryPackProfile(uint16_t packMah)
{
  struct Step
  {
    uint16_t mah;
    lc709203_adjustment_t apa;
  };
  static const Step kSteps[] = {
      {100, LC709203F_APA_100MAH},   {200, LC709203F_APA_200MAH},
      {500, LC709203F_APA_500MAH},   {1000, LC709203F_APA_1000MAH},
      {2000, LC709203F_APA_2000MAH}, {3000, LC709203F_APA_3000MAH},
  };
  constexpr uint8_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

  uint8_t best = 0;
  uint32_t bestDistance = UINT32_MAX;
  for (uint8_t i = 0; i < kStepCount; ++i) {
    const uint32_t distance = (packMah > kSteps[i].mah) ? (packMah - kSteps[i].mah) : (kSteps[i].mah - packMah);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return kSteps[best].apa;
}

// Bring up the LC709203F on the free D4/D5 I2C pins. Called once per wake cycle
// (deep sleep resets the MCU, but the gauge itself stays powered and keeps
// tracking the pack, so its state-of-charge estimate survives across cycles).
// coldBoot is true for a power-on or reset, false when waking from deep sleep.
void setupBatteryGauge(bool coldBoot)
{
  batteryGaugeReady = false;
  if (!weather_config::kBatteryGaugeEnabled) {
    return;
  }

  Wire.begin(weather_config::kBatterySdaPin, weather_config::kBatterySclPin);
  if (!batteryGauge.begin(&Wire)) {
    addLog("Battery gauge not found on I2C (checked address 0x0B).");
    return;
  }

  batteryGauge.setPackSize(batteryPackProfile(batteryPackMah));
  if (weather_config::kBatteryThermistorB > 0) {
    batteryGauge.setTemperatureMode(LC709203F_TEMPERATURE_THERMISTOR);
    batteryGauge.setThermistorB(weather_config::kBatteryThermistorB);
  } else {
    // No NTC on the pack: keep the gauge on its internal I2C temperature register.
    batteryGauge.setTemperatureMode(LC709203F_TEMPERATURE_I2C);
  }
  // Keep the gauge running between updates. Operating mode costs ~15 uA (versus
  // ~0.2 uA asleep), which is negligible next to the rest of the board, and it
  // lets the RSOC algorithm keep tracking the pack while the MCU is asleep.
  batteryGauge.setPowerMode(LC709203F_POWER_OPERATE);

  if (coldBoot) {
    // Re-seed the charge estimate from the open-circuit voltage. Only on a cold
    // boot: doing it on every wake would throw away the gauge's own tracking.
    batteryGauge.initRSOC();
  }

  batteryGaugeReady = true;
  addLog(String("Battery gauge ready (IC version 0x") + String(batteryGauge.getICversion(), HEX) + ").");
}

// Consumed charge since the pack was last marked full, in microamp-hours.
// Persisted in NVS because deep sleep wipes RAM every cycle.
uint32_t consumedUah = 0;

void loadConsumedCharge()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading battery estimate.");
    return;
  }
  consumedUah = preferences.getUInt("uah_used", 0);
  preferences.end();
}

// Add one cycle's charge to the running total and persist it. Called from
// enterDeepSleep(), the one place that knows both how long this wake stayed
// awake and how long the coming sleep will be.
//
// Skipped entirely when a fuel gauge is present: the modelled figure is unused
// then, so accumulating it would only cost an NVS write every sleep cycle. If a
// gauge later disappears the stored value resumes from where it stopped and will
// read optimistically - the "Battery charged" button is the reset for that, and
// the "?" marker already flags the number as a guess.
void accumulateConsumedCharge(uint32_t awakeMs, uint32_t sleepMs)
{
  if (batteryGaugeReady) {
    return;
  }

  // mA * ms -> uAh:  mA * 1000 = uA;  ms / 3600000 = h.
  const uint64_t activeUah =
      (static_cast<uint64_t>(weather_config::kEstimatedActiveMa) * 1000ULL * awakeMs) / 3600000ULL;
  const uint64_t sleepUah =
      (static_cast<uint64_t>(weather_config::kEstimatedSleepUa) * sleepMs) / 3600000ULL;
  consumedUah += static_cast<uint32_t>(activeUah + sleepUah);

  if (!preferences.begin(kPrefsNamespace, false)) {
    return;
  }
  preferences.putUInt("uah_used", consumedUah);
  preferences.end();
}

// Reset the estimate to "full". The modeled value has no way to detect charging,
// so it needs this explicit zero point - the web UI exposes it as a button.
void resetConsumedCharge()
{
  consumedUah = 0;
  if (!preferences.begin(kPrefsNamespace, false)) {
    return;
  }
  preferences.putUInt("uah_used", 0);
  preferences.end();
}

// Modeled state of charge from accumulated run time. Only meaningful relative to
// the last "battery charged" reset.
uint8_t estimatedBatteryPercent()
{
  const uint32_t capacityUah = static_cast<uint32_t>(batteryPackMah) * 1000UL;
  if (consumedUah >= capacityUah) {
    return 0;
  }
  return static_cast<uint8_t>(((capacityUah - consumedUah) * 100ULL) / capacityUah);
}

// Previous cell voltage in millivolts, kept in RTC memory so it survives deep
// sleep without an NVS write - the per-cycle write was just removed and must not
// come back by another route. 0 means "no previous reading".
RTC_DATA_ATTR uint16_t rtcLastCellMillivolts = 0;

// Charging cannot be measured on this board: no charger signal reaches the XIAO.
// Two independent inferences cover the realistic cases between them:
//
//   * usbHostConnected - SOF frames from a USB host. True on the bench, but a
//     plain wall charger supplies VBUS without enumerating, so it stays false.
//   * a rising cell voltage, or one sitting near the 4.2 V charge ceiling, which
//     is what catches the wall-charger case.
//
// Both are guesses, not measurements, and the UI labels them as such.
void updatePowerState(const BatteryStatus& battery)
{
#if ARDUINO_USB_CDC_ON_BOOT
  usbHostConnected = Serial.isPlugged();
#else
  usbHostConnected = false;
#endif

  batteryCharging = false;
  if (!battery.valid || battery.estimated) {
    return;
  }

  const uint16_t millivolts = static_cast<uint16_t>(battery.volts * 1000.0f);
  if (millivolts >= 4180) {
    batteryCharging = true;
  } else if (rtcLastCellMillivolts != 0 && millivolts > (rtcLastCellMillivolts + 15)) {
    batteryCharging = true;
  }
  rtcLastCellMillivolts = millivolts;
}

// Single seam for battery state. Prefers the fuel gauge; falls back to the
// modeled estimate so the UI can still show something useful (flagged as a
// guess) when no gauge is fitted.
BatteryStatus readBattery()
{
  BatteryStatus status = {};
  if (!batteryGaugeReady) {
    status.valid = true;
    status.estimated = true;
    status.percent = estimatedBatteryPercent();
    return status;
  }

  const float volts = batteryGauge.cellVoltage();
  const float percent = batteryGauge.cellPercent();
  // A disconnected pack (or a failed transfer) reads as NaN or a nonsense voltage.
  if (isnan(volts) || isnan(percent) || volts < 2.0f || volts > 5.0f) {
    return status;
  }

  status.valid = true;
  status.volts = volts;
  status.percent = static_cast<uint8_t>(constrain(percent, 0.0f, 100.0f) + 0.5f);
  return status;
}

String batteryVoltsText(const BatteryStatus& battery)
{
  return String(battery.volts, 2);
}

// Battery glyph for the header: white outline, white interior so the overlaid
// percentage stays legible, and a yellow (red when low) fill bar for the charge
// level. Occupies kBatteryIconWidth x kBatteryIconHeight pixels from (x, y).
void drawBatteryIcon(int32_t x, int32_t y, const BatteryStatus& battery)
{
  constexpr int32_t kBodyWidth = 28;
  constexpr int32_t kBodyHeight = 13;
  const int32_t interiorWidth = kBodyWidth - 2;
  const int32_t interiorHeight = kBodyHeight - 2;

  // Body outline plus the positive terminal nub on the right.
  epaper.drawRect(x, y, kBodyWidth, kBodyHeight, TFT_WHITE);
  epaper.fillRect(x + kBodyWidth, y + 4, 2, 5, TFT_WHITE);
  epaper.fillRect(x + 1, y + 1, interiorWidth, interiorHeight, TFT_WHITE);

  if (battery.valid) {
    const int32_t fillWidth = (interiorWidth * battery.percent) / 100;
    if (fillWidth > 0) {
      const uint16_t fillColor =
          (battery.percent <= weather_config::kBatteryLowPercent) ? TFT_RED : TFT_YELLOW;
      epaper.fillRect(x + 1, y + 1, fillWidth, interiorHeight, fillColor);
    }
  }

  // Single-argument setTextColor draws with a transparent background, so the
  // digits sit on top of the fill bar instead of erasing it.
  epaper.setTextColor(TFT_BLACK);
  epaper.setTextDatum(TL_DATUM);
  // A modeled value gets a trailing "?" so an estimate is never mistaken for a
  // measurement; with neither, just the "?".
  String label = String("?");
  if (battery.valid) {
    label = String(battery.percent) + (battery.estimated ? "?" : "");
  }
  epaper.drawCentreString(label, x + kBodyWidth / 2, y + 3, 1);
  // Restore an opaque text color for the callers that follow.
  epaper.setTextColor(TFT_WHITE, TFT_BLACK);
}

// The header's first line, when there is nothing more urgent to say. Each preset
// deliberately avoids anything the day cards already show: they carry the icon,
// the label, the max temp, the min temp and the precipitation *probability*, so
// repeating those here would waste the line.
//
// No degree character: the GLCD font is CP437-style and cannot render one, and a
// right-aligned string has no fixed x for drawDegreeSymbol() to draw a circle at.
String buildHeaderPreset(const ForecastData& forecast, const String& mode)
{
  const ForecastDay& today = forecast.days[0];

  if (mode == "off") {
    return String();
  }

  if (mode == "rain") {
    // Amount, which the panel never shows, alongside the probability it does.
    String line = String(ui().labelRain) + " ";
    line += String(forecast.precipitationMm / 10);
    if ((forecast.precipitationMm % 10) != 0) {
      line += "." + String(forecast.precipitationMm % 10);
    }
    line += "mm " + String(today.precipitationProbability) + "%";
    return line;
  }

  if (mode == "sun") {
    return String(ui().labelSun) + " " + forecast.sunrise + "-" + forecast.sunset;
  }

  if (mode == "wind") {
    String line = String(ui().labelWind) + " " + today.windSpeed;
    if (forecast.windGusts > today.windSpeed) {
      line += String(" ") + ui().labelGust + " " + forecast.windGusts;
    }
    return line;
  }

  // "now": feels-like and UV. The actual temperature is on the left of the
  // header, so showing it again here would be the duplication this replaced.
  String line;
  if (forecast.currentValid) {
    line = String(ui().labelFeelsLike) + " " + forecast.feelsLike;
  }
  if (forecast.uvIndexMax > 0) {
    if (!line.isEmpty()) {
      line += " ";
    }
    line += String("UV ") + forecast.uvIndexMax;
  }
  return line;
}

// Draw the fixed top banner: current temperature on the left, status line and
// updated time right-aligned.
// Keeping the banner black preserves contrast for the red/yellow/white text colors.
void renderHeader(const ForecastData& forecast)
{
  const uint16_t displayWidth = epaper.width();
  epaper.fillRect(0, 0, displayWidth, kHeaderHeight, TFT_BLACK);
  // No rule under the banner. The black band against the white cards already
  // separates them, and removing it frees the bottom rows so a large temperature
  // can use the full banner height without painting over a line.
  // Left side, per the selected layout. Only this element and the resulting
  // clamp width differ between layouts; the right-hand column is identical in
  // all four. "temp" and "both" fall back to the city name when there is no
  // current reading yet, so the corner is never blank.
  const bool wantTemperature = (headerLayout == "temp" || headerLayout == "both") && forecast.currentValid;
  const bool wantLocation = (headerLayout == "location" || headerLayout == "both") ||
                            ((headerLayout == "temp") && !forecast.currentValid);
  uint16_t leftWidth = 0;

  if (wantTemperature) {
    const String tempText = String(forecast.currentTemp);
    // "temp" gets the big font; "both" deliberately stays on the smaller one.
    // An 18 pt digit is 25 px tall and lands on rows 1-25, which would run into
    // the city name that "both" draws underneath. Measured from the font header,
    // not guessed - do not "fix" this by making them consistent.
    const bool bigFont = (headerLayout == "temp");
    const GFXfont* font = bigFont ? &FreeSansBold18pt7b : &FreeSansBold12pt7b;
    drawFreeFontLeft(tempText, 4, 0, font, TFT_YELLOW, TFT_BLACK);
    epaper.setFreeFont(font);
    const uint16_t tempWidth = epaper.textWidth(tempText);
    epaper.setFreeFont(nullptr);
    // Free fonts carry no degree glyph; drawDegreeSymbol() draws the ring, which
    // is only possible because the left column sits at a known x. The ring scales
    // with the digits so it does not look stranded next to the larger font.
    drawDegreeSymbol(4 + tempWidth + (bigFont ? 5 : 4), bigFont ? 7 : 5, bigFont ? 3 : 2, TFT_YELLOW);
    leftWidth = tempWidth + (bigFont ? 14 : 10);
  }

  if (wantLocation) {
    if (headerLayout == "both") {
      // Small, tucked under the temperature, clear of the red rule at the bottom.
      epaper.setTextColor(TFT_YELLOW, TFT_BLACK);
      // Below the 12 pt digits (rows 1-18) and clear of the rule at y=29.
      epaper.drawString(forecast.location, 4, 20, 1);
      leftWidth = max(leftWidth, static_cast<uint16_t>(epaper.textWidth(forecast.location, 1)));
    } else {
      drawFreeFontLeft(forecast.location, 4, 2, &FreeSansBold9pt7b, TFT_YELLOW, TFT_BLACK);
      leftWidth = epaper.textWidth(forecast.location, 2);
    }
  }

  // A low pack gets a red strip along the bottom of the banner - visible at a
  // glance from across the room, unlike the small percentage in the glyph.
  if (batteryIsLow()) {
    // Yellow, not red: red on black is 2.7:1 and barely registers, while yellow
    // on black is 9.3:1 and reads as "attention" anyway.
    epaper.fillRect(0, kHeaderHeight - 3, displayWidth, 2, TFT_YELLOW);
  }

  // The battery glyph owns the far right of the banner; both text lines stop
  // short of it so nothing runs underneath.
  drawBatteryIcon(displayWidth - kBatteryIconWidth - 4, 4, currentBattery);
  const uint16_t textRightEdge = displayWidth - kBatteryIconMargin;

  // The first header line is a priority stack rather than a fixed field. The
  // SSID and IP are only worth ~190px of panel while they are new information -
  // after that the same space carries weather that changes every cycle.
  //
  //   1. network info  - cold boot, or the IP moved since last time
  //   2. warning       - red, when a warning source is enabled and something fired
  //   3. today         - the everyday case
  //
  // Network info outranks a warning because it is rare, self-clearing, and the
  // only way to find a device whose address changed; missing one warning cycle
  // after a power cut is the cheaper trade.
  String headerLine;
  uint16_t headerColor = TFT_YELLOW;
  bool warningBar = false;
  if (showNetworkInfo) {
    const String wifiName = currentSsid.isEmpty() ? String("AP") : currentSsid;
    const String ipText =
        (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("--.--.--.--");
    headerLine = String(ui().labelWifi) + ": " + wifiName + " " + ipText;
  } else if (!activeWarning.isEmpty()) {
    headerLine = activeWarning;
    // Drawn as black on a yellow bar below, not as coloured text: red on black
    // measures 2.7:1, which fails even the large-text bar, while black on yellow
    // is 9.3:1 and is what a caution strip looks like everywhere else.
    warningBar = true;
  } else {
    headerLine = buildHeaderPreset(forecast, headerMode);
  }

  // Three 8 px lines, which is what the 26 px banner bought. Clamp against
  // whatever the layout actually drew on the left, so "info" gets the full width.
  const uint16_t maxWidth = (textRightEdge > leftWidth + 12) ? (textRightEdge - leftWidth - 12) : 0;

  lastHeaderLine = headerLine;
  const String clampedLine = clampTextToWidth(headerLine, maxWidth, 1);
  if (warningBar && !clampedLine.isEmpty()) {
    // A filled bar sized to the text, so the warning reads as a caution strip
    // rather than as another line of status text.
    const uint16_t barWidth = epaper.textWidth(clampedLine, 1) + 6;
    epaper.fillRect(textRightEdge - barWidth, 0, barWidth + 2, 9, TFT_YELLOW);
    epaper.setTextColor(TFT_BLACK, TFT_YELLOW);
  } else {
    epaper.setTextColor(headerColor, TFT_BLACK);
  }
  epaper.drawRightString(clampedLine, textRightEdge - 2, 0, 1);

  // Line 2: the second preset, independent of the first. "off" leaves it blank
  // rather than drawing a stale value.
  const String secondLine = buildHeaderPreset(forecast, headerMode2);
  lastHeaderLine2 = secondLine;
  if (!secondLine.isEmpty()) {
    epaper.setTextColor(TFT_YELLOW, TFT_BLACK);
    epaper.drawRightString(clampTextToWidth(secondLine, maxWidth, 1), textRightEdge, 9, 1);
  }

  // Line 3: when the panel was last updated.
  const String updatedLabel = String(ui().labelUpdated) + ": " + forecast.updatedDay + " " + forecast.updatedAt;
  epaper.setTextColor(TFT_WHITE, TFT_BLACK);
  epaper.drawRightString(clampTextToWidth(updatedLabel, maxWidth, 1), textRightEdge, 18, 1);
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

  // Black, not yellow: yellow on white measures 1.6:1, so the old divider was
  // ink spent on something nobody could see.
  epaper.drawFastHLine(x, dividerY, width, TFT_BLACK);
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
// Half-size icon, produced by box-reducing the 40x40 art rather than shipping a
// second icon set. Each 2x2 source block collapses to one pixel, preferring any
// non-white pixel in the block so thin outlines survive the reduction - plain
// nearest-neighbour drops them and the glyph falls apart at 20 px.
void drawWeatherIcon20(const ForecastDay& day, int16_t x, int16_t y)
{
  const uint8_t* icon = weatherIconData(day);
  if (icon == nullptr) {
    return;
  }
  constexpr int16_t kSrc = kWeatherIcon40Width;
  for (int16_t oy = 0; oy < kSrc / 2; ++oy) {
    for (int16_t ox = 0; ox < kSrc / 2; ++ox) {
      uint8_t chosen = 0x00;  // white
      for (int16_t dy = 0; dy < 2; ++dy) {
        for (int16_t dx = 0; dx < 2; ++dx) {
          const int32_t index = (oy * 2 + dy) * kSrc + (ox * 2 + dx);
          const uint8_t byte = pgm_read_byte(&icon[index / 2]);
          const uint8_t value = (index & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
          if (value != 0x00) {
            chosen = value;
          }
        }
      }
      if (chosen != 0x00) {
        epaper.drawPixel(x + ox, y + oy, chosen);
      }
    }
  }
}

// A vertical bar whose fill height is the precipitation probability. A bar is
// read at a glance; "P35%" has to be parsed. Yellow fill inside a black outline -
// yellow alone on white is 1.6:1, but the outline supplies the edge.
void drawPrecipitationBar(int16_t x, int16_t y, int16_t width, int16_t height, int percent)
{
  epaper.drawRect(x, y, width, height, TFT_BLACK);
  const int16_t inner = height - 2;
  int16_t filled = (inner * constrain(percent, 0, 100)) / 100;
  if (percent > 0 && filled < 1) {
    filled = 1;  // never render a non-zero chance as empty
  }
  if (filled > 0) {
    epaper.fillRect(x + 1, y + height - 1 - filled, width - 2, filled, TFT_YELLOW);
  }
}

// The "modern" design: today as a hero panel on the left, the remaining four days
// compact on the right with precipitation bars. Answers "what is it doing now"
// without reading, which five identical cards cannot.
void drawModernForecast(const ForecastData& forecast)
{
  const uint16_t width = epaper.width();
  const uint16_t height = epaper.height();
  constexpr int16_t kStripHeight = 14;
  constexpr int16_t kHeroWidth = 112;

  epaper.setTextWrap(false, false);
  // fillSprite, not fillScreen: fillScreen is bounded by the sprite's unrotated
  // 128 px width, so in landscape it clears only the left 128 columns and leaves
  // the rest of the previous frame behind. The classic layout hid that by
  // repainting every pixel; any layout with white space does not.
  epaper.fillSprite(TFT_WHITE);

  // --- status strip ---------------------------------------------------------
  // Yellow with black text when something is wrong, black with yellow text
  // otherwise. Both are 9.3:1; the colour swap is the signal.
  const bool warn = !activeWarning.isEmpty();
  epaper.fillRect(0, 0, width, kStripHeight, warn ? TFT_YELLOW : TFT_BLACK);
  const uint16_t stripFg = warn ? TFT_BLACK : TFT_YELLOW;
  const uint16_t stripBg = warn ? TFT_YELLOW : TFT_BLACK;
  epaper.setTextColor(stripFg, stripBg);

  drawBatteryIcon(width - kBatteryIconWidth - 4, 1, currentBattery);
  const uint16_t stripRight = width - kBatteryIconMargin;

  if (warn) {
    epaper.drawString(clampTextToWidth(activeWarning, stripRight - 8, 1), 4, 3, 1);
  } else {
    epaper.drawString(clampTextToWidth(forecast.location, 90, 1), 4, 3, 1);
    const String info = showNetworkInfo
                            ? (String(ui().labelWifi) + ": " + WiFi.localIP().toString())
                            : buildHeaderPreset(forecast, headerMode);
    epaper.drawRightString(clampTextToWidth(info, stripRight - 100, 1), stripRight, 3, 1);
  }

  // --- hero: today ----------------------------------------------------------
  const ForecastDay& today = forecast.days[0];
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);

  const String heroTemp = forecast.currentValid ? String(forecast.currentTemp) : String(today.tempMax);
  drawFreeFontLeft(heroTemp, 6, kStripHeight + 2, &FreeSansBold18pt7b, TFT_RED, TFT_WHITE);
  epaper.setFreeFont(&FreeSansBold18pt7b);
  const uint16_t heroWidth = epaper.textWidth(heroTemp);
  epaper.setFreeFont(nullptr);
  drawDegreeSymbol(6 + heroWidth + 5, kStripHeight + 9, 3, TFT_RED);

  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.drawString(weatherLabel(today), 6, kStripHeight + 32, 1);
  drawWeatherIcon(today, 6 + kWeatherIcon40Width / 2, kStripHeight + 44);

  // High/low, then the day's rainfall in millimetres - the amount, which the
  // classic design never shows.
  const String range = String(today.tempMin) + " / " + String(today.tempMax);
  epaper.drawString(range, 52, kStripHeight + 48, 1);
  String rain = String(forecast.precipitationMm / 10);
  if ((forecast.precipitationMm % 10) != 0) {
    rain += "." + String(forecast.precipitationMm % 10);
  }
  epaper.drawString(rain + "mm", 52, kStripHeight + 60, 1);
  epaper.drawString(String(forecast.sunrise) + "-" + forecast.sunset, 6, height - 10, 1);

  epaper.drawFastVLine(kHeroWidth, kStripHeight + 2, height - kStripHeight - 4, TFT_BLACK);

  // --- the next four days ---------------------------------------------------
  const int16_t columnWidth = (width - kHeroWidth) / 4;
  for (uint8_t i = 1; i < kForecastDays; ++i) {
    const ForecastDay& day = forecast.days[i];
    const int16_t cx = kHeroWidth + (i - 1) * columnWidth;
    const int16_t centre = cx + columnWidth / 2;

    drawCenteredText(day.weekday, centre, kStripHeight + 3, 1, TFT_BLACK, TFT_WHITE);
    drawWeatherIcon20(day, centre - 10, kStripHeight + 14);

    epaper.setTextColor(TFT_RED, TFT_WHITE);
    epaper.drawCentreString(String(day.tempMax), centre, kStripHeight + 37, 2);
    epaper.setTextColor(TFT_BLACK, TFT_WHITE);
    epaper.drawCentreString(String(day.tempMin), centre, kStripHeight + 55, 1);

    drawPrecipitationBar(centre - 9, kStripHeight + 68, 18, 22, day.precipitationProbability);
    epaper.drawCentreString(String(day.precipitationProbability) + "%", centre, kStripHeight + 93, 1);
  }
}

// Draw the whole frame into the sprite without pushing it to the panel.
//
// This split is what makes a live preview possible: everything here is RAM work
// and takes milliseconds, while panelUpdate() below spends ~19 s clocking the
// buffer out to the e-paper. A preview renders with proposed settings, reads the
// buffer back out as an image, and never touches the display.
//
// Deliberately not gated on panelReady - the sprite works whether or not the
// panel does, so previews still function on a device with a dead display.
void drawForecastToBuffer(const ForecastData& forecast)
{
  epaper.setRotation(1);
  if (panelDesign == "modern") {
    drawModernForecast(forecast);
    return;
  }
  epaper.setTextWrap(false, false);
  const uint16_t displayWidth = epaper.width();
  const uint16_t displayHeight = epaper.height();
  // fillSprite, not fillScreen: fillScreen is bounded by the sprite's unrotated
  // 128 px width, so in landscape it clears only the left 128 columns and leaves
  // the rest of the previous frame behind. The classic layout hid that by
  // repainting every pixel; any layout with white space does not.
  epaper.fillSprite(TFT_WHITE);
  renderHeader(forecast);

  // No gap: the banner runs right down to the cards, and its rule is the divider.
  const uint16_t usableTop = kHeaderHeight;
  // No bottom margin: those 4 px now belong to the taller banner, which keeps
  // cardHeight identical to before at 98 px.
  const uint16_t cardHeight = displayHeight - usableTop;
  const uint16_t totalGap = kCellGap * (kForecastDays - 1);
  const uint16_t cardWidth = (displayWidth - totalGap) / kForecastDays;

  for (uint8_t i = 0; i < kForecastDays; ++i) {
    const uint16_t cardX = i * (cardWidth + kCellGap);
    renderDayCard(forecast.days[i], cardX, usableTop, cardWidth, cardHeight);
    renderFooterMetrics(forecast.days[i], cardX, usableTop + cardHeight - 12, cardWidth);
  }
}

void renderForecast(const ForecastData& forecast)
{
  if (!panelReady) {
    return;
  }
  drawForecastToBuffer(forecast);
  panelUpdate("forecast");
}

void renderErrorScreen(const String& title, const String& detail)
{
  if (!panelReady) {
    return;
  }
  epaper.setRotation(1);
  epaper.setTextWrap(false, false);
  // fillSprite, not fillScreen: fillScreen is bounded by the sprite's unrotated
  // 128 px width, so in landscape it clears only the left 128 columns and leaves
  // the rest of the previous frame behind. The classic layout hid that by
  // repainting every pixel; any layout with white space does not.
  epaper.fillSprite(TFT_WHITE);
  epaper.fillRect(0, 0, epaper.width(), 22, TFT_RED);
  epaper.setTextColor(TFT_WHITE, TFT_RED);
  epaper.drawString("WEATHER ERROR", 6, 3, 2);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.drawString(title, 8, 38, 2);
  epaper.drawString(detail, 8, 62, 1);
  panelUpdate("error screen");
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
    addLog(String("Loaded stored credentials: SSID=") + currentSsid + " pwd_len=" + currentPassword.length());
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

// Load the deep sleep preference from NVS. Sleep is on by default; the toggle
// exists so the device can be kept permanently reachable while debugging.
void loadSleepPreference()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading sleep mode. Defaulting to enabled.");
    deepSleepEnabled = true;
    return;
  }
  deepSleepEnabled = preferences.getBool("sleep_on", true);
  preferences.end();
}

// Location, timezone and unit preferences. Defaults come from weather_config.h,
// so a device with empty NVS behaves exactly as the compile-time build did.
void loadDisplayPreferences()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading location/timezone. Using defaults.");
    return;
  }
  currentLatitude = preferences.getFloat("lat", weather_config::kDefaultLatitude);
  currentLongitude = preferences.getFloat("lon", weather_config::kDefaultLongitude);
  currentLocationLabel = preferences.getString("loc", weather_config::kLocationLabel);
  currentTimezone = preferences.getString("tz", weather_config::kDefaultTimezone);
  useFahrenheit = preferences.getBool("unit_t", false);
  useMph = preferences.getBool("unit_w", false);
  preferences.end();

  if (currentLocationLabel.isEmpty()) {
    currentLocationLabel = weather_config::kLocationLabel;
  }
  // Snap an unknown zone back to a known one so configTzTime() is never handed
  // something tzset() cannot parse.
  currentTimezone = timeZoneByIana(currentTimezone).iana;
}

void saveLocationPreference(float latitude, float longitude, const String& label)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving location.");
    return;
  }
  preferences.putFloat("lat", latitude);
  preferences.putFloat("lon", longitude);
  preferences.putString("loc", label);
  preferences.end();
  currentLatitude = latitude;
  currentLongitude = longitude;
  currentLocationLabel = label;
}

void saveTimezonePreference(const String& iana)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving timezone.");
    return;
  }
  preferences.putString("tz", iana);
  preferences.end();
  currentTimezone = iana;
}

void saveUnitPreferences(bool fahrenheit, bool mph)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving units.");
    return;
  }
  preferences.putBool("unit_t", fahrenheit);
  preferences.putBool("unit_w", mph);
  preferences.end();
  useFahrenheit = fahrenheit;
  useMph = mph;
}

void loadPowerPreferences()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading power options. Using defaults.");
    return;
  }
  quietHoursEnabled = preferences.getBool("quiet_on", false);
  quietStartHour = preferences.getUChar("quiet_s", 23);
  quietEndHour = preferences.getUChar("quiet_e", 6);
  batteryWarnEnabled = preferences.getBool("batt_on", true);
  batteryWarnPercent = preferences.getUChar("batt_pct", 20);
  preferences.end();
  if (quietStartHour > 23) quietStartHour = 23;
  if (quietEndHour > 23) quietEndHour = 6;
  if (batteryWarnPercent > 100) batteryWarnPercent = 20;
}

void savePowerPreferences()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving power options.");
    return;
  }
  preferences.putBool("quiet_on", quietHoursEnabled);
  preferences.putUChar("quiet_s", quietStartHour);
  preferences.putUChar("quiet_e", quietEndHour);
  preferences.putBool("batt_on", batteryWarnEnabled);
  preferences.putUChar("batt_pct", batteryWarnPercent);
  preferences.end();
}

// True when the battery is low enough to warrant warning and conserving. Applies
// to a modelled estimate as well as a measured one - a guess low enough to worry
// about is still worth acting on.
bool batteryIsLow()
{
  return batteryWarnEnabled && currentBattery.valid && currentBattery.percent <= batteryWarnPercent;
}

// Is the local hour inside the quiet window? Handles windows that wrap midnight
// (23->6), which is the normal case.
bool isQuietHourNow(const struct tm& localTime)
{
  if (!quietHoursEnabled || quietStartHour == quietEndHour) {
    return false;
  }
  const uint8_t hour = static_cast<uint8_t>(localTime.tm_hour);
  if (quietStartHour < quietEndHour) {
    return hour >= quietStartHour && hour < quietEndHour;
  }
  return hour >= quietStartHour || hour < quietEndHour;
}

// Seconds from now until the quiet window ends, so the device can sleep straight
// through the night instead of waking every interval to do nothing.
uint32_t secondsUntilQuietEnds(const struct tm& localTime)
{
  int32_t hoursAhead = static_cast<int32_t>(quietEndHour) - localTime.tm_hour;
  if (hoursAhead <= 0) {
    hoursAhead += 24;
  }
  const int32_t seconds = hoursAhead * 3600 - localTime.tm_min * 60 - localTime.tm_sec;
  return (seconds > 0) ? static_cast<uint32_t>(seconds) : 60;
}

bool isHeaderModeSupported(const String& mode)
{
  return mode == "now" || mode == "rain" || mode == "sun" || mode == "wind";
}

// The second line additionally accepts "off".
bool isHeaderMode2Supported(const String& mode)
{
  return mode == "off" || isHeaderModeSupported(mode);
}

bool isHeaderLayoutSupported(const String& layout)
{
  return layout == "temp" || layout == "location" || layout == "both" || layout == "info";
}

bool isPanelDesignSupported(const String& design)
{
  return design == "classic" || design == "modern";
}

void loadHeaderPreference()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading header mode. Using default.");
    return;
  }
  const String stored = preferences.getString("hdr_mode", "now");
  const String stored2 = preferences.getString("hdr_mode2", "sun");
  const String storedLayout = preferences.getString("layout", "temp");
  const String storedDesign = preferences.getString("design", "classic");
  preferences.end();
  if (isHeaderModeSupported(stored)) {
    headerMode = stored;
  }
  if (isHeaderMode2Supported(stored2)) {
    headerMode2 = stored2;
  }
  if (isHeaderLayoutSupported(storedLayout)) {
    headerLayout = storedLayout;
  }
  if (isPanelDesignSupported(storedDesign)) {
    panelDesign = storedDesign;
  }
}

void saveHeaderPreference(const String& mode)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving header mode.");
    return;
  }
  preferences.putString("hdr_mode", mode);
  preferences.end();
  headerMode = mode;
}

void saveHeaderMode2Preference(const String& mode)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving second header mode.");
    return;
  }
  preferences.putString("hdr_mode2", mode);
  preferences.end();
  headerMode2 = mode;
}

void savePanelDesignPreference(const String& design)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving panel design.");
    return;
  }
  preferences.putString("design", design);
  preferences.end();
  panelDesign = design;
}

void saveHeaderLayoutPreference(const String& layout)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving header layout.");
    return;
  }
  preferences.putString("layout", layout);
  preferences.end();
  headerLayout = layout;
}

bool isWarningSourceSupported(const String& source)
{
  return source == "off" || source == "derived" || source == "dwd";
}

void loadWarningPreference()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading warning source. Using default.");
    return;
  }
  const String stored = preferences.getString("warn_src", "derived");
  preferences.end();
  if (isWarningSourceSupported(stored)) {
    warningSource = stored;
  }
}

void saveWarningPreference(const String& source)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving warning source.");
    return;
  }
  preferences.putString("warn_src", source);
  preferences.end();
  warningSource = source;
}

// The IP is only interesting when it is new. Compare against the last one stored
// and, if it moved, show it on the panel for this cycle. NVS rather than RTC
// memory on purpose: a power cut is exactly when the address matters, and RTC
// memory does not survive one. Only written when it actually changes.
void noteCurrentIpAddress()
{
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  const String ip = WiFi.localIP().toString();
  if (!preferences.begin(kPrefsNamespace, false)) {
    return;
  }
  const String previous = preferences.getString("last_ip", "");
  if (previous != ip) {
    preferences.putString("last_ip", ip);
    showNetworkInfo = true;
    addLog(String("IP address changed to ") + ip + "; showing it on the panel this cycle.");
  }
  preferences.end();
}

// Capacities offered in the web UI. Deliberately more values than the gauge's six
// APA steps: makers should enter what their cell actually is, and the nearest
// profile is derived from it.
constexpr uint16_t kBatteryPackOptions[] = {100, 150, 200, 300, 400, 500, 600, 800,
                                            1000, 1200, 1500, 2000, 2500, 3000};

bool isBatteryPackSupported(uint16_t mah)
{
  for (uint8_t i = 0; i < (sizeof(kBatteryPackOptions) / sizeof(kBatteryPackOptions[0])); ++i) {
    if (kBatteryPackOptions[i] == mah) {
      return true;
    }
  }
  return false;
}

void loadBatteryPackPreference()
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading battery size. Using default.");
    return;
  }
  const uint16_t stored = preferences.getUShort("pack_mah", weather_config::kBatteryCapacityMah);
  preferences.end();
  if (isBatteryPackSupported(stored)) {
    batteryPackMah = stored;
  }
}

void saveBatteryPackPreference(uint16_t mah)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving battery size.");
    return;
  }
  preferences.putUShort("pack_mah", mah);
  preferences.end();
  batteryPackMah = mah;
}

uint32_t refreshIntervalMs()
{
  return static_cast<uint32_t>(refreshIntervalMinutes) * 60UL * 1000UL;
}

bool isRefreshIntervalSupported(uint16_t minutes)
{
  for (uint8_t i = 0; i < (sizeof(kRefreshMinutesOptions) / sizeof(kRefreshMinutesOptions[0])); ++i) {
    if (kRefreshMinutesOptions[i] == minutes) {
      return true;
    }
  }
  return false;
}

// Load the refresh interval from NVS. Longer intervals are the main way to trade
// forecast freshness for battery life, so this is a user setting rather than a
// compile-time constant.
void loadRefreshPreference()
{
  refreshIntervalMinutes = kDefaultRefreshMinutes;
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while loading refresh interval. Using default.");
    return;
  }
  const uint16_t stored = preferences.getUShort("refresh_min", kDefaultRefreshMinutes);
  preferences.end();
  if (isRefreshIntervalSupported(stored)) {
    refreshIntervalMinutes = stored;
  }
}

void saveRefreshPreference(uint16_t minutes)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving refresh interval.");
    return;
  }
  preferences.putUShort("refresh_min", minutes);
  preferences.end();
  refreshIntervalMinutes = minutes;
}

void saveSleepPreference(bool enabled)
{
  if (!preferences.begin(kPrefsNamespace, false)) {
    addLog("Preferences open failed while saving sleep mode.");
    return;
  }
  preferences.putBool("sleep_on", enabled);
  preferences.end();
  deepSleepEnabled = enabled;
}

// Hold the device awake for at least windowMs from now. The deadline only ever
// moves forward, so a short window can never cut a longer one short (e.g. the
// 5 minute cold-boot hold surviving the render's 2 minute window). It is also
// clamped to kMaxAwakeMs: deep sleep resets the MCU, so millis() is the time
// since this wake began and the cap can be compared against it directly.
void extendAwakeWindow(uint32_t windowMs)
{
  uint32_t candidate = millis() + windowMs;
  if (candidate > kMaxAwakeMs) {
    candidate = kMaxAwakeMs;
  }
  if (static_cast<int32_t>(candidate - awakeUntilMs) > 0) {
    awakeUntilMs = candidate;
  }
}

// Push back the deep sleep deadline. Call this from web handlers only - it
// counts and logs the request. Non-HTTP callers that just want to hold the
// device awake should call extendAwakeWindow() directly.
// Queue a forecast refresh + redraw instead of doing it inline. Collapses a
// burst of settings changes into a single render, which also saves ~80 s of
// awake time per save compared with rendering once per setting.
void requestRefresh()
{
  refreshPending = true;
  refreshRequestedMs = millis();
  extendAwakeWindow(kAwakeWindowMs);
}

void noteWebActivity()
{
  // Log the first request after a quiet spell, with the client and path. That is
  // exactly the event that re-arms the awake window, so if something on the LAN
  // (a router probing port 80, a scanner) is holding the device awake, these few
  // entries name it without flooding the 24-entry log ring.
  const uint32_t now = millis();
  if (webRequestCount == 0 || (now - lastWebRequestMs) > kWebQuietLogThresholdMs) {
    addLog(String("Web request from ") + server.client().remoteIP().toString() + " " + server.uri() +
           " after " + ((now - lastWebRequestMs) / 1000) + " s quiet.");
  }
  lastWebRequestMs = now;
  ++webRequestCount;
  extendAwakeWindow(kAwakeWindowMs);
}

// <option> list for the refresh-interval selector, marking the active value.
String buildBatteryPackOptions()
{
  String options;
  for (uint8_t i = 0; i < (sizeof(kBatteryPackOptions) / sizeof(kBatteryPackOptions[0])); ++i) {
    const uint16_t mah = kBatteryPackOptions[i];
    options += "<option value='";
    options += String(mah);
    options += "'";
    if (mah == batteryPackMah) {
      options += " selected";
    }
    options += ">";
    options += String(mah);
    options += " mAh</option>";
  }
  return options;
}

String buildTimezoneOptions()
{
  String options;
  for (uint8_t i = 0; i < kTimeZoneCount; ++i) {
    options += "<option value='";
    options += kTimeZones[i].iana;
    options += "'";
    if (currentTimezone == kTimeZones[i].iana) {
      options += " selected";
    }
    options += ">";
    options += kTimeZones[i].label;
    options += "</option>";
  }
  return options;
}

String buildRefreshOptions()
{
  String options;
  for (uint8_t i = 0; i < (sizeof(kRefreshMinutesOptions) / sizeof(kRefreshMinutesOptions[0])); ++i) {
    const uint16_t minutes = kRefreshMinutesOptions[i];
    options += "<option value='";
    options += String(minutes);
    options += "'";
    if (minutes == refreshIntervalMinutes) {
      options += " selected";
    }
    options += ">";
    options += String(minutes);
    options += " min</option>";
  }
  return options;
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
void rebuildCaptivePageCache();

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
  // Disable ESP-IDF auto-reconnect to prevent it from racing with our manual
  // reconnect logic. When both fire simultaneously, neither succeeds because
  // each tears down the other's in-progress association.
  WiFi.setAutoReconnect(false);
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

// Lightweight reconnect: reuses existing STA mode and skips full teardown.
// Used by the loop() reconnect path to avoid resetting the WiFi stack, which
// causes the ESP-IDF state machine to lose its association context.
bool reconnectWifi(const String& ssid, const String& password)
{
  if (ssid.isEmpty()) {
    return false;
  }

  // Only disconnect the STA link, don't erase stored config (false, false)
  // so the radio stays initialized and the mode stays WIFI_STA.
  staGotIp = false;
  WiFi.disconnect(false, false);
  // Brief settle time for the disconnect event to propagate through the
  // ESP-IDF event loop before we call begin() again.
  delay(200);

  addLog(String("Reconnecting to Wi-Fi SSID: ") + ssid);
  WiFi.begin(ssid.c_str(), password.c_str());

  if (waitForStaConnection(kWifiTimeoutMs)) {
    reconnectFailures = 0;
    lastReconnectAttemptMs = millis();
    addLog(String("Wi-Fi reconnected. IP: ") + WiFi.localIP().toString());
    return true;
  }

  addLog(String("Wi-Fi reconnect failed, status=") + wifiStatusLabel(WiFi.status()));
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
    addLog(String("Connecting to Wi-Fi SSID: ") + ssid + " (attempt " + attempt + "/" + kWifiConnectAttempts +
           ", pwd_len=" + password.length() + (targetNetwork ? ", BSSID pinned" : "") + ")");
    WiFi.disconnect(true, true);
    // Wait for the disconnect event to propagate through ESP-IDF before
    // calling begin(). Without this, the STA state machine can enter begin()
    // while still tearing down the previous association, causing the new
    // connection attempt to silently fail.
    delay(200);
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

    // 4WAY_TIMEOUT can be caused by BSSID pinning issues, radio interference,
    // or wrong password. Clear BSSID target and retry without pinning before
    // giving up. Only AUTH_FAIL is a definitive "wrong password" from the router.
    if (lastDisconnectReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        lastDisconnectReason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
      addLog("4-way handshake failed. Will retry without BSSID pinning.");
      targetNetwork = nullptr;
    }

    if (status == WL_CONNECT_FAILED || lastDisconnectReason == WIFI_REASON_AUTH_FAIL) {
      addLog("Authentication failure — check password.");
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

  // Start AP immediately — do NOT scan first. The blocking STA scan takes 2-5s
  // and delays AP visibility, which causes Apple CNA to miss its ~200ms probe
  // window. The captive page JS will trigger a delayed /scan request on load,
  // which briefly switches to AP_STA mode to scan and then switches back.
  WiFi.mode(WIFI_AP);
  // Let the ESP-IDF WiFi state machine settle after mode change. Without this,
  // softAP() can cause the AP to bounce (start → stop → start) as the
  // subsystem reinitializes, briefly hiding the SSID from clients.
  delay(150);
  if (!WiFi.softAP(kApSsid)) {
    addLog("Failed to start AP mode.");
    renderErrorScreen("AP failed", "Restart device");
    return;
  }

  apModeActive = true;
  // Start DNS immediately after softAP — Apple CNA sends its first DNS probe
  // within ~200ms of WiFi association. If DNS doesn't reply in time, CNA
  // silently gives up and never shows the captive portal sheet.
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  if (!serverStarted) {
    server.begin();
    serverStarted = true;
    addLog("HTTP control server started in AP mode.");
  }

  addLog(String("AP mode active. Connect to SSID: ") + kApSsid);
  addLog(String("AP IP: ") + WiFi.softAPIP().toString());
  // Pre-build the captive page HTML once so that request handlers can serve it
  // instantly (<1ms) without blocking the loop with String.replace() work.
  rebuildCaptivePageCache();
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
  json += "\",\"batteryValid\":";
  json += currentBattery.valid ? "true" : "false";
  json += ",\"batteryPercent\":";
  json += String(currentBattery.percent);
  json += ",\"batteryEstimated\":";
  json += currentBattery.estimated ? "true" : "false";
  json += ",\"panelReady\":";
  json += panelReady ? "true" : "false";
  json += ",\"refreshMinutes\":";
  json += String(refreshIntervalMinutes);
  json += ",\"latitude\":";
  json += String(currentLatitude, 4);
  json += ",\"longitude\":";
  json += String(currentLongitude, 4);
  json += ",\"locationLabel\":\"";
  json += jsonEscape(currentLocationLabel);
  json += "\",\"timezone\":\"";
  json += jsonEscape(currentTimezone);
  json += "\",\"fahrenheit\":";
  json += useFahrenheit ? "true" : "false";
  json += ",\"mph\":";
  json += useMph ? "true" : "false";
  json += ",\"quietEnabled\":";
  json += quietHoursEnabled ? "true" : "false";
  json += ",\"quietStart\":";
  json += String(quietStartHour);
  json += ",\"quietEnd\":";
  json += String(quietEndHour);
  json += ",\"batteryWarnEnabled\":";
  json += batteryWarnEnabled ? "true" : "false";
  json += ",\"batteryWarnPercent\":";
  json += String(batteryWarnPercent);
  json += ",\"batteryPackMah\":";
  json += String(batteryPackMah);
  json += ",\"batteryLow\":";
  json += batteryIsLow() ? "true" : "false";
  json += ",\"warningSource\":\"";
  json += jsonEscape(warningSource);
  json += "\",\"warningText\":\"";
  json += jsonEscape(activeWarning);
  json += "\",\"warningActive\":";
  json += activeWarning.isEmpty() ? "false" : "true";
  json += ",\"showNetworkInfo\":";
  json += showNetworkInfo ? "true" : "false";
  json += ",\"design\":\"";
  json += jsonEscape(panelDesign);
  json += "\",\"layout\":\"";
  json += jsonEscape(headerLayout);
  json += "\",\"headerMode2\":\"";
  json += jsonEscape(headerMode2);
  json += "\",\"headerLine2\":\"";
  json += jsonEscape(lastHeaderLine2);
  json += "\",\"coldBoot\":";
  json += bootWasCold ? "true" : "false";
  json += ",\"wakeCount\":";
  json += String(rtcWakeCount);
  json += ",\"headerLine\":\"";
  json += jsonEscape(lastHeaderLine);
  json += "\",\"currentTemp\":";
  json += String(currentForecast.currentValid ? currentForecast.currentTemp : 0);
  json += ",\"feelsLike\":";
  json += String(currentForecast.currentValid ? currentForecast.feelsLike : 0);
  json += ",\"uvIndexMax\":";
  json += String(currentForecast.uvIndexMax);
  json += ",\"precipitationTenthMm\":";
  json += String(currentForecast.precipitationMm);
  json += ",\"windGusts\":";
  json += String(currentForecast.windGusts);
  json += ",\"sunrise\":\"";
  json += currentForecast.sunrise;
  json += "\",\"sunset\":\"";
  json += currentForecast.sunset;
  json += "\",\"headerMode\":\"";
  json += jsonEscape(headerMode);
  json += "\",\"usbConnected\":";
  json += usbHostConnected ? "true" : "false";
  json += ",\"charging\":";
  json += batteryCharging ? "true" : "false";
  json += ",\"keepAwakeSecondsLeft\":";
  {
    const int32_t leftMs = static_cast<int32_t>(keepAwakeUntilMs - millis());
    json += String((keepAwakeUntilMs != 0 && leftMs > 0) ? (leftMs / 1000) : 0);
  }
  json += ",\"batteryVolts\":\"";
  json += currentBattery.valid ? batteryVoltsText(currentBattery) : String("");
  json += "\",\"deepSleepEnabled\":";
  json += deepSleepEnabled ? "true" : "false";
  // Diagnostics for the sleep cycle: how long until the device may sleep, how
  // many requests have pushed that deadline back, and how long it has been up.
  json += ",\"awakeSecondsLeft\":";
  {
    const int32_t remainingMs = static_cast<int32_t>(awakeUntilMs - millis());
    json += String(remainingMs > 0 ? (remainingMs / 1000) : 0);
  }
  json += ",\"webRequests\":";
  json += String(webRequestCount);
  json += ",\"uptimeSeconds\":";
  json += String(millis() / 1000);
  // Include live network config so the main page JS can pre-fill the static IP
  // fields with the current DHCP-assigned values (gateway, subnet, DNS).
  const bool isConnected = (WiFi.status() == WL_CONNECTED);
  json += ",\"dhcpIp\":\"";
  json += isConnected ? WiFi.localIP().toString() : String("");
  json += "\",\"dhcpGw\":\"";
  json += isConnected ? WiFi.gatewayIP().toString() : String("");
  json += "\",\"dhcpSubnet\":\"";
  json += isConnected ? WiFi.subnetMask().toString() : String("");
  json += "\",\"dhcpDns1\":\"";
  json += isConnected ? WiFi.dnsIP(0).toString() : String("");
  json += "\",\"dhcpDns2\":\"";
  json += isConnected ? WiFi.dnsIP(1).toString() : String("");
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
<html lang="{{LANG}}">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{TITLE}}</title>
  <style>
    :root {
      --page-bg: #f0b37e;
      --page-text: #001F4D;
      --card-bg: #ffffff;
      --card-text: #1f2a3a;
      --card-shadow: 0 2px 8px rgba(0,0,0,0.1);
      --card-title-color: #001F4D;
      --card-title-glow: 0 0 12px #FF4500, 0 0 20px #B22222, 0 0 30px #660000;
      --input-bg: #ffffff;
      --input-text: #111;
      --input-border: #ccc;
      --label-text: #1f2a3a;
      --info-bg: #f8f9fa;
      --info-text: #1f2a3a;
      --btn-bg: rgba(33,150,243,0.18);
      --btn-text: #14243a;
      --btn-border: rgb(33,150,243);
      --btn-save-bg: rgba(33,150,243,0.18);
      --btn-save-text: #14243a;
      --btn-save-border: rgb(33,150,243);
      --btn-danger-bg: rgba(244,67,54,0.18);
      --btn-danger-text: #4a0d0a;
      --btn-danger-border: rgb(244,67,54);
      --btn-nav-bg: rgba(76,175,80,0.18);
      --btn-nav-text: #0f4a22;
      --btn-nav-border: rgb(76,175,80);
      --log-bg: #0b2b5d;
      --log-text: #f2f6ff;
      --log-odd: #000;
      --log-even: #001F4D;
      --header-bg: var(--card-bg);
      --header-border: var(--input-border);
      --switch-off: #bbb;
      --switch-on: #007bff;
      --switch-thumb: #fff;
    }
    .dark-mode {
      --page-bg: #222;
      --page-text: #eee;
      --card-bg: #333;
      --card-text: #eee;
      --card-shadow: 0 2px 8px rgba(0,0,0,0.35);
      --card-title-color: #5AB1FF;
      --input-bg: #444;
      --input-text: #fff;
      --input-border: #666;
      --label-text: #eee;
      --info-bg: #2a2a2a;
      --info-text: #eee;
      --btn-text: rgb(220,232,255);
      --btn-danger-text: rgb(255,214,209);
      --btn-nav-text: rgb(200,247,209);
      --btn-save-text: rgb(220,232,255);
      --log-bg: #0a2453;
      --header-bg: #333;
      --header-border: #444;
      --switch-off: #555;
    }
    * { box-sizing: border-box; }
    html { height: 100%; margin: 0; }
    body {
      font-family: "Arial Unicode MS", "Noto Sans", Arial, sans-serif;
      margin: 0; padding: 0;
      background: var(--page-bg); color: var(--page-text);
      min-height: 100vh;
    }
    .page-header {
      padding: 12px 20px;
      background: var(--header-bg);
      border-bottom: 1px solid var(--header-border);
      display: flex; justify-content: space-between; align-items: center;
      position: relative;
    }
    .header-left { text-align: left; }
    h1 {
      font-size: 1.5em; margin: 6px 0;
      color: var(--card-title-color);
      /* No glow behind title text: haze around glyphs costs legibility. */
    }
    #summary { font-size: 0.85em; margin-top: 2px; }
    .header-right { display: flex; align-items: center; gap: 12px; }
    .lang-row select { padding: 6px 10px; border: 1px solid var(--input-border); border-radius: 4px; background: var(--input-bg); color: var(--input-text); font-size: 13px; }
    .switch { position: relative; display: inline-block; width: 34px; height: 20px; }
    /* Visually hidden, but still focusable: display:none took these out of the
       tab order, making every toggle on the page mouse-only. */
    .switch input { position: absolute; opacity: 0; width: 100%; height: 100%; margin: 0; cursor: pointer; }
    .switch input:focus-visible + .slider { outline: 3px solid var(--switch-on); outline-offset: 2px; }
    .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: var(--switch-off); transition: .3s; border-radius: 10px; }
    .slider:before { position: absolute; content: ""; height: 14px; width: 14px; left: 3px; bottom: 3px; background: var(--switch-thumb); transition: .3s; border-radius: 50%; }
    input:checked + .slider { background: var(--switch-on); }
    input:checked + .slider:before { transform: translateX(14px); }
    /* Multi-column flow, not grid. Grid lays items out in rows and a row is as
       tall as its tallest member, so the very tall Settings card pushed every
       later card - Logs especially - below it however much width was free. Columns
       let each card follow the one above it and spill into the next column, so
       the cards pack. Reading order becomes column-major, which is normal for a
       dashboard. */
    .dashboard {
      column-width: 340px; column-gap: 20px;
      max-width: 1900px; margin: 20px auto; padding: 0 20px;
    }
    .card {
      /* Never split a card across a column boundary. */
      break-inside: avoid; -webkit-column-break-inside: avoid;
      display: inline-block; width: 100%; margin: 0 0 20px;
      min-width: 0;
      background: var(--card-bg); color: var(--card-text);
      border-radius: 10px; padding: 16px;
      box-shadow: var(--card-shadow);
    }
    .card-title {
      font-size: 20px; margin: 0 0 12px 0;
      color: var(--card-title-color);
      /* No glow behind title text: haze around glyphs costs legibility. */
    }
    .card-group-title {
      font-size: 16px; margin: 14px 0 6px;
      color: var(--card-title-color);
      text-shadow: 0 0 8px #FF4500, 0 0 18px #B22222, 0 0 24px #660000;
    }
    .info-grid { display: grid; gap: 8px; margin: 10px 0; }
    .info-item {
      display: flex; justify-content: space-between; align-items: center;
      padding: 8px; background: var(--info-bg); border-radius: 5px;
      font-size: 0.9rem; color: var(--info-text);
    }
    .info-label { font-weight: bold; }
    .info-value { font-family: monospace; }
    /* Battery meter: outlined body with a nub on the right, mirroring the e-paper glyph. */
    .battery { display: inline-flex; align-items: center; gap: 6px; }
    .battery-body {
      position: relative; width: 42px; height: 14px;
      border: 1.5px solid currentColor; border-radius: 3px; padding: 1px;
    }
    .battery-body:after {
      content: ""; position: absolute; right: -4px; top: 3px;
      width: 2px; height: 6px; background: currentColor;
    }
    .battery-fill { height: 100%; background: #35b36a; border-radius: 1px; }
    .battery-fill.low { background: #d9534f; }
    .battery-unknown { opacity: 0.6; }
    .sleep-row {
      display: flex; justify-content: space-between; align-items: center; gap: 10px;
      margin-top: 10px; font-size: 12px; color: var(--info-text);
    }
    /* 128x296 buffer rotated a quarter turn into the 296x128 view the panel shows. */
    .panel-preview {
      /* Scaled up: at 1:1 the 296x128 panel is unreadably small on a phone. */
      height: 192px; display: flex; align-items: center; justify-content: center;
      overflow: hidden; margin: 10px 0;
      border: 1px solid var(--input-border); border-radius: 4px; background: #fff;
    }
    .panel-preview img {
      /* The buffer is portrait and its first row is the panel's right edge, so
         the quarter turn is anticlockwise. Turning it the other way renders the
         preview upside down. */
      transform: rotate(-90deg) scale(1.5);
      image-rendering: pixelated;
      max-width: none;
    }
    .credits {
      max-width: 1900px; margin: 0 auto 24px; padding: 0 20px;
      font-size: 12px; opacity: 0.75; text-align: center; color: var(--info-text);
    }
    .credits a { color: inherit; }
    .sleep-hint { font-size: 11px; opacity: 0.75; margin-top: 4px; color: var(--info-text); }
    label { display: block; margin: 10px 0 4px; font-size: 14px; color: var(--label-text); }
    select, input, textarea {
      width: 100%; padding: 10px; margin: 0;
      border: 1px solid var(--input-border); border-radius: 4px;
      background: var(--input-bg); color: var(--input-text);
      font-size: 14px;
    }
    input[type="checkbox"] { width: 18px; height: 18px; accent-color: #5aa7ff; }
    button, .button {
      display: inline-flex; align-items: center; justify-content: center;
      width: 100%; padding: 10px; margin: 0;
      border: 0.8px solid var(--btn-border); border-radius: 4px;
      background: var(--btn-bg); color: var(--btn-text);
      font-size: 14px; font-weight: 700; cursor: pointer;
      min-height: 44px;
    }
    .btn-save { background: var(--btn-save-bg); color: var(--btn-save-text); border-color: var(--btn-save-border); }
    .btn-danger { background: var(--btn-danger-bg); color: var(--btn-danger-text); border-color: var(--btn-danger-border); }
    .btn-nav { background: var(--btn-nav-bg); color: var(--btn-nav-text); border-color: var(--btn-nav-border); }
    .button-row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin: 10px 0; }
    .log-container {
      background: var(--log-bg); color: var(--log-text);
      border: 1px solid var(--input-border); border-radius: 6px;
      max-height: 320px; overflow-y: auto; padding: 0;
    }
    .log-container pre {
      margin: 0; padding: 10px; white-space: pre-wrap; word-break: break-word;
      font-size: 12px; line-height: 1.5; font-family: monospace;
    }
    @media (max-width: 1000px) {
      /* Narrower columns so a mid-size window still gets two rather than one. */
      .dashboard { column-width: 300px; }
    }
    @media (max-width: 600px) {
      body { padding: 0; }
      h1 { font-size: 1.3em; }
      .dashboard { padding: 0 10px; column-width: auto; column-count: 1; }
      .button-row { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <div class="page-header">
    <div class="header-left">
      <h1>{{TITLE}}</h1>
      <div id="summary">{{LOADING_STATE}}</div>
    </div>
    <div class="header-right">
      <div class="lang-row">
        <select id="language">{{LANGUAGE_OPTIONS}}</select>
      </div>
      <label class="switch" title="Dark mode">
        <input type="checkbox" id="darkToggle" aria-label="Dark mode">
        <span class="slider"></span>
      </label>
    </div>
  </div>
  <div class="dashboard">
    <div class="card">
      <h2 class="card-title">{{STATUS_TITLE}}</h2>
      <div id="status" class="info-grid"></div>
      <div class="sleep-row">
        <span>{{SLEEP_MODE_LABEL}}</span>
        <label class="switch" title="{{SLEEP_MODE_LABEL}}">
          <input type="checkbox" id="sleepToggle" aria-label="{{SLEEP_MODE_LABEL}}">
          <span class="slider"></span>
        </label>
      </div>
      <div class="sleep-hint">{{SLEEP_MODE_HINT}}</div>
      <label for="refreshMinutes">{{REFRESH_INTERVAL_LABEL}}</label>
      <select id="refreshMinutes">{{REFRESH_OPTIONS}}</select>
      <div class="button-row" style="grid-template-columns:1fr">
        <button class="btn-nav" onclick="markBatteryFull()">{{BATTERY_CHARGED_BUTTON}}</button>
      </div>
    </div>
    <div class="card">
      <h2 class="card-title">{{WIFI_SETUP_TITLE}}</h2>
      <label for="ssid">{{SSID_LABEL}}</label>
      <select id="ssid"></select>
      <label for="manual_ssid">{{HIDDEN_SSID_LABEL}}</label>
      <input id="manual_ssid" type="text" placeholder="Enter SSID manually">
      <label for="password">{{PASSWORD_LABEL}}</label>
      <input id="password" type="password" placeholder="WLAN password">
      <h3 class="card-group-title">{{STATIC_IP_TITLE}}</h3>
      <label style="display:flex;align-items:center;gap:8px;">
        <input id="static_enabled" type="checkbox" {{STATIC_IP_CHECKED}}>
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
      <div class="button-row">
        <button class="btn-save" onclick="saveWiFi()">{{SAVE_REBOOT}}</button>
        <button onclick="scanNetworks()">{{RESCAN}}</button>
      </div>
      <div class="button-row">
        <button class="btn-nav" onclick="refreshForecast()">{{REFRESH_FORECAST}}</button>
        <button class="btn-danger" onclick="forgetWiFi()">{{FORGET_WIFI}}</button>
      </div>
    </div>
    <div class="card">
      <h2 class="card-title">{{SETTINGS_TITLE}}</h2>
      <label for="locLabel">{{LOCATION_LABEL}}</label>
      <input type="text" id="locLabel" maxlength="24">
      <label for="lat">{{LATITUDE_LABEL}}</label>
      <input type="number" id="lat" step="0.0001" min="-90" max="90">
      <label for="lon">{{LONGITUDE_LABEL}}</label>
      <input type="number" id="lon" step="0.0001" min="-180" max="180">
      <label for="timezone">{{TIMEZONE_LABEL}}</label>
      <select id="timezone">{{TIMEZONE_OPTIONS}}</select>
      <label for="warningSource">{{WARNING_SOURCE_LABEL}}</label>
      <select id="warningSource">
        <option value="off">{{WARN_SRC_OFF}}</option>
        <option value="derived">{{WARN_SRC_DERIVED}}</option>
        <option value="dwd">{{WARN_SRC_DWD}}</option>
      </select>
      <label for="unitTemp">{{UNITS_LABEL}}</label>
      <select id="unitTemp">
        <option value="c">&deg;C</option>
        <option value="f">&deg;F</option>
      </select>
      <select id="unitWind">
        <option value="kmh">km/h</option>
        <option value="mph">mph</option>
      </select>
      <div class="sleep-row">
        <span>{{QUIET_HOURS_LABEL}}</span>
        <label class="switch"><input type="checkbox" id="quietEnabled" aria-label="{{QUIET_HOURS_LABEL}}"><span class="slider"></span></label>
      </div>
      <label for="quietStart">{{QUIET_FROM_LABEL}}</label>
      <input type="number" id="quietStart" min="0" max="23">
      <label for="quietEnd">{{QUIET_TO_LABEL}}</label>
      <input type="number" id="quietEnd" min="0" max="23">
      <label for="batteryPack">{{BATTERY_PACK_LABEL}}</label>
      <select id="batteryPack">{{BATTERY_PACK_OPTIONS}}</select>
      <div class="sleep-row">
        <span>{{BATTERY_WARN_LABEL}}</span>
        <label class="switch"><input type="checkbox" id="batteryWarnEnabled" aria-label="{{BATTERY_WARN_LABEL}}"><span class="slider"></span></label>
      </div>
      <input type="number" id="batteryWarnPercent" min="0" max="100">
      <div class="button-row" style="grid-template-columns:1fr">
        <button class="btn-save" onclick="saveSettings()">{{SAVE_BUTTON}}</button>
      </div>
      <div id="saveNote" class="sleep-hint"></div>
      <label>{{KEEP_AWAKE_LABEL}}</label>
      <div class="button-row" style="grid-template-columns:repeat(4,1fr)">
        <button class="btn-nav" onclick="keepAwake(15)">15m</button>
        <button class="btn-nav" onclick="keepAwake(30)">30m</button>
        <button class="btn-nav" onclick="keepAwake(60)">60m</button>
        <button class="btn-nav" onclick="keepAwake(0)">off</button>
      </div>
      <div id="keepAwakeState" class="sleep-hint"></div>
    </div>
    <div class="card">
      <h2 class="card-title">{{PREVIEW_TITLE}}</h2>
      <!-- The framebuffer is the panel's native portrait 128x296; rotating here
           avoids second-guessing the sprite's coordinate mapping on the device. -->
      <div class="panel-preview"><img id="panelImg" src="/panel.bmp" alt="{{PREVIEW_TITLE}}"></div>
      <div id="previewNote" class="sleep-hint"></div>
      <label for="design">{{DESIGN_LABEL}}</label>
      <select id="design">
        <option value="classic">{{DESIGN_CLASSIC}}</option>
        <option value="modern">{{DESIGN_MODERN}}</option>
      </select>
      <label for="layout">{{LAYOUT_LABEL}}</label>
      <select id="layout">
        <option value="temp">{{LAYOUT_TEMP}}</option>
        <option value="location">{{LAYOUT_LOCATION}}</option>
        <option value="both">{{LAYOUT_BOTH}}</option>
        <option value="info">{{LAYOUT_INFO}}</option>
      </select>
      <label for="headerMode">{{HEADER_MODE_LABEL}}</label>
      <select id="headerMode">
        <option value="now">{{HEADER_MODE_NOW}}</option>
        <option value="rain">{{HEADER_MODE_RAIN}}</option>
        <option value="sun">{{HEADER_MODE_SUN}}</option>
        <option value="wind">{{HEADER_MODE_WIND}}</option>
      </select>
      <label for="headerMode2">{{HEADER_MODE2_LABEL}}</label>
      <select id="headerMode2">
        <option value="off">{{MODE_OFF}}</option>
        <option value="now">{{HEADER_MODE_NOW}}</option>
        <option value="rain">{{HEADER_MODE_RAIN}}</option>
        <option value="sun">{{HEADER_MODE_SUN}}</option>
        <option value="wind">{{HEADER_MODE_WIND}}</option>
      </select>
      <div class="button-row" style="grid-template-columns:1fr">
        <button class="btn-save" onclick="saveSettings()">{{SAVE_BUTTON}}</button>
      </div>
    </div>
    <div class="card">
      <h2 class="card-title">{{OTA_TITLE}}</h2>
      <div class="sleep-hint">{{OTA_HINT}}</div>
      <input type="file" id="fwFile" accept=".bin">
      <div class="button-row" style="grid-template-columns:1fr">
        <button class="btn-save" onclick="uploadFirmware()">{{OTA_BUTTON}}</button>
      </div>
      <div id="otaProgress" class="sleep-hint"></div>
    </div>
    <div class="card">
      <h2 class="card-title">{{LOGS_TITLE}}</h2>
      <div class="log-container">
        <pre id="logs">{{LOADING_LOGS}}</pre>
      </div>
    </div>
  </div>
  <!-- Open-Meteo publish under CC BY 4.0 and ask for a credit with a link next to
       where their data is shown. The panel itself cannot carry a link, so the
       credit lives here and in the documentation. -->
  <footer class="credits">
    {{CREDIT_PREFIX}} <a href="https://open-meteo.com/" target="_blank" rel="noopener">Open-Meteo.com</a>
    (<a href="https://creativecommons.org/licenses/by/4.0/" target="_blank" rel="noopener">CC BY 4.0</a>)
    <span id="dwdCredit"></span>
  </footer>
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
      forecastMissing: '{{FORECAST_MISSING}}',
      statusBattery: '{{STATUS_BATTERY}}',
      statusSleepIn: '{{STATUS_SLEEP_IN}}',
      batteryUnknown: '{{BATTERY_UNKNOWN}}',
      batteryEstimatedNote: '{{BATTERY_ESTIMATED_NOTE}}',
      confirmSave: '{{CONFIRM_SAVE}}',
      previewUnsaved: '{{PREVIEW_UNSAVED}}',
      previewApplying: '{{PREVIEW_APPLYING}}'
    };
    let currentConnectedSsid = '';
    // Settings inputs are filled from the device only until the first fill
    // completes; after that they belong to the user until they save.
    let settingsLoaded = false;
    // Dark mode
    (function() {
      const toggle = document.getElementById('darkToggle');
      const stored = localStorage.getItem('darkMode');
      // No stored choice: follow the operating system rather than assuming light.
      const prefersDark = stored === null &&
        window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
      if (stored === '1' || prefersDark) {
        document.body.classList.add('dark-mode');
        toggle.checked = true;
      }
      toggle.addEventListener('change', function() {
        document.body.classList.toggle('dark-mode', this.checked);
        localStorage.setItem('darkMode', this.checked ? '1' : '0');
      });
    })();
    async function fetchJson(path, options) {
      const response = await fetch(path, options);
      if (!response.ok) throw new Error(await response.text());
      return response.json();
    }
    // Escape anything that came from the device before it reaches innerHTML.
    // Network names are attacker-chosen: an access point called
    // <img src=x onerror=...> would otherwise execute when the status card renders.
    function esc(v) {
      return String(v == null ? '' : v).replace(/[&<>"']/g,
        c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }
    function makeInfoItem(label, value, isHtml) {
      const shown = isHtml ? value : esc(value);
      return `<div class="info-item"><span class="info-label">${esc(label)}</span><span class="info-value">${shown}</span></div>`;
    }
    // Battery meter mirroring the e-paper glyph: outline plus a proportional fill.
    function makeBatteryValue(status) {
      if (!status.batteryValid) {
        return `<span class="battery battery-unknown"><span class="battery-body"></span>${ui.batteryUnknown}</span>`;
      }
      const percent = Math.max(0, Math.min(100, status.batteryPercent || 0));
      const low = percent <= {{BATTERY_LOW_PERCENT}} ? ' low' : '';
      const volts = status.batteryVolts ? ` (${status.batteryVolts} V)` : '';
      const note = status.batteryEstimated ? ` ~${ui.batteryEstimatedNote}` : '';
      const power = status.charging ? ' \u00b7 charging?' : (status.usbConnected ? ' \u00b7 USB' : '');
      return `<span class="battery"><span class="battery-body"><span class="battery-fill${low}" style="width:${percent}%"></span></span>${percent}%${volts}${note}${power}</span>`;
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
      // Auto-select current SSID after scan
      if (currentConnectedSsid) {
        for (const opt of select.options) {
          if (opt.value === currentConnectedSsid) { opt.selected = true; break; }
        }
      }
    }
    async function updateStatus() {
      const status = await fetchJson('/status');
      currentConnectedSsid = status.ssid || '';
      document.getElementById('summary').textContent =
        `${status.apMode ? ui.summaryAp : ui.summarySta} | ${status.wifiConnected ? ui.summaryConnected : ui.summaryNotConnected} | ${status.location || ui.summaryNoForecast}`;
      document.getElementById('status').innerHTML =
        makeInfoItem(ui.statusSsid, status.ssid || '-') +
        makeInfoItem(ui.statusIp, status.ip || '-') +
        makeInfoItem(ui.statusApIp, status.apIp || '-') +
        makeInfoItem(ui.statusWifiState, status.wifiStatus || '-') +
        makeInfoItem(ui.statusReason, status.disconnectReason || '-') +
        makeInfoItem(ui.statusForecast, status.forecastValid ? ui.forecastValid : ui.forecastMissing) +
        makeInfoItem(ui.statusUpdated, status.updated || '-') +
        makeInfoItem(ui.statusBattery, makeBatteryValue(status), true) +
        makeInfoItem(ui.statusSleepIn, status.deepSleepEnabled ? `${status.awakeSecondsLeft}s` : '-');
      // Populate the settings inputs ONCE. The poll runs every 8 s, and checking
      // only document.activeElement was not enough: as soon as focus left a
      // dropdown the next poll overwrote the user's choice with the stored value,
      // so Save then posted the old value back and the change appeared to be
      // ignored. After a save, saveSettings() re-syncs deliberately.
      const setIfIdle = (id, value) => {
        if (settingsLoaded) { return; }
        const el = document.getElementById(id);
        if (el) {
          if (el.type === 'checkbox') { el.checked = !!value; } else { el.value = value; }
        }
      };
      setIfIdle('locLabel', status.locationLabel);
      setIfIdle('lat', status.latitude);
      setIfIdle('lon', status.longitude);
      setIfIdle('timezone', status.timezone);
      setIfIdle('design', status.design);
      setIfIdle('layout', status.layout);
      setIfIdle('headerMode', status.headerMode);
      setIfIdle('headerMode2', status.headerMode2);
      setIfIdle('batteryPack', status.batteryPackMah);
      setIfIdle('warningSource', status.warningSource);
      const dwd = document.getElementById('dwdCredit');
      if (dwd) {
        dwd.innerHTML = status.warningSource === 'dwd'
          ? ' &middot; Warnings: Deutscher Wetterdienst via <a href="https://brightsky.dev/" target="_blank" rel="noopener">Bright Sky</a>'
          : '';
      }
      settingsLoaded = true;
      const ka = document.getElementById('keepAwakeState');
      if (ka) {
        ka.textContent = status.keepAwakeSecondsLeft > 0
          ? `awake for another ${Math.round(status.keepAwakeSecondsLeft / 60)} min`
          : '';
      }
      setIfIdle('unitTemp', status.fahrenheit ? 'f' : 'c');
      setIfIdle('unitWind', status.mph ? 'mph' : 'kmh');
      setIfIdle('quietEnabled', status.quietEnabled);
      setIfIdle('quietStart', status.quietStart);
      setIfIdle('quietEnd', status.quietEnd);
      setIfIdle('batteryWarnEnabled', status.batteryWarnEnabled);
      setIfIdle('batteryWarnPercent', status.batteryWarnPercent);
      const sleepToggle = document.getElementById('sleepToggle');
      // Do not fight the user mid-click: only sync the toggle when it is idle.
      if (sleepToggle && document.activeElement !== sleepToggle) {
        sleepToggle.checked = !!status.deepSleepEnabled;
      }
      const cb = document.getElementById('static_enabled');
      if (cb && !cb.checked && status.wifiConnected) {
        const fill = (id, val) => { const el = document.getElementById(id); if (el && !el.value) el.value = val || ''; };
        fill('static_ip', status.dhcpIp);
        fill('static_gw', status.dhcpGw);
        fill('static_subnet', status.dhcpSubnet);
        fill('static_dns1', status.dhcpDns1);
        fill('static_dns2', status.dhcpDns2);
      }
      // Pre-select current SSID in dropdown
      if (currentConnectedSsid) {
        const sel = document.getElementById('ssid');
        for (const opt of sel.options) {
          if (opt.value === currentConnectedSsid) { opt.selected = true; break; }
        }
      }
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
          ssid, password, staticEnabled,
          staticIp, staticGw, staticSubnet, staticDns1, staticDns2
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
    async function setSleepMode() {
      const enabled = document.getElementById('sleepToggle').checked;
      await fetch('/sleepMode', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({enabled})
      });
      await updateStatus();
      await updateLogs();
    }
    async function setRefreshInterval() {
      const minutes = parseInt(document.getElementById('refreshMinutes').value, 10);
      await fetch('/refreshInterval', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({minutes})
      });
      await updateStatus();
      await updateLogs();
    }
    async function markBatteryFull() {
      const response = await fetch('/batteryFull', {method: 'POST'});
      alert(await response.text());
      await updateStatus();
    }
    async function saveSettings() {
      if (!confirm(ui.confirmSave)) { return; }
      const label = document.getElementById('locLabel').value.trim();
      const latitude = parseFloat(document.getElementById('lat').value);
      const longitude = parseFloat(document.getElementById('lon').value);
      if (isNaN(latitude) || isNaN(longitude)) { alert('Latitude and longitude are required'); return; }
      let r = await fetch('/location', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({latitude, longitude, label})
      });
      if (!r.ok) { alert(await r.text()); return; }
      await fetch('/timezone', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({timezone: document.getElementById('timezone').value})
      });
      await fetch('/units', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          fahrenheit: document.getElementById('unitTemp').value === 'f',
          mph: document.getElementById('unitWind').value === 'mph'
        })
      });
      await fetch('/design', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({design: document.getElementById('design').value})
      });
      await fetch('/layout', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({layout: document.getElementById('layout').value})
      });
      await fetch('/headerMode', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          mode: document.getElementById('headerMode').value,
          mode2: document.getElementById('headerMode2').value
        })
      });
      await fetch('/batteryPack', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({mah: parseInt(document.getElementById('batteryPack').value, 10)})
      });
      await fetch('/warningSource', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({source: document.getElementById('warningSource').value})
      });
      r = await fetch('/powerOptions', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          quietEnabled: document.getElementById('quietEnabled').checked,
          quietStart: parseInt(document.getElementById('quietStart').value, 10),
          quietEnd: parseInt(document.getElementById('quietEnd').value, 10),
          batteryWarnEnabled: document.getElementById('batteryWarnEnabled').checked,
          batteryWarnPercent: parseInt(document.getElementById('batteryWarnPercent').value, 10)
        })
      });
      // Inline rather than a second popup: the confirmation already interrupted.
      const note = document.getElementById('saveNote');
      if (note) { note.textContent = await r.text(); }
      // Re-sync from the device so the form shows what was actually stored - if a
      // value was rejected, this is what makes that visible instead of silent.
      settingsLoaded = false;
      await updateStatus();
      // The panel redraw is queued and takes ~20 s, so /panel.bmp would still show
      // the old layout right now. Keep the preview up - it already matches what is
      // being drawn - and swap to the real image once it has had time to land.
      const pnote = document.getElementById('previewNote');
      if (pnote) { pnote.textContent = ui.previewApplying; }
      setTimeout(() => {
        document.getElementById('panelImg').src = '/panel.bmp?t=' + Date.now();
        if (pnote) { pnote.textContent = ''; }
      }, 26000);
    }
    // Render the form's current values without saving them. The device draws into
    // its buffer and hands back an image; the display itself is never touched.
    function previewSettings() {
      const q = new URLSearchParams({
        design: document.getElementById('design').value,
        layout: document.getElementById('layout').value,
        mode: document.getElementById('headerMode').value,
        mode2: document.getElementById('headerMode2').value,
        label: document.getElementById('locLabel').value.trim(),
        t: Date.now()
      });
      document.getElementById('panelImg').src = '/preview.bmp?' + q.toString();
      const note = document.getElementById('previewNote');
      if (note) { note.textContent = ui.previewUnsaved; }
    }
    ['design', 'layout', 'headerMode', 'headerMode2'].forEach(id => {
      document.getElementById(id).addEventListener('change', previewSettings);
    });
    document.getElementById('locLabel').addEventListener('change', previewSettings);
    async function keepAwake(minutes) {
      const r = await fetch('/keepAwake', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({minutes})
      });
      document.getElementById('keepAwakeState').textContent = await r.text();
      await updateStatus();
    }
    function uploadFirmware() {
      const input = document.getElementById('fwFile');
      const out = document.getElementById('otaProgress');
      if (!input.files.length) { alert('Choose a .bin file first'); return; }
      const data = new FormData();
      data.append('firmware', input.files[0]);
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/update');
      xhr.upload.onprogress = e => {
        if (e.lengthComputable) {
          out.textContent = `${Math.round((e.loaded / e.total) * 100)}%`;
        }
      };
      xhr.onload = () => { out.textContent = xhr.responseText; };
      xhr.onerror = () => { out.textContent = 'Upload failed (connection lost).'; };
      xhr.send(data);
    }
    document.getElementById('refreshMinutes').addEventListener('change', setRefreshInterval);
    document.getElementById('sleepToggle').addEventListener('change', setSleepMode);
    document.getElementById('language').addEventListener('change', setLanguage);
    document.getElementById('manual_ssid').addEventListener('input', e => {
      if (e.target.value.trim()) {
        document.getElementById('ssid').value = e.target.value.trim();
      }
    });
    scanNetworks().then(updateStatus).then(updateLogs);
    // Only poll while the page is actually visible. A backgrounded tab was
    // costing mobile battery and, worse, holding the device awake: every request
    // extends its awake window, so a forgotten tab kept it out of deep sleep up
    // to the 10-minute cap.
    let statusTimer = null;
    let logsTimer = null;
    function startPolling() {
      if (statusTimer === null) { statusTimer = setInterval(updateStatus, 8000); }
      if (logsTimer === null) { logsTimer = setInterval(updateLogs, 5000); }
    }
    function stopPolling() {
      clearInterval(statusTimer); statusTimer = null;
      clearInterval(logsTimer); logsTimer = null;
    }
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) {
        stopPolling();
      } else {
        updateStatus(); updateLogs(); startPolling();
      }
    });
    startPolling();
  </script>
</body>
</html>
)rawliteral";

  page.replace("{{LANG}}", kUiText[static_cast<uint8_t>(currentLanguage)].code);
  page.replace("{{TITLE}}", t.title);
  page.replace("{{LOADING_STATE}}", t.loadingState);
  page.replace("{{STATUS_TITLE}}", t.statusTitle);
  page.replace("{{STATUS_BATTERY}}", t.statusBattery);
  page.replace("{{STATUS_SLEEP_IN}}", t.statusSleepIn);
  page.replace("{{REFRESH_INTERVAL_LABEL}}", t.refreshIntervalLabel);
  page.replace("{{BATTERY_CHARGED_BUTTON}}", t.batteryChargedButton);
  page.replace("{{BATTERY_ESTIMATED_NOTE}}", t.batteryEstimatedNote);
  page.replace("{{SETTINGS_TITLE}}", t.settingsTitle);
  page.replace("{{WARNING_SOURCE_LABEL}}", t.warningSourceLabel);
  page.replace("{{HEADER_MODE_LABEL}}", t.headerModeLabel);
  page.replace("{{HEADER_MODE2_LABEL}}", t.headerMode2Label);
  page.replace("{{DESIGN_LABEL}}", t.designLabel);
  page.replace("{{DESIGN_CLASSIC}}", t.designClassic);
  page.replace("{{DESIGN_MODERN}}", t.designModern);
  page.replace("{{LAYOUT_LABEL}}", t.layoutLabel);
  page.replace("{{LAYOUT_TEMP}}", t.layoutTemp);
  page.replace("{{LAYOUT_LOCATION}}", t.layoutLocation);
  page.replace("{{LAYOUT_BOTH}}", t.layoutBoth);
  page.replace("{{LAYOUT_INFO}}", t.layoutInfo);
  page.replace("{{PREVIEW_TITLE}}", t.previewTitle);
  page.replace("{{PREVIEW_UNSAVED}}", t.previewUnsaved);
  page.replace("{{PREVIEW_APPLYING}}", t.previewApplying);
  page.replace("{{CONFIRM_SAVE}}", t.confirmSave);
  page.replace("{{CREDIT_PREFIX}}", t.creditPrefix);
  page.replace("{{MODE_OFF}}", t.modeOff);
  page.replace("{{HEADER_MODE_NOW}}", t.headerModeNow);
  page.replace("{{HEADER_MODE_RAIN}}", t.headerModeRain);
  page.replace("{{HEADER_MODE_SUN}}", t.headerModeSun);
  page.replace("{{HEADER_MODE_WIND}}", t.headerModeWind);
  page.replace("{{KEEP_AWAKE_LABEL}}", t.keepAwakeLabel);
  page.replace("{{WARN_SRC_OFF}}", t.warnSourceOff);
  page.replace("{{WARN_SRC_DERIVED}}", t.warnSourceDerived);
  page.replace("{{WARN_SRC_DWD}}", t.warnSourceDwd);
  page.replace("{{LOCATION_LABEL}}", t.locationLabelText);
  page.replace("{{LATITUDE_LABEL}}", t.latitudeLabel);
  page.replace("{{LONGITUDE_LABEL}}", t.longitudeLabel);
  page.replace("{{TIMEZONE_LABEL}}", t.timezoneLabel);
  page.replace("{{UNITS_LABEL}}", t.unitsLabel);
  page.replace("{{QUIET_HOURS_LABEL}}", t.quietHoursLabel);
  page.replace("{{QUIET_FROM_LABEL}}", t.quietFromLabel);
  page.replace("{{QUIET_TO_LABEL}}", t.quietToLabel);
  page.replace("{{BATTERY_WARN_LABEL}}", t.batteryWarnLabel);
  page.replace("{{BATTERY_PACK_LABEL}}", t.batteryPackLabel);
  page.replace("{{BATTERY_PACK_OPTIONS}}", buildBatteryPackOptions());
  page.replace("{{SAVE_BUTTON}}", t.saveButton);
  page.replace("{{TIMEZONE_OPTIONS}}", buildTimezoneOptions());
  page.replace("{{OTA_TITLE}}", t.otaTitle);
  page.replace("{{OTA_HINT}}", t.otaHint);
  page.replace("{{OTA_BUTTON}}", t.otaButton);
  page.replace("{{REFRESH_OPTIONS}}", buildRefreshOptions());
  page.replace("{{BATTERY_UNKNOWN}}", t.batteryUnknown);
  page.replace("{{BATTERY_LOW_PERCENT}}", String(weather_config::kBatteryLowPercent));
  page.replace("{{SLEEP_MODE_LABEL}}", t.sleepModeLabel);
  page.replace("{{SLEEP_MODE_HINT}}", t.sleepModeHint);
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
  // When static IP is configured, show those saved values. Otherwise, pre-fill
  // with the current DHCP-assigned network config so the user only needs to
  // change the IP address to switch to static.
  const bool connected = (WiFi.status() == WL_CONNECTED);
  const bool useStatic = staticIpConfig.enabled && static_cast<uint32_t>(staticIpConfig.ip) != 0;
  page.replace("{{STATIC_IP_VALUE}}", useStatic ? staticIpConfig.ip.toString() : (connected ? WiFi.localIP().toString() : String("")));
  page.replace("{{STATIC_GW_VALUE}}", useStatic ? staticIpConfig.gateway.toString() : (connected ? WiFi.gatewayIP().toString() : String("")));
  page.replace("{{STATIC_SUBNET_VALUE}}", useStatic ? staticIpConfig.subnet.toString() : (connected ? WiFi.subnetMask().toString() : String("")));
  page.replace("{{STATIC_DNS1_VALUE}}", useStatic ? staticIpConfig.dns1.toString() : (connected ? WiFi.dnsIP(0).toString() : String("")));
  page.replace("{{STATIC_DNS2_VALUE}}", useStatic ? staticIpConfig.dns2.toString() : (connected ? WiFi.dnsIP(1).toString() : String("")));
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
<html lang="{{LANG}}">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{SETUP_TITLE}}</title>
  <style>
    * { box-sizing: border-box; }
    body {
      font-family: "Arial Unicode MS", "Noto Sans", -apple-system, Arial, sans-serif;
      margin: 0; padding: 20px;
      background: #f0b37e; color: #001F4D;
      min-height: 100vh;
    }
    .container {
      max-width: 420px; margin: 0 auto; padding: 20px;
      background: #fff; border-radius: 10px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.1);
    }
    h1 {
      font-size: 1.5em; margin: 0 0 8px;
      color: #001F4D;
      text-shadow: 0 0 12px #FF4500, 0 0 20px #B22222, 0 0 30px #660000;
    }
    p { line-height: 1.45; font-size: 14px; }
    label { display: block; margin: 12px 0 4px; font-size: 14px; color: #1f2a3a; }
    select, input {
      width: 100%; padding: 10px;
      border: 1px solid #ccc; border-radius: 4px;
      background: #fff; color: #111; font-size: 14px;
    }
    input[type="password"] { font-family: monospace; }
    button {
      display: block; width: 100%; padding: 12px; margin-top: 14px;
      border: 0.8px solid rgb(33,150,243); border-radius: 4px;
      background: rgba(33,150,243,0.18); color: #14243a;
      font-size: 14px; font-weight: 700; cursor: pointer;
      min-height: 44px;
    }
    .hint { color: #666; font-size: 13px; margin: 8px 0 0; }
    .hint a { color: #007bff; text-decoration: none; }
    .mono { font-family: monospace; }
    .info-section {
      margin-top: 16px; padding-top: 12px;
      border-top: 1px solid #eee;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>{{SETUP_TITLE}}</h1>
    <p>{{SETUP_INTRO}}</p>
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
    <div class="info-section">
      <p class="hint"><a href="/hotspot-detect.html">{{RELOAD_CAPTIVE}}</a></p>
      <p class="hint"><a href="/">{{OPEN_FULL}}</a></p>
      <p class="hint">{{ACCESS_POINT}}: <span class="mono">{{AP_SSID}}</span></p>
      <p class="hint">{{DEVICE_IP}}: <span class="mono">192.168.4.1</span></p>
    </div>
  </div>
  <script>
    (function() {
      var sel = document.getElementById('ssid_select');
      if (sel && sel.options.length <= 1) {
        // Delay the scan by 3 seconds so Apple CNA finishes its captive
        // detection probes before we switch the radio to AP_STA for scanning.
        // Without this delay, the /scan request blocks the loop for 2-5s,
        // starving DNS/HTTP and causing CNA to give up.
        setTimeout(function() {
          sel.innerHTML = '<option value="">Scanning...</option>';
          fetch('/scan').then(function(r) { return r.json(); }).then(function(nets) {
            sel.innerHTML = '';
            if (!nets.length) { sel.innerHTML = '<option value="">No networks found</option>'; return; }
            nets.forEach(function(n) {
              var o = document.createElement('option');
              o.value = n.ssid;
              o.textContent = n.ssid + ' (' + n.rssi + ' dBm)';
              sel.appendChild(o);
            });
          }).catch(function() {});
        }, 3000);
      }
    })();
  </script>
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
  page.replace("{{LANG}}", kUiText[static_cast<uint8_t>(currentLanguage)].code);
  page.replace("{{LANGUAGE_LABEL}}", t.languageLabel);
  page.replace("{{LANGUAGE_OPTIONS}}", buildLanguageOptions());
  page.replace("{{AP_SSID}}", kApSsid);
  return page;
}

void rebuildCaptivePageCache()
{
  cachedCaptivePage = buildCaptivePage();
}

bool isAppleCaptiveRequest()
{
  const String userAgent = server.header("User-Agent");
  return userAgent.indexOf("CaptiveNetworkSupport") >= 0 || userAgent.indexOf("wispr") >= 0;
}

void handleRoot()
{
  noteWebActivity();
  server.sendHeader("Cache-Control", "no-store");
  if (apModeActive) {
    // In AP mode, always serve the lightweight captive setup page at "/".
    // The full control page (buildMainPage) triggers /scan, /status, /logs
    // fetches that all block the loop — unacceptable during CNA detection.
    // Users can still reach the full page at "/" once in station mode.
    server.send(200, "text/html", cachedCaptivePage);
    return;
  }
  server.send(200, "text/html", buildMainPage());
}

void handleLogs()
{
  noteWebActivity();
  server.send(200, "application/json", buildLogsJson());
}

void handleStatus()
{
  noteWebActivity();
  currentBattery = readBattery();
  updatePowerState(currentBattery);
  server.send(200, "application/json", buildStatusJson());
}

void handleScan()
{
  noteWebActivity();
  // Explicit rescan for the full control UI. In AP mode we need to temporarily
  // switch to AP_STA to perform the scan, then switch back to pure AP.
  if (apModeActive) {
    WiFi.mode(WIFI_AP_STA);
    refreshScanCacheSync();
    WiFi.mode(WIFI_AP);
    dnsServer.stop();
    dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
    // Rebuild cached HTML so the new network list appears on the next page load.
    rebuildCaptivePageCache();
  } else {
    refreshScanCacheSync();
  }
  server.send(200, "application/json", buildScanJson());
}

void handleSaveWiFi()
{
  noteWebActivity();
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
  // IEEE 802.11 limits SSID to 32 bytes and WPA2 passphrase to 63 characters.
  // Reject oversized input to prevent NVS overflow and potential buffer issues.
  if (ssid.length() > 32 || password.length() > 63) {
    server.send(400, "text/plain", "SSID (max 32) or password (max 63) too long");
    return;
  }

  if (!lang.isEmpty() && isLanguageCodeSupported(lang)) {
    saveLanguagePreference(lang);
    addLog(String("Language set to ") + lang + " via setup API.");
  }

  // Extract and persist the static IP configuration sent by the main page JS.
  // These fields are populated from the form inputs (static_enabled, static_ip, etc.).
  const bool staticEnabled = doc["staticEnabled"] | false;
  const String staticIp = doc["staticIp"].as<String>();
  const String staticGw = doc["staticGw"].as<String>();
  const String staticSubnet = doc["staticSubnet"].as<String>();
  const String staticDns1 = doc["staticDns1"].as<String>();
  const String staticDns2 = doc["staticDns2"].as<String>();
  saveStaticIpConfig(staticEnabled, staticIp, staticGw, staticSubnet, staticDns1, staticDns2);

  // If the password field is empty and the SSID matches the currently connected
  // network, keep the existing password. This allows users to change only the
  // static IP configuration without losing their WiFi credentials.
  const String effectivePassword = (password.isEmpty() && ssid == currentSsid) ? currentPassword : password;
  saveCredentials(ssid, effectivePassword);
  addLog(String("Saved Wi-Fi credentials for SSID: ") + ssid + " (pwd len=" + effectivePassword.length() + ")");
  if (staticEnabled) {
    addLog(String("Static IP config saved: ") + staticIp);
  }
  server.send(200, "text/plain", "Wi-Fi credentials saved. Device will reboot.");
  delay(500);
  ESP.restart();
}

void handleSaveWiFiForm()
{
  noteWebActivity();
  const String ssid = server.arg("ssid_manual").length() ? server.arg("ssid_manual") : server.arg("ssid_select");
  const String password = server.arg("password");
  const String lang = server.arg("language");
  if (ssid.isEmpty()) {
    server.send(400, "text/plain", "SSID cannot be empty");
    return;
  }
  if (ssid.length() > 32 || password.length() > 63) {
    server.send(400, "text/plain", "SSID (max 32) or password (max 63) too long");
    return;
  }

  if (!lang.isEmpty() && isLanguageCodeSupported(lang)) {
    saveLanguagePreference(lang);
    addLog(String("Language set to ") + lang + " via captive form.");
  }

  saveCredentials(ssid, password);
  addLog(String("Saved Wi-Fi credentials from captive form for SSID: ") + ssid + " (pwd len=" + password.length() + ")");
  server.send(200, "text/html",
              "<!DOCTYPE html><html><body style='font-family:-apple-system,Helvetica,Arial,sans-serif;padding:24px;'>"
              "<h2>Wi-Fi saved</h2><p>The device will reboot and join your router.</p></body></html>");
  delay(500);
  ESP.restart();
}

void handleLanguage()
{
  noteWebActivity();
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
  if (apModeActive) {
    rebuildCaptivePageCache();
  }
  server.send(200, "text/plain", "Language updated.");
  // Queue rather than render inline: the timestamp and weekday strings need
  // re-localizing, but blocking here stalls the rest of the settings burst.
  requestRefresh();
}

void handleForgetWiFi()
{
  noteWebActivity();
  clearCredentials();
  addLog("Stored Wi-Fi credentials erased.");
  server.send(200, "text/plain", "Wi-Fi credentials erased. Device will reboot.");
  delay(500);
  ESP.restart();
}

bool refreshForecastAndDisplay()
{
  // Sample the battery before drawing so the header glyph matches this frame,
  // and grant a fresh awake window so the web UI is reachable after an update.
  currentBattery = readBattery();
  updatePowerState(currentBattery);
  extendAwakeWindow(kAwakeWindowMs);

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
  // Resolve the header warning before rendering so the panel and /status agree.
  updateActiveWarning(currentForecast);
  renderForecast(currentForecast);
  return true;
}

void handleRefresh()
{
  noteWebActivity();
  // Queued rather than performed inline, so the ~20 s panel refresh cannot block
  // other requests. The result shows up in /logs and on the panel.
  requestRefresh();
  server.send(200, "text/plain", "Forecast refresh queued.");
}

void handleSleepMode()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  if (!doc["enabled"].is<bool>()) {
    server.send(400, "text/plain", "Missing 'enabled' flag");
    return;
  }

  saveSleepPreference(doc["enabled"].as<bool>());
  addLog(String("Deep sleep ") + (deepSleepEnabled ? "enabled." : "disabled."));
  server.send(200, "text/plain", deepSleepEnabled ? "Deep sleep enabled." : "Deep sleep disabled.");
}

void handleRefreshInterval()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const uint16_t minutes = doc["minutes"] | 0;
  if (!isRefreshIntervalSupported(minutes)) {
    server.send(400, "text/plain", "Unsupported interval");
    return;
  }

  saveRefreshPreference(minutes);
  addLog(String("Refresh interval set to ") + minutes + " min.");
  server.send(200, "text/plain", String("Refresh interval set to ") + minutes + " min.");
}

// Fast, on-demand panel probe. Re-runs a reset and reports the BUSY line state
// without the full driver init, so it answers in ~1.5 s instead of ~27 s. Lets
// the connector be re-seated while watching a live readout.
void handlePanelProbe()
{
  noteWebActivity();
#ifdef EPAPER_ENABLE
  pinMode(TFT_BUSY, INPUT);
  delay(5);
  const int floating = digitalRead(TFT_BUSY);
  pinMode(TFT_BUSY, INPUT_PULLUP);
  delay(5);
  const int pulledUp = digitalRead(TFT_BUSY);
  pinMode(TFT_BUSY, INPUT);
  const bool ready = resetPanelAndWaitReady(20, 1500);
  String json = "{\"busyFloating\":";
  json += String(floating);
  json += ",\"busyPullup\":";
  json += String(pulledUp);
  json += ",\"panelAnswers\":";
  json += ready ? "true" : "false";
  json += ",\"panelReady\":";
  json += panelReady ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
#else
  server.send(200, "application/json", "{\"panelAnswers\":false}");
#endif
}

// Receive a firmware image chunk by chunk and write it straight to the inactive
// OTA slot. Requires the dual-app partition table (partitions_ota.csv).
void handleUpdateUpload()
{
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaInProgress = true;
    addLog(String("OTA upload started: ") + upload.filename);
    // UPDATE_SIZE_UNKNOWN lets the library size the write from the free slot.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaInProgress = false;
      addLog(String("OTA begin failed: ") + Update.errorString());
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    // Keep the awake window alive across a long upload. The sleep gate also
    // checks otaInProgress, so the hard cap cannot interrupt a flash either.
    extendAwakeWindow(kAwakeWindowMs);
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      addLog(String("OTA write failed: ") + Update.errorString());
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      addLog(String("OTA image written: ") + upload.totalSize + " bytes.");
    } else {
      addLog(String("OTA end failed: ") + Update.errorString());
    }
    otaInProgress = false;
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaInProgress = false;
    addLog("OTA upload aborted.");
  }
}

// Runs after the upload completes. Only reboots when the image verified, so a
// rejected image leaves the running firmware untouched.
void handleUpdateDone()
{
  noteWebActivity();
  const bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  if (ok) {
    server.send(200, "text/plain", "Update OK. Rebooting into the new firmware.");
    delay(500);
    ESP.restart();
  } else {
    server.send(500, "text/plain", String("Update failed: ") + Update.errorString());
  }
}

void handleLocation()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  // Validate before storing: a bad coordinate would otherwise persist and make
  // every future fetch fail with no obvious cause.
  if (!doc["latitude"].is<float>() || !doc["longitude"].is<float>()) {
    server.send(400, "text/plain", "latitude and longitude are required");
    return;
  }
  const float latitude = doc["latitude"].as<float>();
  const float longitude = doc["longitude"].as<float>();
  if (latitude < -90.0f || latitude > 90.0f || longitude < -180.0f || longitude > 180.0f) {
    server.send(400, "text/plain", "Coordinates out of range");
    return;
  }
  String label = doc["label"].as<String>();
  label.trim();
  if (label.isEmpty()) {
    label = weather_config::kLocationLabel;
  }
  // The panel header has room for a short name only.
  if (label.length() > 24) {
    label = label.substring(0, 24);
  }

  // The label is free text and reaches the panel, so fold it the same way.
  saveLocationPreference(latitude, longitude, toDisplayAscii(label));
  addLog(String("Location set to ") + label + " (" + String(latitude, 4) + ", " + String(longitude, 4) + ").");
  server.send(200, "text/plain", "Location saved. Refreshing forecast.");
  requestRefresh();
}

void handleTimezone()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const String iana = doc["timezone"].as<String>();
  if (iana.isEmpty() || String(timeZoneByIana(iana).iana) != iana) {
    server.send(400, "text/plain", "Unsupported timezone");
    return;
  }

  saveTimezonePreference(iana);
  addLog(String("Timezone set to ") + iana + ".");
  server.send(200, "text/plain", String("Timezone set to ") + iana + ".");
  // Re-run the clock so the header timestamp reflects the new zone.
  requestRefresh();
}

void handleUnits()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const bool fahrenheit = doc["fahrenheit"] | false;
  const bool mph = doc["mph"] | false;
  saveUnitPreferences(fahrenheit, mph);
  addLog(String("Units set to ") + (fahrenheit ? "F" : "C") + "/" + (mph ? "mph" : "km/h") + ".");
  server.send(200, "text/plain", "Units saved. Refreshing forecast.");
  requestRefresh();
}

void handlePowerOptions()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  quietHoursEnabled = doc["quietEnabled"] | false;
  const uint8_t start = doc["quietStart"] | 23;
  const uint8_t end = doc["quietEnd"] | 6;
  if (start > 23 || end > 23) {
    server.send(400, "text/plain", "Hours must be 0-23");
    return;
  }
  quietStartHour = start;
  quietEndHour = end;

  batteryWarnEnabled = doc["batteryWarnEnabled"] | true;
  const uint8_t pct = doc["batteryWarnPercent"] | 20;
  if (pct > 100) {
    server.send(400, "text/plain", "Percent must be 0-100");
    return;
  }
  batteryWarnPercent = pct;

  savePowerPreferences();
  addLog(String("Power options: quiet=") + (quietHoursEnabled ? "on" : "off") + " " + quietStartHour +
         "-" + quietEndHour + ", battery warn=" + (batteryWarnEnabled ? "on" : "off") + " at " +
         batteryWarnPercent + "%.");
  server.send(200, "text/plain", "Power options saved.");
}

void handleWarningSource()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const String source = doc["source"].as<String>();
  if (!isWarningSourceSupported(source)) {
    server.send(400, "text/plain", "Unsupported warning source");
    return;
  }

  saveWarningPreference(source);
  addLog(String("Warning source set to ") + source + ".");
  server.send(200, "text/plain", String("Warning source set to ") + source + ".");
  requestRefresh();
}

void handleKeepAwake()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  uint32_t minutes = doc["minutes"] | 0;
  // Clamped so a debug hold cannot quietly become a permanent drain.
  if (minutes > 60) {
    minutes = 60;
  }

  if (minutes == 0) {
    keepAwakeUntilMs = 0;
    addLog("Keep-awake cancelled.");
    server.send(200, "text/plain", "Keep-awake off.");
    return;
  }

  keepAwakeUntilMs = millis() + (minutes * 60UL * 1000UL);
  addLog(String("Keep-awake for ") + minutes + " min.");
  server.send(200, "text/plain", String("Staying awake for ") + minutes + " min.");
}

void handleHeaderMode()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const String mode = doc["mode"].as<String>();
  if (!isHeaderModeSupported(mode)) {
    server.send(400, "text/plain", "Unsupported header mode");
    return;
  }
  // The second line is optional in the request, so an older client that only
  // knows about one line keeps working.
  const String mode2 = doc["mode2"].as<String>();
  if (!mode2.isEmpty() && !isHeaderMode2Supported(mode2)) {
    server.send(400, "text/plain", "Unsupported second header mode");
    return;
  }

  saveHeaderPreference(mode);
  if (!mode2.isEmpty()) {
    saveHeaderMode2Preference(mode2);
  }
  addLog(String("Header lines set to ") + mode + " / " + headerMode2 + ".");
  server.send(200, "text/plain", String("Header line set to ") + mode + ".");
  requestRefresh();
}

void handleBatteryPack()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const uint16_t mah = doc["mah"] | 0;
  if (!isBatteryPackSupported(mah)) {
    server.send(400, "text/plain", "Unsupported battery size");
    return;
  }

  saveBatteryPackPreference(mah);
  // Apply to the gauge immediately; no reboot needed to change its profile.
  if (batteryGaugeReady) {
    batteryGauge.setPackSize(batteryPackProfile(batteryPackMah));
  }
  addLog(String("Battery pack set to ") + batteryPackMah + " mAh.");
  server.send(200, "text/plain", String("Battery size set to ") + batteryPackMah + " mAh.");
}

void handleDesign()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const String design = doc["design"].as<String>();
  if (!isPanelDesignSupported(design)) {
    server.send(400, "text/plain", "Unsupported design");
    return;
  }

  savePanelDesignPreference(design);
  addLog(String("Panel design set to ") + design + ".");
  server.send(200, "text/plain", String("Design set to ") + design + ".");
  requestRefresh();
}

void handleLayout()
{
  noteWebActivity();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const String layout = doc["layout"].as<String>();
  if (!isHeaderLayoutSupported(layout)) {
    server.send(400, "text/plain", "Unsupported layout");
    return;
  }

  saveHeaderLayoutPreference(layout);
  addLog(String("Header layout set to ") + layout + ".");
  server.send(200, "text/plain", String("Layout set to ") + layout + ".");
  requestRefresh();
}

// Serve the panel's framebuffer as a BMP, so the display can be inspected from
// the web UI instead of by walking over to it.
//
// The sprite is 4 bits per pixel, which is exactly what a 4-bit BMP stores, so
// the pixel body is a near-verbatim dump of the buffer: ~19 KB, no conversion
// loop, no image library. The palette maps the four e-paper colours; the sprite
// uses 0x00 white, 0x0F black, 0x0B yellow, 0x06 red.
//
// The buffer is in the panel's native portrait orientation (128x296) regardless
// of the rotation used for drawing, so the image is emitted portrait and the web
// page rotates it. Doing the rotation here would mean second-guessing the
// sprite's internal coordinate mapping for no benefit.
// Emit whatever is currently in the sprite as a 4-bit BMP. Shared by the live
// panel image and the settings preview.
void sendPanelBitmap()
{
#ifdef EPAPER_ENABLE
  constexpr int32_t kBmpWidth = TFT_WIDTH;    // 128
  constexpr int32_t kBmpHeight = TFT_HEIGHT;  // 296
  constexpr uint32_t kRowBytes = kBmpWidth / 2;  // 4bpp, already 4-byte aligned
  constexpr uint32_t kPixelBytes = kRowBytes * kBmpHeight;
  constexpr uint32_t kPaletteBytes = 16 * 4;
  constexpr uint32_t kOffset = 14 + 40 + kPaletteBytes;
  constexpr uint32_t kFileSize = kOffset + kPixelBytes;

  uint8_t header[kOffset] = {0};
  // BITMAPFILEHEADER
  header[0] = 'B';
  header[1] = 'M';
  memcpy(&header[2], &kFileSize, 4);
  memcpy(&header[10], &kOffset, 4);
  // BITMAPINFOHEADER
  const uint32_t infoSize = 40;
  memcpy(&header[14], &infoSize, 4);
  memcpy(&header[18], &kBmpWidth, 4);
  // Negative height: rows top-down, matching the buffer's order.
  const int32_t negHeight = -kBmpHeight;
  memcpy(&header[22], &negHeight, 4);
  const uint16_t planes = 1;
  const uint16_t bpp = 4;
  memcpy(&header[26], &planes, 2);
  memcpy(&header[28], &bpp, 2);
  memcpy(&header[34], &kPixelBytes, 4);
  const uint32_t paletteUsed = 16;
  memcpy(&header[46], &paletteUsed, 4);

  // Palette entries are BGRA. Everything not one of the four panel colours stays
  // black, which makes an unexpected nibble value obvious rather than invisible.
  uint8_t* palette = &header[54];
  auto setPalette = [palette](uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    palette[index * 4 + 0] = b;
    palette[index * 4 + 1] = g;
    palette[index * 4 + 2] = r;
  };
  setPalette(0x00, 0xFF, 0xFF, 0xFF);  // white
  setPalette(0x06, 0xE0, 0x20, 0x20);  // red
  setPalette(0x0B, 0xF0, 0xC0, 0x10);  // yellow
  setPalette(0x0F, 0x00, 0x00, 0x00);  // black

  server.setContentLength(kFileSize);
  server.send(200, "image/bmp", "");
  server.sendContent(reinterpret_cast<const char*>(header), sizeof(header));
  server.sendContent(reinterpret_cast<const char*>(epaper.getPointer()), kPixelBytes);
#else
  server.send(404, "text/plain", "No panel");
#endif
}

void handlePanelBitmap()
{
  noteWebActivity();
  sendPanelBitmap();
}

// Render the frame with proposed settings and return it as an image, without
// saving anything and without touching the panel.
//
// Only the drawing runs - never panelUpdate() - so this costs milliseconds and
// the display is left exactly as it was. The sprite is redrawn with the real
// settings afterwards so /panel.bmp keeps matching the physical panel.
void handlePreviewBitmap()
{
  noteWebActivity();
#ifdef EPAPER_ENABLE
  const String savedDesign = panelDesign;
  const String savedLayout = headerLayout;
  const String savedMode = headerMode;
  const String savedMode2 = headerMode2;
  const String savedLabel = currentLocationLabel;
  const bool savedShowNetwork = showNetworkInfo;

  // Unrecognised values fall back to what is stored rather than erroring: a
  // preview should always render something.
  if (server.hasArg("design") && isPanelDesignSupported(server.arg("design"))) {
    panelDesign = server.arg("design");
  }
  if (server.hasArg("layout") && isHeaderLayoutSupported(server.arg("layout"))) {
    headerLayout = server.arg("layout");
  }
  if (server.hasArg("mode") && isHeaderModeSupported(server.arg("mode"))) {
    headerMode = server.arg("mode");
  }
  if (server.hasArg("mode2") && isHeaderMode2Supported(server.arg("mode2"))) {
    headerMode2 = server.arg("mode2");
  }
  if (server.hasArg("label")) {
    String label = toDisplayAscii(server.arg("label"));
    label.trim();
    if (!label.isEmpty()) {
      currentLocationLabel = label.substring(0, 24);
    }
  }
  // Boot state, not a setting. Left true it would show the SSID line and hide the
  // very preset being previewed.
  showNetworkInfo = false;

  drawForecastToBuffer(currentForecast);
  sendPanelBitmap();

  panelDesign = savedDesign;
  headerLayout = savedLayout;
  headerMode = savedMode;
  headerMode2 = savedMode2;
  currentLocationLabel = savedLabel;
  showNetworkInfo = savedShowNetwork;
  // Put the buffer back so /panel.bmp still reflects the physical panel.
  drawForecastToBuffer(currentForecast);
#else
  server.send(404, "text/plain", "No panel");
#endif
}

void handleBatteryFull()
{
  noteWebActivity();
  resetConsumedCharge();
  currentBattery = readBattery();
  addLog("Battery marked as fully charged; estimate reset to 100%.");
  server.send(200, "text/plain", "Battery estimate reset to 100%.");
}

void handleReboot()
{
  noteWebActivity();
  server.send(200, "text/plain", "Rebooting.");
  delay(300);
  ESP.restart();
}

void handlePortal()
{
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", cachedCaptivePage);
}

void handleNotFound()
{
  if (apModeActive) {
    // Serve the cached captive page for any unknown path. This catches OS
    // captive probes we didn't explicitly register (e.g. Android variants).
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", cachedCaptivePage);
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void handleAppleCaptiveProbe()
{
  // Apple CNA sends GET /hotspot-detect.html and checks if the response body is
  // exactly: <HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>
  // If it matches → "internet works, no captive portal."
  // If it does NOT match → "captive portal detected" → CNA opens a sheet showing
  // the response body directly in the CNA WebKit sheet.
  //
  // We serve the pre-built captive page directly as HTTP 200. This is the fastest
  // path: one DNS query → one HTTP response → CNA renders the form. No redirects,
  // no meta-refresh, no second round-trip. The page is pre-cached in startApMode()
  // so serving it takes <1ms (just sending the already-built String).
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", cachedCaptivePage);
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
    // Android expects 204 for "internet works." Returning 200 with content
    // triggers "sign in to network" notification → opens the portal browser.
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", cachedCaptivePage);
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
  server.on("/portal", HTTP_GET, handlePortal);
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
  server.on("/sleepMode", HTTP_POST, handleSleepMode);
  server.on("/refreshInterval", HTTP_POST, handleRefreshInterval);
  server.on("/location", HTTP_POST, handleLocation);
  server.on("/timezone", HTTP_POST, handleTimezone);
  server.on("/units", HTTP_POST, handleUnits);
  server.on("/powerOptions", HTTP_POST, handlePowerOptions);
  server.on("/warningSource", HTTP_POST, handleWarningSource);
  server.on("/headerMode", HTTP_POST, handleHeaderMode);
  server.on("/layout", HTTP_POST, handleLayout);
  server.on("/design", HTTP_POST, handleDesign);
  server.on("/batteryPack", HTTP_POST, handleBatteryPack);
  server.on("/keepAwake", HTTP_POST, handleKeepAwake);
  server.on("/panel.bmp", HTTP_GET, handlePanelBitmap);
  server.on("/preview.bmp", HTTP_GET, handlePreviewBitmap);
  server.on("/batteryFull", HTTP_POST, handleBatteryFull);
  server.on("/panelProbe", HTTP_GET, handlePanelProbe);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.onNotFound(handleNotFound);
  routesConfigured = true;
}

// Power down the panel and the radio, then sleep until the next update.
// Deep sleep resets the chip, so the next cycle re-enters setup() from scratch;
// the e-paper keeps showing the last frame throughout.
void enterDeepSleep(uint32_t sleepMs)
{
  accumulateConsumedCharge(millis(), sleepMs);
  addLog(String("Entering deep sleep for ") + (sleepMs / 1000) + " s.");
  Serial.flush();

#ifdef EPAPER_ENABLE
  // EPaper::update() already ends in DSLP, but a cycle that never rendered (or
  // one that only errored) may leave the panel awake. sleep() is idempotent.
  if (panelReady) {
    epaper.sleep();
  }
  // Park the panel in reset and hold that level through deep sleep. Left to
  // float, the control pins put the panel in an undefined state that its next
  // init could not always recover from - which is what hung the boot. The image
  // survives regardless: e-paper is bistable and needs no power to hold a frame.
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  // On the C6 (SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP) gpio_hold_en() alone
  // holds the pad through deep sleep; the global gpio_deep_sleep_hold_en() is
  // compiled out for this target.
  gpio_hold_en(static_cast<gpio_num_t>(TFT_RST));
#endif

  if (serverStarted) {
    server.stop();
    serverStarted = false;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepMs) * 1000ULL);
  esp_deep_sleep_start();
}

void setupRuntime()
{
  registerWiFiEvents();
  loadStoredCredentials();
  loadLanguagePreference();
  loadSleepPreference();
  loadRefreshPreference();
  loadDisplayPreferences();
  loadPowerPreferences();
  loadWarningPreference();
  loadHeaderPreference();
  loadBatteryPackPreference();
  loadConsumedCharge();
  loadStaticIpConfig();
  setupWebServer();
  extendAwakeWindow(kAwakeWindowMs);

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
    noteCurrentIpAddress();
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

  const bool coldBoot = (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER);

#ifdef EPAPER_ENABLE
  setupPanel(coldBoot);
#endif

  if (coldBoot) {
    rtcWakeCount = 0;
    addLog("JFG PaperCast booting.");
  } else {
    ++rtcWakeCount;
    addLog(String("Woke from deep sleep (cycle ") + rtcWakeCount + ").");
  }

  // Hold off sleep for the full cold-boot window before anything else can arm a
  // shorter one, so the web UI is always reachable for 5 minutes after a boot.
  extendAwakeWindow(coldBoot ? kColdBootAwakeMs : kAwakeWindowMs);

  // Show SSID/IP on the panel after a power-on or reset - that is when someone is
  // standing there checking it worked. A timer wake starts with it hidden, and
  // noteCurrentIpAddress() turns it back on if the address actually moved.
  bootWasCold = coldBoot;
  showNetworkInfo = coldBoot;
  setupBatteryGauge(coldBoot);
  setupRuntime();
}

void loop()
{
  if (apModeActive) {
    // Process DNS BEFORE HTTP. DNS responses are tiny (~50 bytes, <1ms) but
    // Apple CNA requires them within ~200ms of sending the query. If HTTP
    // request handling (which can take tens of ms) runs first, DNS gets starved
    // and CNA times out → portal never appears or takes minutes.
    dnsServer.processNextRequest();
  }
  if (serverStarted) {
    server.handleClient();
  }

  if (!apModeActive && WiFi.status() == WL_CONNECTED && forecastValid &&
      (millis() - lastRefreshMs) > refreshIntervalMs()) {
    refreshForecastAndDisplay();
  }

  if (!apModeActive && !currentSsid.isEmpty() && WiFi.status() != WL_CONNECTED &&
      (millis() - lastReconnectAttemptMs) > kReconnectCheckIntervalMs) {
    lastReconnectAttemptMs = millis();
    reconnectFailures++;
    addLog(String("Wi-Fi reconnect attempt ") + reconnectFailures + "/" + kMaxReconnectFailuresBeforeAp);

    // Use the lightweight reconnect path: it reuses the existing STA mode
    // instead of doing a full WiFi.mode() teardown, which avoids resetting
    // the ESP-IDF association state machine mid-reconnect.
    if (reconnectWifi(currentSsid, currentPassword)) {
      if (!forecastValid) {
        refreshForecastAndDisplay();
      }
    } else if (reconnectFailures >= kMaxReconnectFailuresBeforeAp) {
      // After repeated failures, fall back to AP mode so the user can
      // reconfigure. The saved credentials are kept for the next boot.
      addLog("Max reconnect failures reached. Falling back to AP mode.");
      reconnectFailures = 0;
      startApMode();
    }
  }

  // Perform a queued refresh once the burst of settings changes has settled.
  if (refreshPending && (millis() - refreshRequestedMs) > kRefreshDebounceMs) {
    refreshPending = false;
    refreshForecastAndDisplay();
  }

  // Sleep once the awake window expires, or unconditionally once the cap is hit
  // so that continuous traffic cannot hold the device up indefinitely. Never
  // sleep in AP mode: the captive portal has to stay up for as long as the user
  // needs it. A failed forecast retries sooner than the normal cycle.
  const bool awakeWindowExpired = static_cast<int32_t>(millis() - awakeUntilMs) > 0;
  const bool awakeCapReached = millis() > kMaxAwakeMs;
  // A timed keep-awake outranks the cap: the cap exists to stop traffic holding
  // the device up indefinitely, not to cut short a hold that expires on its own.
  const bool keepAwakeActive = (keepAwakeUntilMs != 0) && (static_cast<int32_t>(keepAwakeUntilMs - millis()) > 0);
  if (deepSleepEnabled && !apModeActive && !otaInProgress && !refreshPending && !keepAwakeActive &&
      (awakeWindowExpired || awakeCapReached)) {
    if (awakeCapReached && !awakeWindowExpired) {
      addLog("Awake cap reached; sleeping despite recent web activity.");
    }
    const uint32_t elapsedMs = millis();
    uint32_t cycleMs = forecastValid ? refreshIntervalMs() : kRetrySleepMs;

    // Stretch the interval when the pack is nearly flat, so the display keeps
    // working (less often) instead of dying sooner.
    if (forecastValid && batteryIsLow()) {
      cycleMs *= 3;
      addLog(String("Battery at ") + currentBattery.percent + "%; tripling the refresh interval.");
    }

    // Sleep straight through the quiet window rather than waking each interval
    // to render a forecast nobody is awake to read. Only trust this with a valid
    // clock - never extend a sleep based on a bad timestamp.
    struct tm localTime = {};
    if (forecastValid && quietHoursEnabled && getLocalTime(&localTime, 100) && isQuietHourNow(localTime)) {
      const uint32_t quietMs = secondsUntilQuietEnds(localTime) * 1000UL;
      if (quietMs > cycleMs) {
        addLog(String("Quiet hours: sleeping ") + (quietMs / 60000) + " min until " + quietEndHour + ":00.");
        cycleMs = quietMs;
      }
    }

    // Subtract the time this wake cycle already burned so the render-to-render
    // period stays at the configured interval rather than drifting by boot time.
    enterDeepSleep((cycleMs > elapsedMs) ? (cycleMs - elapsedMs) : 1000UL);
  }
}
