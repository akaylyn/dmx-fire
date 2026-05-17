#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "log.h"
#include "palettes.h"
#include "towers.h"
#include "confluence.h"
#include "button_fsm.h"
#include "morse.h"
#include "storage.h"
#include "secrets.h"
#include "dmx.h"
#include "web.h"

static const char* AP_SSID = WIFI_SSID;
static const char* AP_PASS = WIFI_PASS;

// Optional station mode — defined in secrets.h. Falls back to empty so old
// secrets.h files still compile.
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID ""
#endif
#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS ""
#endif
static const char* STA_SSID = WIFI_STA_SSID;
static const char* STA_PASS = WIFI_STA_PASS;

static WebServer server(80);
static DNSServer  dns;

// --- HTML helpers ---

static String paletteSelect(const String& name, const String& selected) {
  String s = F("<label>Palette<select name='");
  s += name;
  s += F("'>");
  const char* opts[][2] = {{"green","Green fire"},{"blue","Blue fire"},{"fire","Natural fire"}};
  for (auto& o : opts) {
    s += F("<option value='");
    s += o[0];
    s += '\'';
    if (selected == o[0]) s += F(" selected");
    s += '>';
    s += o[1];
    s += F("</option>");
  }
  s += F("</select></label>");
  return s;
}

static String rangeSlider(const String& label, const String& name,
                           uint16_t value, uint16_t lo, uint16_t hi, uint16_t step = 1) {
  String v = String(value);
  String s = F("<label>");
  s += label;
  s += F("<div class='sr'><input type='range' name='");
  s += name;
  s += F("' min='");  s += lo;
  s += F("' max='");  s += hi;
  s += F("' step='"); s += step;
  s += F("' value='");
  s += v;
  s += F("' oninput=\"this.nextElementSibling.textContent=this.value\">"
         "<span class='val'>");
  s += v;
  s += F("</span></div></label>");
  return s;
}

static String connectedCheck(const String& id, bool checked) {
  String s = F("<div class='cr'>"
               "<input type='checkbox' name='connected' id='");
  s += id;
  s += '\'';
  if (checked) s += F(" checked");
  s += F("><label for='");
  s += id;
  s += F("' style='display:inline;margin-top:0'>Connected</label></div>");
  return s;
}

