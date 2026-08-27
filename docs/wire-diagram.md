# Wiring the Adafruit LC709203F Fuel Gauge

Step-by-step integration of an **Adafruit LC709203F LiPoly / LiIon Fuel Gauge** (PID 4712) into the
XIAO ESP32-C6 + ePaper Driver Board weather display.

> **Status:** the gauge is **on order** for this project. The firmware already ships with
> `kBatteryGaugeEnabled = true`; until the part arrives the device simply logs
> `Battery gauge not found on I2C` once per wake and shows **?** / **No sensor** in the header and
> the web UI. Nothing else changes — no reflash is needed when you plug it in.

---

## Why a fuel gauge and not a resistor divider

The driver board schematic (rev 1.0) never routes the `BAT_4V2` net to the XIAO, and on the
XIAO **ESP32-C6** the only ADC-capable pins — D0/A0, D1/A1, D2/A2 — are already taken by the
e-paper panel (RST, CS, BUSY). D4–D7 are GPIO22/23/16/17 and have no ADC at all.

So there is no way to *measure* the battery with this MCU. An I²C gauge on the free D4/D5 pins
sidesteps the problem entirely, and gives a real state-of-charge estimate rather than a voltage
guess.

---

## 1. Parts

| Item | Notes |
|---|---|
| Adafruit LC709203F breakout (PID 4712) | I²C address **0x0B**, fixed — not configurable |
| 2-pin JST-PH extension / pass-through cable | Gauge → driver board `CN_BAT` |
| 4 × jumper wires (or a cut STEMMA QT cable) | 3V3, GND, SDA, SCL |
| Soldering iron, multimeter | The multimeter is **not** optional — see step 3 |

The breakout already carries 10 kΩ pull-ups on SDA and SCL, so no extra resistors are needed.

---

## 2. Pin map

```
   XIAO ESP32-C6                    Adafruit LC709203F (4712)
  (on the driver board)
  ┌───────────────┐                 ┌──────────────────────────┐
  │ 3V3        ●──┼─────────────────┼──● VIN   (3–5 V)         │
  │ GND        ●──┼─────────────────┼──● GND                   │
  │ D4  GPIO22 ●──┼─────────────────┼──● SDA   (10K pullup)    │
  │ D5  GPIO23 ●──┼─────────────────┼──● SCL   (10K pullup)    │
  │               │                 │                          │
  │               │                 │  ● INT   – leave open    │
  │               │                 │  ● THERM – leave open*   │
  └───────────────┘                 └───┬──────────────┬───────┘
                                    JST │              │ JST
                                    (A) │              │ (B)
                                        │              │
                       LiPo ────────────┘              └──────────► driver board CN_BAT
                     (1000 mAh)                                     (the board's BAT connector)
```

`*` THERM is only used if your pack has a 10 kΩ NTC. Leave it open and keep
`kBatteryThermistorB = 0` in [weather_config.h](../include/weather_config.h).

The two JST ports on the breakout are **electrically identical** (battery in / load out) — the gauge
sits in parallel across the pack and passes the current straight through, so either port can face
the battery.

**Do not** power VIN from `VDD_5V`. The XIAO's I²C pins are 3.3 V and the board's pull-ups tie SDA
and SCL up to VIN.

---

## 3. ⚠️ Check JST polarity before you plug anything in

Adafruit and Seeed do **not** guarantee the same pin order on 2-pin JST-PH connectors. Getting this
wrong shorts or reverse-feeds a lithium cell.

1. Unplug the battery from everything.
2. Set the multimeter to DC volts and probe the **battery's own JST plug**. Note which physical
   contact is **+** (should read ~3.7–4.2 V relative to the other).
3. Probe the driver board's `CN_BAT` footprint and confirm which pin is **+** (trace it to the
   `BAT_4V2` net; the `H1` test pad sits on it).
4. Probe the gauge's JST ports against its own `GND` pin.
5. If any of the three disagree, re-pin the extension cable (lift the little plastic tab on the JST
   housing with a fingernail and swap the crimps) — **never** just force it in.

---

## 4. Assembly

1. **Power everything down.** Unplug USB and slide the driver board's power switch to off. Unplug
   the LiPo.
2. **Solder the header** onto the LC709203F breakout (or use its STEMMA QT socket and cut the far
   end off a QT cable: black = GND, red = VIN, blue = SDA, yellow = SCL).
3. **Optional but recommended:** cut the trace on the back of the breakout that enables the
   power LED. It saves a few hundred µA — far more than the gauge itself draws.
4. **Wire the four signals** per the map above. On the driver board, D4 and D5 are broken out on the
   `CN1` header (the `A4_D4_SDA` and `A5_D5_SCL` nets), and 3V3/GND are on `CN2` — solder there if
   you'd rather not touch the XIAO's own pads. Confirm against the silkscreen before soldering.
