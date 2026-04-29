/**
 * @file      wwvb_clock.ino
 * @brief     ESP32-S3 WWVB Atomic Clock with ES100 Receiver
 * @author    Generated for LilyGo T-Display-S3 AMOLED
 * @date      2025
 * 
 * Hardware Configuration:
 * - LilyGo T-Display-S3 AMOLED (1.91 inch, 240x536)
 * - ES100 WWVB Receiver Module
 * 
 * Display Library: LilyGo-AMOLED-Series with TFT_eSPI Sprite
 * 
 * ES100 Connections:
 * - SDA: GPIO3 (STEMMA QT connector)
 * - SCL: GPIO2 (STEMMA QT connector)
 * - EN:  GPIO1 (ES100 enable/power control)
 * - IRQ: GPIO4 (ES100 interrupt, active low)
 */

#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <RTClib.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "LilyGo_AMOLED.h"
#include "ES100.h"
#include "TimeManager.h"
#include "ReceptionHistory.h"
#include "NTPServer.h"
#include "CaptivePortal.h"
#include "StatusServer.h"
#include "config.h"

// ============================================================================
// Pin Definitions for LilyGo T-Display-S3 AMOLED
// ============================================================================
#define I2C_SDA_PIN         3       // Wire bus 0: touch, PMU, DS3231
#define I2C_SCL_PIN         2
#define ES100_SDA_PIN       15      // Wire1 bus 1: ES100 (isolated)
#define ES100_SCL_PIN       16
#define ES100_EN_PIN        40
#define ES100_IRQ_PIN       41

// ============================================================================
// Display Objects - LilyGo AMOLED + TFT_eSPI Sprite
// ============================================================================
LilyGo_Class amoled;
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);

// Display dimensions (will be set after initialization)
uint16_t DISPLAY_WIDTH;
uint16_t DISPLAY_HEIGHT;

// ============================================================================
// Global Objects
// ============================================================================
ES100 es100(ES100_EN_PIN, ES100_IRQ_PIN);
RTC_DS3231 rtc;
TimeManager timeManager;
ReceptionHistory receptionHistory;
Preferences preferences;

// ============================================================================
// State Variables
// ============================================================================
volatile bool es100InterruptFlag = false;
volatile unsigned long es100IRQMillis = 0;   // millis() captured at ISR fire
bool es100Receiving = false;
bool es100Available = false;
bool es100TrackingReady = false;        // True after a successful normal-mode decode
bool es100UsingTracking = false;        // True if current attempt is tracking mode
bool pendingTrackingStart = false;      // True when waiting to write Control 0 at second :55
unsigned long trackingStartAtMs = 0;   // millis() target for the Control 0 write
uint32_t trackingWriteUnixTime = 0;    // Unix time when Control 0 was written at :55
unsigned long es100TrackingReadySinceMs = 0; // millis() when es100TrackingReady was last set
unsigned long lastDisplayUpdate = 0;
unsigned long lastSyncAttempt = 0;
uint8_t daytimeFailures = 0;          // Consecutive daytime sync failures
bool daytimeSkipActive = false;       // True when backed off until nighttime
int8_t utcOffset = DEFAULT_UTC_OFFSET;
bool dstActive = false;
unsigned long lastDSTCheck = 0;  // millis() of last auto-DST computation

// Leap second warning decoded from WWVB frame (0=none, 1=positive, 2=negative)
uint8_t wwvbLeapSecondWarning = 0;

// Per-antenna sync success counters (persisted in NVS)
uint16_t ant1Successes = 0;
uint16_t ant2Successes = 0;

// Sync log ring buffer — last 20 individual sync outcomes
SyncLogEntry syncLog[SYNC_LOG_SIZE];
uint8_t syncLogHead   = 0;   // next write index
uint8_t syncLogFilled = 0;   // valid entry count (0..SYNC_LOG_SIZE)

// ES100 initialization retry tracking
uint8_t es100InitRetries = 0;
unsigned long lastES100InitAttempt = 0;
const uint8_t MAX_ES100_INIT_RETRIES = 5;
const unsigned long ES100_RETRY_INTERVALS[] = {5000, 10000, 30000, 60000, 300000};  // 5s, 10s, 30s, 1m, 5m

// DS3231 RTC tracking
bool rtcAvailable = false;
float rtcTemperature = 0.0;  // Temperature from DS3231 (Celsius)
unsigned long lastTempRead = 0;  // millis() of last temperature read
volatile bool rtcSqwInterruptFlag = false;
volatile uint32_t rtcSqwEdgeMicros = 0;
unsigned long lastRtcSqwSeenMillis = 0;
unsigned long lastRtcSqwStatusLogMillis = 0;
bool rtcWritePending = false;
bool rtcWriteDonePending = false;   // true for one SQW edge after DS3231 write
uint16_t rtcWriteSubsecMs = 0;     // milliseconds into current second at write time

// Low battery tracking
bool lowBatteryAlerted = false;         // True once 10% alert has been triggered
unsigned long lastLowBattWarn = 0;      // millis() of last serial warning
uint8_t critBattCount = 0;              // Consecutive critical-battery readings (debounce)

// Time source tracking
enum TimeSource { TIME_SRC_NONE, TIME_SRC_RTC, TIME_SRC_NTP, TIME_SRC_WWVB };
TimeSource lastTimeSource = TIME_SRC_NONE;
unsigned long lastTimeSyncMillis  = 0;  // millis() when last synced (any source)
unsigned long lastWWVBSyncMillis  = 0;  // millis() when last synced from WWVB
unsigned long lastNormalSuccessMillis = 0;  // millis() of last successful normal-mode sync

// Display regions (will be calculated based on display height)
uint16_t CLOCK_Y;
uint16_t DATE_Y;
uint16_t STATUS_Y;
uint16_t CHART_Y;
uint16_t CHART_HEIGHT;

// ============================================================================
// Page Navigation & Touch State
// ============================================================================
enum DisplayPage { PAGE_CLOCK, PAGE_UTC, PAGE_SYNC, PAGE_SETTINGS, PAGE_WIFI };
DisplayPage currentPage = PAGE_CLOCK;
uint8_t currentBrightness = DISPLAY_BRIGHTNESS;

// Touch tracking
int16_t touchStartX = 0, touchStartY = 0;
int16_t touchLastX = 0, touchLastY = 0;
unsigned long touchStartTime = 0;
bool touchActive = false;
bool swipeHandled = false;
bool sliderDragging = false;

// Shutdown long-press tracking
bool shutdownCountdownActive = false;

// Slider geometry (set in initDisplay based on screen size)
int16_t sliderX = 40;
int16_t sliderWidth = 456;
int16_t sliderY = 100;
int16_t sliderHeight = 30;
int16_t sliderTouchPadding = 20;  // Extra touch area above/below slider

// ============================================================================
// WiFi State
// ============================================================================
enum WiFiState {
    WIFI_STATE_OFF,
    WIFI_STATE_SCANNING,
    WIFI_STATE_IDLE,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_MODE
};

WiFiState wifiState = WIFI_STATE_OFF;
String wifiSSID, wifiPassword;
String scannedSSIDs[WIFI_MAX_NETWORKS];
int8_t scannedRSSI[WIFI_MAX_NETWORKS];
bool scannedSecure[WIFI_MAX_NETWORKS];
uint8_t scannedCount = 0;
int8_t selectedNetwork = -1;
int8_t listScrollOffset = 0;
unsigned long wifiConnectStart = 0;
unsigned long lastWifiScan = 0;
String wifiErrorMsg = "";

NTPServer ntpServer;
CaptivePortal captivePortal;
StatusServer statusServer;
StatusData statusData;
bool portalCredsReceived = false;  // Flag: credentials arrived from captive portal, handle in loop

// ============================================================================
// Forward Declarations
// PlatformIO does not auto-generate declarations for functions defined later
// in the file when they are referenced inside lambdas or earlier functions.
// Default args are omitted here; the definitions supply them.
// ============================================================================
void startWWVBSync(bool forceNormal);
void startWWVBSyncTracking();
void recordSyncFailure(bool wasTracking);
void addSyncLogEntry(bool success, bool tracking, uint8_t antenna);
void processDS3231SquareWave();

// ============================================================================
// On-Screen Keyboard State
// ============================================================================
enum KeyboardMode { KB_HIDDEN, KB_LOWER, KB_UPPER, KB_SYMBOLS };
KeyboardMode kbMode = KB_HIDDEN;
char passwordBuffer[KB_PASSWORD_MAX + 1] = {0};
uint8_t passwordLen = 0;
bool passwordVisible = false;

// Keyboard layout arrays
static const char KB_ROW1_LOWER[] = "qwertyuiop";
static const char KB_ROW2_LOWER[] = "asdfghjkl";
static const char KB_ROW3_LOWER[] = "zxcvbnm";
static const char KB_ROW1_UPPER[] = "QWERTYUIOP";
static const char KB_ROW2_UPPER[] = "ASDFGHJKL";
static const char KB_ROW3_UPPER[] = "ZXCVBNM";
static const char KB_ROW1_SYM[]   = "1234567890";
static const char KB_ROW2_SYM[]   = "!@#$%^&*(";
static const char KB_ROW3_SYM[]   = ")-_+=[]";

// Colors (RGB565 format)
#define COLOR_BACKGROUND    TFT_BLACK
#define COLOR_TIME          TFT_CYAN
#define COLOR_DATE          TFT_WHITE
#define COLOR_SYNC_OK       TFT_GREEN
#define COLOR_SYNC_FAIL     TFT_RED
#define COLOR_SYNC_PENDING  TFT_YELLOW
#define COLOR_CHART_BAR     TFT_GREEN
#define COLOR_CHART_GRID    0x4208  // Dark gray (RGB565: R=8, G=65, B=8)
#define COLOR_CHART_BAR_DIM 0x0400  // Very dark green (RGB565: R=0, G=32, B=0)
#define COLOR_TEXT_DIM      TFT_DARKGREY

// ============================================================================
// Interrupt Service Routine
// ============================================================================
void IRAM_ATTR es100ISR() {
    es100IRQMillis = millis();    // Capture time at IRQ edge (IRAM-safe on ESP32)
    es100InterruptFlag = true;    // Set flag after timestamp for ordering guarantee
}

void IRAM_ATTR rtcSqwISR() {
    rtcSqwEdgeMicros = micros();
    rtcSqwInterruptFlag = true;
}

// ============================================================================
// Battery Percentage (LiPo discharge curve)
// ============================================================================

/**
 * @brief Convert battery voltage to percentage using LiPo discharge curve
 * @param mv Battery voltage in millivolts
 * @return Percentage (0-100)
 * @details Uses a lookup table with linear interpolation between points.
 *          Approximates a typical single-cell LiPo at moderate discharge rate.
 *          The LiPo curve is nonlinear: flat plateau at ~3.7-3.9V, steep
 *          drop-off below 3.5V and above 4.1V.
 */
int lipoBatteryPercent(uint16_t mv) {
    // Voltage (mV) → Percentage lookup table
    static const uint16_t voltTable[] = {
        3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 3950, 4000, 4100, 4200
    };
    static const uint8_t pctTable[] = {
           0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100
    };
    static const int tableSize = sizeof(voltTable) / sizeof(voltTable[0]);

    if (mv <= voltTable[0]) return 0;
    if (mv >= voltTable[tableSize - 1]) return 100;

    // Linear interpolation between surrounding table entries
    for (int i = 1; i < tableSize; i++) {
        if (mv <= voltTable[i]) {
            int rangeMv  = voltTable[i] - voltTable[i - 1];
            int rangePct = pctTable[i]  - pctTable[i - 1];
            return pctTable[i - 1] + (int32_t)(mv - voltTable[i - 1]) * rangePct / rangeMv;
        }
    }
    return 100;
}

// ============================================================================
// Display Functions
// ============================================================================
void initDisplay() {
    Serial.println("[DISPLAY] Starting AMOLED initialization...");

    // Initialize LilyGo AMOLED - auto-detects board type
    if (!amoled.begin()) {
        Serial.println("[DISPLAY] ERROR: AMOLED initialization failed!");
        Serial.println("[DISPLAY] Check power connections and board selection");
        while (1) {
            delay(1000);
            Serial.println("[DISPLAY] HALTED - AMOLED init failed");
        }
    }
    Serial.println("[DISPLAY] AMOLED initialized successfully");

    // IMPORTANT: Give the display time to fully initialize
    delay(100);

    // Set display to landscape orientation
    // Rotation values for this display: 0/2=landscape (536x240), 1/3=portrait (240x536)
    amoled.setRotation(0);  // Landscape mode: 536 wide x 240 tall

    // Get display dimensions from the AMOLED object
    DISPLAY_WIDTH = amoled.width();
    DISPLAY_HEIGHT = amoled.height();

    Serial.printf("Display initialized: %d x %d\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);

    // Calculate layout positions dynamically based on display orientation
    // Different layouts for landscape vs portrait mode
    if (DISPLAY_WIDTH > DISPLAY_HEIGHT) {
        // Landscape mode (536x240) - 4 equal sections of 60 pixels each
        CLOCK_Y = 0;                            // Section 1: Time
        DATE_Y = 60;                            // Section 2: Date
        STATUS_Y = 120;                         // Section 3: Status
        CHART_Y = 180;                          // Section 4: Chart
        CHART_HEIGHT = 60;                      // Equal height for chart
    } else {
        // Portrait mode (240x536) - percentage-based layout
        CLOCK_Y = DISPLAY_HEIGHT * 0.04;        // 4% from top
        DATE_Y = DISPLAY_HEIGHT * 0.26;         // 26% from top
        STATUS_Y = DISPLAY_HEIGHT * 0.37;       // 37% from top
        CHART_Y = DISPLAY_HEIGHT * 0.52;        // 52% from top
        CHART_HEIGHT = DISPLAY_HEIGHT * 0.38;   // 38% of height for chart
    }
    
    // Load saved settings
    preferences.begin("wwvb", true);
    currentBrightness = preferences.getUChar("brightness", DISPLAY_BRIGHTNESS);
    utcOffset = preferences.getChar("utcOffset", DEFAULT_UTC_OFFSET);
    preferences.end();
    amoled.setBrightness(currentBrightness);
    Serial.printf("[DISPLAY] Brightness set to %d\n", currentBrightness);

    // Calculate slider geometry based on display dimensions
    sliderX = 40;
    sliderWidth = DISPLAY_WIDTH - 80;
    sliderY = 100;
    sliderHeight = 30;

    // NOTE: We do NOT call tft.init() - it's only needed for sprite creation
    // Calling init() would try to initialize non-existent hardware and crash

    // Check PSRAM availability (warn but continue if not found)
    if (!psramFound()) {
        Serial.println("WARNING: PSRAM not detected!");
        Serial.println("Sprite creation may fail. Check platformio.ini PSRAM settings.");
    } else {
        Serial.printf("PSRAM available: %d bytes free\n", ESP.getFreePsram());
    }

    // Create sprite for entire screen (double-buffering)
    Serial.printf("[DISPLAY] Creating sprite: %d x %d (%lu bytes)\n",
                 DISPLAY_WIDTH, DISPLAY_HEIGHT,
                 (unsigned long)(DISPLAY_WIDTH * DISPLAY_HEIGHT * 2));

    void* spritePtr = sprite.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (spritePtr == nullptr) {
        Serial.println("[DISPLAY] ERROR: Sprite creation failed - not enough memory!");
        Serial.printf("[DISPLAY] Free heap: %d, Free PSRAM: %d\n",
                     ESP.getFreeHeap(), ESP.getFreePsram());
        while (1) {
            delay(1000);
            Serial.println("[DISPLAY] HALTED - Sprite creation failed");
        }
    }
    Serial.println("[DISPLAY] Sprite created successfully");

    sprite.setSwapBytes(true);
    Serial.println("[DISPLAY] Sprite configured (setSwapBytes)");

    // Clear sprite
    sprite.fillSprite(COLOR_BACKGROUND);
    Serial.println("[DISPLAY] Sprite cleared");

    // Clear screen to black
    Serial.println("[DISPLAY] Clearing screen to black...");
    sprite.fillSprite(TFT_BLACK);
    amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t*)sprite.getPointer());

    // Give display time to update
    delay(50);

    Serial.println("[DISPLAY] Display initialization complete!");
}

void pushDisplay() {
    // Push the sprite buffer to the AMOLED display
    amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t*)sprite.getPointer());
}

void drawCenteredText(const char* text, int y, uint16_t color, uint8_t textSize) {
    sprite.setTextSize(textSize);
    sprite.setTextColor(color, COLOR_BACKGROUND);
    sprite.setTextDatum(TC_DATUM);  // Top-center alignment
    sprite.drawString(text, DISPLAY_WIDTH / 2, y);
}

