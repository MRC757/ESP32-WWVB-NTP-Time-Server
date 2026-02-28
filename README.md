# WWVB Atomic Clock — ESP32-S3 with ES100 Receiver

A precision atomic clock using the ESP32-S3 microcontroller with an Everset ES100 WWVB receiver module. Displays time synchronized with NIST's WWVB 60 kHz time signal broadcast from Fort Collins, Colorado. When connected to WiFi, it also operates as a **Stratum 1 NTP server** for your local network.

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
- **NTP Sync**: If WiFi is connected, can also sync to NTP as a secondary time source
- **Time Source Priority**: WWVB > NTP > RTC > none; displayed on the UTC info page
- **Automatic DST Handling**: Uses DST data from the ES100 signal; adjustable in config
- **Adaptive Sync Schedule**: 5-minute attempts until first lock; 1-hour at night (best propagation); 4-hour during the day

### Display & Touch Interface
- **Touch Navigation**: Swipe left/right to move between 4 pages; long-press 10 s to shut down
- **Page 1 — Clock**: Large local time (12 or 24-hour), date, day of week, sync status, 48-hour WWVB reception chart
- **Page 2 — UTC Info**: UTC time, time source, seconds since last sync, DS3231 temperature, battery voltage/percentage/charging status
- **Page 3 — Settings**: Brightness slider (persisted to flash)
- **Page 4 — WiFi**: Scan and connect to WiFi networks; on-screen QWERTY keyboard for password entry
- **Sync Status Colors**: Green (< 2 h), Yellow (2–24 h), Red (> 24 h or never synced)
- **Reception Chart**: 48-hour bar chart of successful WWVB syncs per hour

### Networking
- **Stratum 1 NTP Server**: Serves RFC 5905-compliant NTP responses on UDP port 123; reference ID "WWVB"
- **Status Web Server**: Browseable dashboard at the device's IP (port 80) showing live time, temperature, battery, sync info, and NTP request count
- **Captive Portal**: If no WiFi credentials are stored, broadcasts an open AP (`WWVB-Clock-Setup`) with a browser-based setup page
- **WiFi Credentials**: SSID and password stored in NVS flash (survive reboots)

### Power & Hardware
- **ES100 Power Control**: ES100 is powered down between sync attempts (~0.1 µA off vs. ~8 mA receiving)
- **Deep Sleep Shutdown**: Hold the screen for 10 seconds to enter deep sleep; wake with a tap (GPIO21)
- **Battery Monitoring**: LiPo voltage and charge-state read from the LilyGo PMU; low-battery alert at 10%
- **Dual I2C Bus**: DS3231 RTC on Wire1 (GPIO15/16); ES100 shares Wire (GPIO2/3) with the touch panel and PMU using the STEMMA QT pull-up resistors

## Hardware Requirements

### Main Components

| Component | Description |
|-----------|-------------|
| LilyGo T-Display-S3 AMOLED | ESP32-S3 with 1.91" AMOLED display (536×240) |
| ES100 WWVB Receiver | Everset Technologies ES100-MOD or compatible |
| 60 kHz Ferrite Bar Antenna | WWVB receive antenna (1 or 2 supported) |
| DS3231 RTC Module | I2C real-time clock (optional but recommended) |

The ES100 module typically requires an external 16 MHz crystal — check your specific module's datasheet.

### Wiring

#### I2C Bus 0 — Wire (GPIO2/3): Touch, PMU, ES100

| ES100 Pin | ESP32-S3 Pin | Notes |
|-----------|--------------|-------|
| VDD | 3.3V | |
| GND | GND | |
| SDA | GPIO3 | STEMMA QT connector |
| SCL | GPIO2 | STEMMA QT connector |
| EN | GPIO40 | Enable / power control |
| IRQ- | GPIO41 | Interrupt (active low) |
| ANT1P/ANT1N | Antenna 1 | First ferrite antenna |
| ANT2P/ANT2N | Antenna 2 | Second ferrite antenna (optional) |

The touch panel and PMU are connected to this same bus by the LilyGo board internally. **Enable the ES100 module's on-board pull-up resistors** — the STEMMA QT connector provides pull-ups on SDA/SCL.

> **Pin conflict warning:** On the LilyGo T-Display-S3 AMOLED, GPIO9–GPIO14 are used by the QSPI display interface (FSPIHD, FSPICS0, FSPID, FSPICLK, FSPIQ, FSPIWP). Do not connect ES100 EN or IRQ to any of those pins — they will not work as GPIO.

#### I2C Bus 1 — Wire1 (GPIO15/16): DS3231

| DS3231 Pin | ESP32-S3 Pin | Notes |
|------------|--------------|-------|
| VCC | 3.3V | |
| GND | GND | |
| SDA | GPIO15 | Wire1 |
| SCL | GPIO16 | Wire1 |

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

