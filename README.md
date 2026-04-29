# WWVB Atomic Clock — ESP32-S3 with ES100 Receiver

A precision atomic clock using the ESP32-S3 microcontroller with an Everset ES100 WWVB receiver module. Displays time synchronized with NIST's WWVB 60 kHz time signal broadcast from Fort Collins, Colorado. When connected to WiFi, it also operates as a **Stratum 1 NTP server** for your local network.
<img width="1049" height="675" alt="Board" src="https://github.com/user-attachments/assets/5446def4-0a4c-4399-90ff-94bfeeb19bf8" />
<img width="715" height="1437" alt="Web_GUI" src="https://github.com/user-attachments/assets/44bd8ee1-7c0b-4cfd-bed4-3cfceeb4ff68" />

## Display Library

This project uses the **LilyGo-AMOLED-Series** library — the official library from LilyGo for their AMOLED display boards. This library:
- Auto-detects your specific board variant
- Handles QSPI display initialization automatically
- Provides optimized performance for LilyGo hardware

**GitHub:** https://github.com/Xinyuan-LilyGO/LilyGo-AMOLED-Series

## Features

### Time Synchronization
- **Atomic Time via WWVB**: Receives the NIST 60 kHz WWVB time signal for precise timekeeping
- **DS3231 RTC Backup**: Hardware real-time clock holds time through power cycles; loaded at boot before first WWVB sync
- **DS3231 1 Hz Phase Discipline**: The DS3231 `SQW/INT` output is wired to `GPIO39` and used as the second-boundary reference for NTP sub-second phase
- **Boundary-Aligned RTC Writes**: After NTP or WWVB updates, DS3231 writes are queued and applied on the next 1 Hz boundary. The sub-second offset at write time is saved; on the first post-write SQW edge the phase anchor is back-dated by that offset, restoring the WWVB/NTP sync's fractional second with microsecond accuracy
- **NTP Sync**: If WiFi is connected, can also sync to NTP as a secondary time source
- **Time Source Priority**: WWVB > NTP > RTC > none; displayed on the UTC info page
- **Automatic DST Handling**: Uses DST data from the ES100 signal; adjustable in config
- **Adaptive Sync Schedule**: 5-minute attempts until first lock; 1-hour at night (best propagation); 4-hour during the day
- **Tracking Mode**: After the first successful normal-mode sync, subsequent syncs use ES100 tracking mode (~24.5 s vs ~134 s). Tracking decodes only the WWVB sync word to snap the seconds field with ±4 s tolerance. The Control 0 register write must occur at second :55 of any minute; the firmware schedules this non-blocking via the main loop. Tracking uses the same antenna selected by historical success counts as normal mode. Falls back to normal mode after 7 days without a full sync. A sanity check validates that the decoded result falls 10–35 s after the :55 write; results outside this window are rejected to prevent applying the correction to the wrong minute when the clock is significantly off.
- **Nightly Normal-Mode Anchor**: Tracking mode cannot self-correct clock errors larger than the ES100's ±4 s timing tolerance — once the clock drifts outside that window, each tracking sync perpetuates rather than corrects the error. To prevent this, the firmware forces a full normal-mode sync at the start of each night (10 PM local) and retries every hour until one succeeds. After a successful full-frame decode, tracking mode resumes for the rest of the night.
- **Leap Second Detection**: The WWVB signal carries a leap second warning in the week(s) before a scheduled UTC adjustment. The firmware extracts this from the ES100 Status 0 register (bits 3:4) and displays it on the web dashboard.
- **Antenna Performance Tracking**: Per-antenna success counts are accumulated across reboots (NVS). Both normal-mode and tracking-mode syncs automatically use the antenna with the higher historical success count.
- **Latency Compensation**: The firmware measures the delay from ES100 IRQ fire to handler entry and applies it as floor-divided integer seconds plus a sub-second remainder — `delaySeconds = irqProcessingDelay / 1000`, `subSec = irqProcessingDelay % 1000`. For tracking mode the correct Unix second is back-computed from the recorded :55 write timestamp plus the WWVB seconds field. After setting the integer second, the sub-second accumulator is immediately restored to the IRQ latency remainder so NTP fractional timestamps remain stable between syncs. NTP client syncs combine the server's T3 sub-second fraction and half the measured RTT (`totalMs = T3_ms + rttMs/2`) before splitting into integer seconds and milliseconds, correctly handling the carry when `totalMs ≥ 1000 ms`. The NTP server's transmit timestamp (T3) is re-sampled atomically at send time. When the DS3231 square wave is connected, each 1 Hz edge re-anchors `TimeManager` so the NTP sub-second phase is disciplined by the RTC rather than the ESP32 `millis()` clock. DS3231 writes triggered by NTP or WWVB sync are deferred to the next 1 Hz boundary to keep the SQW phase coherent.