void drawClockFace(bool showUTC = false) {
    char timeStr[16];
    char dateStr[32];
    char secStr[8];

    ClockTime dt = showUTC ? timeManager.getUTCTime() : timeManager.getLocalTime(utcOffset, dstActive);
    
    // Format time HH:MM
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", dt.hour, dt.minute);
    
    // Format seconds
    snprintf(secStr, sizeof(secStr), ":%02d", dt.second);
    
    // Format date - Day, Month DD, YYYY
    const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", 
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    int dayOfWeek = timeManager.calculateDayOfWeek(dt.year, dt.month, dt.day);
    snprintf(dateStr, sizeof(dateStr), "%s, %s %02d, %04d", 
             dayNames[dayOfWeek], 
             monthNames[dt.month - 1], 
             dt.day, 
             dt.year);
    
    // Clear clock area (from CLOCK_Y to STATUS_Y)
    sprite.fillRect(0, CLOCK_Y, DISPLAY_WIDTH, STATUS_Y - CLOCK_Y, COLOR_BACKGROUND);
    
    // Draw main time (same size as date)
    sprite.setTextSize(2);
    sprite.setTextColor(COLOR_TIME, COLOR_BACKGROUND);
    sprite.setTextDatum(TC_DATUM);

    // Calculate widths for proper centering
    int16_t timeWidth = sprite.textWidth(timeStr);
    sprite.setTextSize(2);  // Temporarily set to seconds size
    int16_t secWidth = sprite.textWidth(secStr);
    int16_t spacing = 5;
    int16_t totalWidth = timeWidth + spacing + secWidth;
    int16_t timeX = (DISPLAY_WIDTH - totalWidth) / 2;

    // Draw time string
    sprite.setTextSize(2);
    sprite.setTextDatum(TL_DATUM);  // Top-left for precise positioning
    sprite.drawString(timeStr, timeX, CLOCK_Y);

    // Draw seconds (same size, to the right)
    sprite.setTextSize(2);
    sprite.drawString(secStr, timeX + timeWidth + spacing, CLOCK_Y);
    
    // Draw date
    drawCenteredText(dateStr, DATE_Y, COLOR_DATE, 2);

    // Draw time zone label in top-left
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    if (showUTC) {
        sprite.drawString("UTC", 2, 2);
    } else {
        char tzLabel[12];
        snprintf(tzLabel, sizeof(tzLabel), "UTC%+d", utcOffset);
        sprite.drawString(tzLabel, 2, 2);
    }

    // Low battery warning in top-right corner
    if (lowBatteryAlerted) {
        sprite.setTextSize(1);
        sprite.setTextColor(TFT_RED, COLOR_BACKGROUND);
        sprite.setTextDatum(TR_DATUM);
        sprite.drawString("LOW BATT", DISPLAY_WIDTH - 2, 2);
    }
}

void drawSyncStatus() {
    char statusStr[64];
    uint16_t statusColor;
    
    // Clear status area
    sprite.fillRect(0, STATUS_Y, DISPLAY_WIDTH, 70, COLOR_BACKGROUND);
    
    // Get sync status
    unsigned long lastSync = receptionHistory.getLastSuccessTime();
    int totalSuccess = receptionHistory.getTotalSuccessCount();
    int total48h = receptionHistory.getRecentSuccessCount();
    
    if (!es100Available) {
        snprintf(statusStr, sizeof(statusStr), "ES100 RETRY %d", es100InitRetries);
        statusColor = COLOR_SYNC_PENDING;
    } else if (es100Receiving) {
        snprintf(statusStr, sizeof(statusStr), es100UsingTracking ? "TRACKING..." : "SYNCING...");
        statusColor = COLOR_SYNC_PENDING;
    } else if (lastSync == 0) {
        snprintf(statusStr, sizeof(statusStr), "NO SYNC YET");
        statusColor = COLOR_SYNC_FAIL;
    } else {
        unsigned long elapsed = (millis() - lastSync) / 1000;
        int hours = elapsed / 3600;
        int mins = (elapsed % 3600) / 60;
        
        if (hours > 0) {
            snprintf(statusStr, sizeof(statusStr), "Last sync: %dh %dm ago", hours, mins);
        } else {
            snprintf(statusStr, sizeof(statusStr), "Last sync: %dm ago", mins);
        }
        
        if (elapsed < 7200) {
            statusColor = COLOR_SYNC_OK;
        } else if (elapsed < 86400) {
            statusColor = COLOR_SYNC_PENDING;
        } else {
            statusColor = COLOR_SYNC_FAIL;
        }
    }
    
    drawCenteredText(statusStr, STATUS_Y, statusColor, 2);

    // Time source and 48h reception stats
    const char* srcName = "---";
    switch (lastTimeSource) {
        case TIME_SRC_WWVB: srcName = "WWVB"; break;
        case TIME_SRC_NTP:  srcName = "NTP";  break;
        case TIME_SRC_RTC:  srcName = "RTC";  break;
        default: break;
    }
    char statsStr[64];
    snprintf(statsStr, sizeof(statsStr), "Src: %s | 48h: %d syncs | Total: %d",
             srcName, total48h, totalSuccess);
    drawCenteredText(statsStr, STATUS_Y + 22, COLOR_TEXT_DIM, 1);

    // Draw DST indicator if active
    if (dstActive) {
        sprite.setTextSize(1);
        sprite.setTextColor(COLOR_SYNC_OK, COLOR_BACKGROUND);
        sprite.setTextDatum(TR_DATUM);
        sprite.drawString("DST", DISPLAY_WIDTH - 5, STATUS_Y);
    }
}

void drawReceptionChart() {
    // Chart area dimensions
    int chartX = 25;
    int chartWidth = DISPLAY_WIDTH - 50;
    int chartHeight = CHART_HEIGHT - 40;
    int chartBottom = CHART_Y + 15 + chartHeight;
    
    // Clear chart area
    sprite.fillRect(0, CHART_Y, DISPLAY_WIDTH, CHART_HEIGHT + 30, COLOR_BACKGROUND);
    
    // Draw chart title
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("WWVB Reception History (48h)", chartX, CHART_Y);
    
    // Draw chart frame
    sprite.drawRect(chartX - 1, CHART_Y + 15, chartWidth + 2, chartHeight + 2, COLOR_CHART_GRID);
    
    // Draw horizontal grid lines
    for (int i = 1; i < 4; i++) {
        int y = CHART_Y + 15 + (chartHeight * i / 4);
        sprite.drawFastHLine(chartX, y, chartWidth, COLOR_CHART_GRID);
    }
    
    // Get history data
    uint8_t data[48];
    receptionHistory.getHistoryData(data);
    uint8_t maxVal = receptionHistory.getMaxValue();
    if (maxVal < 1) maxVal = 1;
    
    // Calculate bar width
    float barWidth = (float)chartWidth / 48.0f;
    
    // Draw bars with bounds checking
    for (int i = 0; i < 48; i++) {
        if (data[i] > 0) {
            // Calculate bar height with bounds checking
            int barHeight = (data[i] * (chartHeight - 5)) / maxVal;
            if (barHeight < 2) barHeight = 2;
            if (barHeight > chartHeight) barHeight = chartHeight;  // Clamp to max height

            int x = chartX + (int)(i * barWidth);
            int y = chartBottom - barHeight;
            int w = (int)barWidth - 1;
            if (w < 1) w = 1;

            // Ensure bar stays within chart bounds
            if (y < CHART_Y + 15) y = CHART_Y + 15;
            if (y + barHeight > chartBottom) barHeight = chartBottom - y;

            // Color based on success count
            uint16_t barColor = (data[i] >= 2) ? COLOR_CHART_BAR : COLOR_CHART_BAR_DIM;
            sprite.fillRect(x, y, w, barHeight, barColor);
        }
    }
    
    // Draw time labels
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("-48h", chartX - 5, chartBottom + 5);
    
    sprite.setTextDatum(TC_DATUM);
    sprite.drawString("-24h", chartX + chartWidth/2, chartBottom + 5);
    
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString("Now", chartX + chartWidth + 5, chartBottom + 5);
    
    // Draw Y-axis labels
    sprite.setTextDatum(TL_DATUM);
    char maxStr[8];
    snprintf(maxStr, sizeof(maxStr), "%d", maxVal);
    sprite.drawString(maxStr, 2, CHART_Y + 20);
    sprite.drawString("0", 2, chartBottom - 5);
}

void drawTemperature() {
    if (!rtcAvailable) {
        return;  // Don't draw if RTC not available
    }

    char tempStr[16];
    // Convert Celsius to Fahrenheit
    float tempF = (rtcTemperature * 9.0 / 5.0) + 32.0;

    // Format complete string with degree symbol and F
    // Using 'o' as degree symbol approximation
    snprintf(tempStr, sizeof(tempStr), "%.1foF", tempF);

    // Draw in top-right corner
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TR_DATUM);  // Top-right alignment

    sprite.drawString(tempStr, DISPLAY_WIDTH - 2, 2);
}

// ============================================================================
// Last WWVB Sync Time — one line drawn on clock pages at STATUS_Y
// ============================================================================
void drawLastSyncTime() {
    unsigned long lastSync = receptionHistory.getLastSuccessTime();
    if (lastSync == 0) return;

    unsigned long elapsed = (millis() - lastSync) / 1000;
    int hours = elapsed / 3600;
    int mins  = (elapsed % 3600) / 60;
    char str[40];
    if (hours > 0) {
        snprintf(str, sizeof(str), "Last WWVB: %dh %dm ago", hours, mins);
    } else {
        snprintf(str, sizeof(str), "Last WWVB: %dm ago", mins);
    }
    uint16_t color = (elapsed < 43200) ? COLOR_SYNC_OK :      // < 12 h → green
                     (elapsed < 86400) ? COLOR_SYNC_PENDING : // < 24 h → yellow
                                         COLOR_SYNC_FAIL;     // ≥ 24 h → red

    sprite.setTextSize(1);
    sprite.setTextColor(color, COLOR_BACKGROUND);
    sprite.setTextDatum(TC_DATUM);
    sprite.drawString(str, DISPLAY_WIDTH / 2, STATUS_Y);
}

// ============================================================================
// Sync Status Page
// ============================================================================
void drawSyncPage() {
    sprite.fillSprite(COLOR_BACKGROUND);

    int w = DISPLAY_WIDTH;

    // ── Title ──────────────────────────────────────────────
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_TIME, COLOR_BACKGROUND);
    sprite.setTextDatum(TC_DATUM);
    sprite.drawString("SYNC STATUS", w / 2, 8);
    sprite.drawFastHLine(20, 22, w - 40, COLOR_CHART_GRID);

    // ── 48-hour sync count ─────────────────────────────────
    int total48h = receptionHistory.getRecentSuccessCount();
    const char* sigQuality;
    uint16_t    sigColor;
    if      (total48h >= 8) { sigQuality = "STRONG"; sigColor = COLOR_SYNC_OK;      }
    else if (total48h >= 4) { sigQuality = "GOOD";   sigColor = TFT_GREEN;           }
    else if (total48h >= 1) { sigQuality = "FAIR";   sigColor = COLOR_SYNC_PENDING;  }
    else                    { sigQuality = "POOR";   sigColor = COLOR_SYNC_FAIL;     }

    // ── Two-column rows: label left, value right ────────────
    const int labelX = 22;
    const int valueX = w - 22;
    int       rowY   = 30;
    const int rowH   = 24;

    sprite.setTextSize(1);

    // 48-hour sync count
    char countStr[8];
    snprintf(countStr, sizeof(countStr), "%d", total48h);
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("48H SYNCS", labelX, rowY);
    sprite.setTextColor(sigColor, COLOR_BACKGROUND);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(countStr, valueX, rowY);
    rowY += rowH;

    // Source
    const char* srcName;
    uint16_t    srcColor;
    switch (lastTimeSource) {
        case TIME_SRC_WWVB: srcName = "WWVB"; srcColor = COLOR_SYNC_OK;      break;
        case TIME_SRC_NTP:  srcName = "NTP";  srcColor = COLOR_TIME;          break;
        case TIME_SRC_RTC:  srcName = "RTC";  srcColor = COLOR_SYNC_PENDING;  break;
        default:            srcName = "---";  srcColor = COLOR_TEXT_DIM;      break;
    }
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("SOURCE", labelX, rowY);
    sprite.setTextColor(srcColor, COLOR_BACKGROUND);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(srcName, valueX, rowY);
    rowY += rowH;

    // Mode
    const char* modeStr;
    uint16_t    modeColor;
    if (!es100Available) {
        modeStr = "ES100 FAULT";  modeColor = COLOR_SYNC_FAIL;
    } else if (es100Receiving) {
        modeStr = es100UsingTracking ? "TRACKING..." : "SYNCING...";
        modeColor = COLOR_SYNC_PENDING;
    } else if (es100TrackingReady) {
        modeStr = "TRACKING";   modeColor = COLOR_SYNC_OK;
    } else {
        modeStr = "NORMAL";     modeColor = COLOR_TEXT_DIM;
    }
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("MODE", labelX, rowY);
    sprite.setTextColor(modeColor, COLOR_BACKGROUND);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(modeStr, valueX, rowY);
    rowY += rowH;

    sprite.drawFastHLine(20, rowY, w - 40, COLOR_CHART_GRID);
    rowY += 8;

    // Signal quality
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("SIGNAL", labelX, rowY);
    sprite.setTextColor(sigColor, COLOR_BACKGROUND);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(sigQuality, valueX, rowY);
    rowY += rowH;

    sprite.drawFastHLine(20, rowY, w - 40, COLOR_CHART_GRID);
    rowY += 8;

    // Antenna successes
    char antStr[8];
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("ANTENNA 1", labelX, rowY);
    snprintf(antStr, sizeof(antStr), "%d", ant1Successes);
    sprite.setTextColor(COLOR_DATE, COLOR_BACKGROUND);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(antStr, valueX, rowY);
    rowY += rowH;

    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("ANTENNA 2", labelX, rowY);
    snprintf(antStr, sizeof(antStr), "%d", ant2Successes);
    sprite.setTextColor(COLOR_DATE, COLOR_BACKGROUND);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(antStr, valueX, rowY);
    rowY += rowH;

    sprite.drawFastHLine(20, rowY, w - 40, COLOR_CHART_GRID);
    rowY += 8;

    // Last WWVB sync time
    unsigned long lastSync = receptionHistory.getLastSuccessTime();
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TC_DATUM);
    if (lastSync > 0) {
        unsigned long elapsed = (millis() - lastSync) / 1000;
        int hours = elapsed / 3600;
        int mins  = (elapsed % 3600) / 60;
        char lastSyncStr[40];
        if (hours > 0) {
            snprintf(lastSyncStr, sizeof(lastSyncStr), "Last WWVB: %dh %dm ago", hours, mins);
        } else {
            snprintf(lastSyncStr, sizeof(lastSyncStr), "Last WWVB: %dm ago", mins);
        }
        sprite.drawString(lastSyncStr, w / 2, rowY);
    } else {
        sprite.setTextColor(COLOR_SYNC_FAIL, COLOR_BACKGROUND);
        sprite.drawString("No WWVB sync yet", w / 2, rowY);
    }

}

// ============================================================================
// Brightness Settings Page
// ============================================================================
void drawSettingsPage() {
    sprite.fillSprite(COLOR_BACKGROUND);

    // Title
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_TIME, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("SETTINGS", DISPLAY_WIDTH / 2, 10);

    // Battery level — top right
    uint16_t battMv = amoled.getBattVoltage();
    if (battMv > 0) {
        int battPct = lipoBatteryPercent(battMv);
        char battStr[24];
        if (amoled.isCharging()) {
            snprintf(battStr, sizeof(battStr), "CHG %d%% %.2fV", battPct, battMv / 1000.0);
        } else {
            snprintf(battStr, sizeof(battStr), "BAT %d%% %.2fV", battPct, battMv / 1000.0);
        }
        sprite.setTextSize(1);
        sprite.setTextColor(battPct > 20 ? COLOR_SYNC_OK : COLOR_SYNC_FAIL, COLOR_BACKGROUND);
        sprite.setTextDatum(TR_DATUM);
        sprite.drawString(battStr, DISPLAY_WIDTH - 5, 40);
    }

    // Subtitle
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_DATE, COLOR_BACKGROUND);
    sprite.setTextDatum(TC_DATUM);
    sprite.drawString("Screen Brightness", DISPLAY_WIDTH / 2, 65);

    // Slider track (dark background)
    sprite.fillRoundRect(sliderX, sliderY, sliderWidth, sliderHeight, 6, COLOR_CHART_GRID);

    // Slider fill (proportional to brightness)
    int fillWidth = map(currentBrightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX, 0, sliderWidth);
    if (fillWidth > 0) {
        sprite.fillRoundRect(sliderX, sliderY, fillWidth, sliderHeight, 6, COLOR_TIME);
    }

    // Slider knob
    int knobX = sliderX + fillWidth - 3;
    if (knobX < sliderX) knobX = sliderX;
    sprite.fillRoundRect(knobX, sliderY - 4, 6, sliderHeight + 8, 3, COLOR_DATE);

    // Min/Max labels
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("Min", sliderX, sliderY + sliderHeight + 8);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString("Max", sliderX + sliderWidth, sliderY + sliderHeight + 8);

    // WiFi disable/enable button — bottom left
    int btnW = 120;
    int btnH = 26;
    int btnX = 10;
    int btnY = DISPLAY_HEIGHT - btnH - 6;
    bool wifiOn = (wifiState != WIFI_STATE_OFF);
    uint16_t btnColor = wifiOn ? COLOR_SYNC_FAIL : COLOR_SYNC_OK;
    const char* btnLabel = wifiOn ? "Disable WiFi" : "Enable WiFi";
    sprite.fillRoundRect(btnX, btnY, btnW, btnH, 6, btnColor);
    sprite.setTextSize(1);
    sprite.setTextColor(TFT_WHITE, btnColor);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString(btnLabel, btnX + btnW / 2, btnY + btnH / 2);

    // UTC Offset control — bottom right
    // Layout: "UTC" label [-] value [+]
    int utcBtnW = 30, utcBtnH = 26;
    int utcRightEdge = DISPLAY_WIDTH - 10;
    int utcBtnY = btnY;  // Same row as WiFi button

    // [+] button (rightmost)
    int utcPlusX = utcRightEdge - utcBtnW;
    sprite.fillRoundRect(utcPlusX, utcBtnY, utcBtnW, utcBtnH, 4, COLOR_CHART_GRID);
    sprite.setTextColor(TFT_WHITE, COLOR_CHART_GRID);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString("+", utcPlusX + utcBtnW / 2, utcBtnY + utcBtnH / 2);

    // Offset value
    char offsetStr[16];
    snprintf(offsetStr, sizeof(offsetStr), "UTC%+d", utcOffset);
    sprite.setTextColor(COLOR_TIME, COLOR_BACKGROUND);
    sprite.setTextDatum(MC_DATUM);
    int valueX = utcPlusX - 45;
    sprite.drawString(offsetStr, valueX, utcBtnY + utcBtnH / 2);

    // [-] button
    int utcMinusX = valueX - 55 - utcBtnW / 2;
    sprite.fillRoundRect(utcMinusX, utcBtnY, utcBtnW, utcBtnH, 4, COLOR_CHART_GRID);
    sprite.setTextColor(TFT_WHITE, COLOR_CHART_GRID);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString("-", utcMinusX + utcBtnW / 2, utcBtnY + utcBtnH / 2);
}

