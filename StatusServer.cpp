/**
 * @file      StatusServer.cpp
 * @brief     Status Web Server Implementation
 * @details   Serves a live dashboard on port 80 when connected to WiFi (STA mode).
 */

#include "StatusServer.h"

StatusServer::StatusServer()
    : _httpServer(80), _running(false), _timeManager(nullptr),
      _ntpServer(nullptr), _statusData(nullptr), _receptionHistory(nullptr),
      _onSyncRequest(nullptr), _onTrackingRequest(nullptr), _onSettingsRequest(nullptr) {
}

bool StatusServer::begin() {
    if (_running) return true;

    Serial.printf("[STATUS] Starting web server on %s:80\n",
                  WiFi.localIP().toString().c_str());

    _httpServer.on("/", HTTP_GET, [this]() { handleRoot(); });
    _httpServer.on("/api/status", HTTP_GET, [this]() { handleApiStatus(); });
    _httpServer.on("/api/sync", HTTP_POST, [this]() { handleApiSync(); });
    _httpServer.on("/api/sync/tracking", HTTP_POST, [this]() { handleApiTrackingSync(); });
    _httpServer.on("/api/settings", HTTP_GET,  [this]() { handleApiSettings(); });
    _httpServer.on("/api/settings", HTTP_POST, [this]() { handleApiSettings(); });
    _httpServer.on("/api/log", HTTP_GET, [this]() { handleApiLog(); });
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

void StatusServer::setOnTrackingRequest(std::function<void()> cb) {
    _onTrackingRequest = cb;
}

void StatusServer::setOnSettingsRequest(std::function<void(int8_t, bool)> cb) {
    _onSettingsRequest = cb;
}

void StatusServer::setSyncLog(const SyncLogEntry* log, const uint8_t* head,
                              const uint8_t* filled) {
    _syncLog       = log;
    _syncLogHead   = head;
    _syncLogFilled = filled;
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
    char buf[1024];
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
    if (pos < (int)sizeof(buf) - 80) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"es100avail\":%s,\"es100recv\":%s,\"es100trk\":%s,\"es100pend\":%s",
            _statusData->es100Available      ? "true" : "false",
            _statusData->es100Receiving      ? "true" : "false",
            _statusData->es100Tracking       ? "true" : "false",
            _statusData->es100PendingTracking ? "true" : "false");
    }

    // Leap second warning + antenna performance
    if (pos < (int)sizeof(buf) - 40) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"lsw\":%d,\"ant1\":%d,\"ant2\":%d",
            (int)_statusData->leapSecondWarning,
            (int)_statusData->ant1Successes,
            (int)_statusData->ant2Successes);
    }

    // Close the JSON object
    if (pos < (int)sizeof(buf) - 1) {
        buf[pos++] = '}';
        buf[pos] = '\0';
    }

    _httpServer.send(200, "application/json", buf);
}

bool StatusServer::checkEs100Ready(bool checkPending) {
    if (!_statusData || !_statusData->es100Available) {
        _httpServer.send(503, "application/json", "{\"error\":\"ES100 not available\"}");
        return false;
    }
    bool busy = _statusData->es100Receiving ||
                (checkPending && _statusData->es100PendingTracking);
    if (busy) {
        _httpServer.send(409, "application/json", "{\"error\":\"sync already in progress\"}");
        return false;
    }
    return true;
}

void StatusServer::handleApiSync() {
    if (!checkEs100Ready(false)) return;
    if (_onSyncRequest) {
        _onSyncRequest();
        _httpServer.send(200, "application/json", "{\"status\":\"started\"}");
    } else {
        _httpServer.send(503, "application/json", "{\"error\":\"not configured\"}");
    }
}

void StatusServer::handleApiTrackingSync() {
    if (!checkEs100Ready(true)) return;
    if (_onTrackingRequest) {
        _onTrackingRequest();
        _httpServer.send(200, "application/json", "{\"status\":\"scheduled\"}");
    } else {
        _httpServer.send(503, "application/json", "{\"error\":\"not configured\"}");
    }
}

void StatusServer::handleApiSettings() {
    if (_httpServer.method() == HTTP_POST) {
        if (!_onSettingsRequest) {
            _httpServer.send(501, "application/json", "{\"error\":\"not configured\"}");
            return;
        }
        int8_t off = (int8_t)_httpServer.arg("off").toInt();
        bool   dst = _httpServer.arg("dst") == "1";
        _onSettingsRequest(off, dst);
        _httpServer.send(200, "application/json", "{\"ok\":true}");
    } else {
        if (!_statusData) {
            _httpServer.send(503, "application/json", "{\"error\":\"not ready\"}");
            return;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"off\":%d,\"dst\":%s}",
                 (int)_statusData->utcOffset,
                 _statusData->dstActive ? "true" : "false");
        _httpServer.send(200, "application/json", buf);
    }
}

