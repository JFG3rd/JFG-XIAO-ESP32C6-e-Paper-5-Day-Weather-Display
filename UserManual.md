# User Manual

A 5‑day weather display on a 2.9" 4‑colour e‑paper panel, driven by a Seeed XIAO ESP32‑C6. It wakes
on a timer, fetches a forecast, redraws the panel and goes back to sleep — everything else is
configured from a web page it serves itself.

For build and hardware notes see [README.md](README.md). This document is about *using* it.

---

## 1. What the display shows

```
┌──────────────────────────────────────────────────────────────┐
│  26°        WiFi: MBUmain 192.168.178.222            [ 95 ]  │  ← banner
│             Sun 06:12-20:02                                  │
│             Updated: SAT 29/08 16:18                         │
├──────────┬──────────┬──────────┬──────────┬──────────────────┤
│   SAT    │   SUN    │   MON    │   TUE    │   WED            │
│   ☁      │   ☀      │   ☀      │   ☀      │   ☁              │
│  CLOUD   │ SHOWERS  │ SHOWERS  │ SHOWERS  │  CLOUD           │
│   26°    │   25°    │   25°    │   22°    │   24°            │
│ L18° P35%│ L18° P25%│ L18° P70%│ L16° P14%│ L14° P17%        │
└──────────┴──────────┴──────────┴──────────┴──────────────────┘
```

**The banner** carries a left element (configurable — see §4), up to two information lines, the time
of the last update, and a battery indicator. **The five cards** are the forecast: weekday, an icon,
a short condition label, the day's high in red, and a footer with `L` = the low and `P` = the
**probability** of precipitation.

`P` is a percentage chance, *not* an amount. A 60 % chance of 0.2 mm and a 60 % chance of 15 mm look
identical on the cards, which is why the `Rain in mm` header preset exists — it shows the amount.

**The battery indicator** shows the charge as a bar with the percentage inside it. A trailing `?`
(e.g. `79?`) means the value is *estimated* from run time rather than measured — see §5.

---

## 2. First‑time setup

1. Power the device. With no saved Wi‑Fi it starts its own access point,
   **`JFG-PaperCast-Setup`**.
2. Join that network from a phone or laptop. A setup page should open automatically (it answers the
   captive‑portal probes used by iOS, Android and Windows). If it does not, browse to
   **`http://192.168.4.1`**.
3. Pick your network, enter the password, and save. The device reboots and connects.
4. **Find its address**: after a cold boot the panel shows `WiFi: <ssid> <ip>` on the first banner
   line. It also shows it again automatically whenever the address changes, so a new DHCP lease
   never leaves you hunting.

Open `http://<device-ip>/` for everything else.

> The address line is deliberately temporary. On the next timer wake it gives way to weather
> information, because the address is only interesting when it is new.

---

## 3. The web interface

### Status
Live state: SSID, IP, Wi‑Fi state, whether the forecast is valid, when it last updated, the battery
(with voltage, and `· charging` / `· USB` when detected), and how long until the device sleeps.

- **Deep sleep between updates** — turn off to keep the device permanently reachable. Convenient for
  debugging, but it will flatten the battery; prefer the timed keep‑awake in Settings.
- **Refresh every** — 15 / 30 / 60 / 120 / 240 minutes. This is the single biggest influence on
  battery life.
- **Battery charged** — see §5.

### Wi‑Fi Setup
Change networks, enter a hidden SSID, or configure a static IP. **Forget Wi‑Fi** clears the
credentials and returns the device to its setup access point.

### Panel preview
A picture of what is actually on the e‑paper, fetched from the device as an image of its own
framebuffer. It is a real screenshot, not a re‑drawing, so what you see is what the panel shows.

It also **previews unsaved changes**: change the layout, either header line, or the location name and
the preview redraws immediately with those values. Nothing is applied until you press **Save**, and
the preview says so while it is showing something unsaved. This costs the device nothing and never
touches the display — it draws into memory and hands back the picture.

Two things it does not preview: **units**, because °C/°F conversion happens at the weather service and
would need a fresh forecast rather than a redraw; and the **network line**, which is suppressed so you
can see the preset you are choosing rather than the IP address.

Press **Reload preview** to go back to what is physically on the panel.

### Settings
Location, time zone, layout, the two header lines, warnings, units, **battery size**, quiet hours and
the low‑battery threshold. Press **Save** to apply them together. The form is filled from the device when the page
loads and then left alone, so it will not overwrite something you are part‑way through changing.