// ============================================================================
// Shutdown Overlay
// ============================================================================
void drawShutdownOverlay(unsigned long holdDuration) {
    // Only show after SHUTDOWN_FEEDBACK_MS of holding
    if (holdDuration < SHUTDOWN_FEEDBACK_MS) return;

    unsigned long remaining = SHUTDOWN_HOLD_MS - holdDuration;
    int seconds = (remaining + 999) / 1000;  // Round up

    // Semi-transparent overlay effect: dark rectangle
    sprite.fillRect(0, DISPLAY_HEIGHT / 2 - 40, DISPLAY_WIDTH, 80, TFT_BLACK);
    sprite.drawRect(0, DISPLAY_HEIGHT / 2 - 40, DISPLAY_WIDTH, 80, COLOR_SYNC_FAIL);

    // Countdown text
    char msg[32];
    snprintf(msg, sizeof(msg), "Shutdown in %d...", seconds);
    sprite.setTextSize(2);
    sprite.setTextColor(COLOR_SYNC_FAIL, TFT_BLACK);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString(msg, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 10);

    // Progress bar
    int barX = 40;
    int barW = DISPLAY_WIDTH - 80;
    int barY = DISPLAY_HEIGHT / 2 + 15;
    int barH = 8;
    sprite.drawRect(barX, barY, barW, barH, COLOR_SYNC_FAIL);
    float progress = (float)(holdDuration - SHUTDOWN_FEEDBACK_MS) / (SHUTDOWN_HOLD_MS - SHUTDOWN_FEEDBACK_MS);
    if (progress > 1.0f) progress = 1.0f;
    sprite.fillRect(barX + 1, barY + 1, (int)((barW - 2) * progress), barH - 2, COLOR_SYNC_FAIL);
}

// ============================================================================
// Deep Sleep Shutdown
// ============================================================================
void performShutdown() {
    Serial.println("[SHUTDOWN] Initiating deep sleep...");

    // Show shutdown message
    sprite.fillSprite(TFT_BLACK);
    sprite.setTextSize(2);
    sprite.setTextColor(COLOR_SYNC_FAIL, TFT_BLACK);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString("Shutting down...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2);
    pushDisplay();

    // Save brightness to preferences
    preferences.begin("wwvb", false);
    preferences.putUChar("brightness", currentBrightness);
    preferences.end();
    Serial.printf("[SHUTDOWN] Brightness %d saved\n", currentBrightness);

    // Save current time
    saveTimeToPreferences();

    delay(1500);

    // Turn off display
    amoled.setBrightness(0);

    // Configure touch pin as wakeup source (touch press pulls IRQ low)
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_21, 0);
    Serial.println("[SHUTDOWN] Entering deep sleep (touch to wake)");
    Serial.flush();

    esp_deep_sleep_start();
}

// ============================================================================
// Brightness Slider Touch Handling
// ============================================================================
void handleSliderTouch(int16_t x, int16_t y) {
    // Check if touch is in the slider area (with padding)
    if (y >= (sliderY - sliderTouchPadding) && y <= (sliderY + sliderHeight + sliderTouchPadding) &&
        x >= sliderX && x <= (sliderX + sliderWidth)) {
        // Map touch X position to brightness
        int newBrightness = map(x, sliderX, sliderX + sliderWidth, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
        newBrightness = constrain(newBrightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX);

        if (newBrightness != currentBrightness) {
            currentBrightness = newBrightness;
            amoled.setBrightness(currentBrightness);
        }
        sliderDragging = true;
    }
}

// ============================================================================
// WiFi Management Functions
// ============================================================================
void wifiScan() {
    // Ensure WiFi is in STA mode and disconnected (but NOT stopped)
    WiFi.mode(WIFI_STA);
    WiFi.setMinSecurity(WIFI_AUTH_WEP);  // Allow all security types including WPA/mixed
    WiFi.disconnect();      // Disconnect without stopping WiFi driver
    delay(100);

    // Use synchronous scan — blocks ~2-4s but is reliable on ESP32-S3
    Serial.println("[WIFI] Scan starting (synchronous)...");
    wifiState = WIFI_STATE_SCANNING;
    updateDisplay();  // Show "Scanning..." on screen immediately

    int16_t result = WiFi.scanNetworks(false, false, false, 300);
    // false=synchronous, false=no hidden, false=active scan, 300ms per channel

    if (result > 0) {
        scannedCount = min((int)result, (int)WIFI_MAX_NETWORKS);
        for (int i = 0; i < scannedCount; i++) {
            scannedSSIDs[i] = WiFi.SSID(i);
            scannedRSSI[i] = WiFi.RSSI(i);
            scannedSecure[i] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            Serial.printf("[WIFI]   %d: %s (%d dBm) %s\n", i,
                         scannedSSIDs[i].c_str(), scannedRSSI[i],
                         scannedSecure[i] ? "secured" : "open");
        }
        Serial.printf("[WIFI] Scan complete: %d networks found\n", scannedCount);

        // Build HTML options for captive portal
        String htmlOptions = "";
        for (int i = 0; i < scannedCount; i++) {
            htmlOptions += "<option value='" + scannedSSIDs[i] + "'>" + scannedSSIDs[i];
            htmlOptions += " (" + String(scannedRSSI[i]) + " dBm)";
            if (scannedSecure[i]) htmlOptions += " secured";
            htmlOptions += "</option>";
        }
        captivePortal.setNetworkList(htmlOptions);
        WiFi.scanDelete();
    } else {
        scannedCount = 0;
        Serial.printf("[WIFI] Scan returned %d (0=none found, -1=running, -2=failed)\n", result);
    }

    wifiState = WIFI_STATE_IDLE;
    lastWifiScan = millis();
    listScrollOffset = 0;
}

// No longer needed (scan is synchronous) but kept as no-op for wifiLoop compatibility
void wifiCheckScanResults() {
    // Synchronous scan handles results inline in wifiScan()
}

void wifiConnect(const String& ssid, const String& password) {
    wifiSSID = ssid;
    wifiPassword = password;
    wifiErrorMsg = "";

    WiFi.mode(WIFI_STA);
    WiFi.setMinSecurity(WIFI_AUTH_WEP);  // Allow all security types including WPA/mixed
    delay(100);

    WiFi.begin(ssid.c_str(), password.c_str());
    wifiConnectStart = millis();
    wifiState = WIFI_STATE_CONNECTING;

    Serial.printf("[WIFI] Connecting to: %s (mode=%d, pw_len=%d)\n",
                  ssid.c_str(), WiFi.getMode(), password.length());
}

void wifiCheckConnection() {
    if (WiFi.status() == WL_CONNECTED) {
        wifiState = WIFI_STATE_CONNECTED;
        wifiErrorMsg = "";
        Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

        // Save credentials
        preferences.begin("wifi", false);
        preferences.putString("ssid", wifiSSID);
        preferences.putString("pass", wifiPassword);
        preferences.end();
        Serial.println("[WIFI] Credentials saved");

        // Start NTP server on the local network
        if (!ntpServer.isRunning()) {
            ntpServer.begin(&timeManager);
        }

        // Start status web server on the local network
        if (!statusServer.isRunning()) {
            statusServer.setTimeManager(&timeManager);
            statusServer.setNTPServer(&ntpServer);
            statusServer.setStatusData(&statusData);
            statusServer.setReceptionHistory(&receptionHistory);
            statusServer.setOnSyncRequest([]() {
                daytimeSkipActive = false;
                daytimeFailures = 0;
                if (!es100Receiving) startWWVBSync(true); // force normal mode (cancels any pending tracking)
            });
            statusServer.setOnTrackingRequest([]() {
                daytimeSkipActive = false;
                daytimeFailures = 0;
                if (!es100Receiving && !pendingTrackingStart) startWWVBSyncTracking();
            });
            statusServer.setOnSettingsRequest([](int8_t off, bool dst) {
                utcOffset = off;
                dstActive = dst;
                preferences.begin("wwvb", false);
                preferences.putChar("utcOffset", utcOffset);
                preferences.putBool("dst", dstActive);
                preferences.end();
                captivePortal.setTimezone(utcOffset, dstActive);
                Serial.printf("[SETTINGS] UTC offset=%+d DST=%s (via web)\n",
                              utcOffset, dstActive ? "on" : "off");
            });
            statusServer.setSyncLog(syncLog, &syncLogHead, &syncLogFilled);
            statusServer.begin();
        }

        // NTP auto-sync policy on Wi-Fi connect:
        //   RTC / None  — always sync: corrects any DS3231 set-point error at boot
        //   NTP         — sync only if last sync was >2 h ago (refresh stale fix)
        //   WWVB        — never auto-sync: WWVB is authoritative, preserves Stratum 1
        unsigned long sinceLast = (lastTimeSyncMillis > 0) ? millis() - lastTimeSyncMillis : UINT32_MAX;
        bool shouldNtpSync = false;
        if (lastTimeSource == TIME_SRC_NONE || lastTimeSource == TIME_SRC_RTC) {
            shouldNtpSync = true;                          // always correct DS3231 errors
        } else if (lastTimeSource == TIME_SRC_NTP && sinceLast > 7200000UL) {
            shouldNtpSync = true;                          // refresh stale NTP fix
        }
        // TIME_SRC_WWVB: never — preserves Stratum 1
        if (shouldNtpSync) {
            Serial.println("[WIFI] Auto-syncing time from NTP...");
            ntpClientSync();
        }
    } else if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT) {
        wifiState = WIFI_STATE_IDLE;
        wifiErrorMsg = "Connection failed";
        WiFi.disconnect();
        Serial.println("[WIFI] Connection timeout");
    }
}

void wifiStartAP() {
    Serial.println("[WIFI] Starting AP mode...");
    Serial.flush();

    // Clean up any prior STA state before switching to AP
    WiFi.scanDelete();
    WiFi.disconnect();
    delay(100);

    // Go directly to AP mode — do NOT use WIFI_OFF first,
    // as the ESP32-S3 WiFi driver crashes on OFF→AP transitions
    bool apOk = WiFi.softAP(WIFI_AP_SSID);  // softAP() handles mode switch internally
    if (!apOk) {
        Serial.println("[WIFI] softAP failed!");
        wifiErrorMsg = "AP start failed";
        wifiState = WIFI_STATE_IDLE;
        return;
    }
    delay(500);  // Let AP fully stabilize before starting servers

    Serial.printf("[WIFI] AP started: %s  IP: %s\n",
                 WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    Serial.flush();

    captivePortal.setOnCredentials([](const String& ssid, const String& password) {
        Serial.printf("[PORTAL] Got credentials for: %s\n", ssid.c_str());
        wifiSSID = ssid;
        wifiPassword = password;
        // Don't stop AP or connect here — we're inside the HTTP handler.
        // Tearing down WiFi from within its own callback crashes the driver.
        // Set a flag and handle the AP→STA transition in wifiLoop().
        portalCredsReceived = true;
    });

    // Start NTP server on AP so connected clients can get time
    if (!ntpServer.isRunning()) {
        ntpServer.begin(&timeManager);
        Serial.println("[WIFI] NTP server started on AP");
    }

    // Give captive portal access to time for the clock display
    captivePortal.setTimeManager(&timeManager);
    captivePortal.setTimezone(utcOffset, dstActive);
    captivePortal.setTimeSource((uint8_t)lastTimeSource);

    // Start DNS + HTTP servers on the AP
    Serial.println("[WIFI] Starting portal servers...");
    Serial.flush();
    captivePortal.begin();
    wifiState = WIFI_STATE_AP_MODE;
    Serial.println("[WIFI] AP mode fully started");
}

void wifiStopAP() {
    ntpServer.stop();
    statusServer.stop();
    captivePortal.stop();
    WiFi.softAPdisconnect(false);  // Stop AP but don't deinit WiFi driver
    WiFi.mode(WIFI_STA);           // Back to STA mode
    wifiState = WIFI_STATE_OFF;
    Serial.println("[WIFI] AP mode stopped");
}

bool wifiLoadCredentials() {
    preferences.begin("wifi", true);
    bool hasSSID = preferences.isKey("ssid");
    if (hasSSID) {
        wifiSSID = preferences.getString("ssid", "");
        wifiPassword = preferences.getString("pass", "");
    }
    preferences.end();

    if (hasSSID && wifiSSID.length() > 0) {
        Serial.printf("[WIFI] Loaded credentials for: %s\n", wifiSSID.c_str());
        return true;
    }
    return false;
}

void wifiLoop() {
    switch (wifiState) {
        case WIFI_STATE_SCANNING:
            wifiCheckScanResults();
            break;
        case WIFI_STATE_CONNECTING:
            wifiCheckConnection();
            break;
        case WIFI_STATE_CONNECTED:
            // Check for disconnect — auto-reconnect with saved credentials
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WIFI] Connection lost, reconnecting in 5s...");
                ntpServer.stop();
                statusServer.stop();
                delay(5000);  // Brief delay before reconnect to let the AP stabilize
                if (wifiSSID.length() > 0) {
                    wifiConnect(wifiSSID, wifiPassword);
                } else {
                    wifiState = WIFI_STATE_IDLE;
                }
            }
            break;
        case WIFI_STATE_AP_MODE:
            // Check if captive portal received credentials
            if (portalCredsReceived) {
                portalCredsReceived = false;
                Serial.println("[WIFI] Portal credentials received, shutting down AP...");
                // 1. Stop HTTP/DNS servers (WiFi untouched)
                captivePortal.stop();
                delay(100);
                // 2. Shut down the AP radio — use false to keep WiFi driver alive
                WiFi.softAPdisconnect(false);
                delay(100);
                // 3. Switch to STA-only mode so the radio is free to join any channel
                WiFi.mode(WIFI_STA);
                delay(100);
                // 4. Connect
                wifiConnect(wifiSSID, wifiPassword);
            }
            break;
        default:
            break;
    }
}

// ============================================================================
// On-Screen Keyboard Drawing
// ============================================================================
void drawKeyboardKey(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, uint16_t bgColor, uint16_t textColor) {
    sprite.fillRoundRect(x, y, w, h, 4, bgColor);
    sprite.setTextColor(textColor, bgColor);
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextSize(1);
    sprite.drawString(label, x + w / 2, y + h / 2);
}