static String buildPage() {
  String s;
  s.reserve(5000);
  s += F("<!DOCTYPE html><html><head>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>DMX Fire Config</title><style>"
         "body{font-family:sans-serif;max-width:520px;margin:40px auto;padding:0 16px}"
         "fieldset{border:1px solid #ccc;border-radius:6px;padding:12px;margin-bottom:16px}"
         "legend{font-weight:bold;padding:0 6px}"
         "label{display:block;margin-top:12px;font-weight:bold}"
         "select{width:100%;margin-top:4px;padding:6px;font-size:1rem}"
         ".sr{display:flex;align-items:center;gap:10px;margin-top:4px}"
         "input[type=range]{flex:1}"
         ".val{min-width:2.5em;text-align:right;font-variant-numeric:tabular-nums}"
         ".cr{display:flex;align-items:center;gap:8px;margin-top:8px}"
         "button{margin-top:16px;padding:8px 24px;font-size:1rem}"
         "#testFireBtn{display:block;width:100%;padding:18px;font-size:1.1rem;"
         "background:#c33;color:#fff;border:0;border-radius:6px;cursor:pointer;"
         "user-select:none;-webkit-user-select:none;touch-action:none}"
         "#testFireBtn:active{background:#900}"
         "</style></head><body><h2>DMX Fire Config</h2>");

  // --- Test Fire (API-driven; mirrors physical button) ---
  s += F("<fieldset><legend>Test Fire</legend>"
         "<button type='button' id='testFireBtn'>Press &amp; hold to fire</button>"
         "</fieldset>");

  // --- Button config ---
  s += F("<fieldset><legend>Button Config</legend>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='button'>"
         "<label>Mode<select name='mode' id='modeSelect'>"
         "<option value='0'");
  if (buttonConfig.mode == 0) s += F(" selected");
  s += F(">Fireball</option>"
         "<option value='1'");
  if (buttonConfig.mode == 1) s += F(" selected");
  s += F(">Party</option>"
         "<option value='2'");
  if (buttonConfig.mode == 2) s += F(" selected");
  s += F(">Machine Gun</option>"
         "</select></label>");
  s += rangeSlider("Fire duration (ms)", "fireDurationMs",
                   buttonConfig.fireDurationMs, 50, 10000, 50);
  s += F("<div id='mgRow'");
  if (buttonConfig.mode != 2) s += F(" style='display:none'");
  s += F(">");
  s += rangeSlider("Machine gun burst (ms)", "machineGunBurstMs",
                   buttonConfig.machineGunBurstMs, 50, 2000, 50);
  s += F("</div>");
  s += rangeSlider("Cooldown (ms)", "cooldownMs",
                   buttonConfig.cooldownMs, 50, 30000, 50);
  s += F("<label>End cue<select name='endCuePattern'>"
         "<option value='0'");
  if (buttonConfig.endCuePattern == 0) s += F(" selected");
  s += F(">White flash fade</option>"
         "</select></label>");
  s += F("<button type='submit'>Save</button></form></fieldset>");

  // --- Morse code ---
  s += F("<fieldset><legend>Morse Code</legend>"
         "<label>Message"
         "<input type='text' id='morseText' maxlength='80' placeholder='HELLO WORLD' "
         "style='width:100%;padding:6px;font-size:1rem;margin-top:4px'>"
         "</label>");
  s += rangeSlider("Unit (ms)", "morseUnitMs", morseUnitMs, 50, 500, 10);
  s += F("<button type='button' id='morseGo'>Fire in Morse</button> "
         "<button type='button' id='morseStop'>Stop</button>"
         "</fieldset>");

  // --- Confluence ---
  s += F("<fieldset><legend>Confluence</legend>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='confluence'>");
  s += connectedCheck("cf", confluenceConfig.connected);
  s += rangeSlider("Fire level", "fireLevel", confluenceConfig.fireLevel, 0, 255);
  s += F("<button type='submit'>Save</button></form></fieldset>");

  // --- All towers ---
  s += F("<fieldset><legend>All Towers</legend>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='all'>");
  s += paletteSelect("palette", towerConfigs[0].palName);
  s += rangeSlider("Brightness", "brightness", towerConfigs[0].bright, 0, 255);
  s += rangeSlider("Flame level", "flameLevel", towerConfigs[0].flameLevel, 0, 255);
  s += F("<button type='submit'>Apply to All</button></form></fieldset>");

  // --- Per tower ---
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    s += F("<fieldset><legend>Tower ");
    s += i;
    s += F("</legend><form method='POST' action='/set'>"
           "<input type='hidden' name='target' value='");
    s += i;
    s += F("'>");
    s += connectedCheck("c" + String(i), towerConfigs[i].connected);
    s += paletteSelect("palette", towerConfigs[i].palName);
    s += rangeSlider("Brightness", "brightness", towerConfigs[i].bright, 0, 255);
    s += rangeSlider("Flame level", "flameLevel", towerConfigs[i].flameLevel, 0, 255);
    s += F("<button type='submit'>Save</button></form></fieldset>");
  }

  // --- Auto-save JS + Test Fire button bindings ---
  s += F("<script>"
         "document.querySelectorAll('form').forEach(function(form){"
         "  form.querySelectorAll('select,input[type=range],input[type=checkbox]').forEach(function(el){"
         "    el.addEventListener('change',function(){"
         "      fetch('/set',{method:'POST',body:new FormData(form)});"
         "    });"
         "  });"
         "  form.addEventListener('submit',function(e){"
         "    e.preventDefault();"
         "    fetch('/set',{method:'POST',body:new FormData(form)});"
         "  });"
         "});"
         "(function(){"
         "  var b=document.getElementById('testFireBtn');"
         "  if(!b)return;"
         "  var post=function(p){fetch(p,{method:'POST'});};"
         "  var press=function(e){e.preventDefault();post('/api/button/press');};"
         "  var release=function(e){e.preventDefault();post('/api/button/release');};"
         "  b.addEventListener('mousedown',press);"
         "  b.addEventListener('mouseup',release);"
         "  b.addEventListener('mouseleave',release);"
         "  b.addEventListener('touchstart',press,{passive:false});"
         "  b.addEventListener('touchend',release,{passive:false});"
         "  b.addEventListener('touchcancel',release,{passive:false});"
         "})();"
         "(function(){"
         "  var sel=document.getElementById('modeSelect');"
         "  var row=document.getElementById('mgRow');"
         "  if(!sel||!row)return;"
         "  sel.addEventListener('change',function(){"
         "    row.style.display=sel.value==='2'?'':'none';"
         "  });"
         "})();"
         "(function(){"
         "  var t=document.getElementById('morseText');"
         "  var go=document.getElementById('morseGo');"
         "  var stop=document.getElementById('morseStop');"
         "  if(!t||!go||!stop)return;"
         "  go.addEventListener('click',function(){"
         "    var u=document.querySelector('input[name=morseUnitMs]');"
         "    var fd=new FormData();"
         "    fd.append('text',t.value);"
         "    if(u)fd.append('unitMs',u.value);"
         "    fetch('/api/morse',{method:'POST',body:fd});"
         "  });"
         "  stop.addEventListener('click',function(){"
         "    fetch('/api/morse/stop',{method:'POST'});"
         "  });"
         "})();"
         "</script>");

  s += F("</body></html>");
  return s;
}