### Firmware Update
Upload a new `.bin` — see §6.

### Logs
The device's recent log lines, useful when something is not behaving. The buffer holds the last 24
entries, so it scrolls quickly during a busy refresh.

---

## 4. Display options

**Layout** chooses the left of the banner:

| Layout | Shows |
|---|---|
| **Temperature** | The current temperature, large |
| **Location** | The city name |
| **Temperature + location** | The temperature with the city name beneath it |
| **Info only** | Nothing — the full width goes to the information lines |

Temperature layouts fall back to the city name until the first reading arrives, so the corner is
never blank.

**Header line** and **Second line** each choose what that information line shows:

| Preset | Example | Notes |
|---|---|---|
| Feels‑like + UV | `Gefuehlt 19 UV 5` | Apparent temperature and the UV index |
| Rain in mm | `Regen 2.4mm 60%` | The **amount** plus the probability |
| Sunrise / sunset | `Sonne 06:12-20:02` | |
| Wind + gusts | `Wind 12 Boe 24` | Wind is otherwise not shown at all |
| Off *(second line only)* | | Leaves the line blank |

The first line is not always your preset. It follows a priority order:

1. **Network information** — after a cold boot, or when the IP has changed
2. **A weather warning** — in red, when one is active
3. **Your chosen preset** — the everyday case

**Weather warnings** come from one of:

- **From forecast** — the device's own reading of today's data (thunderstorm, strong wind, heavy
  rain, frost, heat). Works anywhere. These are *not* official warnings.
- **Official (DWD)** — real German weather service warnings. Germany only; elsewhere the line simply
  falls back to your preset.
- **Off**.

---

## 5. Battery and power

Expected runtime on a 400 mAh cell, at roughly 90 mA awake and 0.2 mA asleep:

| Awake window | Interval | Per day | Runtime |
|---|---|---|---|
| 120 s | 30 min | ~184 mAh | ~2 days |
| **30 s** | **60 min** | **~35 mAh** | **~11 days** |

Each cycle: wake, connect, fetch, redraw (~20 s for the panel alone), stay reachable for 30 seconds,
then sleep. A cold boot stays awake for 5 minutes so you can always reach the web page after
powering on. Any web request extends the window, but never beyond a 10‑minute cap — so a browser tab
left open cannot silently drain the battery.

**Quiet hours** skip refreshes overnight, sleeping straight through the window rather than waking
each interval. An 8‑hour window removes about a third of the daily cycles.

**Low battery** draws a red strip across the banner and triples the refresh interval, so the display
keeps working — less often — instead of dying sooner.

**Keep awake (debug)** holds the device up for 15, 30 or 60 minutes, then expires on its own. Use
this rather than switching deep sleep off, which stays off until you remember it.

### Battery size

Set **Battery size** in Settings to your cell's actual capacity. It does two jobs: it tells the
LC709203F which discharge profile to use, and it is the capacity the modelled estimate divides by.

The gauge itself only supports six profiles (100, 200, 500, 1000, 2000 and 3000 mAh), so the firmware
picks the nearest one to whatever you choose — enter 400 for a 400 mAh cell and the gauge gets the
500 mAh profile while the estimate still uses 400. Changing it applies immediately; no reboot.

### Measured versus estimated charge

With an **LC709203F** fuel gauge fitted (see [docs/wire-diagram.md](docs/wire-diagram.md)) the
percentage and voltage are measured, and that is the end of it.

Without one, the firmware *models* the charge from how long it has spent awake and asleep. Modelled
values always carry a trailing **`?`** so they are never mistaken for measurements. The model cannot
detect charging, so it needs a zero point: **press "Battery charged" after every charge**. Without
that press the estimate drifts further from reality with every cycle.

---

## 6. Firmware updates

### Over the air (normal)