void drawKeyboard() {
    if (kbMode == KB_HIDDEN) return;

    const char* row1;
    const char* row2;
    const char* row3;

    if (kbMode == KB_SYMBOLS) {
        row1 = KB_ROW1_SYM;
        row2 = KB_ROW2_SYM;
        row3 = KB_ROW3_SYM;
    } else if (kbMode == KB_UPPER) {
        row1 = KB_ROW1_UPPER;
        row2 = KB_ROW2_UPPER;
        row3 = KB_ROW3_UPPER;
    } else {
        row1 = KB_ROW1_LOWER;
        row2 = KB_ROW2_LOWER;
        row3 = KB_ROW3_LOWER;
    }

    // Input field background
    sprite.fillRoundRect(10, KB_INPUT_Y, DISPLAY_WIDTH - 20, KB_INPUT_HEIGHT, 4, 0x2104);
    sprite.setTextSize(1);
    sprite.setTextDatum(ML_DATUM);
    sprite.setTextColor(COLOR_DATE, 0x2104);

    // Show password dots or text
    if (passwordLen > 0) {
        if (passwordVisible) {
            sprite.drawString(passwordBuffer, 16, KB_INPUT_Y + KB_INPUT_HEIGHT / 2);
        } else {
            char dots[KB_PASSWORD_MAX + 1];
            int dotLen = min((int)passwordLen, KB_PASSWORD_MAX);
            memset(dots, '*', dotLen);
            dots[dotLen] = '\0';
            sprite.drawString(dots, 16, KB_INPUT_Y + KB_INPUT_HEIGHT / 2);
        }
    } else {
        sprite.setTextColor(COLOR_TEXT_DIM, 0x2104);
        sprite.drawString("Enter password", 16, KB_INPUT_Y + KB_INPUT_HEIGHT / 2);
    }

    // Show/hide toggle button (eye icon area)
    uint16_t eyeColor = passwordVisible ? COLOR_SYNC_OK : COLOR_TEXT_DIM;
    sprite.fillRoundRect(DISPLAY_WIDTH - 40, KB_INPUT_Y, 30, KB_INPUT_HEIGHT, 4, eyeColor);
    sprite.setTextColor(TFT_BLACK, eyeColor);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString("Aa", DISPLAY_WIDTH - 25, KB_INPUT_Y + KB_INPUT_HEIGHT / 2);

    uint16_t keyBg = 0x3186;    // Dark gray key background
    uint16_t keyText = COLOR_DATE;
    uint16_t specialBg = 0x2104; // Darker for special keys
    int16_t rowY;
    char keyLabel[2] = {0, 0};

    // Row 1: 10 standard keys
    rowY = KB_Y_OFFSET;
    for (int i = 0; i < 10; i++) {
        keyLabel[0] = row1[i];
        drawKeyboardKey(KB_ROW1_X + i * (KB_KEY_WIDTH + KB_KEY_GAP), rowY, KB_KEY_WIDTH, KB_KEY_HEIGHT, keyLabel, keyBg, keyText);
    }

    // Row 2: 9 standard keys
    rowY = KB_Y_OFFSET + KB_KEY_HEIGHT + KB_KEY_GAP;
    for (int i = 0; i < 9; i++) {
        keyLabel[0] = row2[i];
        drawKeyboardKey(KB_ROW2_X + i * (KB_KEY_WIDTH + KB_KEY_GAP), rowY, KB_KEY_WIDTH, KB_KEY_HEIGHT, keyLabel, keyBg, keyText);
    }

    // Row 3: Shift + 7 keys + Backspace
    rowY = KB_Y_OFFSET + 2 * (KB_KEY_HEIGHT + KB_KEY_GAP);
    // Shift key
    uint16_t shiftBg = (kbMode == KB_UPPER) ? COLOR_TIME : specialBg;
    drawKeyboardKey(KB_ROW3_X, rowY, KB_SPECIAL_WIDTH, KB_KEY_HEIGHT, "Shift", shiftBg, keyText);
    // Letter keys
    int row3Len = strlen(row3);
    int16_t row3KeyX = KB_ROW3_X + KB_SPECIAL_WIDTH + KB_KEY_GAP;
    for (int i = 0; i < row3Len; i++) {
        keyLabel[0] = row3[i];
        drawKeyboardKey(row3KeyX + i * (KB_KEY_WIDTH + KB_KEY_GAP), rowY, KB_KEY_WIDTH, KB_KEY_HEIGHT, keyLabel, keyBg, keyText);
    }
    // Backspace
    int16_t bsX = row3KeyX + row3Len * (KB_KEY_WIDTH + KB_KEY_GAP);
    drawKeyboardKey(bsX, rowY, KB_SPECIAL_WIDTH, KB_KEY_HEIGHT, "<-", specialBg, COLOR_SYNC_FAIL);

    // Row 4: 123 + Space + Connect
    rowY = KB_Y_OFFSET + 3 * (KB_KEY_HEIGHT + KB_KEY_GAP);
    const char* symLabel = (kbMode == KB_SYMBOLS) ? "abc" : "123";
    drawKeyboardKey(KB_ROW4_X, rowY, KB_SPECIAL_WIDTH, KB_KEY_HEIGHT, symLabel, specialBg, keyText);
    drawKeyboardKey(KB_ROW4_X + KB_SPECIAL_WIDTH + KB_KEY_GAP, rowY, KB_SPACE_WIDTH, KB_KEY_HEIGHT, "SPACE", keyBg, keyText);
    int16_t connectX = KB_ROW4_X + KB_SPECIAL_WIDTH + KB_KEY_GAP + KB_SPACE_WIDTH + KB_KEY_GAP;
    drawKeyboardKey(connectX, rowY, KB_CONNECT_WIDTH, KB_KEY_HEIGHT, "Connect", COLOR_SYNC_OK, TFT_BLACK);
}

// Returns the character for a keyboard tap, or special codes:
// '\b' = backspace, '\n' = connect, '\t' = shift toggle, '\r' = 123 toggle, ' ' = space
// '\x01' = show/hide toggle, '\0' = no key hit
char getKeyFromTap(int16_t x, int16_t y) {
    // Check show/hide toggle on input field
    if (y >= KB_INPUT_Y && y <= KB_INPUT_Y + KB_INPUT_HEIGHT && x >= DISPLAY_WIDTH - 40) {
        return '\x01';
    }

    // Must be in keyboard area
    if (y < KB_Y_OFFSET) return '\0';

    int row = (y - KB_Y_OFFSET) / (KB_KEY_HEIGHT + KB_KEY_GAP);
    int16_t rowY = KB_Y_OFFSET + row * (KB_KEY_HEIGHT + KB_KEY_GAP);
    if (y > rowY + KB_KEY_HEIGHT) return '\0';  // In the gap

    const char* row1 = (kbMode == KB_SYMBOLS) ? KB_ROW1_SYM : (kbMode == KB_UPPER) ? KB_ROW1_UPPER : KB_ROW1_LOWER;
    const char* row2 = (kbMode == KB_SYMBOLS) ? KB_ROW2_SYM : (kbMode == KB_UPPER) ? KB_ROW2_UPPER : KB_ROW2_LOWER;
    const char* row3 = (kbMode == KB_SYMBOLS) ? KB_ROW3_SYM : (kbMode == KB_UPPER) ? KB_ROW3_UPPER : KB_ROW3_LOWER;

    if (row == 0) {
        // Row 1: 10 keys starting at KB_ROW1_X
        int col = (x - KB_ROW1_X) / (KB_KEY_WIDTH + KB_KEY_GAP);
        if (col >= 0 && col < 10 && x >= KB_ROW1_X) return row1[col];
    } else if (row == 1) {
        // Row 2: 9 keys starting at KB_ROW2_X
        int col = (x - KB_ROW2_X) / (KB_KEY_WIDTH + KB_KEY_GAP);
        if (col >= 0 && col < 9 && x >= KB_ROW2_X) return row2[col];
    } else if (row == 2) {
        // Row 3: Shift + letters + Backspace
        if (x < KB_ROW3_X + KB_SPECIAL_WIDTH) return '\t';  // Shift
        int16_t row3KeyX = KB_ROW3_X + KB_SPECIAL_WIDTH + KB_KEY_GAP;
        int row3Len = strlen(row3);
        int16_t bsX = row3KeyX + row3Len * (KB_KEY_WIDTH + KB_KEY_GAP);
        if (x >= bsX) return '\b';  // Backspace
        int col = (x - row3KeyX) / (KB_KEY_WIDTH + KB_KEY_GAP);
        if (col >= 0 && col < row3Len) return row3[col];
    } else if (row == 3) {
        // Row 4: 123 + Space + Connect
        if (x < KB_ROW4_X + KB_SPECIAL_WIDTH) return '\r';  // 123/abc toggle
        int16_t spaceX = KB_ROW4_X + KB_SPECIAL_WIDTH + KB_KEY_GAP;
        int16_t connectX = spaceX + KB_SPACE_WIDTH + KB_KEY_GAP;
        if (x >= connectX) return '\n';  // Connect
        if (x >= spaceX && x < spaceX + KB_SPACE_WIDTH) return ' ';  // Space
    }

    return '\0';
}

void processKeyboardInput(char key) {
    if (key == '\0') return;

    if (key == '\x01') {
        // Toggle password visibility
        passwordVisible = !passwordVisible;
    } else if (key == '\t') {
        // Shift toggle
        if (kbMode == KB_LOWER) kbMode = KB_UPPER;
        else if (kbMode == KB_UPPER) kbMode = KB_LOWER;
    } else if (key == '\r') {
        // 123/abc toggle
        if (kbMode == KB_SYMBOLS) kbMode = KB_LOWER;
        else kbMode = KB_SYMBOLS;
    } else if (key == '\b') {
        // Backspace
        if (passwordLen > 0) {
            passwordLen--;
            passwordBuffer[passwordLen] = '\0';
        }
    } else if (key == '\n') {
        // Connect button pressed
        if (selectedNetwork >= 0 && selectedNetwork < scannedCount) {
            String ssid = scannedSSIDs[selectedNetwork];
            String pass = String(passwordBuffer);
            kbMode = KB_HIDDEN;
            wifiConnect(ssid, pass);
        }
    } else if (key == ' ' || (key >= 32 && key <= 126)) {
        // Regular character
        if (passwordLen < KB_PASSWORD_MAX) {
            passwordBuffer[passwordLen] = key;
            passwordLen++;
            passwordBuffer[passwordLen] = '\0';
            // Auto-revert to lowercase after typing one uppercase letter
            if (kbMode == KB_UPPER) kbMode = KB_LOWER;
        }
    }
}

// ============================================================================
// WiFi Page Drawing
// ============================================================================
void drawSignalBars(int16_t x, int16_t y, int8_t rssi) {
    // Draw 4 signal bars based on RSSI
    int bars = 0;
    if (rssi > -50) bars = 4;
    else if (rssi > -60) bars = 3;
    else if (rssi > -70) bars = 2;
    else if (rssi > -80) bars = 1;

    for (int i = 0; i < 4; i++) {
        int barH = 4 + i * 3;
        int barY = y + 14 - barH;
        uint16_t color = (i < bars) ? COLOR_SYNC_OK : COLOR_CHART_GRID;
        sprite.fillRect(x + i * 5, barY, 3, barH, color);
    }
}

void drawWiFiButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, uint16_t bgColor) {
    sprite.fillRoundRect(x, y, w, h, 4, bgColor);
    sprite.setTextColor(TFT_BLACK, bgColor);
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextSize(1);
    sprite.drawString(label, x + w / 2, y + h / 2);
}

void drawWiFiPageStatus() {
    // Title (same size as status text)
    sprite.setTextSize(1);
    sprite.setTextColor(COLOR_TIME, COLOR_BACKGROUND);
    sprite.setTextDatum(TL_DATUM);
    sprite.drawString("WiFi", 10, 10);

    // Centered radio status
    sprite.setTextSize(1);
    sprite.setTextDatum(TC_DATUM);
    if (wifiState == WIFI_STATE_OFF) {
        sprite.setTextColor(COLOR_SYNC_FAIL, COLOR_BACKGROUND);
        sprite.drawString("WiFi Disabled", DISPLAY_WIDTH / 2, 10);
    } else {
        sprite.setTextColor(COLOR_SYNC_OK, COLOR_BACKGROUND);
        sprite.drawString("WiFi Enabled", DISPLAY_WIDTH / 2, 10);
    }

    // Status indicator (top right)
    sprite.setTextSize(1);
    sprite.setTextDatum(TR_DATUM);
    if (wifiState == WIFI_STATE_CONNECTED) {
        sprite.setTextColor(COLOR_SYNC_OK, COLOR_BACKGROUND);
        sprite.drawString("Connected", DISPLAY_WIDTH - 10, 5);
        // Show IP, NTP, RSSI on first row; SSID on second row
        sprite.setTextDatum(TL_DATUM);
        sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
        sprite.drawString(WiFi.localIP().toString().c_str(), 10, 30);

        char ntpStr[32];
        snprintf(ntpStr, sizeof(ntpStr), "NTP: %lu req", (unsigned long)ntpServer.getRequestCount());
        sprite.setTextDatum(TC_DATUM);
        sprite.drawString(ntpStr, DISPLAY_WIDTH / 2, 30);

        char rssiStr[16];
        snprintf(rssiStr, sizeof(rssiStr), "%d dBm", WiFi.RSSI());
        sprite.setTextDatum(TR_DATUM);
        sprite.drawString(rssiStr, DISPLAY_WIDTH - 10, 30);

        sprite.setTextDatum(TL_DATUM);
        sprite.drawString(wifiSSID.c_str(), 10, 55);
    } else if (wifiState == WIFI_STATE_SCANNING) {
        sprite.setTextColor(COLOR_SYNC_PENDING, COLOR_BACKGROUND);
        sprite.drawString("Scanning...", DISPLAY_WIDTH - 10, 5);
    } else if (wifiState == WIFI_STATE_CONNECTING) {
        sprite.setTextColor(COLOR_SYNC_PENDING, COLOR_BACKGROUND);
        sprite.drawString("Connecting...", DISPLAY_WIDTH - 10, 5);
    } else if (wifiState == WIFI_STATE_AP_MODE) {
        sprite.setTextColor(COLOR_SYNC_PENDING, COLOR_BACKGROUND);
        sprite.drawString("AP Mode", DISPLAY_WIDTH - 10, 5);
    } else if (wifiState == WIFI_STATE_OFF) {
        sprite.setTextColor(COLOR_SYNC_FAIL, COLOR_BACKGROUND);
        sprite.drawString("Disabled", DISPLAY_WIDTH - 10, 5);
    } else {
        sprite.setTextColor(COLOR_SYNC_OK, COLOR_BACKGROUND);
        sprite.drawString("Enabled", DISPLAY_WIDTH - 10, 5);
    }

    // Error message
    if (wifiErrorMsg.length() > 0) {
        sprite.setTextDatum(TC_DATUM);
        sprite.setTextColor(COLOR_SYNC_FAIL, COLOR_BACKGROUND);
        sprite.drawString(wifiErrorMsg.c_str(), DISPLAY_WIDTH / 2, 32);
    }
}

void drawWiFiNetworkList() {
    // Separator line
    sprite.drawFastHLine(0, 48, DISPLAY_WIDTH, COLOR_CHART_GRID);

    int16_t listY = 52;
    int16_t rowH = 24;

    if (scannedCount == 0) {
        sprite.setTextSize(1);
        sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
        sprite.setTextDatum(MC_DATUM);
        sprite.drawString("No networks found. Tap Scan.", DISPLAY_WIDTH / 2, listY + 60);
        return;
    }

    int visibleCount = min((int)scannedCount - listScrollOffset, (int)WIFI_MAX_VISIBLE);
    for (int i = 0; i < visibleCount; i++) {
        int idx = i + listScrollOffset;
        int16_t rowTop = listY + i * rowH;

        // Highlight selected network
        if (idx == selectedNetwork) {
            sprite.fillRect(0, rowTop, DISPLAY_WIDTH, rowH, 0x2104);
        }

        // Connected indicator
        if (wifiState == WIFI_STATE_CONNECTED && scannedSSIDs[idx] == wifiSSID) {
            sprite.setTextColor(COLOR_SYNC_OK, (idx == selectedNetwork) ? 0x2104 : COLOR_BACKGROUND);
            sprite.setTextDatum(TL_DATUM);
            sprite.setTextSize(1);
            sprite.drawString(">", 4, rowTop + 6);
        }

        // SSID name
        uint16_t bg = (idx == selectedNetwork) ? 0x2104 : COLOR_BACKGROUND;
        sprite.setTextColor(COLOR_DATE, bg);
        sprite.setTextDatum(TL_DATUM);
        sprite.setTextSize(1);

        // Truncate long SSIDs
        String displaySSID = scannedSSIDs[idx];
        if (displaySSID.length() > 28) {
            displaySSID = displaySSID.substring(0, 25) + "...";
        }
        sprite.drawString(displaySSID.c_str(), 14, rowTop + 6);

        // Signal bars
        drawSignalBars(DISPLAY_WIDTH - 60, rowTop + 4, scannedRSSI[idx]);

        // Lock icon for secured networks
        if (scannedSecure[idx]) {
            sprite.setTextColor(COLOR_SYNC_PENDING, bg);
            sprite.setTextDatum(TR_DATUM);
            sprite.drawString("Lock", DISPLAY_WIDTH - 8, rowTop + 6);
        }
    }

    // Scroll indicator if there are more networks
    if (scannedCount > WIFI_MAX_VISIBLE) {
        sprite.setTextSize(1);
        sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
        sprite.setTextDatum(TC_DATUM);
        char scrollStr[16];
        snprintf(scrollStr, sizeof(scrollStr), "%d/%d", listScrollOffset + 1, scannedCount);
        sprite.drawString(scrollStr, DISPLAY_WIDTH / 2, listY + WIFI_MAX_VISIBLE * rowH + 2);
    }
}

