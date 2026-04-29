#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "palettes.h"
#include "towers.h"
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

static String brightnessSlider(const String& name, uint8_t value) {
  String v = String(value);
  String s = F("<label>Brightness<div class='sr'>");
  s += F("<input type='range' name='");
  s += name;
  s += F("' min='0' max='255' value='");
  s += v;
  s += F("' oninput=\"this.nextElementSibling.textContent=this.value\">");
  s += F("<span class='val'>");
  s += v;
  s += F("</span></div></label>");
  return s;
}

static String buildPage() {
  String s;
  s.reserve(3000);
  s += F("<!DOCTYPE html><html><head>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>DMX Fire Config</title><style>"
         "body{font-family:sans-serif;max-width:500px;margin:40px auto;padding:0 16px}"
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

  // --- All towers (reflects Tower 0 as the representative state) ---
  s += F("<fieldset><legend>All Towers</legend>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='all'>");
  s += paletteSelect("palette", towerConfigs[0].palName);
  s += brightnessSlider("brightness", towerConfigs[0].bright);
  s += F("<button type='submit'>Apply to All</button></form></fieldset>");

  // --- Per tower ---
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    s += F("<fieldset><legend>Tower ");
    s += i;
    s += F("</legend><form method='POST' action='/set'>"
           "<input type='hidden' name='target' value='");
    s += i;
    s += F("'><div class='cr'>"
           "<input type='checkbox' name='connected' id='c");
    s += i;
    s += '\'';
    if (towerConfigs[i].connected) s += F(" checked");
    s += F("><label for='c");
    s += i;
    s += F("' style='display:inline;margin-top:0'>Connected</label></div>");
    s += paletteSelect("palette", towerConfigs[i].palName);
    s += brightnessSlider("brightness", towerConfigs[i].bright);
    s += F("<button type='submit'>Save</button></form></fieldset>");
  }

  s += F("</body></html>");
  return s;
}

// --- Handlers ---

static void handleRoot() {
  server.send(200, "text/html", buildPage());
}

static void handleSet() {
  String target  = server.arg("target");
  String palName = server.arg("palette");
  uint8_t bright = (uint8_t)server.arg("brightness").toInt();

  if (target == "all") {
    CRGBPalette256 pal = palFromName(palName);
    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      towerConfigs[i].palName = palName;
      towerConfigs[i].pal     = pal;
      towerConfigs[i].bright  = bright;
    }
  } else {
    uint8_t idx = (uint8_t)target.toInt();
    if (idx < NUM_TOWERS) {
      towerConfigs[idx].connected = server.hasArg("connected");
      towerConfigs[idx].palName   = palName;
      towerConfigs[idx].pal       = palFromName(palName);
      towerConfigs[idx].bright    = bright;
    }
  }

  towerConfigUpdated = true;
  server.sendHeader("Location", "/");
  server.send(303);
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
