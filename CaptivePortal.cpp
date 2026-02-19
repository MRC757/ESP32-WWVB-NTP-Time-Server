/**
 * @file      CaptivePortal.cpp
 * @brief     Captive Portal Implementation
 * @details   Open AP "WWVB-Clock-Setup" with DNS redirect and HTTP form.
 */

#include "CaptivePortal.h"

static const byte DNS_PORT = 53;

CaptivePortal::CaptivePortal()
    : _httpServer(80), _running(false), _statusMessage("Not connected"), _timeManager(nullptr) {
}

bool CaptivePortal::begin() {
    // AP must already be started by the caller via WiFi.softAP()
    // We only start DNS + HTTP servers here
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[PORTAL] Starting servers on AP IP: %s\n", apIP.toString().c_str());

    // Start DNS server — redirect all domains to our AP IP
    _dnsServer.start(DNS_PORT, "*", apIP);

    // Configure HTTP routes
    _httpServer.on("/", HTTP_GET, [this]() { handleRoot(); });
    _httpServer.on("/connect", HTTP_POST, [this]() { handleConnect(); });
    _httpServer.on("/status", HTTP_GET, [this]() { handleStatus(); });
    _httpServer.on("/time", HTTP_GET, [this]() { handleTime(); });
    _httpServer.onNotFound([this]() { handleNotFound(); });

    _httpServer.begin();
    _running = true;

    Serial.println("[PORTAL] HTTP server started on port 80");
    return true;
}

void CaptivePortal::stop() {
    if (!_running) return;

    _httpServer.stop();
    _dnsServer.stop();
    // Do NOT call WiFi.softAPdisconnect() here — the caller manages WiFi state.
    // Calling it with 'true' deinitializes WiFi entirely, corrupting the driver
    // when transitioning AP → STA on ESP32-S3.
    _running = false;

    Serial.println("[PORTAL] Stopped");
}

void CaptivePortal::handleClient() {
    if (!_running) return;
    _dnsServer.processNextRequest();
    _httpServer.handleClient();
}

void CaptivePortal::setNetworkList(const String& htmlOptions) {
    _networkOptions = htmlOptions;
}

void CaptivePortal::setOnCredentials(std::function<void(const String&, const String&)> cb) {
    _onCredentials = cb;
}

void CaptivePortal::setTimeManager(TimeManager* tm) {
    _timeManager = tm;
}

void CaptivePortal::setStatus(const String& status) {
    _statusMessage = status;
}

bool CaptivePortal::isRunning() const {
    return _running;
}

void CaptivePortal::handleRoot() {
    _httpServer.send(200, "text/html", buildPage());
}

void CaptivePortal::handleConnect() {
    String ssid = _httpServer.arg("ssid");
    String password = _httpServer.arg("password");

    if (ssid.length() == 0) {
        _httpServer.send(400, "text/html", "<html><body><h2>SSID required</h2><a href='/'>Back</a></body></html>");
        return;
    }

    Serial.printf("[PORTAL] Credentials received: SSID=%s\n", ssid.c_str());

    _statusMessage = "Connecting to " + ssid + "...";

    String response = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    response += "<style>body{font-family:sans-serif;text-align:center;padding:40px;background:#1a1a2e;color:#e0e0e0;}</style>";
    response += "<meta http-equiv='refresh' content='5;url=/status'></head><body>";
    response += "<h2>Connecting to " + ssid + "...</h2>";
    response += "<p>Please wait. This page will update automatically.</p>";
    response += "</body></html>";
    _httpServer.send(200, "text/html", response);

    if (_onCredentials) {
        _onCredentials(ssid, password);
    }
}

void CaptivePortal::handleStatus() {
    String json = "{\"status\":\"" + _statusMessage + "\"}";
    _httpServer.send(200, "application/json", json);
}

void CaptivePortal::handleTime() {
    if (_timeManager) {
        ClockTime t = _timeManager->getUTCTime();
        char buf[64];
        snprintf(buf, sizeof(buf),
            "{\"h\":%d,\"m\":%d,\"s\":%d,\"Y\":%d,\"M\":%d,\"D\":%d}",
            t.hour, t.minute, t.second, t.year, t.month, t.day);
        _httpServer.send(200, "application/json", buf);
    } else {
        _httpServer.send(200, "application/json", "{\"error\":\"no time source\"}");
    }
}