void StatusServer::handleApiLog() {
    if (!_syncLog || !_syncLogHead || !_syncLogFilled) {
        _httpServer.send(200, "application/json", "[]");
        return;
    }

    uint8_t count = *_syncLogFilled;
    uint8_t head  = *_syncLogHead;

    // Build JSON array of last `count` entries, newest first
    char buf[1200];
    int pos = 0;
    buf[pos++] = '[';

    for (uint8_t i = 0; i < count && pos < (int)sizeof(buf) - 80; i++) {
        uint8_t idx = (head + SYNC_LOG_SIZE - 1 - i) % SYNC_LOG_SIZE;
        const SyncLogEntry& e = _syncLog[idx];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"t\":\"%s\",\"ok\":%s,\"trk\":%s,\"ant\":%d}",
            i > 0 ? "," : "",
            e.timeStr,
            e.success  ? "true" : "false",
            e.tracking ? "true" : "false",
            (int)e.antenna);
    }

    if (pos < (int)sizeof(buf) - 1) {
        buf[pos++] = ']';
        buf[pos] = '\0';
    }

    _httpServer.send(200, "application/json", buf);
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
    html += ".sync-btns{display:flex;gap:8px;justify-content:center;margin-bottom:6px;}";
    html += "#syncbtn{flex:1;max-width:170px;padding:10px 8px;background:#00d4ff;color:#000;border:none;border-radius:6px;font-size:14px;font-weight:bold;cursor:pointer;}";
    html += "#syncbtn:disabled{background:#444;color:#888;cursor:default;}";
    html += "#trkbtn{flex:1;max-width:170px;padding:10px 8px;background:#ff9900;color:#000;border:none;border-radius:6px;font-size:14px;font-weight:bold;cursor:pointer;}";
    html += "#trkbtn:disabled{background:#444;color:#888;cursor:default;}";
    html += ".trkwarn{font-size:11px;color:#777;margin-top:3px;}";
    html += "#syncmsg{font-size:12px;color:#888;margin-top:6px;min-height:16px;}";
    html += ".adj{padding:1px 7px;margin:0 2px;background:#2a2a4a;color:#e0e0e0;border:1px solid #444;border-radius:4px;font-size:13px;cursor:pointer;}";
    html += ".adj:active{background:#3a3a5a;}";
    html += ".log{margin:16px 0;background:#0d0d1a;border-radius:8px;border:1px solid #333;padding:10px 12px;}";
    html += ".log-title{font-size:13px;color:#888;text-align:center;margin-bottom:6px;}";
    html += "#logtbl{width:100%;border-collapse:collapse;font-size:11px;font-family:monospace;}";
    html += "#logtbl th{color:#666;padding:3px 4px;border-bottom:1px solid #333;text-align:left;font-weight:normal;}";
    html += "#logtbl td{padding:2px 4px;border-bottom:1px solid #1a1a2a;}";
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
    html += "<div class='row'><span>UTC Offset</span><span>"
            "<button class='adj' onclick='changeTz(-1)'>&#8722;</button>"
            "<span id='tzoff' style='margin:0 4px'>--</span>"
            "<button class='adj' onclick='changeTz(1)'>+</button>&nbsp;"
            "<button class='adj' id='dstbtn' onclick='toggleDst()'>DST --</button>"
            "</span></div>";
    html += "<div class='row'><span>Leap Second</span><span id='lsw'>--</span></div>";
    html += "<div class='row'><span>Antenna Successes</span><span id='ant'>--</span></div>";
    html += "</div>";

    // Manual sync buttons
    html += "<div class='sync'>";
    html += "<div class='sync-btns'>";
    html += "<button id='syncbtn' onclick='doSync()' disabled>Normal Sync</button>";
    html += "<button id='trkbtn' onclick='doTrackingSync()' disabled>Tracking Sync</button>";
    html += "</div>";
    html += "<div class='trkwarn'>&#9888; Tracking must start at second :55 &mdash; waits up to 60s</div>";
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

    // Reception log table
    html += "<div class='log'>";
    html += "<div class='log-title'>Sync Log (last 20)</div>";
    html += "<table id='logtbl'><tr><th>Time (UTC)</th><th>Mode</th><th>Ant</th><th>Result</th></tr></table>";
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
    // Timezone state (updated from server; used by changeTz/toggleDst)
    html += "var _tz=0,_dst=false;";
    // Leap second label array
    html += "var _lswLabels=['None','\u26a0\ufe0f Positive (+1s, end of month)','\u26a0\ufe0f Negative (-1s, end of month)'];";
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
    // Update ES100 status row and sync buttons
    html += "var es100El=document.getElementById('es100status');";
    html += "var btn=document.getElementById('syncbtn');";
    html += "var trkBtn=document.getElementById('trkbtn');";
    html += "var busy=!d.es100avail||d.es100recv||d.es100pend;";
    html += "if(es100El){";
    html += "if(!d.es100avail){es100El.textContent='Not Available';}";
    html += "else if(d.es100recv&&d.es100trk){es100El.textContent='Tracking\u2026';}";
    html += "else if(d.es100recv){es100El.textContent='Receiving\u2026';}";
    html += "else if(d.es100pend){es100El.textContent='Waiting for :55\u2026';}";
    html += "else{es100El.textContent='Idle';}}";
    html += "if(btn&&!btn._userDisabled){btn.disabled=busy;}";
    html += "if(trkBtn&&!trkBtn._userDisabled){trkBtn.disabled=busy;}";
    // Timezone controls
    html += "_tz=d.tz.off;_dst=d.tz.dst;";
    html += "var v=d.tz.off;document.getElementById('tzoff').textContent=(v>=0?'+':'')+v+'h';";
    html += "document.getElementById('dstbtn').textContent='DST '+(_dst?'ON':'OFF');";
    // Leap second
    html += "document.getElementById('lsw').textContent=_lswLabels[d.lsw||0]||'None';";
    // Antenna successes
    html += "document.getElementById('ant').textContent='Ant1: '+d.ant1+' / Ant2: '+d.ant2;";
    html += "}).catch(()=>{});}";
    // Fetch sync log and rebuild table
    html += "function fetchLog(){fetch('/api/log').then(r=>r.json()).then(rows=>{";
    html += "var t=document.getElementById('logtbl');";
    html += "while(t.rows.length>1)t.deleteRow(1);";
    html += "if(!rows.length){var r=t.insertRow();r.insertCell().colSpan=4;r.cells[0].textContent='No syncs yet';r.cells[0].style.color='#666';return;}";
    html += "rows.forEach(function(e){var r=t.insertRow();";
    html += "r.insertCell().textContent=e.t;";
    html += "r.insertCell().textContent=e.trk?'Tracking':'Normal';";
    html += "r.insertCell().textContent=e.ant?'Ant'+e.ant:'?';";
    html += "var rc=r.insertCell();rc.textContent=e.ok?'\u2713':'\u2717';rc.style.color=e.ok?'#88cc88':'#cc4444';";
    html += "});}).catch(()=>{});}";
    html += "function doSync(){";
    html += "var btn=document.getElementById('syncbtn');";
    html += "var msg=document.getElementById('syncmsg');";
    html += "btn.disabled=true;btn._userDisabled=true;";
    html += "msg.textContent='Requesting normal sync\u2026';";
    html += "fetch('/api/sync',{method:'POST'}).then(r=>r.json())";
    html += ".then(d=>{msg.textContent=d.status?'Normal sync started \u2014 listening for WWVB signal...':('Error: '+d.error);})";
    html += ".catch(()=>{msg.textContent='Request failed';})";
    html += ".finally(()=>{setTimeout(()=>{btn._userDisabled=false;},5000);});}";
    html += "function doTrackingSync(){";
    html += "var btn=document.getElementById('trkbtn');";
    html += "var msg=document.getElementById('syncmsg');";
    html += "btn.disabled=true;btn._userDisabled=true;";
    html += "msg.textContent='Scheduling tracking sync\u2026 waiting for second :55';";
    html += "fetch('/api/sync/tracking',{method:'POST'}).then(r=>r.json())";
    html += ".then(d=>{";
    html += "if(d.status){msg.textContent='Tracking sync scheduled \u2014 will start at second :55 (up to 60s wait)...';}";
    html += "else{msg.textContent='Error: '+(d.error||'unknown');}";
    html += "})";
    html += ".catch(()=>{msg.textContent='Request failed';})";
    html += ".finally(()=>{setTimeout(()=>{btn._userDisabled=false;},65000);});}";
    html += "function changeTz(d){";
    html += "var o=_tz+d;if(o<-12||o>14)return;_tz=o;";
    html += "fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'off='+o+'&dst='+(_dst?1:0)})";
    html += ".then(r=>r.json()).then(function(){";
    html += "var v=_tz;document.getElementById('tzoff').textContent=(v>=0?'+':'')+v+'h';";
    html += "}).catch(()=>{});}";
    html += "function toggleDst(){_dst=!_dst;";
    html += "fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'off='+_tz+'&dst='+(_dst?1:0)})";
    html += ".then(r=>r.json()).then(function(){";
    html += "document.getElementById('dstbtn').textContent='DST '+(_dst?'ON':'OFF');";
    html += "}).catch(()=>{});}";
    html += "setInterval(clockTick,250);setInterval(function(){tick();fetchLog();},30000);tick();fetchLog();";
    html += "</script>";

    html += "</body></html>";

    return html;
}
