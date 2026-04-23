#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "palettes.h"
#include "secrets.h"
#include "web.h"

static const char* AP_SSID = WIFI_SSID;
static const char* AP_PASS = WIFI_PASS;

static WebServer server(80);

static const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>DMX Fire Config</title>
  <style>
    body { font-family: sans-serif; max-width: 400px; margin: 40px auto; padding: 0 16px; }
    label { display: block; margin-top: 20px; font-weight: bold; }
    select { width: 100%; margin-top: 4px; padding: 6px; font-size: 1rem; }
    .slider-row { display: flex; align-items: center; gap: 10px; margin-top: 4px; }
    input[type=range] { flex: 1; }
    .val { min-width: 2.5em; text-align: right; font-variant-numeric: tabular-nums; }
    button { margin-top: 28px; padding: 10px 28px; font-size: 1rem; }
  </style>
</head>
<body>
  <h2>DMX Fire Config</h2>
  <form method="POST" action="/set">
    <label>Idle palette
      <select name="palette">
        <option value="green" %GREEN%>Green fire</option>
        <option value="blue"  %BLUE%>Blue fire</option>
        <option value="fire"  %FIRE%>Natural fire</option>
      </select>
    </label>
    <label>Idle brightness
      <div class="slider-row">
        <input type="range" name="brightness" min="0" max="255" value="%BRIGHT%"
               oninput="this.nextElementSibling.textContent=this.value">
        <span class="val">%BRIGHT%</span>
      </div>
    </label>
    <button type="submit">Apply</button>
  </form>
</body>
</html>
)rawliteral";

static void handleRoot() {
  String page = FPSTR(HTML_PAGE);
  page.replace("%GREEN%", idlePalName == "green" ? "selected" : "");
  page.replace("%BLUE%",  idlePalName == "blue"  ? "selected" : "");
  page.replace("%FIRE%",  idlePalName == "fire"  ? "selected" : "");
  page.replace("%BRIGHT%", String(idleBright));
  server.send(200, "text/html", page);
}

static void handleSet() {
  if (server.hasArg("palette")) {
    idlePalName = server.arg("palette");
    if      (idlePalName == "green") idlePal = electricGreenFirePal;
    else if (idlePalName == "blue")  idlePal = electricBlueFirePal;
    else if (idlePalName == "fire")  idlePal = firepal;
  }
  if (server.hasArg("brightness")) {
    idleBright = (byte)server.arg("brightness").toInt();
  }
  idleUpdated = true;  // main loop applies this when the button isn't held
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