void CaptivePortal::handleNotFound() {
    // Redirect all unknown URLs to the root (captive portal behavior)
    _httpServer.sendHeader("Location", "http://192.168.4.1/", true);
    _httpServer.send(302, "text/plain", "");
}

String CaptivePortal::buildPage() {
    // Get current time for initial display
    String timeStr = "--:--:--";
    String dateStr = "----/--/--";
    if (_timeManager) {
        ClockTime t = _timeManager->getUTCTime();
        char buf[12];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
        timeStr = buf;
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d", t.year, t.month, t.day);
        dateStr = buf;
    }

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>WWVB Clock WiFi Setup</title>";
    html += "<style>";
    html += "body{font-family:sans-serif;max-width:400px;margin:20px auto;padding:15px;background:#1a1a2e;color:#e0e0e0;}";
    html += "h1{color:#00d4ff;font-size:22px;text-align:center;margin-bottom:4px;}";
    html += "h2{color:#aaa;font-size:14px;text-align:center;font-weight:normal;margin-top:0;}";
    html += ".clock{text-align:center;margin:12px 0;padding:12px;background:#0d0d1a;border-radius:8px;border:1px solid #333;}";
    html += ".clock-time{font-size:36px;font-family:monospace;color:#00ff88;letter-spacing:2px;}";
    html += ".clock-date{font-size:14px;color:#888;margin-top:4px;}";
    html += ".clock-label{font-size:11px;color:#666;margin-top:2px;}";
    html += ".ntp-info{text-align:center;margin:8px 0 16px;padding:8px;background:#1a2a1a;border-radius:6px;border:1px solid #2a4a2a;font-size:12px;color:#88cc88;}";
    html += "label{display:block;margin:12px 0 4px;font-size:14px;}";
    html += "select,input{width:100%;padding:10px;border:1px solid #444;border-radius:6px;font-size:16px;background:#2a2a3e;color:#e0e0e0;box-sizing:border-box;}";
    html += "button{width:100%;padding:12px;margin-top:16px;background:#00d4ff;color:#000;border:none;border-radius:6px;font-size:16px;font-weight:bold;cursor:pointer;}";
    html += "button:active{background:#00a8cc;}";
    html += ".status{text-align:center;margin-top:12px;padding:8px;border-radius:4px;background:#2a2a3e;font-size:13px;}";
    html += ".show{display:flex;align-items:center;gap:8px;margin-top:4px;font-size:13px;}";
    html += ".show input{width:auto;}";
    html += "</style></head><body>";

    // Header
    html += "<h1>WWVB Atomic Clock</h1>";
    html += "<h2>WiFi Configuration</h2>";

    // Live clock display
    html += "<div class='clock'>";
    html += "<div class='clock-time' id='utc'>" + timeStr + "</div>";
    html += "<div class='clock-date' id='date'>" + dateStr + "</div>";
    html += "<div class='clock-label'>UTC (WWVB Synchronized)</div>";
    html += "</div>";

    // NTP server info
    html += "<div class='ntp-info'>";
    html += "NTP Server active on 192.168.4.1:123<br>";
    html += "Stratum 1 | Reference: WWVB";
    html += "</div>";

    // WiFi form
    html += "<form action='/connect' method='POST'>";
    html += "<label>Network:</label>";
    html += "<select name='ssid'>";
    if (_networkOptions.length() > 0) {
        html += _networkOptions;
    } else {
        html += "<option value=''>No networks scanned</option>";
    }
    html += "</select>";
    html += "<label>Password:</label>";
    html += "<input type='password' name='password' id='pw' placeholder='Enter WiFi password'>";
    html += "<div class='show'><input type='checkbox' onclick=\"document.getElementById('pw').type=this.checked?'text':'password'\"> Show password</div>";
    html += "<button type='submit'>Connect</button>";
    html += "</form>";
    html += "<div class='status'>" + _statusMessage + "</div>";

    // JavaScript: poll /time every second and update the clock
    html += "<script>";
    html += "function pad(n){return n<10?'0'+n:n;}";
    html += "function tick(){fetch('/time').then(r=>r.json()).then(d=>{";
    html += "if(d.h!==undefined){";
    html += "document.getElementById('utc').textContent=pad(d.h)+':'+pad(d.m)+':'+pad(d.s);";
    html += "document.getElementById('date').textContent=d.Y+'/'+pad(d.M)+'/'+pad(d.D);";
    html += "}}).catch(()=>{});}";
    html += "setInterval(tick,1000);";
    html += "</script>";

    html += "</body></html>";

    return html;
}