// --- Handlers ---

static void handleRoot() {
  LOG_I("[WEB] GET /  client=%s", server.client().remoteIP().toString().c_str());
  server.send(200, "text/html", buildPage());
}

static void handleSet() {
  String target = server.arg("target");
  // Build a compact args string for the log line
  String args;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "target") continue;
    if (args.length()) args += ' ';
    args += server.argName(i) + '=' + server.arg(i);
  }
  LOG_I("[WEB] POST /set  target=%s  %s", target.c_str(), args.c_str());

  if (target == "confluence") {
    confluenceConfig.connected = server.hasArg("connected");
    confluenceConfig.fireLevel = (uint8_t)server.arg("fireLevel").toInt();

  } else if (target == "button") {
    buttonConfig.mode              = (uint8_t)server.arg("mode").toInt();
    buttonConfig.fireDurationMs    = (uint16_t)server.arg("fireDurationMs").toInt();
    buttonConfig.cooldownMs        = (uint16_t)server.arg("cooldownMs").toInt();
    buttonConfig.endCuePattern     = (uint8_t)server.arg("endCuePattern").toInt();
    buttonConfig.machineGunBurstMs = (uint16_t)server.arg("machineGunBurstMs").toInt();

  } else if (target == "all") {
    String  palName    = server.arg("palette");
    uint8_t bright     = (uint8_t)server.arg("brightness").toInt();
    uint8_t flameLevel = (uint8_t)server.arg("flameLevel").toInt();
    CRGBPalette256 pal = palFromName(palName);
    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      towerConfigs[i].palName    = palName;
      towerConfigs[i].pal        = pal;
      towerConfigs[i].bright     = bright;
      towerConfigs[i].flameLevel = flameLevel;
    }

  } else {
    uint8_t idx = (uint8_t)target.toInt();
    if (idx < NUM_TOWERS) {
      towerConfigs[idx].connected  = server.hasArg("connected");
      towerConfigs[idx].palName    = server.arg("palette");
      towerConfigs[idx].pal        = palFromName(towerConfigs[idx].palName);
      towerConfigs[idx].bright     = (uint8_t)server.arg("brightness").toInt();
      towerConfigs[idx].flameLevel = (uint8_t)server.arg("flameLevel").toInt();
    }
  }

  towerConfigUpdated = true;
  storageSave();
  server.send(200);
}

// --- API handlers ---

static void handleApiPress() {
  LOG_I("[WEB] POST /api/button/press");
  buttonInjectPress();
  server.send(200);
}

static void handleApiRelease() {
  LOG_I("[WEB] POST /api/button/release");
  buttonInjectRelease();
  server.send(200);
}

static void handleApiReset() {
  LOG_I("[WEB] POST /api/button/reset");
  buttonInjectReset();
  server.send(200);
}