1. **Get a `.bin`** — either download `firmware-vX.Y.Z.bin` from the
   [Releases page](https://github.com/JFG3rd/JFG-PaperCast/releases),
   or build one yourself:

   ```bash
   pio run -e seeed_xiao_esp32c6
   # artifact: .pio/build/seeed_xiao_esp32c6/firmware.bin
   ```

2. Open the web page, go to **Firmware Update**, choose the file, press **Upload firmware**.
3. Watch the progress percentage. The device verifies the image and reboots into it — a ~1.3 MB
   image takes about 15 seconds. Deep sleep is suppressed for the whole upload, so it cannot be cut
   short.

If the image is rejected the running firmware is left untouched and the error is reported, so a bad
upload is not fatal.

You can also upload from the command line:

```bash
curl -F "firmware=@.pio/build/seeed_xiao_esp32c6/firmware.bin" http://<device-ip>/update
```

### Over a cable (rarely needed)

**`esptool` cannot talk to this board** — it reaches the ROM bootloader but never syncs. Use the
chip's built‑in JTAG instead:

```bash
PLATFORMIO_UPLOAD_PROTOCOL=esp-builtin pio run -e seeed_xiao_esp32c6 -t upload
```

The device must be awake for its USB port to exist; it disappears during deep sleep.

> **Upgrading from before v1.1.0 needs one wired flash.** OTA relies on a dual‑app partition table
> that OTA itself cannot install. Saved Wi‑Fi credentials survive the change.

---

## 7. Troubleshooting

**The device does not answer.** Most likely it is asleep — that is normal, and it is unreachable for
most of every hour. Wait for the next wake, or press reset for a 5‑minute window.

**The panel is blank or not updating.** Check `panelReady` in the status card. If it is false, the
panel is not responding: `http://<device-ip>/panelProbe` reports the BUSY line's electrical state,
and `busyFloating=0, busyPullup=1` means nothing is driving it — re‑seat the 24‑pin FPC connector
(power off first, and check the ribbon is the right way round). The firmware keeps running headless
so the web page stays available for exactly this.

**A setting will not stick.** Press **Save**; individual dropdowns do not apply on their own. If it
still reverts, check the Logs card for a rejection — an out‑of‑range coordinate or an unsupported
value is refused rather than stored.

**The page hangs for ~20 seconds after saving.** Expected. Redrawing the panel blocks the web server
for the duration of the refresh, and the server is single‑threaded.

**The battery shows `?`.** No fuel gauge is detected, so the value is modelled. Check the Logs for
`Battery gauge not found on I2C`.

**The clock is wrong.** Set the **Time zone** in Settings. Times are NTP‑derived, but the zone must
be chosen — the device has no way to infer it.

---

## 8. HTTP endpoints

Useful for scripting or debugging.

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | The control page |
| GET | `/status` | Full device state as JSON |
| GET | `/logs` | Recent log lines as JSON |
| GET | `/panel.bmp` | The panel's framebuffer as an image |
| GET | `/preview.bmp` | Render proposed settings without saving: `?layout=&mode=&mode2=&label=` |
| GET | `/panelProbe` | Re‑test the panel; reports the BUSY line state |
| GET | `/scan` | Wi‑Fi scan results |
| POST | `/update` | Upload firmware (multipart) |
| POST | `/refresh` | Queue a forecast refresh and redraw |
| POST | `/location` | `{latitude, longitude, label}` |
| POST | `/timezone` | `{timezone}` — an IANA name |
| POST | `/units` | `{fahrenheit, mph}` |
| POST | `/layout` | `{layout}` — `temp` / `location` / `both` / `info` |
| POST | `/headerMode` | `{mode, mode2}` — the two information lines |
| POST | `/warningSource` | `{source}` — `off` / `derived` / `dwd` |
| POST | `/refreshInterval` | `{minutes}` |
| POST | `/powerOptions` | Quiet hours and low‑battery settings |
| POST | `/batteryPack` | `{mah}` — battery capacity |
| POST | `/keepAwake` | `{minutes}` — 0 cancels |
| POST | `/sleepMode` | `{enabled}` — deep sleep on/off |
| POST | `/batteryFull` | Reset the modelled charge estimate to 100 % |
| POST | `/language` | `{language}` — `en` / `de` / `es` / `fr` |
| POST | `/saveWiFi` | Credentials and static IP |
| POST | `/forgetWiFi` | Clear credentials and reboot to setup |
| POST | `/reboot` | Restart |

---

## 9. Credits

Forecasts come from **[Open-Meteo.com](https://open-meteo.com/)** under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); the credit in the web UI footer is there
because their licence asks for one next to where the data is shown.

Official weather warnings, when that source is selected, come from the **Deutscher Wetterdienst** via
**[Bright Sky](https://brightsky.dev/)**, and DWD's terms of use apply to them.
