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
    : _timeManager(nullptr), _running(false), _requestCount(0),
      _stratum(1), _leapIndicator(0), _lastSyncUnixTime(0) {
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

    // RFC 5905: only respond to client (mode 3) or symmetric-active (mode 1) requests.
    // Ignore server probes (4), broadcast (5), control (6), and private (7) messages.
    if (clientMode != 3 && clientMode != 1) {
        Serial.printf("[NTP] Ignoring non-client mode %d from %s:%d\n",
                     clientMode, remoteIP.toString().c_str(), remotePort);
        return;
    }

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

    // Atomically sample seconds + milliseconds — avoids second-boundary race where
    // getUnixTime() and getMilliseconds() could straddle a tick() call.
    uint32_t unixNow;
    uint16_t ms;
    _timeManager->getTimeSnapshot(unixNow, ms);
    uint32_t ntpNow      = unixToNTP(unixNow);
    uint32_t ntpFraction = (uint32_t)ms * 4294967UL;

    // Byte 0: LI (2 bits) + VN (3 bits) + Mode (3 bits)
    // LI reflects the leap-second warning decoded from the WWVB frame (RFC 5905).
    uint8_t clientVersion = (request[0] >> 3) & 0x07;
    if (clientVersion < 3) clientVersion = 3;  // Floor at NTPv3
    response[0] = (_leapIndicator << 6) | (clientVersion << 3) | 0x04;

    // Byte 1: Stratum (1=primary/WWVB, 2=NTP-synced)
    response[1] = _stratum;

    // Byte 2: Poll interval — echo client's requested poll interval
    response[2] = request[2];

    // Byte 3: Precision = -10 (2^-10 ~ 1 millisecond, matches millis()-based timekeeping)
    response[3] = 0xF6;

    // Bytes 4-7: Root Delay = 0 (primary reference clock)
    // Already zeroed by memset

    // Bytes 8-11: Root Dispersion — grows at DS3231 drift rate since last sync.
    // NTP fixed-point 16.16: integer part in upper 16 bits, fraction in lower 16 bits.
    uint32_t dispersion = NTP_MIN_DISPERSION;
    if (_lastSyncUnixTime > 0 && unixNow > _lastSyncUnixTime) {
        uint32_t ageSec = unixNow - _lastSyncUnixTime;
        // drift = ageSec * ppm / 1,000,000  →  fixed-point 16.16
        uint32_t driftFixed = (uint32_t)((uint64_t)ageSec * NTP_DS3231_DRIFT_PPM
                                         * 65536ULL / 1000000ULL);
        dispersion = NTP_MIN_DISPERSION + driftFixed;
        if (dispersion > NTP_MAX_DISPERSION) dispersion = NTP_MAX_DISPERSION;
    }
    writeUint32(&response[8], dispersion);

    // Bytes 12-15: Reference ID
    memcpy(&response[12], _refId, 4);

    // Bytes 16-23: Reference Timestamp — time of most recent clock sync (RFC 5905 §7.3)
    uint32_t refNTP = (_lastSyncUnixTime > 0)
                      ? unixToNTP(_lastSyncUnixTime)
                      : ntpNow;  // Fallback to now if sync time not yet recorded
    writeUint32(&response[16], refNTP);
    writeUint32(&response[20], 0);        // Fraction: 1-second sync precision

    // Bytes 24-31: Origin Timestamp = client's Transmit Timestamp
    // Copy bytes 40-47 from the request
    memcpy(&response[24], &request[40], 8);

    // Bytes 32-39: Receive Timestamp — re-sample atomically before writing.
    // Must NOT reuse the stale unixNow/ntpFraction from the beginning of this function:
    // if processing above takes >1s, the timestamp becomes stale and NTP clients will
    // compensate by advancing their clocks by the processing delay, causing errors of 1-3s.
    uint32_t rxUnix;
    uint16_t rxMs;
    _timeManager->getTimeSnapshot(rxUnix, rxMs);
    writeUint32(&response[32], unixToNTP(rxUnix));
    writeUint32(&response[36], (uint32_t)rxMs * 4294967UL);

    // Bytes 40-47: Transmit Timestamp — re-sample atomically at send time.
    // Must NOT reuse ntpNow (receive-time integer second): if a second boundary
    // passes between T2 and T3, the fraction wraps to a small value while the
    // integer second stays stale, producing a T3 that is ~1 s behind reality.
    uint32_t txUnix;
    uint16_t txMs;
    _timeManager->getTimeSnapshot(txUnix, txMs);
    writeUint32(&response[40], unixToNTP(txUnix));
    writeUint32(&response[44], (uint32_t)txMs * 4294967UL);
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

void NTPServer::setLastSyncTime(uint32_t unixTime) {
    _lastSyncUnixTime = unixTime;
}

void NTPServer::setLeapIndicator(uint8_t li) {
    _leapIndicator = li & 0x03;
}