### Display & Touch Interface
- **Touch Navigation**: Swipe left/right to move between 5 pages; long-press 10 s to shut down
- **Page 1 — Clock**: Large local time (12 or 24-hour), date, day of week, last WWVB sync time, 48-hour WWVB reception chart
- **Page 2 — UTC Info**: UTC time, time source, seconds since last sync, DS3231 temperature, battery voltage/percentage/charging status, last WWVB sync time, 48-hour reception chart
- **Page 3 — Sync Status**: 48-hour sync count, time source, ES100 mode, signal quality, per-antenna success counts, last WWVB sync time
- **Page 4 — Settings**: Brightness slider (persisted to flash)
- **Page 5 — WiFi**: Scan and connect to WiFi networks; on-screen QWERTY keyboard for password entry
- **Sync Status Colors**: Green (< 12 h), Yellow (12–24 h), Red (> 24 h or never synced)
- **Reception Chart**: 48-hour bar chart of successful WWVB syncs per hour

### Networking
- **Stratum 1 NTP Server**: Serves RFC 5905-compliant NTP responses on UDP port 123; reference ID "WWVB". Stratum transitions automatically: **Stratum 1** after any WWVB sync; **Stratum 2** (upstream IP as reference) if NTP client sync was the last source; **Stratum 16** (LI=3, "LOCL") after 48 hours without any sync. Wi-Fi auto-connect no longer demotes stratum — if WWVB is the current time source, NTP client sync is skipped on connect. Responds only to client (mode 3) and symmetric-active (mode 1) NTP requests per RFC 5905.
- **NTP Fallback**: When Wi-Fi is connected and WWVB has never synced (or NTP is the configured source), the firmware queries a configurable host (`NTP_FALLBACK_HOST`, default `time.nist.gov`) as a secondary time reference.
- **Status Web Server**: Browseable dashboard at the device's IP (port 80) showing live time, temperature, battery, sync info, NTP request count, 48-hour WWVB reception chart, manual sync buttons, timezone controls, leap second warning, antenna statistics, and a recent sync log
- **Captive Portal**: If no WiFi credentials are stored, broadcasts an open AP (`WWVB-Clock-Setup`) with a browser-based setup page showing UTC time, local time, and a sync-source badge
- **WiFi Credentials**: SSID and password stored in NVS flash (survive reboots)

### Power & Hardware
- **ES100 Power Control**: ES100 is powered down between sync attempts (~0.1 µA off vs. ~8 mA receiving)
- **Deep Sleep Shutdown**: Hold the screen for 10 seconds to enter deep sleep; wake with a tap (GPIO21)
- **Battery Monitoring**: LiPo voltage and charge-state read from the LilyGo PMU; low-battery alert at 10%
- **Dual I2C Bus**: ES100 isolated on Wire1 (GPIO15/16) to prevent I2C contention with the touch panel and PMU during WWVB reception; DS3231 on Wire (GPIO2/3)

## Hardware Requirements

### Main Components

| Component | Description |
|-----------|-------------|
| LilyGo T-Display-S3 AMOLED | ESP32-S3 with 1.91" AMOLED display (536×240) |
| ES100 WWVB Receiver | Everset Technologies ES100-MOD or compatible |
| 60 kHz Ferrite Bar Antenna | WWVB receive antenna (1 or 2 supported) |
| DS3231 RTC Module | I2C real-time clock (optional but recommended) — **use a genuine Adafruit or SparkFun module; cheap clones drift 30–70 ppm** |

The ES100 chip requires an external 16 MHz crystal for its internal oscillator. The official **ES100-MOD** module includes this crystal onboard. Third-party breakout boards may omit it — check your specific module before assuming the crystal is present.

### Wiring

#### I2C Bus 0 — Wire (GPIO2/3): Touch, PMU, DS3231

| DS3231 Pin | ESP32-S3 Pin | Notes |
|------------|--------------|-------|
| VCC | 3.3V | |
| GND | GND | |
| SDA | GPIO3 | STEMMA QT connector |
| SCL | GPIO2 | STEMMA QT connector |
| SQW/INT | GPIO39 | 1 Hz square wave for RTC-disciplined NTP phase |