static void handleApiMorse() {
  String text = server.arg("text");
  if (server.hasArg("unitMs")) {
    uint16_t u = (uint16_t)server.arg("unitMs").toInt();
    if (u >= 50 && u <= 2000) morseUnitMs = u;
  }
  LOG_I("[WEB] POST /api/morse  text='%s' unit=%u", text.c_str(), morseUnitMs);
  if (morseStart(text)) {
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "no codable characters");
  }
}

static void handleApiMorseStop() {
  LOG_I("[WEB] POST /api/morse/stop");
  morseStop();
  server.send(200);
}


static void handleApiState() {
  String s;
  s.reserve(2048);
  s += '{';

  s += F("\"uptime_ms\":");
  s += millis();

  s += F(",\"fsm\":{\"state\":\"");
  s += fsmStateName(fsmState);
  s += F("\",\"elapsed_ms\":");
  s += fsmElapsedMs();
  s += '}';

  s += F(",\"button\":{\"mode\":");
  s += buttonConfig.mode;
  s += F(",\"fireDurationMs\":");
  s += buttonConfig.fireDurationMs;
  s += F(",\"cooldownMs\":");
  s += buttonConfig.cooldownMs;
  s += F(",\"endCuePattern\":");
  s += buttonConfig.endCuePattern;
  s += F(",\"machineGunBurstMs\":");
  s += buttonConfig.machineGunBurstMs;
  s += '}';

  s += F(",\"confluence\":{\"connected\":");
  s += (confluenceConfig.connected ? F("true") : F("false"));
  s += F(",\"fireLevel\":");
  s += confluenceConfig.fireLevel;
  s += '}';

  s += F(",\"towers\":[");
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    if (i) s += ',';
    s += F("{\"connected\":");
    s += (towerConfigs[i].connected ? F("true") : F("false"));
    s += F(",\"palette\":\"");
    s += towerConfigs[i].palName;
    s += F("\",\"brightness\":");
    s += towerConfigs[i].bright;
    s += F(",\"flameLevel\":");
    s += towerConfigs[i].flameLevel;
    s += '}';
  }
  s += ']';

  s += F(",\"dmx\":{\"ch\":[");
  for (uint16_t i = 0; i < DMX_SHADOW_SIZE; i++) {
    if (i) s += ',';
    s += dmxLastFrame[i];
  }
  s += F("]}}");

  server.send(200, "application/json", s);
}

void webSetup() {
  WiFi.disconnect(true);

  bool wantSta = (STA_SSID && STA_SSID[0] != '\0');
  WiFi.mode(wantSta ? WIFI_AP_STA : WIFI_AP);

  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  LOG_I("AP %s: SSID=%s  IP=%s", ok ? "UP" : "FAILED", AP_SSID, WiFi.softAPIP().toString().c_str());

  // Log whenever an AP client connects or disconnects
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    LOG_I("[WIFI] AP client connected    — total=%d", WiFi.softAPgetStationNum());
  }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    LOG_I("[WIFI] AP client disconnected — total=%d", WiFi.softAPgetStationNum());
  }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

  // Optional station mode — also join an existing WiFi so the device is
  // reachable on the LAN without joining the AP.
  if (wantSta) {
    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
      LOG_I("[WIFI] STA got IP: %s  (joined %s)",
            WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
      LOG_I("[WIFI] STA disconnected — will retry");
      WiFi.reconnect();
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    LOG_I("[WIFI] STA joining SSID=%s ...", STA_SSID);
    WiFi.begin(STA_SSID, STA_PASS);
  }

  // Captive portal: redirect all DNS queries to this device so iOS/Android
  // open the config page automatically instead of blocking non-internet networks.
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/",    HTTP_GET,  handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/api/state",          HTTP_GET,  handleApiState);
  server.on("/api/button/press",   HTTP_POST, handleApiPress);
  server.on("/api/button/release", HTTP_POST, handleApiRelease);
  server.on("/api/button/reset",   HTTP_POST, handleApiReset);
  server.on("/api/morse",          HTTP_POST, handleApiMorse);
  server.on("/api/morse/stop",     HTTP_POST, handleApiMorseStop);
  // Catch all captive portal probe URLs and redirect to the config page
  server.onNotFound([]() { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302); });
  server.begin();
  LOG_I("HTTP server + captive portal DNS started");
}

void webTick() {
  dns.processNextRequest();
  server.handleClient();
}
