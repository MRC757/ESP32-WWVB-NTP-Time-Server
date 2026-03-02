/**
 * @file      StatusServer.cpp
 * @brief     Status Web Server Implementation
 * @details   Serves a live dashboard on port 80 when connected to WiFi (STA mode).
 */

#include "StatusServer.h"

StatusServer::StatusServer()
    : _httpServer(80), _running(false), _timeManager(nullptr),
      _ntpServer(nullptr), _statusData(nullptr), _receptionHistory(nullptr),
      _onSyncRequest(nullptr) {
}

bool StatusServer::begin() {
    if (_running) return true;

    Serial.printf("[STATUS] Starting web server on %s:80\n",
                  WiFi.localIP().toString().c_str());

    _httpServer.on("/", HTTP_GET, [this]() { handleRoot(); });
    _httpServer.on("/api/status", HTTP_GET, [this]() { handleApiStatus(); });
    _httpServer.on("/api/sync", HTTP_POST, [this]() { handleApiSync(); });
    _httpServer.onNotFound([this]() { handleNotFound(); });

    _httpServer.begin();
    _running = true;

    Serial.println("[STATUS] Web server started");
    return true;
}

void StatusServer::stop() {
    if (!_running) return;

    _httpServer.stop();
    _running = false;

    Serial.println("[STATUS] Web server stopped");
}

void StatusServer::handleClient() {
    if (!_running) return;
    _httpServer.handleClient();
}

void StatusServer::setTimeManager(TimeManager* tm) {
    _timeManager = tm;
}

void StatusServer::setNTPServer(NTPServer* ntp) {
    _ntpServer = ntp;
}

void StatusServer::setStatusData(StatusData* data) {
    _statusData = data;
}

void StatusServer::setReceptionHistory(ReceptionHistory* rh) {
    _receptionHistory = rh;
}

void StatusServer::setOnSyncRequest(std::function<void()> cb) {
    _onSyncRequest = cb;
}

bool StatusServer::isRunning() const {
    return _running;
}

const char* StatusServer::timeSourceName(uint8_t src) {
    switch (src) {
        case 3:  return "WWVB";
        case 2:  return "NTP";
        case 1:  return "RTC";
        default: return "None";
    }
}

void StatusServer::handleRoot() {
    _httpServer.send(200, "text/html; charset=utf-8", buildPage());
}

void StatusServer::handleApiStatus() {
    if (!_timeManager || !_statusData) {
        _httpServer.send(503, "application/json", "{\"error\":\"not ready\"}");
        return;
    }

    ClockTime utc = _timeManager->getUTCTime();
    ClockTime local = _timeManager->getLocalTime(_statusData->utcOffset, _statusData->dstActive);

    float tempC = _statusData->temperatureC;
    float tempF = (tempC * 9.0f / 5.0f) + 32.0f;

    uint16_t battMv = _statusData->batteryMv;
    int battPct = _statusData->batteryPct;

    uint32_t ntpReq = _ntpServer ? _ntpServer->getRequestCount() : 0;

    unsigned long syncAgo = 0;
    if (_statusData->lastSyncMillis > 0) {
        syncAgo = (millis() - _statusData->lastSyncMillis) / 1000;
    }

    int8_t totalOff = _statusData->utcOffset + (_statusData->dstActive ? 1 : 0);
    char tzLabel[16];
    snprintf(tzLabel, sizeof(tzLabel), "UTC%+d%s", totalOff,
             _statusData->dstActive ? " DST" : "");

    // Build JSON — base fields first
    char buf[768];
    int pos = snprintf(buf, sizeof(buf),
        "{\"utc\":{\"h\":%d,\"m\":%d,\"s\":%d,\"Y\":%d,\"M\":%d,\"D\":%d},"
        "\"local\":{\"h\":%d,\"m\":%d,\"s\":%d,\"Y\":%d,\"M\":%d,\"D\":%d},"
        "\"tz\":{\"off\":%d,\"dst\":%s,\"label\":\"%s\"},"
        "\"temp\":{\"c\":%.1f,\"f\":%.1f},"
        "\"batt\":{\"mv\":%d,\"pct\":%d,\"chg\":%s},"
        "\"ntp\":{\"req\":%lu},"
        "\"sync\":{\"src\":\"%s\",\"ago\":%lu,\"time\":\"%s\"}",
        utc.hour, utc.minute, utc.second, utc.year, utc.month, utc.day,
        local.hour, local.minute, local.second, local.year, local.month, local.day,
        (int)_statusData->utcOffset, _statusData->dstActive ? "true" : "false", tzLabel,
        tempC, tempF,
        (int)battMv, battPct, _statusData->batteryCharging ? "true" : "false",
        (unsigned long)ntpReq,
        timeSourceName(_statusData->timeSource), (unsigned long)syncAgo,
        _statusData->lastSyncTimeStr);

    // Add reception history if available
    if (_receptionHistory && pos < (int)sizeof(buf) - 200) {
        uint8_t histData[HISTORY_BUCKETS];
        _receptionHistory->getHistoryData(histData);

        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"wwvb\":{\"rate\":%d,\"ok\":%d,\"tries\":%d,\"h\":[",
            _receptionHistory->getSuccessRate(),
            _receptionHistory->getTotalSuccessCount(),
            _receptionHistory->getTotalAttemptCount());

        for (int i = 0; i < HISTORY_BUCKETS && pos < (int)sizeof(buf) - 10; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%d",
                           i > 0 ? "," : "", histData[i]);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }

    // ES100 hardware state
    if (pos < (int)sizeof(buf) - 60) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"es100avail\":%s,\"es100recv\":%s,\"es100trk\":%s",
            _statusData->es100Available ? "true" : "false",
            _statusData->es100Receiving ? "true" : "false",
            _statusData->es100Tracking  ? "true" : "false");
    }

    // Close the JSON object
    if (pos < (int)sizeof(buf) - 1) {
        buf[pos++] = '}';
        buf[pos] = '\0';
    }

    _httpServer.send(200, "application/json", buf);
}