void drawWiFiPage() {
    sprite.fillSprite(COLOR_BACKGROUND);

    if (kbMode != KB_HIDDEN) {
        // Keyboard sub-view
        sprite.setTextSize(1);
        sprite.setTextColor(COLOR_TIME, COLOR_BACKGROUND);
        sprite.setTextDatum(TL_DATUM);
        if (selectedNetwork >= 0 && selectedNetwork < scannedCount) {
            String title = "Connect to: " + scannedSSIDs[selectedNetwork];
            sprite.drawString(title.c_str(), 10, 5);
        }
        drawKeyboard();
    } else if (wifiState == WIFI_STATE_CONNECTING) {
        // Connecting sub-view
        drawWiFiPageStatus();
        sprite.setTextSize(2);
        sprite.setTextColor(COLOR_SYNC_PENDING, COLOR_BACKGROUND);
        sprite.setTextDatum(MC_DATUM);
        sprite.drawString("Connecting...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 20);

        // Progress bar
        unsigned long elapsed = millis() - wifiConnectStart;
        float progress = (float)elapsed / WIFI_CONNECT_TIMEOUT;
        if (progress > 1.0f) progress = 1.0f;
        int barW = DISPLAY_WIDTH - 80;
        sprite.drawRect(40, DISPLAY_HEIGHT / 2 + 10, barW, 8, COLOR_CHART_GRID);
        sprite.fillRect(41, DISPLAY_HEIGHT / 2 + 11, (int)((barW - 2) * progress), 6, COLOR_SYNC_PENDING);

        // Cancel button
        drawWiFiButton(DISPLAY_WIDTH / 2 - 50, DISPLAY_HEIGHT - 35, 100, 26, "Cancel", COLOR_SYNC_FAIL);
    } else if (wifiState == WIFI_STATE_AP_MODE) {
        // AP Mode sub-view
        sprite.setTextSize(2);
        sprite.setTextColor(COLOR_TIME, COLOR_BACKGROUND);
        sprite.setTextDatum(TC_DATUM);
        sprite.drawString("AP Mode Active", DISPLAY_WIDTH / 2, 10);

        sprite.setTextSize(1);
        sprite.setTextColor(COLOR_DATE, COLOR_BACKGROUND);
        sprite.setTextDatum(TL_DATUM);
        sprite.drawString("Network:", 30, 60);
        sprite.setTextColor(COLOR_SYNC_OK, COLOR_BACKGROUND);
        sprite.drawString(WIFI_AP_SSID, 110, 60);

        sprite.setTextColor(COLOR_DATE, COLOR_BACKGROUND);
        sprite.drawString("Password:", 30, 80);
        sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
        sprite.drawString("(none)", 110, 80);

        sprite.setTextColor(COLOR_DATE, COLOR_BACKGROUND);
        sprite.drawString("IP:", 30, 100);
        sprite.setTextColor(COLOR_SYNC_OK, COLOR_BACKGROUND);
        sprite.drawString("192.168.4.1", 110, 100);

        sprite.setTextColor(COLOR_TEXT_DIM, COLOR_BACKGROUND);
        sprite.setTextDatum(TC_DATUM);
        sprite.drawString("Connect your phone to this", DISPLAY_WIDTH / 2, 135);
        sprite.drawString("network, then open a browser.", DISPLAY_WIDTH / 2, 150);

        drawWiFiButton(DISPLAY_WIDTH / 2 - 50, DISPLAY_HEIGHT - 35, 100, 26, "Stop AP", COLOR_SYNC_FAIL);
    } else {
        // Main WiFi page: status + network list + buttons
        drawWiFiPageStatus();
        drawWiFiNetworkList();

        // Bottom buttons
        drawWiFiButton(40, DISPLAY_HEIGHT - 30, 130, 26, "Scan", COLOR_TIME);
        if (wifiState == WIFI_STATE_CONNECTED) {
            drawWiFiButton(DISPLAY_WIDTH / 2 - 55, DISPLAY_HEIGHT - 30, 110, 26, "Sync NTP", COLOR_SYNC_OK);
        }
        drawWiFiButton(DISPLAY_WIDTH - 170, DISPLAY_HEIGHT - 30, 130, 26, "Start AP", COLOR_SYNC_PENDING);
    }

}

// ============================================================================
// WiFi Page Touch Handling
// ============================================================================
void handleWiFiTap(int16_t x, int16_t y) {
    if (kbMode != KB_HIDDEN) {
        // Keyboard is visible — process key tap
        char key = getKeyFromTap(x, y);
        processKeyboardInput(key);
        return;
    }

    if (wifiState == WIFI_STATE_CONNECTING) {
        // Cancel button
        if (y >= DISPLAY_HEIGHT - 35 && y <= DISPLAY_HEIGHT - 9 &&
            x >= DISPLAY_WIDTH / 2 - 50 && x <= DISPLAY_WIDTH / 2 + 50) {
            WiFi.disconnect();
            wifiState = WIFI_STATE_IDLE;
            wifiErrorMsg = "Cancelled";
            Serial.println("[WIFI] Connection cancelled");
        }
        return;
    }

    if (wifiState == WIFI_STATE_AP_MODE) {
        // Stop AP button
        if (y >= DISPLAY_HEIGHT - 35 && y <= DISPLAY_HEIGHT - 9 &&
            x >= DISPLAY_WIDTH / 2 - 50 && x <= DISPLAY_WIDTH / 2 + 50) {
            wifiStopAP();
        }
        return;
    }

    // Main page buttons
    // Scan button: X:40-170, Y: DISPLAY_HEIGHT-30 to DISPLAY_HEIGHT-4
    if (y >= DISPLAY_HEIGHT - 30 && y <= DISPLAY_HEIGHT - 4) {
        if (x >= 40 && x <= 170) {
            wifiScan();
            return;
        }
        // Sync NTP button (center, only when connected)
        if (wifiState == WIFI_STATE_CONNECTED &&
            x >= DISPLAY_WIDTH / 2 - 55 && x <= DISPLAY_WIDTH / 2 + 55) {
            Serial.println("[WIFI] Sync NTP button pressed");
            ntpClientSync();
            return;
        }
        if (x >= DISPLAY_WIDTH - 170 && x <= DISPLAY_WIDTH - 40) {
            wifiStartAP();
            return;
        }
    }

    // Network list tap: Y between 52 and 52 + WIFI_MAX_VISIBLE * 24
    int16_t listY = 52;
    int16_t rowH = 24;
    if (y >= listY && y < listY + WIFI_MAX_VISIBLE * rowH && scannedCount > 0) {
        int tapIdx = (y - listY) / rowH + listScrollOffset;
        if (tapIdx >= 0 && tapIdx < scannedCount) {
            selectedNetwork = tapIdx;
            Serial.printf("[WIFI] Selected network: %s\n", scannedSSIDs[tapIdx].c_str());

            if (scannedSecure[tapIdx]) {
                // Open keyboard for password entry
                kbMode = KB_LOWER;
                passwordLen = 0;
                passwordBuffer[0] = '\0';
                passwordVisible = false;
            } else {
                // Open network — connect directly
                wifiConnect(scannedSSIDs[tapIdx], "");
            }
        }
    }
}

// ============================================================================
// Touch Gesture Handler
// ============================================================================
void handleTouch() {
    int16_t x, y;
    bool touched = amoled.getPoint(&x, &y);

    if (touched && !touchActive) {
        // === Touch Start ===
        touchActive = true;
        touchStartX = x;
        touchStartY = y;
        touchLastX = x;
        touchLastY = y;
        touchStartTime = millis();
        swipeHandled = false;
        sliderDragging = false;
        shutdownCountdownActive = false;
    } else if (touched && touchActive) {
        // === Touch Continue (finger still down) ===
        touchLastX = x;
        touchLastY = y;
        unsigned long holdDuration = millis() - touchStartTime;

        // Check finger movement from start
        int16_t dx = abs(x - touchStartX);
        int16_t dy = abs(y - touchStartY);
        bool fingerStill = (dx < LONG_PRESS_MOVE_PX && dy < LONG_PRESS_MOVE_PX);

        // Long-press shutdown detection (only when keyboard is hidden)
        if (kbMode == KB_HIDDEN && fingerStill && holdDuration >= SHUTDOWN_HOLD_MS) {
            performShutdown();
            return;
        }

        // Show shutdown countdown overlay if holding still long enough
        if (kbMode == KB_HIDDEN && fingerStill && holdDuration >= SHUTDOWN_FEEDBACK_MS) {
            shutdownCountdownActive = true;
        }

        // Slider dragging on settings page
        if (currentPage == PAGE_SETTINGS && !fingerStill) {
            handleSliderTouch(x, y);
        }
    } else if (!touched && touchActive) {
        // === Touch End (finger lifted) ===
        touchActive = false;
        shutdownCountdownActive = false;
        unsigned long holdDuration = millis() - touchStartTime;

        int16_t deltaX = touchLastX - touchStartX;
        int16_t deltaY = touchLastY - touchStartY;

        // Save brightness if slider was dragged
        if (sliderDragging) {
            sliderDragging = false;
            preferences.begin("wwvb", false);
            preferences.putUChar("brightness", currentBrightness);
            preferences.end();
            Serial.printf("[TOUCH] Brightness saved: %d\n", currentBrightness);
        }
        // Swipe detection (disabled when keyboard is visible)
        else if (kbMode == KB_HIDDEN && abs(deltaX) > SWIPE_THRESHOLD_PX && holdDuration < 1000) {
            if (deltaX < -SWIPE_THRESHOLD_PX && currentPage < PAGE_WIFI) {
                currentPage = (DisplayPage)(currentPage + 1);
                Serial.printf("[TOUCH] Swipe left -> page %d\n", currentPage);
            } else if (deltaX > SWIPE_THRESHOLD_PX && currentPage > PAGE_CLOCK) {
                currentPage = (DisplayPage)(currentPage - 1);
                Serial.printf("[TOUCH] Swipe right -> page %d\n", currentPage);
                // Hide keyboard when leaving WiFi page
                if (currentPage != PAGE_WIFI) {
                    kbMode = KB_HIDDEN;
                }
            }
        }
        // Vertical scroll on WiFi network list
        else if (currentPage == PAGE_WIFI && kbMode == KB_HIDDEN && abs(deltaY) > 30 && abs(deltaX) < 30) {
            if (deltaY < -30 && listScrollOffset < (int)scannedCount - WIFI_MAX_VISIBLE) {
                listScrollOffset++;
            } else if (deltaY > 30 && listScrollOffset > 0) {
                listScrollOffset--;
            }
        }
        // Tap on settings page (short press, not a swipe)
        else if (currentPage == PAGE_SETTINGS && holdDuration < 500 && abs(deltaX) < SWIPE_THRESHOLD_PX) {
            // WiFi toggle button hit test (bottom left)
            int btnW = 120, btnH = 26;
            int btnX = 10;
            int btnY = DISPLAY_HEIGHT - btnH - 6;
            if (touchStartX >= btnX && touchStartX <= btnX + btnW &&
                touchStartY >= btnY && touchStartY <= btnY + btnH) {
                if (wifiState != WIFI_STATE_OFF) {
                    // Disable WiFi
                    ntpServer.stop();
                    statusServer.stop();
                    captivePortal.stop();
                    WiFi.disconnect();
                    WiFi.softAPdisconnect(false);
                    WiFi.mode(WIFI_OFF);
                    wifiState = WIFI_STATE_OFF;
                    scannedCount = 0;
                    Serial.println("[WIFI] Radio disabled from settings");
                } else {
                    // Enable WiFi — scan for networks
                    wifiScan();
                    Serial.println("[WIFI] Radio enabled from settings");
                }
            } else {
                // UTC offset +/- button hit test (bottom-right, matches drawSettingsPage)
                int utcBtnW = 30, utcBtnH = 26;
                int utcBtnY = DISPLAY_HEIGHT - utcBtnH - 6;
                int utcRightEdge = DISPLAY_WIDTH - 10;
                int utcPlusX = utcRightEdge - utcBtnW;
                int valueX = utcPlusX - 45;
                int utcMinusX = valueX - 45 - utcBtnW / 2;

                if (touchStartY >= utcBtnY && touchStartY <= utcBtnY + utcBtnH) {
                    if (touchStartX >= utcMinusX && touchStartX <= utcMinusX + utcBtnW && utcOffset > -12) {
                        utcOffset--;
                        preferences.begin("wwvb", false);
                        preferences.putChar("utcOffset", utcOffset);
                        preferences.end();
                        captivePortal.setTimezone(utcOffset, dstActive);
                        Serial.printf("[SETTINGS] UTC offset: %+d\n", utcOffset);
                    } else if (touchStartX >= utcPlusX && touchStartX <= utcPlusX + utcBtnW && utcOffset < 14) {
                        utcOffset++;
                        preferences.begin("wwvb", false);
                        preferences.putChar("utcOffset", utcOffset);
                        preferences.end();
                        captivePortal.setTimezone(utcOffset, dstActive);
                        Serial.printf("[SETTINGS] UTC offset: %+d\n", utcOffset);
                    }
                } else {
                    // Slider tap
                    handleSliderTouch(touchStartX, touchStartY);
                    if (sliderDragging) {
                        sliderDragging = false;
                        preferences.begin("wwvb", false);
                        preferences.putUChar("brightness", currentBrightness);
                        preferences.end();
                        Serial.printf("[TOUCH] Brightness tapped: %d\n", currentBrightness);
                    }
                }
            }
        }
        // Tap on WiFi page (short press)
        else if (currentPage == PAGE_WIFI && holdDuration < 500 && abs(deltaX) < SWIPE_THRESHOLD_PX) {
            handleWiFiTap(touchStartX, touchStartY);
        }
    }
}

// ============================================================================
// Display Update (with page support)
// ============================================================================
void updateDisplay() {
    static bool firstRun = true;
    if (firstRun) {
        Serial.println("updateDisplay() called for first time");
        firstRun = false;
    }

    // Clear sprite
    sprite.fillSprite(COLOR_BACKGROUND);

    // Draw current page
    if (currentPage == PAGE_CLOCK) {
        drawClockFace(false);
        drawLastSyncTime();
        drawReceptionChart();
        drawTemperature();
    } else if (currentPage == PAGE_UTC) {
        drawClockFace(true);
        drawLastSyncTime();
        drawReceptionChart();
        drawTemperature();
    } else if (currentPage == PAGE_SYNC) {
        drawSyncPage();
    } else if (currentPage == PAGE_SETTINGS) {
        drawSettingsPage();
    } else if (currentPage == PAGE_WIFI) {
        drawWiFiPage();
    }

    // Draw shutdown countdown overlay on top of any page
    if (shutdownCountdownActive && touchActive) {
        unsigned long holdDuration = millis() - touchStartTime;
        drawShutdownOverlay(holdDuration);
    }

    // Push to display
    pushDisplay();
}

// ============================================================================
// Time Persistence Functions
// ============================================================================
void saveTimeToPreferences() {
    preferences.begin("wwvb", false);  // Open in read-write mode

    ClockTime utc = timeManager.getUTCTime();
    preferences.putUShort("year", utc.year);
    preferences.putUChar("month", utc.month);
    preferences.putUChar("day", utc.day);
    preferences.putUChar("hour", utc.hour);
    preferences.putUChar("minute", utc.minute);
    preferences.putUChar("second", utc.second);
    preferences.putBool("dst", dstActive);
    preferences.putULong("saveMillis", millis());

    // Persist tracking state so the 7-day window survives reboots.
    // Save the elapsed age (ms since tracking was established); restored on load
    // by reconstructing the reference point with unsigned arithmetic.
    preferences.putBool("trkReady", es100TrackingReady);
    unsigned long trkAge = es100TrackingReady ? (millis() - es100TrackingReadySinceMs) : 0xFFFFFFFFUL;
    preferences.putULong("trkAge", trkAge);

    // Persist per-antenna success counters
    preferences.putUShort("ant1ok", ant1Successes);
    preferences.putUShort("ant2ok", ant2Successes);

    preferences.end();
    Serial.println("Time saved to preferences");
}

bool loadTimeFromPreferences() {
    preferences.begin("wwvb", true);  // Open in read-only mode

    // Check if we have saved time data
    if (!preferences.isKey("year")) {
        preferences.end();
        Serial.println("No saved time found in preferences");
        return false;
    }

    uint16_t year = preferences.getUShort("year", 2025);
    uint8_t month = preferences.getUChar("month", 1);
    uint8_t day = preferences.getUChar("day", 1);
    uint8_t hour = preferences.getUChar("hour", 0);
    uint8_t minute = preferences.getUChar("minute", 0);
    uint8_t second = preferences.getUChar("second", 0);
    dstActive = preferences.getBool("dst", false);
    unsigned long saveMillis = preferences.getULong("saveMillis", 0);
    bool savedTrkReady = preferences.getBool("trkReady", false);
    unsigned long savedTrkAge = preferences.getULong("trkAge", 0xFFFFFFFFUL);
    ant1Successes = preferences.getUShort("ant1ok", 0);
    ant2Successes = preferences.getUShort("ant2ok", 0);

    preferences.end();

    // Restore tracking state if it was active and the 7-day window hasn't expired.
    // Unsigned subtraction reconstructs the original reference point correctly.
    if (savedTrkReady && savedTrkAge < ES100_TRACKING_FALLBACK_MS) {
        es100TrackingReady = true;
        es100TrackingReadySinceMs = millis() - savedTrkAge;
        Serial.printf("Tracking state restored (age: %lu ms, %lu ms remaining)\n",
                     savedTrkAge, ES100_TRACKING_FALLBACK_MS - savedTrkAge);
    }

    // Set the time
    timeManager.setTime(year, month, day, hour, minute, second);

    // Add elapsed seconds since save (only if reasonable - max 1 hour)
    // This handles quick reboots but avoids issues with millis() overflow
    unsigned long elapsedMillis = millis() - saveMillis;
    // Only apply if less than 1 hour (avoids millis() wrap issues)
    if (elapsedMillis < 3600000UL) {  // 3600000ms = 1 hour
        unsigned long elapsedSeconds = elapsedMillis / 1000;
        Serial.printf("Adjusting time forward by %lu seconds\n", elapsedSeconds);
        // Manually tick forward the time by elapsed seconds
        for (unsigned long i = 0; i < elapsedSeconds; i++) {
            timeManager.tick();
        }
    } else {
        Serial.println("Elapsed time too large or millis() wrapped - not adjusting");
    }

    Serial.printf("Loaded time from preferences: %04d-%02d-%02d %02d-%02d:%02d (DST: %s)\n",
                 year, month, day, hour, minute, second, dstActive ? "Yes" : "No");
    return true;
}

// ============================================================================
// DS3231 RTC Functions
// ============================================================================
bool initializeDS3231() {
    Serial.println("Attempting DS3231 RTC initialization...");

    // First verify a device responds at DS3231 address (0x68)
    Wire.beginTransmission(0x68);
    uint8_t i2cError = Wire.endTransmission();
    if (i2cError != 0) {
        Serial.printf("DS3231 not found at I2C address 0x68 (error: %d)\n", i2cError);
        rtcAvailable = false;
        return false;
    }

    if (!rtc.begin(&Wire)) {
        Serial.println("DS3231 library initialization failed");
        rtcAvailable = false;
        return false;
    }

    Serial.println("DS3231 RTC initialized successfully");

    // Check if RTC lost power and needs to be reset
    if (rtc.lostPower()) {
        Serial.println("WARNING: DS3231 lost power, time may be invalid");
        // Don't set a default time - wait for ES100 sync
    }

    rtc.disable32K();
    rtc.writeSqwPinMode(DS3231_SquareWave1Hz);
#if PIN_DS3231_SQW >= 0
    pinMode(PIN_DS3231_SQW, INPUT_PULLUP);
    rtcSqwInterruptFlag = false;
    attachInterrupt(digitalPinToInterrupt(PIN_DS3231_SQW), rtcSqwISR, FALLING);
    Serial.printf("DS3231 1Hz SQW enabled on GPIO%d\n", PIN_DS3231_SQW);
#else
    Serial.println("DS3231 1Hz SQW support compiled in but disabled (PIN_DS3231_SQW = -1)");
#endif

    rtcAvailable = true;
    return true;
}

bool loadTimeFromDS3231() {
    if (!rtcAvailable) {
        Serial.println("DS3231 not available");
        return false;
    }

    DateTime now = rtc.now();

    // Sanity check - year should be reasonable
    if (now.year() < 2025 || now.year() > 2100) {
        Serial.printf("DS3231 time appears invalid: %04d-%02d-%02d\n",
                     now.year(), now.month(), now.day());
        return false;
    }

    // Set TimeManager with UTC time from RTC
    timeManager.setTime(now.year(), now.month(), now.day(),
                       now.hour(), now.minute(), now.second());

    Serial.printf("Loaded time from DS3231: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                 now.year(), now.month(), now.day(),
                 now.hour(), now.minute(), now.second());

    // Only set source to RTC if no better source has synced
    if (lastTimeSource == TIME_SRC_NONE) {
        lastTimeSource = TIME_SRC_RTC;
        lastTimeSyncMillis = millis();
        snapshotSyncTime();
        captivePortal.setTimeSource((uint8_t)lastTimeSource);
    }

    return true;
}

void saveTimeToDS3231() {
    if (!rtcAvailable) {
        return;
    }

#if PIN_DS3231_SQW >= 0
    rtcWritePending = true;
    Serial.println("Queued DS3231 update for next 1Hz boundary");
#else
    ClockTime utc = timeManager.getUTCTime();
    rtc.adjust(DateTime(utc.year, utc.month, utc.day,
                       utc.hour, utc.minute, utc.second));

    Serial.printf("Saved time to DS3231: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                 utc.year, utc.month, utc.day,
                 utc.hour, utc.minute, utc.second);
#endif
}

void syncFromDS3231() {
    if (!rtcAvailable) {
        return;
    }

    // Suppress DS3231 corrections for WWVB_SYNC_TRUST_WINDOW_MS after a WWVB sync.
    // Allows the DS3231 I2C write and its own 1Hz tick to settle before we read back.
    // After the window expires, DS3231 resumes as the normal drift-correction source.
    if (lastWWVBSyncMillis > 0 &&
        (millis() - lastWWVBSyncMillis) < WWVB_SYNC_TRUST_WINDOW_MS) {
        return;
    }

    // Read DS3231 as the authoritative time source every call
    // The DS3231 TCXO is ±2 ppm vs ESP32 crystal at ±20 ppm
    DateTime rtcTime = rtc.now();

    // Sanity check
    if (rtcTime.year() < 2025 || rtcTime.year() > 2100) {
        return;
    }

    // Only sync if discrepancy is ≥2 seconds (using Unix time for correct boundary handling).
    // A 1-second difference is normal: tick() may advance TimeManager just before the DS3231
    // register increments, causing a false mismatch that rolls the second back and produces
    // a consistent ~1-second display lag. Requiring ≥2 seconds eliminates the race while
    // still correcting genuine drift (ESP32 ±20ppm vs DS3231 ±2ppm = ~1.5s/day max).
    uint32_t rtcUnix = rtcTime.unixtime();
    uint32_t tmUnix  = timeManager.getUnixTime();
    int32_t  diff    = (int32_t)(rtcUnix - tmUnix);
    if (diff < -1 || diff > 1) {
        // Use the preserve-millis variant so DS3231 drift corrections do not
        // reset the sub-second accumulator that feeds NTP fractional timestamps.
        timeManager.setUnixTimePreserveMillis(rtcUnix);
    }
}

void processDS3231SquareWave() {
#if PIN_DS3231_SQW < 0
    return;
#else
    if (!rtcSqwInterruptFlag) {
        if (timeManager.hasRTCPhaseAnchor() &&
            lastRtcSqwSeenMillis > 0 &&
            (millis() - lastRtcSqwSeenMillis) > 2500UL) {
            timeManager.clearRTCPhaseAnchor();
            Serial.println("[RTC-SQW] lost lock");
            lastRtcSqwStatusLogMillis = 0;
        }
        return;
    }

    noInterrupts();
    uint32_t edgeMicros = rtcSqwEdgeMicros;
    rtcSqwInterruptFlag = false;
    interrupts();

    bool wasLocked = timeManager.hasRTCPhaseAnchor();
    uint32_t anchorUnix = 0;

    if (rtcWritePending) {
        anchorUnix = timeManager.getUnixTime();
        rtcWriteSubsecMs = timeManager.getMilliseconds();  // save phase for next edge
        // Write the current integer second to DS3231.  The DS3231 oscillator resets
        // at the write moment; the next SQW edge fires ~1 s later at anchorUnix+1,
        // offset by rtcWriteSubsecMs from the true second boundary.  We correct for
        // that offset in the rtcWriteDonePending handler below.
        ClockTime utc;
        {
            uint32_t seconds = anchorUnix;
            uint16_t year = 1970;
            while (true) {
                uint32_t secondsInYear = TimeManager::isLeapYear(year) ? 366UL * 86400UL : 365UL * 86400UL;
                if (seconds < secondsInYear) break;
                seconds -= secondsInYear;
                year++;
            }
            uint8_t month = 1;
            while (month <= 12) {
                uint32_t secondsInMonth = (uint32_t)TimeManager::daysInMonth(year, month) * 86400UL;
                if (seconds < secondsInMonth) break;
                seconds -= secondsInMonth;
                month++;
            }
            utc.year   = year;
            utc.month  = month;
            utc.day    = 1 + (seconds / 86400UL);
            seconds   %= 86400UL;
            utc.hour   = seconds / 3600UL;
            seconds   %= 3600UL;
            utc.minute = seconds / 60UL;
            utc.second = seconds % 60UL;
        }
        rtc.adjust(DateTime(utc.year, utc.month, utc.day,
                           utc.hour, utc.minute, utc.second));
        rtcWritePending = false;
        rtcWriteDonePending = true;  // next edge uses phase-corrected anchor
        Serial.printf("Saved time to DS3231 on 1Hz boundary: %04d-%02d-%02d %02d:%02d:%02d UTC (subsec=%ums)\n",
                     utc.year, utc.month, utc.day,
                     utc.hour, utc.minute, utc.second, rtcWriteSubsecMs);
        return;
    } else if (rtcWriteDonePending) {
        // First natural SQW edge after a DS3231 write.  The DS3231 oscillator was
        // reset at write time T.rtcWriteSubsecMs, so this edge fires at T+1.rtcWriteSubsecMs.
        // rtc.now() correctly returns T+1; back-date the anchor micros by rtcWriteSubsecMs
        // so the phase-locked clock shows T+1.rtcWriteSubsecMs at this edge — matching
        // the TimeManager's sub-second phase from the WWVB sync that triggered the write.
        rtcWriteDonePending = false;
        if (!rtcAvailable) return;
        DateTime rtcTime = rtc.now();
        if (rtcTime.year() < 2025 || rtcTime.year() > 2100) return;
        anchorUnix = rtcTime.unixtime();
        timeManager.setRTCPhaseAnchor(anchorUnix,
                                      edgeMicros - (uint32_t)rtcWriteSubsecMs * 1000UL);
        lastRtcSqwSeenMillis = millis();
        Serial.printf("[RTC-SQW] write-edge re-lock: unix=%lu subsec=%ums anchor_micros=%lu\n",
                     (unsigned long)anchorUnix, rtcWriteSubsecMs,
                     (unsigned long)(edgeMicros - (uint32_t)rtcWriteSubsecMs * 1000UL));
        return;
    } else {
        if (!rtcAvailable) {
            return;
        }

        DateTime rtcTime = rtc.now();
        if (rtcTime.year() < 2025 || rtcTime.year() > 2100) {
            return;
        }
        anchorUnix = rtcTime.unixtime();
    }

    // Apply the same phase offset as the write-done handler: the DS3231 oscillator was
    // reset at write time, so every SQW edge fires rtcWriteSubsecMs ms after the true
    // second boundary until the next write.  Without this correction the phase established
    // by the write-done handler is overwritten on E2 and all subsequent edges.
    uint32_t correctedMicros = edgeMicros - (uint32_t)rtcWriteSubsecMs * 1000UL;
    timeManager.setRTCPhaseAnchor(anchorUnix, correctedMicros);
    lastRtcSqwSeenMillis = millis();
    if (!wasLocked) {
        Serial.printf("[RTC-SQW] lock acquired: unix=%lu micros=%lu (subsec=%ums)\n",
                      (unsigned long)anchorUnix,
                      (unsigned long)correctedMicros, rtcWriteSubsecMs);
        lastRtcSqwStatusLogMillis = lastRtcSqwSeenMillis;
    } else if ((lastRtcSqwSeenMillis - lastRtcSqwStatusLogMillis) >= 60000UL) {
        Serial.printf("[RTC-SQW] edge ok: unix=%lu micros=%lu (subsec=%ums)\n",
                      (unsigned long)anchorUnix,
                      (unsigned long)correctedMicros, rtcWriteSubsecMs);
        lastRtcSqwStatusLogMillis = lastRtcSqwSeenMillis;
    }
#endif
}

void readDS3231Temperature() {
    if (!rtcAvailable) {
        rtcTemperature = 0.0;
        return;
    }

    rtcTemperature = rtc.getTemperature();
}

// ============================================================================
// NTP Client Sync (query time.nist.gov as redundant time source)
// ============================================================================
bool ntpClientSync() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NTP-CLIENT] Not connected to WiFi");
        return false;
    }

    Serial.printf("[NTP-CLIENT] Querying %s...\n", NTP_FALLBACK_HOST);

    // Resolve DNS first so we can store the upstream IP for Stratum 2 refId
    IPAddress ntpServerIP;
    if (WiFi.hostByName(NTP_FALLBACK_HOST, ntpServerIP) != 1) {
        Serial.printf("[NTP-CLIENT] DNS resolution failed for %s\n", NTP_FALLBACK_HOST);
        return false;
    }
    Serial.printf("[NTP-CLIENT] Resolved %s → %s\n", NTP_FALLBACK_HOST, ntpServerIP.toString().c_str());

    WiFiUDP udp;
    if (!udp.begin(0)) {  // Ephemeral local port
        Serial.println("[NTP-CLIENT] Failed to open UDP socket");
        return false;
    }

    // Build 48-byte NTP request: LI=0, VN=4, Mode=3 (client)
    uint8_t packet[48];
    memset(packet, 0, 48);
    packet[0] = 0x23;  // 0b00_100_011

    // Send to resolved IP
    if (!udp.beginPacket(ntpServerIP, 123)) {
        Serial.println("[NTP-CLIENT] beginPacket failed");
        udp.stop();
        return false;
    }
    udp.write(packet, 48);
    udp.endPacket();
    unsigned long t1 = millis();  // T1: approximate send time (just after endPacket)

    // Wait for response (up to 2 seconds)
    unsigned long start = millis();
    int cb = 0;
    while ((cb = udp.parsePacket()) < 48) {
        if (millis() - start > 2000) {
            Serial.println("[NTP-CLIENT] Timeout waiting for response");
            udp.stop();
            return false;
        }
        delay(10);
    }
    unsigned long t4 = millis();  // T4: response received

    // Read response
    udp.read(packet, 48);
    udp.stop();

    // Extract T3 (server transmit timestamp, bytes 40-43)
    uint32_t t3_ntp = ((uint32_t)packet[40] << 24) |
                      ((uint32_t)packet[41] << 16) |
                      ((uint32_t)packet[42] << 8)  |
                      (uint32_t)packet[43];

    if (t3_ntp == 0) {
        Serial.println("[NTP-CLIENT] Invalid response (zero timestamp)");
        return false;
    }

    // Convert NTP timestamp (since 1900) to Unix timestamp (since 1970)
    uint32_t unixTime = t3_ntp - NTP_EPOCH_OFFSET;

    // Extract T3 sub-second fraction (bytes 44–47): NTP 32-bit fraction → milliseconds.
    uint32_t t3_frac = ((uint32_t)packet[44] << 24) | ((uint32_t)packet[45] << 16) |
                       ((uint32_t)packet[46] << 8)  |  (uint32_t)packet[47];
    uint16_t t3_ms = (uint16_t)(t3_frac / 4294967UL);

    // RTT compensation: apply half the measured round-trip as a one-way delay offset.
    // Combine T3 sub-second (t3_ms) with half-RTT before splitting into integer seconds
    // and milliseconds.  This correctly handles the carry when t3_ms + rttMs/2 >= 1000 —
    // the previous approach computed offsetSec from rttMs/2 alone and missed the carry,
    // which set the clock exactly 1 second behind whenever T3 was late in the second.
    unsigned long rttMs     = t4 - t1;
    uint32_t totalMs        = (uint32_t)t3_ms + (uint32_t)(rttMs / 2);
    uint16_t subSecOffsetMs = (uint16_t)(totalMs % 1000);

    timeManager.setUnixTime(unixTime + totalMs / 1000);
    // Restore sub-second phase from T3 fraction + half-RTT so NTP fractional
    // timestamps are stable between polls (same fix as WWVB tracking path).
    timeManager.setSubSecondOffset(subSecOffsetMs);

    Serial.printf("[NTP-CLIENT] RTT=%lums, T3_ms=%u, totalMs=%lu (+%lus + %ums sub-sec)\n",
                  (unsigned long)rttMs, t3_ms, (unsigned long)totalMs,
                  (unsigned long)(totalMs / 1000), subSecOffsetMs);
    saveTimeToDS3231();

    // Track time source — NTP is Stratum 2
    // RFC 5905: Stratum 2+ reference ID = upstream server's IPv4 address
    lastTimeSource = TIME_SRC_NTP;
    lastTimeSyncMillis = millis();
    snapshotSyncTime();
    ntpServer.setStratum(2, ntpServerIP);
    ntpServer.setLastSyncTime(timeManager.getUnixTime());
    ntpServer.setLeapIndicator(0);  // Clear any alarm state from prior stratum-16 period
    captivePortal.setTimeSource((uint8_t)lastTimeSource);

    // Recompute DST from calendar after NTP time update
    if (AUTO_DST_ENABLED) {
        dstActive = computeUSDST();
        Serial.printf("[DST] Auto-DST after NTP sync: %s\n", dstActive ? "active" : "inactive");
    }

    Serial.printf("[NTP-CLIENT] Synced: Unix=%lu\n", (unsigned long)unixTime);
    return true;
}

