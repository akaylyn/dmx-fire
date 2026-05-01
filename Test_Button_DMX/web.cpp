#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "palettes.h"
#include "towers.h"
#include "confluence.h"
#include "button_fsm.h"
#include "storage.h"
#include "secrets.h"
#include "web.h"

static const char* AP_SSID = WIFI_SSID;
static const char* AP_PASS = WIFI_PASS;

static WebServer server(80);

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
         "</style></head><body><h2>DMX Fire Config</h2>");

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

  // --- Button config ---
  s += F("<fieldset><legend>Button Config</legend>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='button'>"
         "<label>Mode<select name='mode'>"
         "<option value='0'");
  if (buttonConfig.mode == 0) s += F(" selected");
  s += F(">Fireball</option>"
         "<option value='1'");
  if (buttonConfig.mode == 1) s += F(" selected");
  s += F(">Party</option>"
         "</select></label>");
  s += rangeSlider("Fire duration (ms)", "fireDurationMs",
                   buttonConfig.fireDurationMs, 500, 10000, 500);
  s += rangeSlider("Cooldown (ms)", "cooldownMs",
                   buttonConfig.cooldownMs, 2000, 30000, 1000);
  s += F("<label>End cue<select name='endCuePattern'>"
         "<option value='0'");
  if (buttonConfig.endCuePattern == 0) s += F(" selected");
  s += F(">White flash fade</option>"
         "</select></label>");
  s += F("<button type='submit'>Save</button></form></fieldset>");

  // --- Auto-save JS ---
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
         "</script>");

  s += F("</body></html>");
  return s;
}

// --- Handlers ---

static void handleRoot() {
  server.send(200, "text/html", buildPage());
}

static void handleSet() {
  String target = server.arg("target");

  if (target == "confluence") {
    confluenceConfig.connected = server.hasArg("connected");
    confluenceConfig.fireLevel = (uint8_t)server.arg("fireLevel").toInt();

  } else if (target == "button") {
    buttonConfig.mode           = (uint8_t)server.arg("mode").toInt();
    buttonConfig.fireDurationMs = (uint16_t)server.arg("fireDurationMs").toInt();
    buttonConfig.cooldownMs     = (uint16_t)server.arg("cooldownMs").toInt();
    buttonConfig.endCuePattern  = (uint8_t)server.arg("endCuePattern").toInt();

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

void webSetup() {
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  server.on("/",    HTTP_GET,  handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.begin();
}

void webTick() {
  server.handleClient();
}