void StatusServer::handleApiSync() {
    if (!_statusData || !_statusData->es100Available) {
        _httpServer.send(503, "application/json", "{\"error\":\"ES100 not available\"}");
        return;
    }
    if (_statusData->es100Receiving) {
        _httpServer.send(409, "application/json", "{\"error\":\"sync already in progress\"}");
        return;
    }
    if (_onSyncRequest) {
        _onSyncRequest();
        _httpServer.send(200, "application/json", "{\"status\":\"started\"}");
    } else {
        _httpServer.send(503, "application/json", "{\"error\":\"not configured\"}");
    }
}

void StatusServer::handleNotFound() {
    _httpServer.send(404, "text/plain", "Not Found");
}

String StatusServer::buildPage() {
    // Get initial time values for server-rendered display
    String localStr = "--:--:--", localDate = "----/--/--";
    String utcStr = "--:--:--", utcDate = "----/--/--";
    String tzLabel = "Local Time";

    if (_timeManager && _statusData) {
        ClockTime utc = _timeManager->getUTCTime();
        ClockTime local = _timeManager->getLocalTime(_statusData->utcOffset, _statusData->dstActive);
        char buf[12];

        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.hour, local.minute, local.second);
        localStr = buf;
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d", local.year, local.month, local.day);
        localDate = buf;

        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", utc.hour, utc.minute, utc.second);
        utcStr = buf;
        snprintf(buf, sizeof(buf), "%04d/%02d/%02d", utc.year, utc.month, utc.day);
        utcDate = buf;

        int8_t totalOff = _statusData->utcOffset + (_statusData->dstActive ? 1 : 0);
        char tz[16];
        snprintf(tz, sizeof(tz), "Local (UTC%+d%s)", totalOff,
                 _statusData->dstActive ? " DST" : "");
        tzLabel = tz;
    }

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='300'>";
    html += "<title>WWVB Clock Status</title>";
    html += "<style>";
    html += "body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:15px;background:#1a1a2e;color:#e0e0e0;}";
    html += "h1{color:#00d4ff;font-size:22px;text-align:center;margin-bottom:4px;}";
    html += "h2{color:#aaa;font-size:14px;text-align:center;font-weight:normal;margin-top:0;}";
    html += ".clock{text-align:center;margin:12px 0;padding:12px;background:#0d0d1a;border-radius:8px;border:1px solid #333;}";
    html += ".clock-time{font-size:36px;font-family:monospace;color:#00ff88;letter-spacing:2px;}";
    html += ".clock-date{font-size:14px;color:#888;margin-top:4px;}";
    html += ".clock-label{font-size:11px;color:#666;margin-top:2px;}";
    html += ".info{margin:16px 0;background:#0d0d1a;border-radius:8px;border:1px solid #333;overflow:hidden;}";
    html += ".row{display:flex;justify-content:space-between;padding:10px 14px;border-bottom:1px solid #222;}";
    html += ".row:last-child{border-bottom:none;}";
    html += ".row:nth-child(even){background:#12122a;}";
    html += ".row span:first-child{color:#888;}";
    html += ".row span:last-child{color:#e0e0e0;font-family:monospace;}";
    html += ".ntp-info{text-align:center;margin:12px 0;padding:8px;background:#1a2a1a;border-radius:6px;border:1px solid #2a4a2a;font-size:12px;color:#88cc88;}";
    html += ".chart{margin:16px 0;background:#0d0d1a;border-radius:8px;border:1px solid #333;padding:12px;}";
    html += ".chart-title{font-size:13px;color:#888;text-align:center;margin-bottom:8px;}";
    html += ".chart-bars{display:flex;align-items:flex-end;height:50px;gap:1px;}";
    html += ".chart-bars div{flex:1;background:#00ff88;min-width:2px;border-radius:1px 1px 0 0;transition:height .3s;}";
    html += ".chart-stats{display:flex;justify-content:space-between;margin-top:8px;font-size:11px;color:#666;}";
    html += ".sync{text-align:center;margin:12px 0;}";
    html += "#syncbtn{width:auto;padding:10px 28px;background:#00d4ff;color:#000;border:none;border-radius:6px;font-size:15px;font-weight:bold;cursor:pointer;}";
    html += "#syncbtn:disabled{background:#444;color:#888;cursor:default;}";
    html += "#syncmsg{font-size:12px;color:#888;margin-top:6px;min-height:16px;}";
    html += "</style></head><body>";

    // Header
    html += "<h1>WWVB Atomic Clock</h1>";
    html += "<h2>Status Dashboard</h2>";

    // Local time clock
    html += "<div class='clock'>";
    html += "<div class='clock-time' id='local'>" + localStr + "</div>";
    html += "<div class='clock-date' id='ldate'>" + localDate + "</div>";
    html += "<div class='clock-label' id='tzlabel'>" + tzLabel + "</div>";
    html += "</div>";

    // UTC clock
    html += "<div class='clock'>";
    html += "<div class='clock-time' id='utc'>" + utcStr + "</div>";
    html += "<div class='clock-date' id='udate'>" + utcDate + "</div>";
    html += "<div class='clock-label'>UTC</div>";
    html += "</div>";

    // Info rows
    html += "<div class='info'>";
    html += "<div class='row'><span>Temperature</span><span id='temp'>--</span></div>";
    html += "<div class='row'><span>Battery</span><span id='batt'>--</span></div>";
    html += "<div class='row'><span>NTP Requests</span><span id='ntp'>--</span></div>";
    html += "<div class='row'><span>Time Source</span><span id='src'>--</span></div>";
    html += "<div class='row'><span>Last Sync</span><span id='sync'>--</span></div>";
    html += "<div class='row'><span>ES100</span><span id='es100status'>--</span></div>";
    html += "</div>";

    // Manual sync button
    html += "<div class='sync'>";
    html += "<button id='syncbtn' onclick='doSync()' disabled>Sync Now</button>";
    html += "<div id='syncmsg'></div>";
    html += "</div>";

    // WWVB reception history chart
    html += "<div class='chart'>";
    html += "<div class='chart-title'>WWVB Reception History (48h)</div>";
    html += "<div class='chart-bars' id='bars'>";
    for (int i = 0; i < HISTORY_BUCKETS; i++) {
        html += "<div style='height:0'></div>";
    }
    html += "</div>";
    html += "<div class='chart-stats'>";
    html += "<span id='wrate'>--% success</span>";
    html += "<span id='wcount'>-- syncs / -- attempts</span>";
    html += "</div>";
    html += "</div>";

    // NTP server info
    html += "<div class='ntp-info'>";
    html += "NTP Server: " + WiFi.localIP().toString() + ":123";
    html += " | Stratum 1 | Ref: WWVB";
    html += "</div>";

    // JavaScript: smooth local clock + periodic server data fetch
    // Clock display is driven by Date.now() locally (250 ms interval) so seconds
    // increment smoothly regardless of network jitter.  Non-clock data (battery,
    // temperature, sync status, chart) is refreshed from the server every 30 s.
    html += "<script>";
    html += "function pad(n){return n<10?'0'+n:n;}";
    html += "function fmtd(d){return d.Y+'/'+pad(d.M)+'/'+pad(d.D);}";
    html += "function ago(s){";
    html += "if(s>=86400){var d=Math.floor(s/86400);return d+'d '+Math.floor((s%86400)/3600)+'h ago';}";
    html += "if(s>=3600){return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m ago';}";
    html += "if(s>=60){return Math.floor(s/60)+'m '+s%60+'s ago';}";
    html += "return s+'s ago';}";
    // Reference point updated on each server fetch
    html += "var _ref=null;";
    // Advance a {h,m,s} time by s seconds, handling midnight rollover
    html += "function addSecs(t,s){";
    html += "var tot=((t.h*3600+t.m*60+t.s+s)%86400+86400)%86400;";
    html += "return{h:Math.floor(tot/3600),m:Math.floor((tot%3600)/60),s:tot%60};}";
    // Runs every 250 ms — updates only the clock digits from local time
    html += "function clockTick(){";
    html += "if(!_ref)return;";
    html += "var s=Math.floor((Date.now()-_ref.at)/1000);";
    html += "var u=addSecs(_ref.utc,s),l=addSecs(_ref.local,s);";
    html += "document.getElementById('utc').textContent=pad(u.h)+':'+pad(u.m)+':'+pad(u.s);";
    html += "document.getElementById('local').textContent=pad(l.h)+':'+pad(l.m)+':'+pad(l.s);}";
    // Runs every 30 s — fetches all data and anchors the local clock reference
    html += "function tick(){fetch('/api/status').then(r=>r.json()).then(d=>{";
    html += "_ref={utc:d.utc,local:d.local,at:Date.now()};";
    html += "document.getElementById('ldate').textContent=fmtd(d.local);";
    html += "document.getElementById('udate').textContent=fmtd(d.utc);";
    html += "document.getElementById('tzlabel').textContent='Local ('+d.tz.label+')';";
    html += "document.getElementById('temp').textContent=d.temp.f.toFixed(1)+'\\u00B0F / '+d.temp.c.toFixed(1)+'\\u00B0C';";
    html += "var b=d.batt;";
    html += "document.getElementById('batt').textContent=b.pct+'% '+(b.mv/1000).toFixed(2)+'V'+(b.chg?' \\u26A1':'');";
    html += "document.getElementById('ntp').textContent=d.ntp.req;";
    html += "document.getElementById('src').textContent=d.sync.src;";
    html += "document.getElementById('sync').textContent=d.sync.ago>0?ago(d.sync.ago)+(d.sync.time?' ('+d.sync.time+' UTC)':''):'Never';";
    // Update reception history chart
    html += "if(d.wwvb){";
    html += "var bars=document.getElementById('bars').children;";
    html += "var h=d.wwvb.h,mx=Math.max.apply(null,h)||1;";
    html += "for(var i=0;i<h.length&&i<bars.length;i++){";
    html += "bars[i].style.height=h[i]?Math.max(2,(h[i]/mx)*100)+'%':'0';}";
    html += "document.getElementById('wrate').textContent=d.wwvb.rate+'% success';";
    html += "document.getElementById('wcount').textContent=d.wwvb.ok+' syncs / '+d.wwvb.tries+' attempts';}";
    // Update ES100 status row and sync button
    html += "var es100El=document.getElementById('es100status');";
    html += "var btn=document.getElementById('syncbtn');";
    html += "if(es100El){";
    html += "if(!d.es100avail){es100El.textContent='Not Available';}";
    html += "else if(d.es100recv&&d.es100trk){es100El.textContent='Tracking\u2026';}";
    html += "else if(d.es100recv){es100El.textContent='Receiving\u2026';}";
    html += "else{es100El.textContent='Idle';}}";
    html += "if(btn&&!btn._userDisabled){btn.disabled=!d.es100avail||d.es100recv;}";
    html += "}).catch(()=>{});}";
    html += "function doSync(){";
    html += "var btn=document.getElementById('syncbtn');";
    html += "var msg=document.getElementById('syncmsg');";
    html += "btn.disabled=true;btn._userDisabled=true;";
    html += "msg.textContent='Requesting sync\u2026';";
    html += "fetch('/api/sync',{method:'POST'}).then(r=>r.json())";
    html += ".then(d=>{msg.textContent=d.status?'Sync started \u2014 listening for WWVB signal...':('Error: '+d.error);})";
    html += ".catch(()=>{msg.textContent='Request failed';})";
    html += ".finally(()=>{setTimeout(()=>{btn._userDisabled=false;},5000);});}";
    html += "setInterval(clockTick,250);setInterval(tick,30000);tick();";
    html += "</script>";

    html += "</body></html>";

    return html;
}