// ============================================================================
// Sync Time Snapshot
// ============================================================================
// Records the current UTC time as a formatted string in statusData.lastSyncTimeStr.
// Call immediately after setting lastTimeSyncMillis at each sync point.
void snapshotSyncTime() {
    ClockTime utc = timeManager.getUTCTime();
    snprintf(statusData.lastSyncTimeStr, sizeof(statusData.lastSyncTimeStr),
             "%04d-%02d-%02d %02d:%02d:%02d",
             utc.year, utc.month, utc.day, utc.hour, utc.minute, utc.second);
}

// ============================================================================
// ES100 Initialization Functions
// ============================================================================
bool initializeES100() {
    Serial.println("Attempting ES100 initialization...");

    if (es100.begin(&Wire1, ES100_SDA_PIN, ES100_SCL_PIN)) {
        Serial.println("ES100 initialized successfully");
        es100Available = true;
        es100InitRetries = 0;  // Reset retry counter on success
        return true;
    } else {
        Serial.printf("ES100 initialization failed (attempt %d, will retry)\n",
                     es100InitRetries + 1);
        es100Available = false;
        return false;
    }
}

bool retryES100Initialization() {
    if (es100Available) {
        return false;  // Already initialized
    }

    unsigned long now = millis();
    uint8_t retryIndex = (es100InitRetries < MAX_ES100_INIT_RETRIES) ? es100InitRetries : (MAX_ES100_INIT_RETRIES - 1);
    unsigned long retryInterval = ES100_RETRY_INTERVALS[retryIndex];

    if (now - lastES100InitAttempt >= retryInterval) {
        lastES100InitAttempt = now;
        es100InitRetries++;

        if (initializeES100()) {
            Serial.println("ES100 recovery successful!");
            return true;
        }
    }

    return false;
}

