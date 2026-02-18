/**
 * @file      CaptivePortal.h
 * @brief     Captive Portal for WiFi credential entry via phone/browser
 * @details   Starts an open AP, DNS server (redirecting all domains to portal),
 *            and HTTP server with a simple form for entering WiFi credentials.
 */

#ifndef CAPTIVEPORTAL_H
#define CAPTIVEPORTAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <functional>
#include "config.h"
#include "TimeManager.h"

class CaptivePortal {
public:
    CaptivePortal();

    /**
     * @brief Start the captive portal (AP + DNS + HTTP)
     * @return true if AP started successfully
     */
    bool begin();

    /**
     * @brief Stop the captive portal and close AP
     */
    void stop();

    /**
     * @brief Process HTTP and DNS requests (call from loop)
     */
    void handleClient();

    /**
     * @brief Update the network list shown in the HTML form
     * @param htmlOptions HTML <option> tags for each scanned network
     */
    void setNetworkList(const String& htmlOptions);

    /**
     * @brief Set callback for when credentials are submitted
     * @param cb Callback receiving (ssid, password)
     */
    void setOnCredentials(std::function<void(const String&, const String&)> cb);

    /**
     * @brief Set the connection status message shown on the portal
     */
    void setStatus(const String& status);

    /**
     * @brief Set time manager for clock display on portal
     */
    void setTimeManager(TimeManager* tm);

    /**
     * @brief Check if portal is running
     */
    bool isRunning() const;

private:
    WebServer _httpServer;
    DNSServer _dnsServer;
    bool _running;
    String _networkOptions;
    String _statusMessage;
    TimeManager* _timeManager;
    std::function<void(const String&, const String&)> _onCredentials;

    void handleRoot();
    void handleConnect();
    void handleStatus();
    void handleTime();
    void handleNotFound();
    String buildPage();
};

#endif // CAPTIVEPORTAL_H
