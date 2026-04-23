/**
 * @file      config.h
 * @brief     Configuration Settings for WWVB Atomic Clock
 * 
 * Display Library: LilyGo-AMOLED-Series (official LilyGo library)
 * 
 * Modify these settings to match your hardware configuration.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// HARDWARE CONFIGURATION
// ============================================================================

// The LilyGo-AMOLED-Series library auto-detects board type
// Supported boards include:
//   - T-Display-S3 AMOLED 1.91" (240x536)
//   - T-Display-S3 AMOLED 1.43" (466x466)
//   - T-Display-S3 AMOLED 1.64"
//   - T4-S3 AMOLED
//   - And others...

// ============================================================================
// TIMEZONE CONFIGURATION
// ============================================================================

// UTC offset for your timezone (hours)
// Examples:
//   -5 = Eastern Standard Time (EST)
//   -6 = Central Standard Time (CST)
//   -7 = Mountain Standard Time (MST)
//   -8 = Pacific Standard Time (PST)
//   0  = UTC / GMT
//   1  = Central European Time (CET)
#define DEFAULT_UTC_OFFSET    -5

// Enable automatic DST handling based on ES100 data
#define AUTO_DST_ENABLED      true

// ============================================================================
// SYNC CONFIGURATION
// ============================================================================

// Interval between sync attempts when never synced (milliseconds)
// Default: 5 minutes — aggressive until first time lock
#define SYNC_INTERVAL_INITIAL_MS   300000UL

// Nighttime sync interval (10 PM - 6 AM local) — best LF propagation
// Default: 1 hour
#define SYNC_INTERVAL_NIGHT_MS     3600000UL

// Daytime sync interval (6 AM - 10 PM local) — poor LF propagation
// Default: 4 hours (saves power, low success rate during day)
#define SYNC_INTERVAL_DAY_MS       14400000UL

// Nighttime window boundaries (local time, 24-hour)
#define SYNC_NIGHT_START_HOUR      22   // 10 PM local
#define SYNC_NIGHT_END_HOUR        6    // 6 AM local

// Consecutive daytime normal-mode failures before skipping until nighttime
#define SYNC_DAY_MAX_FAILURES      3

// Consecutive daytime tracking-mode failures before skipping until nighttime.
// Tracking attempts cost only ~10 s vs ~134 s for normal mode, so a higher
// threshold is acceptable without meaningfully increasing wasted time.
#define SYNC_DAY_MAX_TRACKING_FAILURES  12

// Maximum time for a sync attempt before timeout (milliseconds)
// Normal mode: 3 minutes (ES100 1-minute frame + antenna toggle retry)
#define SYNC_TIMEOUT_NORMAL_MS     180000UL
// Tracking mode: 30 seconds (typically completes in ~10s)
#define SYNC_TIMEOUT_TRACKING_MS   30000UL

// Maximum time to wait for the :55 second boundary before aborting a pending
// tracking start (one full minute + 5 second safety margin)
#define TRACKING_PENDING_TIMEOUT_MS  65000UL

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================

// Display brightness (0-255)
#define DISPLAY_BRIGHTNESS    200
#define BRIGHTNESS_MIN        10      // Minimum usable brightness
#define BRIGHTNESS_MAX        255     // Maximum brightness

// Touch gesture thresholds
#define SWIPE_THRESHOLD_PX    50      // Minimum pixels for swipe detection
#define LONG_PRESS_MOVE_PX    30      // Max movement for long-press (pixels)
#define SHUTDOWN_HOLD_MS      10000   // Long-press duration for shutdown (10 seconds)
#define SHUTDOWN_FEEDBACK_MS  2000    // Show countdown after this hold time

// 12-hour vs 24-hour time format
// true = 12-hour with AM/PM
// false = 24-hour
#define USE_12_HOUR_FORMAT    false

// Show seconds on main clock display
#define SHOW_SECONDS          true

// ============================================================================
// PIN ASSIGNMENTS
// The display pins are handled automatically by the LilyGo library.
// All pin definitions below are for reference; the actual constants are
// declared in wwvb_clock.ino.
//
// Dual I2C Bus Architecture:
//   Wire  (bus 0, 400 kHz): GPIO2/3   — touch panel, PMU, DS3231 RTC
//   Wire1 (bus 1, 100 kHz): GPIO15/16 — ES100 WWVB receiver (isolated)
//
// ES100 is isolated on Wire1 to prevent I2C contention with the touch controller
// and PMU during WWVB frame reception. Enable pull-up resistors on the ES100 module.
// ============================================================================

// I2C Bus 0 (Wire) — touch, PMU, DS3231 RTC (STEMMA QT connector)
#define PIN_I2C_SDA         3
#define PIN_I2C_SCL         2

// I2C Bus 1 (Wire1) — ES100 WWVB receiver (isolated)
#define PIN_ES100_SDA       15
#define PIN_ES100_SCL       16

// ES100 control pins
#define PIN_ES100_EN        40    // ES100 Enable (power control)
#define PIN_ES100_IRQ       41    // ES100 IRQ (active low, interrupt)

// Touch panel interrupt — handled by LilyGo library
#define PIN_TOUCH_INT       21

// DS3231 1 Hz square-wave output.
// Set this to the ESP32 GPIO wired to the DS3231 SQW/INT pin to discipline
// NTP sub-second phase from the RTC. Leave at -1 to disable the feature.
#define PIN_DS3231_SQW      39

// ============================================================================
// ES100 ANTENNA CONFIGURATION
// ============================================================================

// Antenna selection for reception
// true = Start with Antenna 1
// false = Start with Antenna 2
// The ES100 will toggle between antennas on failed attempts
#define ES100_START_ANT1      true

// Use tracking mode for periodic drift adjustments after a successful normal-mode sync.
// Tracking mode completes in ~10 s vs ~134 s for normal mode, and is sufficient when
// the RTC holds time to within ~1 s/week between syncs.
#define ES100_USE_TRACKING    true

// How long to persist tracking mode through consecutive failures before falling back
// to a full normal-mode sync. If the signal is too weak for 10-second tracking, it is
// also too weak for 134-second normal mode. The DS3231 drifts ~173 µs/day (2 ppm max);
// 30 s of drift would take ~174 days, so 7 days (~1.2 ms total drift) is well within
// the tracking mode tolerance.
#define ES100_TRACKING_FALLBACK_MS  604800000UL  // 7 days

// Hours between forced normal-mode syncs during the nighttime window (10 PM – 6 AM).
// Tracking mode cannot self-correct errors larger than the ES100's ±4 s timing tolerance;
// a nightly full-frame sync re-anchors absolute time and breaks any error cycle.
// At 10 PM, if no normal-mode success in this many hours, normal mode is retried every
// hour until it succeeds, then tracking resumes for the rest of the night.
#define NIGHTLY_NORMAL_SYNC_HOURS   20UL

// ============================================================================
// WATCHDOG CONFIGURATION
// ============================================================================

// Task watchdog timeout (milliseconds).
// 15 s is safe: all sync I/O is interrupt-driven; no loop operation blocks
// longer than a single I2C transaction (~2 ms at 100 kHz).
#define WATCHDOG_TIMEOUT_MS   15000

// ============================================================================
// DEBUG CONFIGURATION
// ============================================================================

// Enable serial debug output
#define DEBUG_SERIAL          true

// Serial baud rate
#define DEBUG_BAUD_RATE       115200

// Verbose I2C debugging
#define DEBUG_I2C             false

// ============================================================================
// WIFI CONFIGURATION
// ============================================================================

// Access Point name for captive portal fallback
#define WIFI_AP_SSID          "WWVB-Clock-Setup"
#define WIFI_AP_CHANNEL       1
#define WIFI_CONNECT_TIMEOUT  15000   // 15 seconds to connect
#define WIFI_SCAN_INTERVAL    30000   // Re-scan every 30s on WiFi page
#define WIFI_MAX_NETWORKS     15      // Max networks to store from scan
#define WIFI_MAX_VISIBLE      6       // Max networks visible at once in list

// ============================================================================
// NTP SERVER CONFIGURATION
// ============================================================================

#define NTP_PORT              123
#define NTP_EPOCH_OFFSET      2208988800UL  // Seconds between 1900-01-01 and 1970-01-01

// NTP fallback host — queried when Wi-Fi is connected and no WWVB sync is available.
// Change to a regional pool (e.g., "us.pool.ntp.org") if time.nist.gov is unreachable.
#define NTP_FALLBACK_HOST     "time.nist.gov"

// ============================================================================
// ON-SCREEN KEYBOARD GEOMETRY
// ============================================================================

#define KB_INPUT_Y            22      // Input field Y position
#define KB_INPUT_HEIGHT       26      // Input field height
#define KB_Y_OFFSET           56      // First keyboard row Y position
#define KB_KEY_WIDTH          48      // Standard key width
#define KB_KEY_HEIGHT         38      // Key height
#define KB_KEY_GAP            2       // Gap between keys
#define KB_ROW1_X             18      // Row 1 starting X (10 keys)
#define KB_ROW2_X             43      // Row 2 starting X (9 keys, centered)
#define KB_ROW3_X             8       // Row 3 starting X (Shift + 7 keys + Backspace)
#define KB_ROW4_X             8       // Row 4 starting X (123 + Space + Connect)
#define KB_SPECIAL_WIDTH      62      // Width of Shift, Backspace, 123 keys
#define KB_SPACE_WIDTH        270     // Width of space bar
#define KB_CONNECT_WIDTH      120     // Width of Connect button
#define KB_PASSWORD_MAX       63      // Max password length

// ============================================================================
// ADVANCED SETTINGS
// ============================================================================

// I2C clock speed (Hz)
// ES100 supports up to 400kHz
#define I2C_CLOCK_SPEED       400000

// Display update rate (milliseconds)
// Lower values = smoother updates but more CPU usage
#define DISPLAY_UPDATE_MS     1000

// Chart history display (hours)
// Must match HISTORY_HOURS in ReceptionHistory.h
#define CHART_HOURS           48

// ============================================================================
// BATTERY CONFIGURATION
// ============================================================================

// Low battery alert threshold (percentage, 0-100)
#define LOW_BATTERY_THRESHOLD   10

// Critical battery threshold — forces deep sleep to protect NVS
// Battery will not charge below ~3.0V; shutting down at 5% prevents corruption
#define CRITICAL_BATTERY_THRESHOLD  5

// Low battery serial warning interval (milliseconds)
#define LOW_BATTERY_WARN_MS     60000

// ============================================================================
// WWVB SYNC TRUST WINDOW
// ============================================================================

// Duration (ms) to suppress DS3231 read-back after a WWVB sync.
// Prevents the DS3231's own 1Hz register boundary from overwriting a fresh WWVB correction.
#define WWVB_SYNC_TRUST_WINDOW_MS        10000UL

// ============================================================================
// NTP SERVER QUALITY PARAMETERS
// ============================================================================

// DS3231 maximum drift rate (ppm) — used to compute NTP Root Dispersion growth.
// Datasheet spec is ±2 ppm; use 2 as the conservative drift rate.
#define NTP_DS3231_DRIFT_PPM             2UL

// Minimum root dispersion when clock was just synced (NTP fixed-point 16.16 seconds).
// 0x000003E8 ≈ 0.015 s (15 ms) — reflects millis()-based timekeeping resolution.
#define NTP_MIN_DISPERSION               0x000003E8UL

// Maximum root dispersion cap (NTP fixed-point 16.16).
// 0x00010000 = 1.0 s — cap prevents dispersion from growing unreasonably large.
#define NTP_MAX_DISPERSION               0x00010000UL

// Seconds without any sync before the NTP server degrades to Stratum 16.
// 48 hours: DS3231 at 2 ppm drifts ~345 ms/day — still within 1 s, but advertising
// Stratum 1 beyond 48 h without a reference is misleading per RFC 5905.
#define NTP_UNSYNC_STRATUM_THRESHOLD_S   172800UL

#endif // CONFIG_H