The touch panel and PMU are connected to this same bus by the LilyGo board internally.

#### I2C Bus 1 — Wire1 (GPIO15/16): ES100 (isolated)

The ES100 is on its own dedicated I2C bus to prevent contention with the touch controller and PMU during WWVB frame reception. **Enable the pull-up resistors on the ES100 module** — GPIO15/16 have no external pull-ups.

> **Pin conflict warning:** On the LilyGo T-Display-S3 AMOLED, GPIO9–GPIO14 are used by the QSPI display interface (FSPIHD, FSPICS0, FSPID, FSPICLK, FSPIQ, FSPIWP). Do not connect ES100 EN or IRQ to any of those pins — they will not work as GPIO.

```
ES100 Pin    →    ESP32-S3 Pin    Description
──────────────────────────────────────────────────
VDD          →    3.3V            Power supply
GND          →    GND             Ground
SDA          →    GPIO15          ES100 I2C Data (Wire1)
SCL          →    GPIO16          ES100 I2C Clock (Wire1)
EN           →    GPIO40          Enable / power control
IRQ-         →    GPIO41          Interrupt (active low)
ANT1P/ANT1N  →    Antenna 1       First ferrite antenna
ANT2P/ANT2N  →    Antenna 2       Second ferrite antenna (optional)
```

## Software Setup

### Using PlatformIO (Recommended)

1. Install [PlatformIO](https://platformio.org/install)
2. Clone or download this project
3. Open the project folder in VS Code with the PlatformIO extension
4. Edit `config.h` to set your timezone and preferences
5. Build and upload:
   ```bash
   pio run --target upload
   ```

All library dependencies are downloaded automatically from `platformio.ini`. The platform is pinned to `espressif32@^6.9.0` (Arduino core 2.x) because `TFT_eSPI` and `LilyGo-AMOLED-Series` depend on `SPIFFS.h`, which was removed from Arduino core 3.x.

### Using Arduino IDE

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software)

2. **Add ESP32 board support:**
   - File → Preferences → Additional Boards Manager URLs:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Tools → Board → Board Manager → search "esp32" → install **esp32 by Espressif Systems**