5. **Insert the gauge into the battery path:** LiPo → gauge JST (A); gauge JST (B) → driver board
   `CN_BAT`.
6. **Double-check** that no bare wire can touch the e-paper FPC or the panel's metal frame.
7. Plug the battery back in, then power up.

---

## 5. Verify

**a) The gauge answers on I²C.** Watch the serial monitor at 115200 baud on boot:

```
00:00:01 Battery gauge ready (IC version 0x2a1).
```

If instead you see `Battery gauge not found on I2C (checked address 0x0B).`, go to
Troubleshooting below.

**b) The panel** shows the battery glyph in the top-right of the header, filled proportionally with
the percentage overlaid (instead of `?`).

**c) The web UI** at `http://<device-ip>/` shows a **Battery** row with a meter, a percentage and a
voltage. Or check the JSON directly:

```bash
curl -s http://<device-ip>/status | python3 -m json.tool | grep -i batt
```

```json
"batteryValid": true,
"batteryPercent": 87,
"batteryVolts": "4.03",
```

---

## 6. Firmware configuration

All knobs live in [include/weather_config.h](../include/weather_config.h):

| Constant | Default | Meaning |
|---|---|---|
| `kBatteryGaugeEnabled` | `true` | Set `false` to build without the gauge entirely |
| `kBatterySdaPin` / `kBatterySclPin` | `D4` / `D5` | Move the bus if you rewire it |
| `kBatteryPackMah` | `500` | Gauge profile. **Must** be 100/200/500/1000/2000/3000 — 500 is the nearest to the 400 mAh cell in use |
| `kBatteryCapacityMah` | `400` | True pack capacity, used by the modelled estimate (not restricted to the profile steps) |
| `kBatteryThermistorB` | `0` | Set to `3950` if you fit a 10 kΩ NTC on THERM |
| `kBatteryLowPercent` | `20` | At or below this, the glyph and web meter turn red |

`kBatteryPackMah` picks the gauge's internal discharge profile, so set it to the nearest supported
step for your cell — 500 for the 400 mAh pack in use. `kBatteryCapacityMah` is separate and takes
the true capacity; it drives the modelled estimate used while no gauge is fitted.

Until the gauge arrives the display shows a modelled percentage with a trailing `?` (see the
README's "Battery level" section). Fitting the gauge replaces it with a real reading and the `?`
disappears — no code change needed.

---

## 7. How the firmware drives the gauge

- `setupBatteryGauge()` runs once per wake cycle (deep sleep resets the MCU) and configures pack
  size, temperature mode and power mode.
- The gauge is left in **`LC709203F_POWER_OPERATE`**, not put to sleep. Operating draw is ~15 µA
  against ~0.2 µA asleep — negligible here, and it lets the RSOC algorithm keep tracking the pack
  while the MCU is in deep sleep, which is the whole reason for using a gauge.
- `initRSOC()` is called **only on a cold boot** (power-on or reset), never on a timer wake.
  Re-seeding the estimate every 30 minutes would throw away the tracking you just paid for.
- `readBattery()` is the single read seam. It rejects NaN and out-of-range voltages, so a
  disconnected pack degrades to "no sensor" instead of showing nonsense.

---

## 8. Troubleshooting

| Symptom | Likely cause |
|---|---|
| `Battery gauge not found on I2C` | SDA/SCL swapped; VIN not connected; solder bridge. The address 0x0B is fixed — there is nothing to reconfigure. |
| Found, but percent reads 0 or jumps around | `kBatteryPackMah` doesn't match the cell, or the battery is wired to only one JST contact. |
| Percent is wildly optimistic after a battery swap | Reset the board once — `initRSOC()` re-seeds on cold boot. |
| Panel shows `?` but serial says the gauge is ready | `readBattery()` rejected the reading: check the pack is actually connected through the gauge's JST ports. |
| Display works, gauge dies when USB is unplugged | VIN is wired to `VDD_5V` instead of `3V3`. |
| I²C hangs the whole board | Missing common ground between the breakout and the XIAO. |

---

## References

- [Adafruit LC709203F breakout — pinouts](https://learn.adafruit.com/adafruit-lc709203f-lipo-lipoly-battery-monitor/pinouts)
- [Adafruit_LC709203F Arduino library](https://github.com/adafruit/Adafruit_LC709203F)
- [LC709203F datasheet (onsemi)](https://cdn-learn.adafruit.com/assets/assets/000/094/597/original/LC709203F-D.PDF)
- [ePaper Driver Board wiki](https://wiki.seeedstudio.com/xiao_eink_expansion_board_v2/)
