/**
 * @file      NTPServer.cpp
 * @brief     Stratum 1 NTP Server Implementation
 * @details   RFC 5905 compliant NTP server responding on UDP port 123.
 *            Provides WWVB-synced UTC time as a Stratum 1 source.
 */

#include "NTPServer.h"

// NTP packet is always 48 bytes
#define NTP_PACKET_SIZE 48

NTPServer::NTPServer()
    : _timeManager(nullptr), _running(false), _requestCount(0), _stratum(1) {
    memcpy(_refId, "WWVB", 4);
}

bool NTPServer::begin(TimeManager* tm) {
    if (!tm) {
        Serial.println("[NTP] Error: null TimeManager");
        return false;
    }

    _timeManager = tm;

    if (_udp.begin(NTP_PORT)) {
        _running = true;
        _requestCount = 0;
        Serial.printf("[NTP] Server started on UDP port %d\n", NTP_PORT);
        return true;
    }

    Serial.println("[NTP] Failed to bind UDP port");
    return false;
}

void NTPServer::stop() {
    if (_running) {
        _udp.stop();
        _running = false;
        Serial.println("[NTP] Server stopped");
    }
}

void NTPServer::handleClient() {
    if (!_running) return;

    int packetSize = _udp.parsePacket();
    if (packetSize == 0) return;  // No packet available

    // Log every incoming packet for diagnostics
    IPAddress remoteIP = _udp.remoteIP();
    uint16_t remotePort = _udp.remotePort();

    if (packetSize < NTP_PACKET_SIZE) {
        Serial.printf("[NTP] Undersized packet (%d bytes) from %s:%d — ignored\n",
                     packetSize, remoteIP.toString().c_str(), remotePort);
        // Flush the undersized packet so it doesn't block the buffer
        uint8_t discard[NTP_PACKET_SIZE];
        _udp.read(discard, packetSize);
        return;
    }

    // Read incoming NTP request
    uint8_t request[NTP_PACKET_SIZE];
    _udp.read(request, NTP_PACKET_SIZE);

    uint8_t clientVN = (request[0] >> 3) & 0x07;
    uint8_t clientMode = request[0] & 0x07;

    // Don't respond if time hasn't been set — would serve year-2000 timestamps
    if (!_timeManager->isTimeSet()) {
        Serial.printf("[NTP] Ignoring request from %s:%d — time not set\n",
                     remoteIP.toString().c_str(), remotePort);
        return;
    }

    // Verify NTP timestamp is reasonable (after year 2020 = NTP 3786825600)
    uint32_t ntpNow = unixToNTP(_timeManager->getUnixTime());
    if (ntpNow < 3786825600UL) {
        Serial.printf("[NTP] Ignoring request — NTP timestamp %lu is before 2020\n",
                     (unsigned long)ntpNow);
        return;
    }

    // Build response
    uint8_t response[NTP_PACKET_SIZE];
    buildResponse(request, response);

    // Send response back to the client
    _udp.beginPacket(remoteIP, remotePort);
    _udp.write(response, NTP_PACKET_SIZE);
    int sent = _udp.endPacket();

    _requestCount++;

    // Log every request for debugging (can reduce later once working)
    Serial.printf("[NTP] #%lu %s:%d v%d mode%d → stratum %d, NTP-ts %lu, send=%s\n",
                 (unsigned long)_requestCount,
                 remoteIP.toString().c_str(), remotePort,
                 clientVN, clientMode,
                 _stratum, (unsigned long)ntpNow,
                 sent ? "OK" : "FAIL");

    // Hex dump of response for first request (helps diagnose Windows issues)
    if (_requestCount == 1) {
        Serial.print("[NTP] Response hex: ");
        for (int i = 0; i < NTP_PACKET_SIZE; i++) {
            Serial.printf("%02X", response[i]);
            if (i % 4 == 3) Serial.print(" ");
        }
        Serial.println();
    }
}

bool NTPServer::isRunning() const {
    return _running;
}

uint32_t NTPServer::getRequestCount() const {
    return _requestCount;
}

void NTPServer::buildResponse(const uint8_t* request, uint8_t* response) {
    memset(response, 0, NTP_PACKET_SIZE);

    // Get current time as NTP timestamp
    uint32_t ntpNow = unixToNTP(_timeManager->getUnixTime());

    // Sub-second fraction: convert milliseconds (0-999) to NTP fraction (0-2^32)
    // fraction ≈ ms * 4294967 (approximates ms/1000 * 2^32)
    uint16_t ms = _timeManager->getMilliseconds();
    uint32_t ntpFraction = (uint32_t)ms * 4294967UL;

    // Byte 0: LI (2 bits) + VN (3 bits) + Mode (3 bits)
    // Echo client's version number per RFC 5905 — Windows sends v3, others may send v4
    uint8_t clientVersion = (request[0] >> 3) & 0x07;
    if (clientVersion < 3) clientVersion = 3;  // Floor at NTPv3
    response[0] = (clientVersion << 3) | 0x04;  // LI=0 + client's VN + Mode=4 (server)

    // Byte 1: Stratum (1=primary/WWVB, 2=NTP-synced)
    response[1] = _stratum;

    // Byte 2: Poll interval — echo client's requested poll interval
    response[2] = request[2];

    // Byte 3: Precision = -10 (2^-10 ~ 1 millisecond, matches millis()-based timekeeping)
    response[3] = 0xF6;

    // Bytes 4-7: Root Delay = 0 (primary reference clock)
    // Already zeroed by memset

    // Bytes 8-11: Root Dispersion (NTP fixed-point 16.16)
    // ~15.26ms = 0x000003E8 — reasonable for millis()-based clock with WWVB sync
    writeUint32(&response[8], 0x000003E8);

    // Bytes 12-15: Reference ID
    memcpy(&response[12], _refId, 4);

    // Bytes 16-23: Reference Timestamp (last time clock was synced)
    // Use current time as approximation (fraction=0, sync precision is 1s)
    writeUint32(&response[16], ntpNow);  // Seconds
    writeUint32(&response[20], 0);        // Fraction

    // Bytes 24-31: Origin Timestamp = client's Transmit Timestamp
    // Copy bytes 40-47 from the request
    memcpy(&response[24], &request[40], 8);

    // Bytes 32-39: Receive Timestamp (when we received the request)
    writeUint32(&response[32], ntpNow);      // Seconds
    writeUint32(&response[36], ntpFraction);  // Fraction (~1ms resolution)

    // Bytes 40-47: Transmit Timestamp (when we send the response)
    // Re-sample milliseconds for slightly more accurate transmit time
    uint32_t txFraction = (uint32_t)_timeManager->getMilliseconds() * 4294967UL;
    writeUint32(&response[40], ntpNow);      // Seconds
    writeUint32(&response[44], txFraction);   // Fraction (~1ms resolution)
}

uint32_t NTPServer::unixToNTP(uint32_t unixTime) {
    return unixTime + NTP_EPOCH_OFFSET;
}

void NTPServer::writeUint32(uint8_t* buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

void NTPServer::setStratum(uint8_t stratum, const char* refId) {
    _stratum = stratum;
    memset(_refId, 0, 4);
    if (refId) {
        size_t len = strlen(refId);
        if (len > 4) len = 4;
        memcpy(_refId, refId, len);
    }
}

void NTPServer::setStratum(uint8_t stratum, IPAddress refIP) {
    _stratum = stratum;
    // RFC 5905: For stratum 2+, reference ID = IPv4 address of upstream server
    _refId[0] = refIP[0];
    _refId[1] = refIP[1];
    _refId[2] = refIP[2];
    _refId[3] = refIP[3];
}