3. **Install required libraries** (Sketch → Include Library → Add .ZIP Library or Library Manager):
   - [LilyGo-AMOLED-Series](https://github.com/Xinyuan-LilyGO/LilyGo-AMOLED-Series) — download ZIP and install; also copy all folders from its `libdeps` folder to your Arduino libraries folder
   - **TFT_eSPI** ≥ 2.5.43 (via Library Manager)
   - **RTClib** by Adafruit ≥ 2.1.4 (via Library Manager)

4. **Select board settings:**
   - Tools → Board → esp32 → **ESP32S3 Dev Module**
   - Tools → USB CDC On Boot → **Enabled**
   - Tools → Flash Size → **16MB (128Mb)**
   - Tools → PSRAM → **OPI PSRAM**
   - Tools → Partition Scheme → **16M Flash (3MB APP/9.9MB FATFS)** or similar 16 MB scheme

5. Edit `config.h`, then upload the sketch.

## Configuration

Edit `config.h` to customize the clock.

### Timezone

```c
#define DEFAULT_UTC_OFFSET    -5    // Eastern Time
// -5 = EST, -6 = CST, -7 = MST, -8 = PST, 0 = UTC, 1 = CET
#define AUTO_DST_ENABLED      true  // Use DST data from ES100 signal
```

The UTC offset can also be changed at runtime from the web dashboard (Settings page → UTC Offset `−`/`+` buttons, DST toggle) without reflashing. Changes are saved to NVS immediately.

### Sync Schedule

```c
#define SYNC_INTERVAL_INITIAL_MS        300000UL   // 5 min (until first lock)
#define SYNC_INTERVAL_NIGHT_MS         3600000UL   // 1 hour (10 PM – 6 AM)
#define SYNC_INTERVAL_DAY_MS          14400000UL   // 4 hours (6 AM – 10 PM)
#define SYNC_DAY_MAX_FAILURES               3      // Normal-mode failures before skipping to night
#define SYNC_DAY_MAX_TRACKING_FAILURES     12      // Tracking-mode failures before skipping to night
```

### ES100 Tracking Mode

```c
#define ES100_USE_TRACKING          true           // Enable tracking mode after first sync
#define ES100_TRACKING_FALLBACK_MS  604800000UL    // Fall back to normal mode after 7 days
#define TRACKING_PENDING_TIMEOUT_MS 65000UL        // Max wait for :55 boundary (60 s + 5 s margin)
#define NIGHTLY_NORMAL_SYNC_HOURS   20UL           // Force normal mode if no full sync in this many hours
```

Tracking mode is enabled automatically after any successful normal-mode reception and persists for up to 7 days. Set `ES100_USE_TRACKING false` to always use normal mode.

Each night at 10 PM, the firmware forces a full normal-mode sync if no successful full-frame decode has occurred in the past `NIGHTLY_NORMAL_SYNC_HOURS` (default 20). Normal mode is retried every hour until it succeeds, then tracking resumes. This prevents tracking mode from perpetuating clock errors that exceed the ES100's ±4 s timing tolerance.

### Display

```c
#define USE_12_HOUR_FORMAT    false   // true = 12-hour with AM/PM
#define SHOW_SECONDS          true
#define DISPLAY_BRIGHTNESS    200     // 0–255 (also adjustable on Settings page)
```

### NTP Fallback Server

```c
#define NTP_FALLBACK_HOST     "time.nist.gov"
// Change to a regional pool if time.nist.gov is unreachable, e.g.:
//   "us.pool.ntp.org"   — IANA NTP Pool (Americas)
//   "time.cloudflare.com"
```

The fallback host is queried only when Wi-Fi is connected and the current time source is not WWVB. It has no effect on the Stratum 1 NTP server.

### WiFi

```c
#define WIFI_AP_SSID          "WWVB-Clock-Setup"   // Captive portal AP name
#define WIFI_CONNECT_TIMEOUT  15000               // 15 s connect timeout
```

### Pin Assignments

Pins are documented in `config.h` for reference and defined as local constants in `wwvb_clock.ino`:

| Signal | GPIO | Bus |
|--------|------|-----|
| I2C SDA (touch, PMU, DS3231) | 3 | Wire (bus 0) |
| I2C SCL (touch, PMU, DS3231) | 2 | Wire (bus 0) |
| ES100 SDA | 15 | Wire1 (bus 1) |
| ES100 SCL | 16 | Wire1 (bus 1) |
| ES100 EN | 40 | — |
| ES100 IRQ | 41 | — |
| Touch INT | 21 | — |
| DS3231 SQW/INT | 39 | 1 Hz interrupt input |

## Operation

### Startup Sequence

1. Display initializes (LilyGo AMOLED auto-detects board variant)
2. DS3231 RTC checked — if present, time is loaded from RTC immediately
3. Saved WiFi credentials loaded; connection attempt begins if credentials exist
4. On Wi-Fi connect: NTP sync runs automatically if time source is RTC or None (validates DS3231 set-point before WWVB arrives)
5. DS3231 `SQW/INT` is configured for 1 Hz output on `GPIO39`; each detected edge re-anchors the internal phase clock
6. ES100 detected and verified on Wire1 (bus 1, isolated); first WWVB sync attempt starts
7. Clock displays running time from the best available source

### Touch Interface

| Gesture | Action |
|---------|--------|
| Swipe left | Next page |
| Swipe right | Previous page |
| Hold 2 s | Shutdown countdown starts |
| Hold 10 s | Deep sleep (tap screen to wake) |
| Tap brightness slider | Adjust brightness (Settings page) |

### Pages

| Page | Content |
|------|---------|
| Clock | Local time, date, day of week, last WWVB sync time, 48-hour reception chart |
| UTC Info | UTC time, time source, seconds since last sync, RTC temperature, battery, last WWVB sync time, 48-hour reception chart |
| Sync Status | 48h sync count, source, ES100 mode, signal quality, antenna success counts, last WWVB sync time |
| Settings | Brightness slider |
| WiFi | Network list, connect button, on-screen keyboard for password |

### WiFi Setup

**Option 1 — On-screen keyboard:** Go to the WiFi page, select a network from the scanned list, type the password using the QWERTY touch keyboard, and tap Connect.

**Option 2 — Captive portal:** If no credentials are stored, the clock broadcasts an open WiFi access point named `WWVB-Clock-Setup`. Connect your phone or computer to it, open a browser, and a setup page appears for entering credentials. The AP shows the current time at the top of the page.

Once connected to WiFi:
- The NTP server starts automatically on UDP port 123
- The status web server starts on port 80 (browse to the device IP)
- Time can also be synced from NTP as a fallback when WWVB is unavailable

### NTP Server

When the clock has a valid time and WiFi is connected, it acts as a **Stratum 1 NTP server**:
- UDP port 123, RFC 5905 compliant
- Responds only to client (mode 3) and symmetric-active (mode 1) requests
- NTP transmit timestamps use the DS3231 1 Hz square wave on `GPIO39` as the sub-second phase reference when available
- After NTP/WWVB updates, the DS3231 is programmed on the next 1 Hz boundary so its `SQW/INT` phase stays aligned with UTC

**Stratum transitions:**

| Condition | Stratum | Reference ID |
|-----------|---------|--------------|
| After any WWVB sync | 1 | `WWVB` |
| After NTP client sync (WWVB unavailable) | 2 | Upstream server IPv4 |
| No sync for 48+ hours | 16 (LI=3) | `LOCL` |

**Wi-Fi connect NTP auto-sync policy:**

| Time source at connect | Behavior |
|------------------------|----------|
| RTC or None | Always NTP-sync — corrects any DS3231 set-point error before WWVB has a chance to run |
| NTP | NTP-sync only if last sync was > 2 hours ago |
| WWVB | Never NTP-sync — WWVB is authoritative; device stays at Stratum 1 |

This ensures the DS3231's stored time is validated against a network reference on every cold boot while never demoting a freshly WWVB-synced clock to Stratum 2.

Point any NTP client (router, computer, smart home hub) at the device's IP address.

### Status Web Server

Browse to the device's IP address on port 80 to see a live dashboard:
- Current UTC and local time (updated every 250 ms from a local reference; full data fetched every 30 s)
- Time source (WWVB / NTP / RTC / None) and time since last sync
- DS3231 temperature (°C / °F)
- Battery voltage, percentage, and charging status
- NTP requests served
- ES100 receiver state: Idle / Waiting for :55… / Receiving… / Tracking…
- UTC offset controls (− / + buttons) and DST toggle — saved to NVS immediately
- Leap second warning from the WWVB signal (shown when a UTC ±1 s adjustment is pending)
- Antenna success counters (Ant1 / Ant2)
- 48-hour WWVB reception history bar chart (syncs per hour)
- Sync log table — last 20 individual sync outcomes (time, mode, antenna, result)

**Manual sync buttons:**

| Button | Mode | Duration | Notes |
|--------|------|----------|-------|
| Normal Sync (blue) | Full 1-minute frame | ~134 s | Provides full date, time, and DST data |
| Tracking Sync (orange) | Sync-word only | ~24.5 s | Snaps seconds field only; waits up to 60 s for the :55 boundary |

Both buttons are disabled while a sync is in progress or a tracking start is pending. The Normal Sync button re-enables after 5 seconds; the Tracking Sync button re-enables after 65 seconds (matching the maximum :55 wait).

**API endpoints:**

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Full JSON snapshot (time, battery, ES100, chart data, leap second, antenna stats) |
| `/api/sync` | POST | Start a normal-mode WWVB sync |
| `/api/sync/tracking` | POST | Schedule a tracking-mode WWVB sync |
| `/api/settings` | GET | Returns `{"off": -5, "dst": false}` |
| `/api/settings` | POST | Set UTC offset and DST (`off=-5&dst=0`) |
| `/api/log` | GET | JSON array of last 20 sync log entries |

### Sync Status Indicators

| Color | Indicator | Meaning |
|-------|-----------|---------|
| Green | Last WWVB: Xm ago | Recently synced (< 12 hours) |
| Yellow | Last WWVB: Xh Xm ago | Sync aging (12–24 hours) |
| Red | Last WWVB: Xh Xm ago | Stale sync (> 24 hours) |
| Yellow | SYNCING… | Reception attempt in progress |
| Red | NO SYNC YET | Never successfully synced |

### Signal Quality

Displayed on the device's Sync Status page and in the web dashboard status table.

```
STRONG | TRACKING | Ant: A1
```

| Field | Values | Meaning |
|-------|--------|---------|
| Quality | STRONG | ≥ 8 successful syncs in the last 48 h |
| | GOOD | ≥ 4 syncs in the last 48 h |
| | FAIR | ≥ 1 sync in the last 48 h |
| | POOR | 0 syncs in the last 48 h |
| Mode | TRACKING | A successful normal-mode sync has been achieved; subsequent syncs use the faster tracking mode (~24.5 s). Indicates sustained good signal. |
| | NORMAL | No normal-mode sync yet this session; using full 1-minute frame decodes |
| Antenna | A1 / A2 | One antenna has ≥ 2× the success count of the other |
| | A1+A2 | Both antennas contribute roughly equally |

The quality color matches the sync headline: green (STRONG/GOOD), yellow (FAIR), red (POOR). The ES100 does not expose an RSSI or SNR register; quality is derived entirely from reception statistics.

### Reception Chart

The bar chart shows successful WWVB receptions over the past 48 hours:
- X-axis: –48 h → Now
- Y-axis: Successful syncs per hour
- Green bars = successful reception

### Best Reception Tips

1. **Antenna orientation**: Ferrite antenna perpendicular to the direction of Fort Collins, CO
2. **Time of day**: Nighttime (10 PM–6 AM) is best — less atmospheric absorption, less interference
3. **Location**: Away from switching power supplies, LED drivers, motors, and fluorescent lights
4. **Distance**: WWVB covers most of North America; signal is weakest near the West Coast and in Canada

## Troubleshooting

### Display doesn't initialize

- Confirm you are using the LilyGo-AMOLED-Series library (not a generic display driver)
- In Arduino IDE, verify the board package and board settings match the table above

### "ES100: FAILED" at startup

- Check Wire1 wiring: SDA→GPIO15, SCL→GPIO16
- Check EN→GPIO40 and IRQ→GPIO41
- **Enable the pull-up resistors on the ES100 module** — GPIO15/16 have no external pull-ups; the ES100 will not respond on I2C without them
- Verify 3.3V power to the ES100 module
- Make sure the ES100 has the required 16 MHz crystal
- **Do not use GPIO9–GPIO14 for EN or IRQ** — these are QSPI display pins (FSPIQ, FSPIWP, etc.) on the LilyGo AMOLED and cannot be used as GPIO

### No successful syncs

- Reposition the antenna (perpendicular to the Fort Collins direction, away from metal objects)
- Try syncing at night — daytime LF propagation is poor and the code backs off to 4-hour intervals
- Move away from WiFi routers, switching supplies, and motor controllers (major WWVB interference sources)
- Check antenna connections at the ES100 ANT1P/ANT1N terminals

### Clock drifting between syncs

The ESP32-S3 crystal has ~20 ppm accuracy. A **genuine DS3231** (Maxim/Analog Devices) has ~2 ppm with temperature compensation, providing much better holdover between WWVB syncs.

With the DS3231 `SQW/INT` output wired to `GPIO39`, the NTP server no longer free-runs its sub-second phase on the ESP32 clock alone. Whole seconds come from WWVB/NTP/RTC sync state, and the DS3231 1 Hz edge continuously re-anchors the fractional phase used in NTP responses.

RTC writes are queued and applied at the next 1 Hz boundary. The `rtc.adjust()` call resets the DS3231 oscillator phase; the firmware saves the sub-second offset at write time (`rtcWriteSubsecMs`) and on the following SQW edge back-dates the phase anchor by that value, fully restoring the WWVB/NTP sync's fractional second. Three bugs were found and fixed during development:

- **+1 s jump after each sync** (fixed April 2026): `processDS3231SquareWave()` was calling `setRTCPhaseAnchor(M+1, currentEdge)` immediately after the write — anchoring second M+1 to the old oscillator's edge. Fixed by skipping the anchor on the write edge and waiting for the next natural edge.
- **Sub-second phase loss after write** (fixed April 2026): the first post-write SQW edge fires at `T + subsecMs` past the true second boundary. Without correction, `rtc.now()` returns the integer second and the anchor is set `subsecMs` early, making the clock slow by that amount. Fixed by back-dating the anchor micros by `subsecMs × 1000 µs`.
- **Phase correction dropped after first post-write edge** (fixed April 2026): the write-done handler correctly back-dates the anchor on edge E1, but the DS3231 SQW remains `rtcWriteSubsecMs` late relative to the true second boundary on every subsequent edge until the next write. The normal edge handler was using raw `edgeMicros` (no correction), overwriting the E1 anchor and leaving the clock permanently slow by `rtcWriteSubsecMs` (up to ~500 ms). Fixed by applying `edgeMicros − rtcWriteSubsecMs × 1000 µs` on every normal SQW edge, not just E1.

**Clone DS3231 modules are a known problem.** Cheap breakout boards from Amazon/AliExpress (HiLetgo, etc.) frequently pair the DS3231 die with an off-spec or aged crystal, resulting in 30–70 ppm drift — 15–35× outside spec. Symptoms:

- Clock drifts 1–2 seconds over 6 hours between WWVB syncs
- Tracking sync must reanchor every hour to maintain accuracy
- Drift is too large for the ES100's ±4 s tracking tolerance to absorb after a missed overnight normal-mode sync

**How to identify a suspect module:** Check the date code on the chip. A code like `1438` means week 38 of 2014 — a 10+ year old chip. Even genuine Maxim silicon drifts more as the crystal ages. Modules with no legible Maxim/ADI logo are almost certainly clones.

**Recommended modules:**
- Adafruit DS3231 Precision RTC Breakout (#3013) — genuine DS3231SN with a Seiko Epson crystal
- SparkFun DeadOn RTC Breakout (BOB-12708) — also uses genuine parts

The nightly normal-mode WWVB anchor (10 PM) limits maximum DS3231 holdover to ~16 hours. At spec (2 ppm) that is < 120 ms of drift. At 65 ppm (bad clone) that is ~3.7 s — near the ES100's ±4 s tracking tolerance limit.

### WiFi won't connect

- Verify the password on the WiFi page (long-press Backspace to clear)
- The clock only supports 2.4 GHz networks
- If the target AP and the clock's captive-portal AP are on different channels, STA connection may fail (single-radio limitation) — disable the captive portal by entering credentials via the on-screen keyboard instead

## Technical Details

### ES100 Register Map

| Address | Register | Description |
|---------|----------|-------------|
| 0x00 | Control 0 | Reception control |
| 0x02 | IRQ Status | Interrupt status |
| 0x03 | Status 0 | Reception status |
| 0x04–0x09 | Date/Time | BCD-encoded year/month/day/hour/min/sec (normal mode only) |
| 0x0A–0x0C | Next DST | Next DST transition month/day/hour |
| 0x0D | Device ID | Should read 0x10 |

> **Tracking mode register note:** In tracking mode the ES100 START write clears registers 0x04–0x08 (year through minute) to 0x00. Only register 0x09 (Second) is populated after a successful tracking reception. Reading the date/time registers after a tracking sync would corrupt the RTC with year 2000 — the driver's `readTrackingResult()` reads only register 0x09 and snaps it onto the current RTC time.

### Reception Timing

| Mode | Duration | Notes |
|------|----------|-------|
| Normal (1-minute frame) | ~134 seconds | Full date, time, DST; any second |
| Tracking mode | ~24.5 seconds | Second only; Control 0 must be written at second :55 |
| ES100 wakeup time | ~20 ms | EN high → I2C ready |
| Tracking schedule delay | 0–60 seconds | Time until the next :55 boundary |

**Tracking mode timing:** The ES100 datasheet requires the Control 0 register write at exactly second :55 (±4 s drift tolerance) so that the 22-second reception window captures the WWVB frame sync word. The firmware computes the time until the next :55 boundary and schedules the write non-blocking, powering the ES100 on ~50 ms early to satisfy the wakeup requirement. The 30-second reception timeout is measured from the moment the Control 0 register is written at :55, not from when the sync was scheduled. After the IRQ fires, the correct Unix second is back-computed from the :55 write timestamp plus the WWVB seconds field; a sanity check confirms the result falls 10–35 s after the write before the correction is applied.

### Time Accuracy Chain

Each stage in the path from the WWVB signal to a client clock introduces error. Understanding each stage helps set realistic expectations and identify where improvement is possible.

```
WWVB signal → ES100 → ESP32 NTP server → WiFi → Client PC → Browser → time.gov
```

| Stage | Typical Error | Notes |
|-------|--------------|-------|
| WWVB signal (NIST) | < 1 µs | Primary UTC reference; error is negligible |
| ES100 decode + IRQ latency | < 1 ms | Firmware measures and compensates IRQ delay; tracking back-computes from :55 write timestamp |
| DS3231 holdover (daytime) | < 120 ms | ~2 ppm drift over up to 16 h of daytime (no WWVB reception). RTC writes are boundary-aligned so SQW phase stays coherent after NTP/WWVB syncs |
| ESP32 WiFi NTP response | 5–50 ms jitter | WiFi adds variable one-way latency. T3 is re-sampled atomically at transmit time; `getUnixTime()` accounts for sub-second elapsed time between 1 Hz ticks; client RTT/2 compensation removes the constant part. Jitter is the residual asymmetric delay |
| **NTP client poll interval** | **0 ms – seconds** | **Largest controllable error.** Windows default can poll as infrequently as every 9 hours, allowing the PC clock to drift by seconds. For a local NTP server, reduce to 64–1024 s (see below) |
| time.gov browser measurement | 10–50 ms floor | JavaScript timer resolution (~15 ms on Windows) plus internet RTT to time.gov servers |

**Overall accuracy to UTC when everything is working:**
- NTP server vs. UTC: sustained < 100 ms vs time.gov; < 120 ms worst-case during daytime DS3231 holdover between WWVB syncs
- PC clock vs. NTP server: < 20 ms after a recent poll (limited by WiFi one-way jitter)
- Visible on time.gov: typically < 100 ms (browser JavaScript timer resolution + internet RTT to NIST servers)

#### Real-World Accuracy Example

Measured April 2026 with all SQW phase-anchor fixes applied, WWVB as time source:

- `w32tm /stripchart /computer:192.168.0.243 /samples:5` — offsets `−0.063 s` to `−0.015 s` (variation due to WiFi path asymmetry, not clock error)
- time.gov browser reading: sustained **< 100 ms** vs UTC across multiple measurement sessions
- Result: sub-second phase maintained continuously across SQW edges; DS3231 holdover between WWVB syncs adds < 120 ms worst-case; remaining variation is WiFi jitter and browser measurement noise

> **Root Dispersion note:** `w32tm` maintains a filter of 8 historical offset samples. If the clock source was previously off by seconds, dispersion stays elevated until all 8 bad samples are replaced — at 1024 s/poll that takes about 2.3 hours. This is a Windows client artifact; it does not indicate the WWVB device is inaccurate. Once the filter converges, dispersion drops below 1 s and continues falling.

#### Recommended Windows NTP client settings for a local server

The default Windows NTP configuration polls as infrequently as every 9 hours, which allows the PC clock to drift by seconds between polls. Run these commands in an elevated command prompt to poll every 64–1024 seconds instead:

```
w32tm /config /manualpeerlist:"<device-ip>,0x8" /syncfromflags:manual /update
w32tm /config /MinPollInterval:6 /MaxPollInterval:10 /update
net stop w32tm && net start w32tm
w32tm /resync /force
```

Replace `<device-ip>` with the clock's IP address. The `0x8` flag selects NTP client mode without the SpecialPollInterval override. After these changes, `w32tm /query /status` should show `Poll Interval: 6 (64s)` growing toward `10 (1024s)` as the source stabilises.

### Power Consumption (ES100)

| State | Current |
|-------|---------|
| Receiving | ~8 mA |
| Processing | ~28 mA |
| Idle (EN high) | ~2.5 mA |
| Shutdown (EN low) | ~0.1 µA |

### I2C Bus Architecture

```
Wire  (bus 0, 400 kHz, GPIO2/3):
  ├── CST816T  — Capacitive touch (0x15)
  ├── AXP2101  — PMU / battery (0x34)
  └── DS3231   — RTC (0x68)

Wire1 (bus 1, 100 kHz, GPIO15/16):
  └── ES100    — WWVB receiver (0x32)  ← isolated to prevent contention
```

> **I2C note:** The ES100 requires a STOP condition between the register-address write and the data read (no repeated start). This differs from many I2C devices. The driver uses `endTransmission(true)` for all ES100 reads.

## Project Files

| File | Description |
|------|-------------|
| `wwvb_clock.ino` | Main application: display, touch, WiFi state machine, page rendering |
| `config.h` | All configuration constants (timezone, sync timing, pins, display) |
| `ES100.h` / `ES100.cpp` | Everset ES100 WWVB receiver driver |
| `TimeManager.h` / `TimeManager.cpp` | UTC timekeeping using ESP32 millis(); local time conversion |
| `ReceptionHistory.h` / `ReceptionHistory.cpp` | Rolling 48-hour sync history for the reception chart |
| `NTPServer.h` | Stratum 1 NTP server (UDP 123) |
| `CaptivePortal.h` | Open-AP captive portal for browser-based WiFi setup |
| `StatusServer.h` | HTTP status dashboard (port 80) |
| `platformio.ini` | PlatformIO build configuration |

## License

MIT License — see the LICENSE file for details.

## Acknowledgments

- ES100 datasheet: Everset Technologies
- LilyGo-AMOLED-Series library: Xinyuan-LilyGO
- NIST WWVB time signal service