// ============================================================================
// WWVB Sync Schedule — time-aware with daytime backoff
// ============================================================================

/**
 * @brief Check if current local time is within the nighttime reception window
 * @return true if local hour is between SYNC_NIGHT_START_HOUR and SYNC_NIGHT_END_HOUR
 */
bool isNighttimeWindow() {
    if (!timeManager.isTimeSet()) return true;  // If no time yet, assume night (try aggressively)
    ClockTime local = timeManager.getLocalTime(utcOffset, dstActive);
    // Nighttime wraps midnight: e.g. 22:00 - 06:00
    return (local.hour >= SYNC_NIGHT_START_HOUR || local.hour < SYNC_NIGHT_END_HOUR);
}

/**
 * @brief Compute US DST status from the calendar
 * @details US rules: DST starts 2nd Sunday of March at 2:00 AM local standard time,
 *          ends 1st Sunday of November at 2:00 AM local standard time.
 *          Uses standard time (no DST applied) to avoid circular dependency.
 * @return true if DST should be active
 */
bool computeUSDST() {
    if (!timeManager.isTimeSet()) return false;

    // Get local STANDARD time (DST=false) to determine if DST should be active
    ClockTime local = timeManager.getLocalTime(utcOffset, false);

    // Jan, Feb, Dec: never DST
    if (local.month < 3 || local.month > 11) return false;

    // Apr through Oct: always DST
    if (local.month > 3 && local.month < 11) return true;

    // March: DST starts 2nd Sunday at 2:00 AM standard time
    if (local.month == 3) {
        int dow1 = TimeManager::calculateDayOfWeek(local.year, 3, 1);  // 0=Sunday
        int secondSunday = (1 + (7 - dow1) % 7) + 7;
        return (local.day > secondSunday) ||
               (local.day == secondSunday && local.hour >= 2);
    }

    // November: DST ends 1st Sunday at 2:00 AM standard time
    int dow1 = TimeManager::calculateDayOfWeek(local.year, 11, 1);
    int firstSunday = 1 + (7 - dow1) % 7;
    return (local.day < firstSunday) ||
           (local.day == firstSunday && local.hour < 2);
}

/**
 * @brief Get the appropriate sync interval based on time of day and failure history
 * @return Interval in milliseconds until next sync attempt
 */
unsigned long getSyncInterval() {
    // Never synced — aggressive 5-minute attempts regardless of time
    if (receptionHistory.getLastSuccessTime() == 0) {
        return SYNC_INTERVAL_INITIAL_MS;
    }

    if (isNighttimeWindow()) {
        // Nighttime: best propagation, sync every hour
        daytimeFailures = 0;
        daytimeSkipActive = false;
        return SYNC_INTERVAL_NIGHT_MS;
    }

    // Daytime: backed off after repeated failures — wait for nighttime
    if (daytimeSkipActive) {
        // Check if night has arrived to resume
        // (isNighttimeWindow() returned false, so still daytime — keep skipping)
        return SYNC_INTERVAL_DAY_MS;
    }

    // Daytime: normal 4-hour interval
    return SYNC_INTERVAL_DAY_MS;
}

/**
 * @brief Record a sync failure and activate daytime backoff if threshold reached
 */
void recordSyncFailure(bool wasTracking = false) {
    receptionHistory.recordAttempt(false);

    if (!isNighttimeWindow()) {
        uint8_t threshold = wasTracking ? SYNC_DAY_MAX_TRACKING_FAILURES : SYNC_DAY_MAX_FAILURES;
        daytimeFailures++;
        if (daytimeFailures >= threshold) {
            daytimeSkipActive = true;
            Serial.printf("[SYNC] %d consecutive daytime %s failures — skipping until nighttime\n",
                         daytimeFailures, wasTracking ? "tracking" : "normal");
        }
    }
}

// ============================================================================
// Sync Log Helper
// ============================================================================

/**
 * @brief Append one entry to the sync log ring buffer
 * @param success  True if the sync succeeded
 * @param tracking True if tracking mode was used
 * @param antenna  Antenna that was used (1 or 2; 0 = unknown)
 */
void addSyncLogEntry(bool success, bool tracking, uint8_t antenna) {
    SyncLogEntry& e = syncLog[syncLogHead];
    ClockTime t = timeManager.getUTCTime();
    snprintf(e.timeStr, sizeof(e.timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
             t.year, t.month, t.day, t.hour, t.minute, t.second);
    e.success  = success;
    e.tracking = tracking;
    e.antenna  = antenna;
    syncLogHead = (syncLogHead + 1) % SYNC_LOG_SIZE;
    if (syncLogFilled < SYNC_LOG_SIZE) syncLogFilled++;
}

// ============================================================================
// ES100 Sync Functions
// ============================================================================

// Convenience wrapper: ensures tracking-ready state before calling startWWVBSync.
// Use this instead of manually setting es100TrackingReady before the call.
void startWWVBSyncTracking() {
    if (!es100TrackingReady) {
        es100TrackingReady = true;
        es100TrackingReadySinceMs = millis();
    }
    startWWVBSync(false);
}

void startWWVBSync(bool forceNormal = false) {
    if (es100Receiving) return;

    // Cancel any pending scheduled tracking start (e.g. manual sync button pressed)
    if (pendingTrackingStart) {
        pendingTrackingStart = false;
        if (es100.isPoweredOn()) es100.powerOff();
        Serial.println("[WWVB] Pending tracking start cancelled");
    }

    if (!es100Available) {
        lastSyncAttempt = millis();  // Prevent repeated calls
        return;
    }

    es100UsingTracking = false;

    // Nightly normal-mode anchor: force a full-frame sync at the start of each night
    // (10 PM local) and retry every hour until one succeeds.  Tracking mode cannot
    // self-correct errors larger than the ES100's ±4 s timing tolerance; once the clock
    // drifts outside that window, every tracking sync perpetuates the error.  One
    // successful full-frame decode per night re-anchors absolute time and breaks the cycle.
    if (!forceNormal && isNighttimeWindow()) {
        bool needsAnchor = (lastNormalSuccessMillis == 0) ||
                           (millis() - lastNormalSuccessMillis >=
                            NIGHTLY_NORMAL_SYNC_HOURS * 3600000UL);
        if (needsAnchor) {
            forceNormal = true;
            unsigned long hoursAgo = lastNormalSuccessMillis == 0 ? 999UL
                                     : (millis() - lastNormalSuccessMillis) / 3600000UL;
            Serial.printf("[WWVB] Nightly anchor required (%luh since last full sync) — forcing normal mode\n",
                          hoursAgo);
        }
    }

    // Only use tracking mode if the clock is fresh enough that second :55 scheduling
    // is accurate. DS3231 at 2 ppm drifts ~173 ms/day; 5 days = ~865 ms worst case,
    // still within the ±4s tolerance of tracking mode.
    bool clockFreshEnoughForTracking =
        (timeManager.getSecondsSinceSync() < 432000UL);  // 5 days
    if (!forceNormal && ES100_USE_TRACKING && es100TrackingReady && clockFreshEnoughForTracking) {
        if (millis() - es100TrackingReadySinceMs >= ES100_TRACKING_FALLBACK_MS) {
            Serial.println("[WWVB] Tracking mode expired (7 days), switching to normal mode");
            es100TrackingReady = false;
        } else {
            // Tracking reception MUST start (Control 0 write) at second :55.
            // The 22-second window captures the WWVB sync word within ±4s of drift.
            // Schedule the start; the main loop fires it at the right moment.
            ClockTime utc = timeManager.getUTCTime();
            uint8_t sec = utc.second;
            unsigned long msUntil55 = (sec <= 55) ? (unsigned long)(55 - sec) * 1000UL
                                                   : (unsigned long)(60 - sec + 55) * 1000UL;

            es100UsingTracking = true;
            lastSyncAttempt = millis();  // Prevent re-triggering during the wait
            es100InterruptFlag = false;

            // Schedule the Control 0 write for the :55 boundary.
            // Power on the ES100 shortly before :55 to satisfy the 20ms wakeup.
            // If msUntil55 is 0 (exactly at :55), the loop fires it immediately.
            pendingTrackingStart = true;
            trackingStartAtMs = millis() + msUntil55;
            Serial.printf("[WWVB] Tracking scheduled in %lus (current second: %02d)\n",
                          msUntil55 / 1000UL, sec);
            return;
        }
    }

    // Normal mode — start with historically better antenna (toggles if it fails)
    uint8_t ctrl = (ant2Successes > ant1Successes) ? ES100_CTRL0_NORMAL_ANT2
                                                   : ES100_CTRL0_NORMAL;
    Serial.printf("[WWVB] Starting normal mode sync (prefer Ant%d, successes: Ant1=%d Ant2=%d)...\n",
                  (ant2Successes > ant1Successes) ? 2 : 1, ant1Successes, ant2Successes);
    es100InterruptFlag = false;
    if (es100.startReception(ctrl)) {
        es100Receiving = true;
        lastSyncAttempt = millis();
    } else {
        Serial.println("[WWVB] Failed to start normal reception");
        recordSyncFailure();
    }
}

void stopWWVBSync() {
    es100.stopReception();
    es100Receiving = false;
    Serial.println("WWVB sync stopped");
}

void handleES100Interrupt() {
    if (!es100InterruptFlag || !es100Receiving) return;

    es100InterruptFlag = false;
    unsigned long irqFiredAt         = es100IRQMillis;
    unsigned long irqProcessingDelay = millis() - irqFiredAt;
    // Clamp: defend against millis() wrap or a stale IRQMillis from a prior cycle
    if (irqProcessingDelay > 10000UL) irqProcessingDelay = 0UL;

    uint8_t irqStatus = es100.readIRQStatus();
    Serial.printf("ES100 IRQ Status: 0x%02X\n", irqStatus);
    
    if (irqStatus & ES100_IRQ_RX_COMPLETE) {
        uint8_t status0 = es100.readStatus0();

        if (status0 & ES100_STATUS_RX_OK) {
            bool usedTracking = (status0 & ES100_STATUS_TRACKING) != 0;
            Serial.printf("WWVB reception successful! (%s mode)\n",
                         usedTracking ? "tracking" : "normal");

            bool syncOk  = false;
            bool ant2Used = false;  // antenna used this reception (shared by both branches)

            if (usedTracking) {
                // Tracking mode: only register 0x09 (Second) is valid.
                // Year/Month/Day/Hour/Minute are cleared to 0x00 by the START write
                // and must NOT be used — doing so would corrupt the RTC with year 2000.
                // Instead, back-compute the correct Unix second from the IRQ timestamp.
                uint8_t wwvbSecond;
                if (es100.readTrackingResult(&wwvbSecond, &ant2Used, status0)) {
                    // Apply correction FIRST to minimise IRQ→setTime latency.
                    // Back-compute time at IRQ fire moment, then patch WWVB second.
                    uint32_t timeAtIRQ  = timeManager.getUnixTime()
                                         - (irqProcessingDelay / 1000UL);
                    uint32_t minuteBase = (timeAtIRQ / 60UL) * 60UL;
                    uint32_t corrected  = minuteBase + wwvbSecond;
                    // Resolve minute-boundary ambiguity: pick the minute closest to timeAtIRQ
                    int32_t delta = (int32_t)(corrected - timeAtIRQ);
                    if (delta >  30) corrected -= 60UL;
                    if (delta < -30) corrected += 60UL;

                    // Sanity check: corrected time should be 10–35 s after the :55 write.
                    // Guards against the clock being ~60s off (delta=0 passes ±30s check
                    // but lands in the wrong minute).
                    bool sanityOk = true;
                    if (trackingWriteUnixTime > 0) {
                        int32_t elapsed = (int32_t)(corrected - trackingWriteUnixTime);
                        if (elapsed < 10 || elapsed > 35) {
                            Serial.printf("[WWVB] Tracking sanity FAIL: elapsed=%ds from :55 write (expected 10-35) — rejected\n",
                                          elapsed);
                            sanityOk = false;
                        }
                    }

                    if (sanityOk) {
                        // corrected = second at IRQ fire.  Add whole-second delay so the
                        // clock lands at corrected + irqProcessingDelay, matching true time.
                        // Without this, a 3–4 s main-loop stall (NTP traffic, display) drops
                        // the whole-second part and leaves the clock 3–4 s behind.
                        uint32_t delaySeconds = irqProcessingDelay / 1000UL;
                        timeManager.setUnixTime(corrected + delaySeconds);
                        timeManager.setSubSecondOffset((uint16_t)(irqProcessingDelay % 1000));
                        syncOk = true;
                    }

                    // Log AFTER correction so timestamp in message is accurate
                    ClockTime cur = timeManager.getUTCTime();
                    Serial.printf("Tracking snap: IRQ delay=%lums, corrected to "
                                  "%04d-%02d-%02d %02d:%02d:%02d UTC, "
                                  "WWVB second=%02d (Ant%d)\n",
                                  irqProcessingDelay,
                                  cur.year, cur.month, cur.day,
                                  cur.hour, cur.minute, cur.second,
                                  wwvbSecond, ant2Used ? 2 : 1);
                } else {
                    Serial.println("Failed to read tracking result");
                }
            } else {
                // Normal 1-minute frame: all time registers are valid
                ES100Time rxTime;
                if (es100.readDateTime(&rxTime)) {
                    // Apply correction FIRST (before any Serial output) to minimise
                    // the gap between irqFiredAt and when the clock is actually set.
                    timeManager.setTime(rxTime.year, rxTime.month, rxTime.day,
                                       rxTime.hour, rxTime.minute, rxTime.second);
                    // Compensate for measured IRQ→processing delay.
                    // Use floor division so delaySeconds and setSubSecondOffset partition
                    // irqProcessingDelay without overlap.  The previous round-to-nearest
                    // formula caused a +1 s set-point error whenever irqProcessingDelay
                    // was ≥ 500 ms (e.g. main loop busy with NTP traffic): delaySeconds
                    // rounded up by 1, then setSubSecondOffset added the full fractional
                    // part again, applying 1000 + frac ms instead of just frac ms.
                    uint32_t delaySeconds = irqProcessingDelay / 1000UL;
                    if (delaySeconds > 0)
                        timeManager.setUnixTime(timeManager.getUnixTime() + delaySeconds);
                    // Sub-second accumulator: remainder after removing whole seconds.
                    timeManager.setSubSecondOffset((uint16_t)(irqProcessingDelay % 1000));

                    // DST only comes from normal mode (tracking does not provide it)
                    uint8_t dstBits = (status0 >> 5) & 0x03;
                    dstActive = (dstBits >= 2);

                    // Leap second warning (bits 3:4 of Status 0; normal mode only)
                    uint8_t lsw = (status0 & ES100_STATUS_LSW_MASK) >> 3;
                    if (lsw != wwvbLeapSecondWarning) {
                        wwvbLeapSecondWarning = lsw;
                        ntpServer.setLeapIndicator(lsw);  // propagate to NTP (RFC 5905)
                        if (lsw) Serial.printf("[WWVB] Leap second warning: %s\n",
                                               lsw == 1 ? "+1s end of month" : "-1s end of month");
                    }

                    ant2Used = rxTime.antenna2Used;
                    syncOk = true;

                    // Log AFTER correction so timestamp in message is accurate
                    Serial.printf("Normal sync: IRQ delay=%lums, set to %04d-%02d-%02d "
                                  "%02d:%02d:%02d UTC (Ant%d)\n",
                                  irqProcessingDelay,
                                  rxTime.year, rxTime.month, rxTime.day,
                                  rxTime.hour, rxTime.minute, rxTime.second,
                                  rxTime.antenna2Used ? 2 : 1);
                } else {
                    Serial.println("Failed to read time data");
                }
            }

            if (syncOk) {
                receptionHistory.recordAttempt(true);

                // Track per-antenna successes and log the event
                if (ant2Used) ant2Successes++; else ant1Successes++;
                addSyncLogEntry(true, usedTracking, ant2Used ? 2 : 1);

                // Save time to persistent storage
                saveTimeToPreferences();
                saveTimeToDS3231();  // Also update RTC

                // Track time source
                lastTimeSource     = TIME_SRC_WWVB;
                lastTimeSyncMillis = millis();
                lastWWVBSyncMillis = millis();
                if (!usedTracking) lastNormalSuccessMillis = millis();
                snapshotSyncTime();
                ntpServer.setStratum(1, "WWVB");
                ntpServer.setLastSyncTime(timeManager.getUnixTime());
                if (!wwvbLeapSecondWarning) ntpServer.setLeapIndicator(0);
                captivePortal.setTimeSource((uint8_t)lastTimeSource);

                // Enable tracking mode for subsequent syncs
                es100TrackingReady = true;
                es100TrackingReadySinceMs = millis();

                // Reset daytime failure tracking on any successful sync
                daytimeFailures = 0;
                daytimeSkipActive = false;

                Serial.println("Time synchronized!");
            } else {
                addSyncLogEntry(false, usedTracking, (status0 & ES100_STATUS_ANT) ? 2 : 1);
                recordSyncFailure(usedTracking);
            }
        } else {
            // Reception complete but decode failed
            if (es100UsingTracking) {
                // Tracking decode failed — poor signal conditions make normal mode (~134 s)
                // equally unlikely to succeed. Retry tracking on the next scheduled sync.
                // startWWVBSync() will fall back to normal mode after ES100_TRACKING_FALLBACK_MS.
                Serial.println("Tracking mode failed, will retry tracking on next scheduled sync");
                es100Receiving = false;
                es100UsingTracking = false;
                es100.stopReception();
                addSyncLogEntry(false, true, (status0 & ES100_STATUS_ANT) ? 2 : 1);
                recordSyncFailure(true);
                return;
            }
            Serial.println("RX_OK not set despite RX_COMPLETE");
            addSyncLogEntry(false, false, (status0 & ES100_STATUS_ANT) ? 2 : 1);
            recordSyncFailure();
        }

        es100Receiving = false;
        es100UsingTracking = false;
        es100.stopReception();

    } else if (irqStatus & ES100_IRQ_CYCLE_COMPLETE) {
        Serial.println("Reception cycle failed, ES100 retrying...");
    }
}

// ============================================================================
// Setup
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(2000);

    // Force serial output to flush
    Serial.flush();

    Serial.println("\n\n\n========================================");
    Serial.println("  WWVB Atomic Clock - ESP32-S3");
    Serial.println("  ES100 WWVB Receiver");
    Serial.println("  Display: LilyGo-AMOLED-Series");
    Serial.println("========================================");
    Serial.printf("Boot time: %lu ms\n", millis());
    Serial.println("========================================\n");

    // Configure task watchdog timer — auto-resets on I2C lockup or infinite loop
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdtConfig);
#else
    esp_task_wdt_init(WATCHDOG_TIMEOUT_MS / 1000, true);