All library dependencies are downloaded automatically from `platformio.ini`.

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

### Sync Schedule

```c
#define SYNC_INTERVAL_INITIAL_MS   300000UL   // 5 min (until first lock)
#define SYNC_INTERVAL_NIGHT_MS    3600000UL   // 1 hour (10 PM – 6 AM)
#define SYNC_INTERVAL_DAY_MS     14400000UL   // 4 hours (6 AM – 10 PM)
#define SYNC_DAY_MAX_FAILURES      3          // Failures before skipping to night
```

### Display

```c
#define USE_12_HOUR_FORMAT    false   // true = 12-hour with AM/PM
#define SHOW_SECONDS          true
#define DISPLAY_BRIGHTNESS    200     // 0–255 (also adjustable on Settings page)
```

### WiFi

```c
#define WIFI_AP_SSID          "WWVB-Clock-Setup"   // Captive portal AP name
#define WIFI_CONNECT_TIMEOUT  15000               // 15 s connect timeout
```

### Pin Assignments

Pins are documented in `config.h` for reference and defined as local constants in `wwvb_clock.ino`:

| Signal | GPIO | Bus |
|--------|------|-----|
| I2C SDA (touch, PMU, ES100) | 3 | Wire (bus 0) |
| I2C SCL (touch, PMU, ES100) | 2 | Wire (bus 0) |
| DS3231 SDA | 15 | Wire1 (bus 1) |
| DS3231 SCL | 16 | Wire1 (bus 1) |
| ES100 EN | 40 | — |
| ES100 IRQ | 41 | — |
| Touch INT | 21 | — |

## Operation

### Startup Sequence

1. Display initializes (LilyGo AMOLED auto-detects board variant)
2. DS3231 RTC checked — if present, time is loaded from RTC immediately
3. Saved WiFi credentials loaded; connection attempt begins if credentials exist
4. ES100 detected and verified on Wire (bus 0); first WWVB sync attempt starts
5. Clock displays running time from the best available source

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
| Clock | Local time, date, day of week, sync status, 48-hour reception chart |
| UTC Info | UTC time, time source, seconds since last sync, RTC temperature, battery |
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
- Reference clock identifier: `WWVB`
- Falls back to Stratum 2 if time source is NTP rather than WWVB

Point any NTP client (router, computer, smart home hub) at the device's IP address.

### Status Web Server

Browse to the device's IP address on port 80 to see a live dashboard:
- Current UTC and local time
- Time source (WWVB / NTP / RTC / None)
- Seconds since last WWVB sync
- DS3231 temperature (°C)
- Battery voltage, percentage, and charging status
- NTP requests served

### Sync Status Indicators

| Color | Indicator | Meaning |
|-------|-----------|---------|
| Green | Last sync: Xm ago | Recently synced (< 2 hours) |
| Yellow | Last sync: Xh ago | Sync aging (2–24 hours) |
| Red | Last sync: Xh ago | Stale sync (> 24 hours) |
| Yellow | SYNCING… | Reception attempt in progress |
| Red | NO SYNC YET | Never successfully synced |

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

- Check Wire wiring: SDA→GPIO3, SCL→GPIO2 (STEMMA QT connector)
- Check EN→GPIO40 and IRQ→GPIO41
- **Enable the pull-up resistors on the ES100 module** — the ES100 will not respond on I2C without them
- Verify 3.3V power to the ES100 module
- Make sure the ES100 has the required 16 MHz crystal
- **Do not use GPIO9–GPIO14 for EN or IRQ** — these are QSPI display pins (FSPIQ, FSPIWP, etc.) on the LilyGo AMOLED and cannot be used as GPIO

### No successful syncs

- Reposition the antenna (perpendicular to the Fort Collins direction, away from metal objects)
- Try syncing at night — daytime LF propagation is poor and the code backs off to 4-hour intervals
- Move away from WiFi routers, switching supplies, and motor controllers (major WWVB interference sources)
- Check antenna connections at the ES100 ANT1P/ANT1N terminals

### Clock drifting between syncs

Normal behavior. The ESP32-S3 crystal has ~20 ppm accuracy. The DS3231 RTC has ~2 ppm (with temperature compensation), so the RTC provides much better holdover between syncs.

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
| 0x04–0x09 | Date/Time | BCD-encoded year/month/day/hour/min/sec |
| 0x0A–0x0C | Next DST | Next DST transition month/day/hour |
| 0x0D | Device ID | Should read 0x10 |

### Reception Timing

| Mode | Duration |
|------|----------|
| Normal (1-minute frame) | ~134 seconds |
| Tracking mode | ~24.5 seconds |
| ES100 wakeup time | ~1–2 ms |

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
  └── ES100    — WWVB receiver (0x32)

Wire1 (bus 1, 100 kHz, GPIO15/16):
  └── DS3231   — RTC (0x68)
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
