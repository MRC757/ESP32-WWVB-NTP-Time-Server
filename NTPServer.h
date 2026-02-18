/**
 * @file      NTPServer.h
 * @brief     Stratum 1 NTP Server for WWVB-synced ESP32
 * @details   Listens on UDP port 123 and responds to NTP client requests
 *            with the current UTC time from TimeManager (WWVB-synced).
 *            Reference ID: "WWVB" (Stratum 1 primary reference clock)
 */

#ifndef NTPSERVER_H
#define NTPSERVER_H

#include <Arduino.h>
#include <WiFiUdp.h>
#include "TimeManager.h"
#include "config.h"

class NTPServer {
public:
    NTPServer();

    /**
     * @brief Start the NTP server on UDP port 123
     * @param tm Pointer to the TimeManager providing UTC time
     * @return true if server started successfully
     */
    bool begin(TimeManager* tm);

    /**
     * @brief Stop the NTP server
     */
    void stop();

    /**
     * @brief Process incoming NTP requests (call from loop)
     */
    void handleClient();

    /**
     * @brief Check if the server is running
     */
    bool isRunning() const;

    /**
     * @brief Get total number of NTP requests served
     */
    uint32_t getRequestCount() const;

    /**
     * @brief Set stratum level and reference ID (ASCII for Stratum 1)
     * @param stratum 1=WWVB primary
     * @param refId 4-char reference ID (e.g., "WWVB")
     */
    void setStratum(uint8_t stratum, const char* refId);

    /**
     * @brief Set stratum level with upstream server IP as reference ID
     * @param stratum 2+ = NTP-synced
     * @param refIP IPv4 address of upstream NTP server (stored as 4 binary bytes per RFC 5905)
     */
    void setStratum(uint8_t stratum, IPAddress refIP);

private:
    WiFiUDP _udp;
    TimeManager* _timeManager;
    bool _running;
    uint32_t _requestCount;
    uint8_t _stratum;
    char _refId[4];

    /**
     * @brief Build a 48-byte NTP response packet
     * @param request The incoming request packet (48 bytes)
     * @param response Output buffer for the response (48 bytes)
     */
    void buildResponse(const uint8_t* request, uint8_t* response);

    /**
     * @brief Convert Unix timestamp to NTP timestamp (seconds since 1900-01-01)
     */
    uint32_t unixToNTP(uint32_t unixTime);

    /**
     * @brief Write a 32-bit value in network byte order to a buffer
     */
    void writeUint32(uint8_t* buf, uint32_t val);
};

#endif // NTPSERVER_H