#endif
    esp_task_wdt_add(NULL);
    Serial.printf("[WDT] Watchdog configured: %ds timeout\n", WATCHDOG_TIMEOUT_MS / 1000);

    Serial.println("[BOOT] Starting initialization...");
    Serial.printf("[BOOT] Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[BOOT] PSRAM: %s\n", psramFound() ? "Found" : "Not found");

    // Initialize I2C bus 0 (touch, PMU, DS3231) - 400kHz fast mode
    Serial.println("[BOOT] Initializing I2C...");
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);
    Serial.println("[BOOT] Wire initialized (400kHz) - touch, PMU, DS3231 on GPIO 2/3");

    // Initialize I2C bus 1 (ES100 isolated) - 100kHz standard mode
    Wire1.begin(ES100_SDA_PIN, ES100_SCL_PIN);
    Wire1.setClock(100000);
    Serial.println("[BOOT] Wire1 initialized (100kHz) - ES100 on GPIO 15/16 (isolated)");

    // I2C bus scan on both buses
    // Note: ES100 (0x32) will NOT appear here — it only responds when EN is HIGH,
    // and EN is not driven until initializeES100() is called later in the main loop.
    Serial.println("[BOOT] Scanning Wire for devices...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[BOOT] Wire found device at 0x%02X\n", addr);
        }
    }
    Serial.println("[BOOT] Scanning Wire1 for devices...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire1.beginTransmission(addr);
        if (Wire1.endTransmission() == 0) {
            Serial.printf("[BOOT] Wire1 found device at 0x%02X\n", addr);
        }
    }
    Serial.println("[BOOT] I2C scan complete");

    // Initialize DS3231 RTC
    Serial.println("[BOOT] Initializing DS3231 RTC...");
    initializeDS3231();
    readDS3231Temperature();  // Get initial temperature reading

    // Initialize display (amoled.begin() re-initializes Wire internally for touch/PMU)
    Serial.println("[BOOT] Initializing display...");
    initDisplay();
    Serial.println("[BOOT] Display initialization complete");
    Serial.flush();  // Ensure all display messages are sent

    // Re-apply Wire clock after amoled.begin() which may reset it
    Wire.setClock(400000);
    Serial.println("[BOOT] Wire clock re-applied (400kHz)");

    // Show startup message
    Serial.println("[BOOT] Drawing startup message...");
    sprite.fillSprite(TFT_BLACK);

    // Explicitly set font to built-in Font 4
    sprite.setFreeFont(NULL);  // Use built-in fonts
    sprite.setTextFont(4);     // Font 4 is a good size

    sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString("WWVB Clock", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 20);
    sprite.drawString("Starting...", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 + 20);
    Serial.println("[BOOT] Pushing startup message to display...");
    amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t*)sprite.getPointer());
    Serial.println("[BOOT] Startup message displayed");
    delay(2000);
    
    // Initialize ES100 with retry tracking
    lastES100InitAttempt = millis();

    // Clear sprite for ES100 status message
    sprite.fillSprite(TFT_BLACK);
    sprite.setFreeFont(NULL);
    sprite.setTextFont(4);
    sprite.setTextDatum(MC_DATUM);

    if (initializeES100()) {
        sprite.setTextColor(COLOR_SYNC_OK, TFT_BLACK);
        sprite.drawString("ES100: OK", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2);
    } else {
        sprite.setTextColor(COLOR_SYNC_PENDING, TFT_BLACK);
        sprite.drawString("ES100: Initializing...", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2);
        Serial.println("ES100 will retry initialization in background");
    }
    pushDisplay();
    delay(1000);

    // Show DS3231 status
    sprite.fillSprite(TFT_BLACK);
    sprite.setFreeFont(NULL);
    sprite.setTextFont(4);
    sprite.setTextDatum(MC_DATUM);

    if (rtcAvailable) {
        sprite.setTextColor(COLOR_SYNC_OK, TFT_BLACK);
        sprite.drawString("DS3231 RTC: OK", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2);
    } else {
        sprite.setTextColor(COLOR_SYNC_FAIL, TFT_BLACK);
        sprite.drawString("DS3231 RTC: Not Found", DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2);
    }
    pushDisplay();
    delay(1000);

    // Attach interrupt
    Serial.println("Attaching ES100 interrupt...");
    attachInterrupt(digitalPinToInterrupt(ES100_IRQ_PIN), es100ISR, FALLING);
    Serial.println("Interrupt attached");

    // Try to load time - priority: DS3231 > Preferences > Default
    Serial.println("Loading time...");
    bool timeLoaded = false;

    if (rtcAvailable) {
        Serial.println("Trying to load time from DS3231...");
        timeLoaded = loadTimeFromDS3231();
    }

    if (!timeLoaded) {
        Serial.println("Trying to load time from preferences...");
        timeLoaded = loadTimeFromPreferences();
    }

    if (!timeLoaded) {
        // No saved time, use default
        Serial.println("Using default time: 2025-01-01 00:00:00 UTC");
        timeManager.setTime(2025, 1, 1, 0, 0, 0);
    }
    Serial.println("Time initialization complete");

    // Compute DST from calendar (overrides persisted value when no WWVB)
    if (AUTO_DST_ENABLED) {
        bool prevDST = dstActive;
        dstActive = computeUSDST();
        Serial.printf("[DST] Auto-DST: %s (was %s)\n",
                     dstActive ? "active" : "inactive",
                     prevDST ? "active" : "inactive");
    }

    // Initialize reception history
    Serial.println("Initializing reception history...");
    receptionHistory.begin();
    Serial.println("Reception history initialized");

    delay(2000);

    // Disable touch auto-sleep so touch stays responsive
    amoled.disableAutoSleep();
    Serial.println("[BOOT] Touch screen enabled");

    // Try auto-connect WiFi with saved credentials
    if (wifiLoadCredentials()) {
        Serial.printf("[BOOT] Saved WiFi: %s — connecting...\n", wifiSSID.c_str());
        wifiConnect(wifiSSID, wifiPassword);
    }

    // Start main display
    Serial.println("Calling updateDisplay() for first time...");
    updateDisplay();
    Serial.println("updateDisplay() completed");

    // Start first sync attempt
    Serial.println("Starting initial WWVB sync...");
    startWWVBSync();

    Serial.println("Setup complete! Entering main loop...");
}

// ============================================================================
// Main Loop
// ============================================================================
void loop() {
    esp_task_wdt_reset();  // Feed watchdog — resets if loop hangs >15s

    static bool firstLoop = true;
    if (firstLoop) {
        Serial.println("*** ENTERED MAIN LOOP ***");
        firstLoop = false;
    }

    // Handle touch input (high frequency for responsive gestures)
    handleTouch();

    // WiFi state machine and services
    wifiLoop();
    if (ntpServer.isRunning()) ntpServer.handleClient();
    if (statusServer.isRunning()) statusServer.handleClient();
    if (captivePortal.isRunning()) captivePortal.handleClient();

    // Force display refresh during shutdown countdown for smooth progress bar
    if (shutdownCountdownActive) {
        updateDisplay();
    }

    // Retry ES100 initialization if needed
    if (retryES100Initialization()) {
        updateDisplay();  // Update display to show ES100 is now available
    }

    processDS3231SquareWave();

    // Handle ES100 interrupts
    if (es100InterruptFlag) {
        handleES100Interrupt();
    }

    // Update display every second
    if (millis() - lastDisplayUpdate >= 1000) {
        lastDisplayUpdate = millis();

        timeManager.tick();
        syncFromDS3231();  // Read DS3231 as authoritative time source

        // Degrade NTP stratum to 16 if too long without any sync (RFC 5905 §7.3).
        // LI=3 (alarm) is required by RFC 5905 when the server is unsynchronized.
        if (timeManager.isTimeSet() &&
            timeManager.getSecondsSinceSync() > NTP_UNSYNC_STRATUM_THRESHOLD_S) {
            if (ntpServer.isRunning()) {
                ntpServer.setStratum(16, "LOCL");
                ntpServer.setLeapIndicator(3);  // RFC 5905: LI=3 when unsynchronized
            }
        }

        // Read DS3231 temperature every 64 seconds (matches sensor update rate)
        if (millis() - lastTempRead >= 64000) {
            readDS3231Temperature();
            lastTempRead = millis();
        }
        // Update status data for web server
        statusData.utcOffset = utcOffset;
        statusData.dstActive = dstActive;
        statusData.temperatureC = rtcTemperature;
        statusData.batteryMv = amoled.getBattVoltage();
        statusData.batteryPct = lipoBatteryPercent(statusData.batteryMv);
        statusData.batteryCharging = amoled.isCharging();
        statusData.timeSource = (uint8_t)lastTimeSource;
        statusData.lastSyncMillis = lastTimeSyncMillis;
        statusData.es100Available        = es100Available;
        statusData.es100Receiving        = es100Receiving;
        statusData.es100Tracking         = es100UsingTracking;
        statusData.es100PendingTracking  = pendingTrackingStart;
        statusData.es100TrackingReady    = es100TrackingReady;
        statusData.leapSecondWarning     = wwvbLeapSecondWarning;
        statusData.ant1Successes         = ant1Successes;
        statusData.ant2Successes         = ant2Successes;

        // Critical battery — force deep sleep after 3 consecutive readings to protect NVS
        if (statusData.batteryPct <= CRITICAL_BATTERY_THRESHOLD &&
            !statusData.batteryCharging && statusData.batteryMv > 0) {
            critBattCount++;
            if (critBattCount >= 3) {
                Serial.printf("[BATT] CRITICAL %d%% (%.2fV) — forcing deep sleep to protect NVS\n",
                             statusData.batteryPct, statusData.batteryMv / 1000.0);
                performShutdown();
            }
        } else {
            critBattCount = 0;
        }

        // Low battery alert at threshold
        if (statusData.batteryPct <= LOW_BATTERY_THRESHOLD &&
            !statusData.batteryCharging && statusData.batteryMv > 0) {
            if (!lowBatteryAlerted) {
                lowBatteryAlerted = true;
                Serial.printf("[BATT] LOW BATTERY ALERT: %d%% (%.2fV)\n",
                             statusData.batteryPct, statusData.batteryMv / 1000.0);
            }
            // Periodic reminder every 60 seconds
            if (millis() - lastLowBattWarn >= LOW_BATTERY_WARN_MS) {
                lastLowBattWarn = millis();
                Serial.printf("[BATT] Warning: %d%% (%.2fV) — connect charger\n",
                             statusData.batteryPct, statusData.batteryMv / 1000.0);
            }
        } else if (statusData.batteryPct > LOW_BATTERY_THRESHOLD + 5) {
            // Clear alert with 5% hysteresis to prevent flapping
            lowBatteryAlerted = false;
        }

        // Auto-DST check every 60 seconds (catches transitions without WWVB)
        if (AUTO_DST_ENABLED && millis() - lastDSTCheck >= 60000) {
            lastDSTCheck = millis();
            bool newDST = computeUSDST();
            if (newDST != dstActive) {
                dstActive = newDST;
                Serial.printf("[DST] Transition detected: DST now %s\n",
                             dstActive ? "active" : "inactive");
            }
        }

        updateDisplay();
        receptionHistory.hourlyTick();
    }
    
    // Pending tracking start: fire Control 0 write at second :55
    // (tracking reception requires the write to happen at exactly :55 ± drift tolerance)
    if (pendingTrackingStart && !es100Receiving) {
        unsigned long now = millis();

        // Abort if we've waited longer than the maximum allowed pending time
        if (now > trackingStartAtMs + TRACKING_PENDING_TIMEOUT_MS) {
            Serial.println("[WWVB] Pending tracking start timed out — aborting");
            pendingTrackingStart = false;
            if (es100.isPoweredOn()) es100.powerOff();
            es100UsingTracking = false;
            recordSyncFailure(true);
        }
        // Power on ES100 ~50ms before the :55 boundary to satisfy wakeup time
        else if (!es100.isPoweredOn() && now >= trackingStartAtMs - (ES100_WAKEUP_TIME_MS + 30UL)) {
            es100.powerOn();
        }
        // Write Control 0 at the :55 boundary
        else if (now >= trackingStartAtMs) {
            pendingTrackingStart = false;
            uint8_t trkCtrl = (ant2Successes > ant1Successes) ? ES100_CTRL0_TRACK_ANT2
                                                              : ES100_CTRL0_TRACK_ANT1;
            if (es100.startReception(trkCtrl)) {
                es100Receiving = true;
                lastSyncAttempt = millis();  // Reset timeout clock from actual start, not schedule time
                trackingWriteUnixTime = timeManager.getUnixTime();  // Anchor for sanity check
                Serial.printf("[WWVB] Tracking mode sync started at :55 (Ant%d, successes: Ant1=%d Ant2=%d)\n",
                              (ant2Successes > ant1Successes) ? 2 : 1, ant1Successes, ant2Successes);
            } else {
                Serial.println("[WWVB] Failed to start tracking reception at :55");
                if (es100.isPoweredOn()) es100.powerOff();
                es100UsingTracking = false;
                recordSyncFailure(true);
            }
        }
    }

    // Periodic sync attempts — time-aware schedule with daytime backoff
    if (!es100Receiving && !pendingTrackingStart) {
        unsigned long timeSinceLastAttempt = millis() - lastSyncAttempt;
        unsigned long interval = getSyncInterval();

        // Skip daytime attempts entirely after too many consecutive failures
        bool skip = daytimeSkipActive && !isNighttimeWindow();

        if (!skip && timeSinceLastAttempt >= interval) {
            startWWVBSync();
        }
    }

    // Timeout for reception attempts
    unsigned long rxTimeout = es100UsingTracking ? SYNC_TIMEOUT_TRACKING_MS : SYNC_TIMEOUT_NORMAL_MS;
    if (es100Receiving && (millis() - lastSyncAttempt > rxTimeout)) {
        if (es100UsingTracking) {
            // Tracking timed out — poor signal; normal mode would also fail.
            // Retry tracking on the next scheduled sync.
            // startWWVBSync() falls back to normal mode after ES100_TRACKING_FALLBACK_MS.
            Serial.println("Tracking mode timeout, will retry tracking on next scheduled sync");
            stopWWVBSync();
            es100UsingTracking = false;
            recordSyncFailure(true);
        } else {
            Serial.println("Reception timeout - stopping");
            stopWWVBSync();
            recordSyncFailure();
        }
    }
    
    delay(10);
}
