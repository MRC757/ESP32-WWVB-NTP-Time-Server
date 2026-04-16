/**
 * @file      StatusServer.h
 * @brief     Status web server for monitoring WWVB clock via browser
 * @details   Runs on port 80 in STA mode (when connected to WiFi).
 *            Serves a live dashboard showing time, temperature, battery,
 *            NTP stats, and sync information.
 */

#ifndef STATUSSERVER_H
#define STATUSSERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <functional>
#include "config.h"
#include "TimeManager.h"
#include "NTPServer.h"
#include "ReceptionHistory.h"

/**
 * @brief Data snapshot for the status web page.
 *        Populated by the main sketch each loop iteration.
 */
struct StatusData {
    int8_t   utcOffset;          // Current UTC offset (hours)
    bool     dstActive;          // DST currently applied
    float    temperatureC;       // DS3231 temperature (Celsius)
    uint16_t batteryMv;          // Battery voltage in millivolts
    uint8_t  batteryPct;         // Battery percentage (LiPo curve, 0-100)
    bool     batteryCharging;    // True if USB charging
    uint8_t  timeSource;         // 0=None, 1=RTC, 2=NTP, 3=WWVB
    unsigned long lastSyncMillis; // millis() of last time sync (0 = never)
    bool     es100Available;        // ES100 hardware initialized
    bool     es100Receiving;        // ES100 currently receiving WWVB signal
    bool     es100Tracking;         // Current receive attempt is tracking mode (vs normal)
    bool     es100PendingTracking;  // Waiting to write Control 0 at second :55
    bool     es100TrackingReady;    // At least one normal-mode sync done; tracking mode available
    char     lastSyncTimeStr[20];   // "YYYY-MM-DD HH:MM:SS" UTC of last sync, "" if never
    uint8_t  leapSecondWarning;     // 0=none, 1=positive(+1s), 2=negative(-1s)
    uint16_t ant1Successes;         // Lifetime Antenna 1 sync successes
    uint16_t ant2Successes;         // Lifetime Antenna 2 sync successes
};

// Maximum number of sync log entries kept in memory
#define SYNC_LOG_SIZE 20

/**
 * @brief One entry in the recent sync log ring buffer.
 */
struct SyncLogEntry {
    char    timeStr[20];  // "YYYY-MM-DD HH:MM:SS" UTC; "" = empty slot
    bool    success;
    bool    tracking;     // true=tracking mode, false=normal mode
    uint8_t antenna;      // 1 or 2; 0=unknown (timeout path)
};

class StatusServer {
public:
    StatusServer();

    /**
     * @brief Start the status web server on port 80
     * @return true if server started successfully
     */
    bool begin();

    /**
     * @brief Stop the status web server
     */
    void stop();

    /**
     * @brief Process HTTP requests (call from loop)
     */
    void handleClient();

    /**
     * @brief Set time manager for UTC/local time display
     */
    void setTimeManager(TimeManager* tm);

    /**
     * @brief Set NTP server for request count display
     */
    void setNTPServer(NTPServer* ntp);

    /**
     * @brief Set pointer to status data struct (updated by caller)
     */
    void setStatusData(StatusData* data);

    /**
     * @brief Set reception history for WWVB sync chart
     */
    void setReceptionHistory(ReceptionHistory* rh);

    /**
     * @brief Set callback invoked when the browser requests a normal-mode WWVB sync
     */
    void setOnSyncRequest(std::function<void()> cb);

    /**
     * @brief Set callback invoked when the browser requests a tracking-mode WWVB sync
     */
    void setOnTrackingRequest(std::function<void()> cb);

    /**
     * @brief Set callback invoked when the browser submits new timezone settings
     * @param cb Receives (utcOffset, dstActive)
     */
    void setOnSettingsRequest(std::function<void(int8_t, bool)> cb);

    /**
     * @brief Provide the sync log ring buffer for the /api/log endpoint
     * @param log     Pointer to SyncLogEntry array of size SYNC_LOG_SIZE
     * @param head    Pointer to next-write index (0..SYNC_LOG_SIZE-1)
     * @param filled  Pointer to count of valid entries (0..SYNC_LOG_SIZE)
     */
    void setSyncLog(const SyncLogEntry* log, const uint8_t* head, const uint8_t* filled);

    /**
     * @brief Check if server is running
     */
    bool isRunning() const;

private:
    WebServer _httpServer;
    bool _running;
    TimeManager* _timeManager;
    NTPServer* _ntpServer;
    StatusData* _statusData;
    ReceptionHistory* _receptionHistory;

    std::function<void()> _onSyncRequest;
    std::function<void()> _onTrackingRequest;
    std::function<void(int8_t, bool)> _onSettingsRequest;

    const SyncLogEntry* _syncLog       = nullptr;
    const uint8_t*      _syncLogHead   = nullptr;
    const uint8_t*      _syncLogFilled = nullptr;

    void handleRoot();
    void handleApiStatus();
    void handleApiSync();
    void handleApiTrackingSync();
    void handleApiSettings();
    void handleApiLog();
    void handleNotFound();
    bool checkEs100Ready(bool checkPending);
    String buildPage();
    const char* timeSourceName(uint8_t src);
};

#endif // STATUSSERVER_H
