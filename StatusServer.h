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

    void handleRoot();
    void handleApiStatus();
    void handleNotFound();
    String buildPage();
    const char* timeSourceName(uint8_t src);
};

#endif // STATUSSERVER_H
