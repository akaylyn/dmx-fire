#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_random.h>
#include "log.h"
#include "themes.h"
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

// Per-boot fingerprint emitted as `boot_id` in /api/state. The web UI persists
// the Test Fire armed state alongside this id so the cover re-closes whenever
// the device reboots (id changes → stored armed-state is treated as stale).
static uint32_t bootId = 0;

// When true (default), captive-portal probe URLs route through the redirect
// flow so iOS/Android pop the captive UI on first AP join. The "Open in real
// browser" button in the web UI POSTs /api/captive/dismiss to flip this off
// in RAM, so subsequent probes succeed and the OS closes the popup.
// Intentionally not persisted to NVS — resets to true every boot so first-time
// setup still works.
static bool captivePortalActive = true;

// --- HTML helpers ---

static String themeSelect(const String& name, const String& selected) {
  String s = F("<label>Theme<select name='");
  s += name;
  s += F("'>");
  const char* opts[][2] = {
    {"green",        "Green fire"},
    {"blue",         "Blue fire"},
    {"fire",         "Natural fire"},
    {"simon",        "Simon (rotating)"},
    {"rainbow",      "Rainbow"},
    {"warm_white",   "Warm white"},
    {"bright_white", "Bright white"},
    {"candle",       "Candle light"}
  };
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

// "#rrggbb" for <input type='color'>, which accepts nothing else.
static String hexColor(uint8_t r, uint8_t g, uint8_t b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}

// Parse "#rrggbb" back into bytes. Leaves the outputs untouched on a malformed
// value so a bad POST can never blank the fire look.
static void parseHexColor(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (hex.length() != 7 || hex[0] != '#') return;
  char* end = nullptr;
  long v = strtol(hex.c_str() + 1, &end, 16);
  if (end != hex.c_str() + 7) return;  // trailing junk — reject
  r = (uint8_t)((v >> 16) & 0xff);
  g = (uint8_t)((v >>  8) & 0xff);
  b = (uint8_t)( v        & 0xff);
}

static String connectedCheck(const String& id, bool checked) {
  String s = F("<div class='cr'>"
               "<input type='checkbox' name='connected' id='");
  s += id;
  s += '\'';
  if (checked) s += F(" checked");
  s += F("><label for='");
  s += id;
  s += F("'>Connected</label></div>");
  return s;
}

static String buildPage() {
  String s;
  s.reserve(32000);

  // --- Head + CSS ---
  s += F("<!DOCTYPE html><html><head>"
         "<meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
         "<meta name='theme-color' content='#c33'>"
         "<title>DMX Fire Config</title><style>"
         ":root{--accent:#c33;--accent-dark:#900;--bg:#fff;--fg:#111;--muted:#666;"
         "--line:#ddd;--line-strong:#bbb;--field-bg:#f7f7f7;--tap:48px}"
         "*{box-sizing:border-box}"
         "html,body{margin:0;padding:0;background:var(--bg);color:var(--fg)}"
         "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
         "font-size:16px;line-height:1.4;padding-bottom:max(24px,env(safe-area-inset-bottom))}"
         "header{position:sticky;top:0;z-index:10;background:var(--bg);"
         "border-bottom:1px solid var(--line);padding-top:env(safe-area-inset-top)}"
         "header h1{margin:0;padding:12px 16px 8px;font-size:1.1rem;font-weight:700}"
         "nav.tabs{display:flex;overflow-x:auto;overflow-y:hidden;"
         "-webkit-overflow-scrolling:touch;scrollbar-width:none;padding:0 8px;gap:4px}"
         "nav.tabs::-webkit-scrollbar{display:none}"
         "nav.tabs button{flex:0 0 auto;background:transparent;border:0;padding:14px 18px;"
         "margin:0;font-size:.95rem;font-weight:600;color:var(--muted);"
         "border-bottom:3px solid transparent;cursor:pointer;white-space:nowrap;"
         "min-height:var(--tap);transition:color .15s,border-color .15s}"
         "nav.tabs button.active{color:var(--fg);border-bottom-color:var(--accent)}"
         "nav.tabs button:active{background:rgba(0,0,0,.04)}"
         "main{max-width:640px;margin:0 auto;padding:16px}"
         "section.tab[hidden]{display:none}"
         "section.tab h2{margin:0 0 12px;font-size:1.05rem;color:var(--muted);"
         "text-transform:uppercase;letter-spacing:.04em}"
         "fieldset{border:1px solid var(--line);border-radius:10px;padding:14px 16px 18px;"
         "margin:0 0 16px;background:var(--bg)}"
         "legend{font-weight:700;padding:0 8px;font-size:1rem}"
         "label{display:block;margin-top:16px;font-weight:600;font-size:.95rem}"
         "label:first-of-type{margin-top:4px}"
         "select,input[type=text]{width:100%;margin-top:6px;padding:12px 14px;font-size:16px;"
         "border:1px solid var(--line-strong);border-radius:8px;background:var(--field-bg);"
         "color:var(--fg);-webkit-appearance:none;appearance:none;min-height:var(--tap)}"
         "select{background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath fill='%23555' d='M6 8L0 0h12z'/%3E%3C/svg%3E\");"
         "background-repeat:no-repeat;background-position:right 14px center;padding-right:36px}"
         ".sr{display:flex;align-items:center;gap:14px;margin-top:8px}"
         "input[type=range]{flex:1;height:36px;margin:0;-webkit-appearance:none;"
         "appearance:none;background:transparent}"
         "input[type=range]::-webkit-slider-runnable-track{height:6px;background:var(--line);border-radius:3px}"
         "input[type=range]::-moz-range-track{height:6px;background:var(--line);border-radius:3px}"
         "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:28px;height:28px;"
         "border-radius:50%;background:var(--accent);margin-top:-11px;border:2px solid #fff;"
         "box-shadow:0 1px 3px rgba(0,0,0,.25);cursor:pointer}"
         "input[type=range]::-moz-range-thumb{width:28px;height:28px;border-radius:50%;"
         "background:var(--accent);border:2px solid #fff;box-shadow:0 1px 3px rgba(0,0,0,.25);cursor:pointer}"
         ".val{min-width:4em;text-align:right;font-variant-numeric:tabular-nums;"
         "font-weight:600;font-size:1rem;color:var(--fg)}"
         ".cr{display:flex;align-items:center;gap:12px;margin-top:12px;min-height:var(--tap)}"
         ".cr input[type=checkbox]{width:24px;height:24px;accent-color:var(--accent);margin:0}"
         ".cr label{display:inline;margin:0;font-weight:600}"
         "button[type=submit],.btn{margin-top:18px;padding:14px 24px;font-size:1rem;"
         "font-weight:600;border:0;border-radius:8px;background:var(--fg);color:#fff;"
         "cursor:pointer;min-height:var(--tap);width:100%}"
         "button[type=submit]:active,.btn:active{opacity:.85}"
         ".testfire-wrap{display:flex;flex-direction:column;align-items:stretch;gap:12px;margin-top:8px}"
         ".arm-row{display:flex;align-items:center;gap:14px;padding:14px 16px;"
         "border:1px solid var(--line-strong);border-radius:10px;background:var(--field-bg);"
         "min-height:var(--tap)}"
         ".arm-row input[type=checkbox]{width:28px;height:28px;accent-color:var(--accent);"
         "margin:0;flex:0 0 auto}"
         ".arm-row label{margin:0;font-weight:700;font-size:1rem;flex:1}"
         ".arm-row .arm-state{font-size:.8rem;font-weight:700;text-transform:uppercase;"
         "letter-spacing:.06em;padding:4px 10px;border-radius:999px;background:var(--muted);color:#fff}"
         ".arm-row.is-armed .arm-state{background:var(--accent)}"
         "#testFireBtn,#purgeBtn{display:block;width:100%;padding:48px 24px;font-size:1.3rem;font-weight:700;"
         "background:var(--accent);color:#fff;border:0;border-radius:14px;cursor:pointer;"
         "user-select:none;-webkit-user-select:none;touch-action:none;"
         "box-shadow:0 4px 0 var(--accent-dark);min-height:160px;"
         "transition:background .15s,box-shadow .15s,opacity .15s}"
         "#testFireBtn:active,#purgeBtn:active{background:var(--accent-dark);box-shadow:0 0 0 var(--accent-dark);"
         "transform:translateY(4px)}"
         "#purgeBtn{background:#d67d00;box-shadow:0 4px 0 #8a5100}"
         "#purgeBtn:active{background:#8a5100;box-shadow:0 0 0 #8a5100}"
         "#testFireBtn.disarmed,#purgeBtn.disarmed{background:repeating-linear-gradient(135deg,#999 0 16px,#777 16px 32px);"
         "color:rgba(255,255,255,.85);box-shadow:0 4px 0 #555;cursor:not-allowed}"
         "#testFireBtn.disarmed:active,#purgeBtn.disarmed:active{background:repeating-linear-gradient(135deg,#999 0 16px,#777 16px 32px);"
         "box-shadow:0 4px 0 #555;transform:none}"
         "#testFireBtn small,#purgeBtn small{display:block;margin-top:8px;font-size:.8rem;font-weight:600;opacity:.85}"
         ".testfire-hint{text-align:center;color:var(--muted);font-size:.9rem;margin:4px 0 0}"
         ".escape-portal{margin-top:20px;padding-top:16px;border-top:1px dashed var(--line);text-align:center}"
         ".escape-portal p{margin:0 0 10px;font-size:.85rem;color:var(--muted)}"
         ".escape-portal button{display:inline-block;width:auto;min-height:var(--tap);"
         "padding:10px 18px;font-size:.95rem;font-weight:600;background:var(--field-bg);"
         "color:var(--fg);border:1px solid var(--line-strong);border-radius:10px;cursor:pointer}"
         ".escape-portal button:active{opacity:.85}"
         ".modal[hidden]{display:none}"
         ".modal{position:fixed;inset:0;background:rgba(0,0,0,.55);display:flex;"
         "align-items:center;justify-content:center;padding:24px;"
         "padding-top:calc(24px + env(safe-area-inset-top));"
         "padding-bottom:calc(24px + env(safe-area-inset-bottom));z-index:50}"
         ".modal-card{background:var(--bg);color:var(--fg);border-radius:14px;padding:20px 18px;"
         "max-width:420px;width:100%;box-shadow:0 12px 40px rgba(0,0,0,.35)}"
         ".modal-card h3{margin:0 0 12px;font-size:1.1rem}"
         ".modal-card ol{margin:0 0 12px;padding-left:22px;font-size:.95rem;line-height:1.45}"
         ".modal-card ol li{margin-bottom:8px}"
         ".modal-card ol li:last-child{margin-bottom:0}"
         ".modal-card code{background:var(--field-bg);padding:2px 6px;border-radius:4px;font-size:.9rem}"
         ".modal-card .modal-note{font-size:.8rem;color:var(--muted);margin:0 0 14px}"
         ".modal-card button{display:block;width:100%;min-height:var(--tap);padding:12px;"
         "font-size:1rem;font-weight:600;background:var(--accent);color:#fff;border:0;"
         "border-radius:10px;cursor:pointer}"
         ".modal-card button:active{opacity:.85}"
         ".btn-row{display:flex;gap:10px;margin-top:18px}"
         ".btn-row .btn{margin-top:0;flex:1}"
         ".btn-stop{background:var(--muted)}"
         ".subtabs{display:flex;overflow-x:auto;-webkit-overflow-scrolling:touch;"
         "scrollbar-width:none;gap:6px;margin:0 0 14px;padding-bottom:2px}"
         ".subtabs::-webkit-scrollbar{display:none}"
         ".subtabs button{flex:0 0 auto;padding:10px 16px;font-size:.9rem;font-weight:600;"
         "border:1px solid var(--line-strong);border-radius:999px;background:var(--bg);"
         "color:var(--fg);cursor:pointer;white-space:nowrap;min-height:40px}"
         ".subtabs button.active{background:var(--fg);color:#fff;border-color:var(--fg)}"
         ".tower-panel[hidden]{display:none}"
         ".dmx-addr{font-size:.85rem;color:var(--muted);margin:0 0 14px;padding:8px 12px;"
         "background:var(--field-bg);border-left:3px solid var(--accent);border-radius:4px}"
         ".dmx-addr code{background:var(--bg);padding:2px 6px;border-radius:3px;"
         "font-weight:700;color:var(--fg);font-size:.95rem}"
         "</style></head><body>");

  // --- Header + tab nav ---
  s += F("<header><h1>DMX Fire</h1>"
         "<nav class='tabs' id='tabs' role='tablist'>"
         "<button type='button' class='active' data-tab='test-fire' role='tab'>Test Fire</button>"
         "<button type='button' data-tab='purge' role='tab'>Empty Accum.</button>"
         "<button type='button' data-tab='button' role='tab'>Button Config</button>"
         "<button type='button' data-tab='morse' role='tab'>Morse</button>"
         "<button type='button' data-tab='confluence' role='tab'>Confluence</button>"
         "<button type='button' data-tab='towers' role='tab'>Tower Configs</button>"
         "</nav></header><main>");

  // --- Test Fire tab ---
  s += F("<section class='tab' data-tab='test-fire' role='tabpanel'>"
         "<h2>Test Fire</h2>"
         "<div class='testfire-wrap'>"
         "<div class='arm-row' id='armRow'>"
         "<input type='checkbox' id='armToggle'>"
         "<label for='armToggle'>Cover open &mdash; armed</label>"
         "<span class='arm-state' id='armState'>Safe</span>"
         "</div>"
         "<button type='button' id='testFireBtn' class='disarmed'>DISARMED<small>open the cover above to arm</small></button>"
         "<p class='testfire-hint'>Mirrors the physical fire button. Release to stop. Cover resets to closed on every page load.</p>"
         "<div class='escape-portal'>"
         "<p>Captive portal feels cramped?</p>"
         "<button type='button' id='openInBrowserBtn'>Open in real browser</button>"
         "</div>"
         "</div>"
         "</section>"
         "<div class='modal' id='browserEscapeModal' hidden role='dialog' aria-modal='true' aria-labelledby='browserEscapeTitle'>"
         "<div class='modal-card' role='document'>"
         "<h3 id='browserEscapeTitle'>Open in your browser</h3>"
         "<ol>"
         "<li>Close this captive portal &mdash; iOS: tap <strong>Cancel</strong> at the top, then <em>Use Without Internet</em>. Android: tap <strong>&times;</strong> or <em>Use this network as is</em>.</li>"
         "<li>Open Safari or Chrome.</li>"
         "<li>Visit <code>http://192.168.4.1</code></li>"
         "</ol>"
         "<p class='modal-note'>Your fire controls keep running &mdash; this just switches you to a normal browser tab.</p>"
         "<button type='button' id='browserEscapeClose'>Got it</button>"
         "</div>"
         "</div>");

  // --- Empty Accumulator (purge) tab ---
  s += F("<section class='tab' data-tab='purge' role='tabpanel' hidden>"
         "<h2>Empty Accumulator</h2>"
         "<div class='testfire-wrap'>"
         "<div class='arm-row' id='purgeArmRow'>"
         "<input type='checkbox' id='purgeArmToggle'>"
         "<label for='purgeArmToggle'>Cover open &mdash; armed</label>"
         "<span class='arm-state' id='purgeArmState'>Safe</span>"
         "</div>"
         "<button type='button' id='purgeBtn' class='disarmed'>DISARMED<small>open the cover above to arm</small></button>"
         "<p class='testfire-hint'>Holds <strong>every tower valve and the central Confluence solenoid</strong> open for as long as you hold &mdash; no time limit, no cooldown. Release to close. Cover resets to closed on every page load.</p>"
         "</div>"
         "</section>");

  // --- Button Config tab ---
  s += F("<section class='tab' data-tab='button' role='tabpanel' hidden>"
         "<h2>Button Config</h2>"
         "<fieldset><legend>Fire Behaviour</legend>"
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
                   buttonConfig.fireDurationMs, 10, 10000, 10);
  s += F("<div id='mgRow'");
  if (buttonConfig.mode != 2) s += F(" style='display:none'");
  s += F(">");
  s += rangeSlider("Machine gun burst (ms)", "machineGunBurstMs",
                   buttonConfig.machineGunBurstMs, 10, 2000, 10);
  s += F("</div>");
  s += rangeSlider("Cooldown (ms)", "cooldownMs",
                   buttonConfig.cooldownMs, 0, 30000, 10);
  s += rangeSlider("End cue (ms)", "endCueMs",
                   buttonConfig.endCueMs, 0, 2000, 10);
  s += F("<p class='dmx-addr'>Set <strong>End cue</strong> to 0 to skip the white-flash "
         "state entirely &mdash; that is what gates rapid retrigger. The DMX bus runs at "
         "20&nbsp;Hz, so a valve can only change once per <strong>50&nbsp;ms</strong> frame: "
         "shorter durations round up to one frame and the fastest achievable shot cycle is "
         "about 100&nbsp;ms.</p>"
         "<button type='submit'>Save</button></form></fieldset>"
         "<fieldset><legend>Fire Uplight</legend>"
         "<p class='dmx-addr'>Colour the uplights hold while <strong>any valve is open</strong> "
         "&mdash; firing or Empty Accumulator. Applies to all four towers. The accumulator "
         "strips are not affected and keep running their theme.</p>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='fireup'>"
         "<label>Colour<input type='color' name='fireUpColor' value='");
  s += hexColor(buttonConfig.fireUpR, buttonConfig.fireUpG, buttonConfig.fireUpB);
  s += F("'></label>");
  s += rangeSlider("White", "fireUpW", buttonConfig.fireUpW, 0, 255);
  s += F("<button type='submit'>Save</button></form></fieldset></section>");

  // --- Morse tab ---
  s += F("<section class='tab' data-tab='morse' role='tabpanel' hidden>"
         "<h2>Morse Code</h2>"
         "<fieldset><legend>Send Message</legend>"
         "<label>Message"
         "<input type='text' id='morseText' maxlength='80' placeholder='HELLO WORLD' "
         "autocomplete='off' autocapitalize='characters'>"
         "</label>");
  s += rangeSlider("Unit (ms)", "morseUnitMs", morseUnitMs, 50, 500, 10);
  s += F("<div class='btn-row'>"
         "<button type='button' class='btn' id='morseGo'>Fire in Morse</button>"
         "<button type='button' class='btn btn-stop' id='morseStop'>Stop</button>"
         "</div></fieldset></section>");

  // --- Confluence tab ---
  s += F("<section class='tab' data-tab='confluence' role='tabpanel' hidden>"
         "<h2>Confluence</h2>"
         "<fieldset><legend>Propane Solenoid</legend>"
         "<p class='dmx-addr'>Central solenoid driver: <code>A001</code>, 3-channel mode "
         "(CH&nbsp;1 = central valve). "
         "Each tower's accumulator valve also opens via its own decoder CH&nbsp;4 during fire.</p>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='confluence'>");
  s += connectedCheck("cf", confluenceConfig.connected);
  s += rangeSlider("Fire level", "fireLevel", confluenceConfig.fireLevel, 0, 255);
  s += F("<button type='submit'>Save</button></form></fieldset></section>");

  // --- Towers tab (sub-tabs + Apply-to-All + per-tower loop) ---
  s += F("<section class='tab' data-tab='towers' role='tabpanel' hidden>"
         "<h2>Tower Configs</h2>"
         "<div class='subtabs' id='towerSubtabs' role='tablist'>"
         "<button type='button' class='active' data-sub='all' role='tab'>All</button>"
         "<button type='button' data-sub='t0' role='tab'>Tower 0</button>"
         "<button type='button' data-sub='t1' role='tab'>Tower 1</button>"
         "<button type='button' data-sub='t2' role='tab'>Tower 2</button>"
         "<button type='button' data-sub='t3' role='tab'>Tower 3</button>"
         "</div>"
         "<div class='tower-panel' data-sub='all'>"
         "<fieldset><legend>Apply to All Towers</legend>"
         "<p class='dmx-addr'>Accumulator decoders (RGB strips, fire on CH4): "
         "<code>A005</code> / <code>A020</code> / <code>A035</code> / <code>A050</code><br>"
         "Uplights (LaluceNatz 4ch R/G/B/W): "
         "<code>A009</code> / <code>A024</code> / <code>A039</code> / <code>A054</code></p>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='all'>");
  s += themeSelect("theme", towerConfigs[0].themeName);
  s += rangeSlider("Brightness", "brightness", towerConfigs[0].bright, 0, 255);
  s += rangeSlider("Speed (%)", "speed", towerConfigs[0].speed, 10, 400, 10);
  s += rangeSlider("Flame level", "flameLevel", towerConfigs[0].flameLevel, 0, 255);
  s += F("<button type='submit'>Apply to All</button></form></fieldset></div>");

  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    // Tower N's 15-channel stride starts at CH (5 + N*15). The first 4 channels
    // are the accumulator decoder (strips + fire CH4) and the next 4 are the
    // uplight in 4-channel mode; the remaining 7 are unclaimed.
    char decoderAddr[5];
    char uplightAddr[5];
    uint16_t blockStart = 5 + (uint16_t)i * 15;
    snprintf(decoderAddr, sizeof(decoderAddr), "A%03u", (unsigned)blockStart);
    snprintf(uplightAddr, sizeof(uplightAddr), "A%03u", (unsigned)(blockStart + 4));

    s += F("<div class='tower-panel' data-sub='t");
    s += i;
    s += '\'';
    s += F(" hidden>"
           "<fieldset><legend>Tower ");
    s += i;
    s += F("</legend>"
           "<p class='dmx-addr'>Accumulator decoder (RGB strips, fire on CH4): <code>");
    s += decoderAddr;
    s += F("</code><br>Uplight (LaluceNatz 4ch R/G/B/W): <code>");
    s += uplightAddr;
    s += F("</code></p>"
           "<form method='POST' action='/set'>"
           "<input type='hidden' name='target' value='");
    s += i;
    s += F("'>");
    s += connectedCheck("c" + String(i), towerConfigs[i].connected);
    s += themeSelect("theme", towerConfigs[i].themeName);
    s += rangeSlider("Brightness", "brightness", towerConfigs[i].bright, 0, 255);
    s += rangeSlider("Speed (%)", "speed", towerConfigs[i].speed, 10, 400, 10);
    s += rangeSlider("Flame level", "flameLevel", towerConfigs[i].flameLevel, 0, 255);
    s += F("<button type='submit'>Save</button></form></fieldset></div>");
  }
  s += F("</section></main>");

  // --- Script (minified): tab nav, sub-tabs, auto-save, fire button +
  //     arm-cover (boot_id-gated), mgRow toggle, morse, captive escape. ---
  s += F("<script>"
         "(function(){var K='dmxFireTab';"
         "var t=document.querySelectorAll('nav.tabs button');"
         "var p=document.querySelectorAll('section.tab');"
         "function a(n){t.forEach(function(b){var o=b.dataset.tab===n;b.classList.toggle('active',o);"
         "if(o)b.scrollIntoView({behavior:'smooth',block:'nearest',inline:'center'});});"
         "p.forEach(function(x){x.hidden=x.dataset.tab!==n;});"
         "try{localStorage.setItem(K,n);}catch(e){}}"
         "t.forEach(function(b){b.addEventListener('click',function(){a(b.dataset.tab);});});"
         "var sv;try{sv=localStorage.getItem(K);}catch(e){}"
         "if(sv&&document.querySelector('section.tab[data-tab=\"'+sv+'\"]'))a(sv);})();"
         "(function(){var b=document.querySelectorAll('#towerSubtabs button');"
         "var p=document.querySelectorAll('.tower-panel');"
         "b.forEach(function(x){x.addEventListener('click',function(){"
         "b.forEach(function(y){y.classList.toggle('active',y===x);});"
         "p.forEach(function(q){q.hidden=q.dataset.sub!==x.dataset.sub;});"
         "x.scrollIntoView({behavior:'smooth',block:'nearest',inline:'center'});});});})();"
         "document.querySelectorAll('form').forEach(function(f){"
         "f.querySelectorAll('select,input[type=range],input[type=checkbox]').forEach(function(e){"
         "e.addEventListener('change',function(){fetch('/set',{method:'POST',body:new FormData(f)});});});"
         "f.addEventListener('submit',function(ev){ev.preventDefault();"
         "fetch('/set',{method:'POST',body:new FormData(f)});});});"
         "(function(){function setup(o){var b=document.getElementById(o.btn);"
         "var ar=document.getElementById(o.arm);"
         "var row=document.getElementById(o.row);"
         "var st=document.getElementById(o.state);"
         "if(!b||!ar)return null;var on=false;"
         "function r(){ar.checked=on;b.classList.toggle('disarmed',!on);"
         "b.innerHTML=on?o.armedHtml:'DISARMED<small>open the cover above to arm</small>';"
         "if(row)row.classList.toggle('is-armed',on);if(st)st.textContent=on?'Armed':'Safe';}"
         "function set(v,opt){var pv=on;on=!!v;r();try{if(on&&window._dmxBootId){"
         "localStorage.setItem(o.key,JSON.stringify({armed:true,bootId:window._dmxBootId}));"
         "}else{localStorage.removeItem(o.key);}}catch(e){}"
         "if(pv&&!on&&!(opt&&opt.silent)){fetch(o.releaseUrl,{method:'POST'}).catch(function(){});}}"
         "set(false,{silent:true});"
         "ar.addEventListener('change',function(){set(ar.checked);});"
         "var po=function(p){fetch(p,{method:'POST'});};"
         "var pr=function(e){if(!on){e.preventDefault();return;}e.preventDefault();po(o.pressUrl);};"
         "var rl=function(e){if(!on){e.preventDefault();return;}e.preventDefault();po(o.releaseUrl);};"
         "b.addEventListener('mousedown',pr);b.addEventListener('mouseup',rl);"
         "b.addEventListener('mouseleave',rl);"
         "b.addEventListener('touchstart',pr,{passive:false});"
         "b.addEventListener('touchend',rl,{passive:false});"
         "b.addEventListener('touchcancel',rl,{passive:false});"
         "return function(){var sv=null;try{sv=JSON.parse(localStorage.getItem(o.key)||'null');}catch(e){}"
         "if(sv&&sv.armed&&window._dmxBootId&&sv.bootId===window._dmxBootId){set(true,{silent:true});}"
         "else{try{localStorage.removeItem(o.key);}catch(e){}}};}"
         "var rf=setup({btn:'testFireBtn',arm:'armToggle',row:'armRow',state:'armState',"
         "key:'dmxFireArm',pressUrl:'/api/button/press',releaseUrl:'/api/button/release',"
         "armedHtml:'PRESS &amp; HOLD<br>TO FIRE'});"
         "var rp=setup({btn:'purgeBtn',arm:'purgeArmToggle',row:'purgeArmRow',state:'purgeArmState',"
         "key:'dmxFirePurgeArm',pressUrl:'/api/purge/start',releaseUrl:'/api/purge/stop',"
         "armedHtml:'PRESS &amp; HOLD<br>TO EMPTY'});"
         "fetch('/api/state').then(function(x){return x.json();}).then(function(s){"
         "window._dmxBootId=s.boot_id||null;if(rf)rf();if(rp)rp();}).catch(function(){});})();"
         "(function(){var sel=document.getElementById('modeSelect');"
         "var row=document.getElementById('mgRow');if(!sel||!row)return;"
         "sel.addEventListener('change',function(){row.style.display=sel.value==='2'?'':'none';});})();"
         "(function(){var t=document.getElementById('morseText');"
         "var g=document.getElementById('morseGo');var sp=document.getElementById('morseStop');"
         "if(!t||!g||!sp)return;"
         "g.addEventListener('click',function(){var u=document.querySelector('input[name=morseUnitMs]');"
         "var fd=new FormData();fd.append('text',t.value);if(u)fd.append('unitMs',u.value);"
         "fetch('/api/morse',{method:'POST',body:fd});});"
         "sp.addEventListener('click',function(){fetch('/api/morse/stop',{method:'POST'});});})();"
         "(function(){var b=document.getElementById('openInBrowserBtn');"
         "var m=document.getElementById('browserEscapeModal');"
         "var c=document.getElementById('browserEscapeClose');"
         "if(!b||!m||!c)return;"
         "function sh(){m.hidden=false;}function hd(){m.hidden=true;}"
         "b.addEventListener('click',function(){fetch('/api/captive/dismiss',{method:'POST'}).then(sh,sh);});"
         "c.addEventListener('click',hd);"
         "m.addEventListener('click',function(e){if(e.target===m)hd();});})();"
         "</script></body></html>");
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
    buttonConfig.machineGunBurstMs = (uint16_t)server.arg("machineGunBurstMs").toInt();
    // endCuePattern is no longer a form control (the "colour cascade" variant was
    // never implemented), but the field is still persisted and reported. Only
    // overwrite it when a client actually sends it.
    if (server.hasArg("endCuePattern"))
      buttonConfig.endCuePattern = (uint8_t)server.arg("endCuePattern").toInt();
    if (server.hasArg("endCueMs"))
      buttonConfig.endCueMs = (uint16_t)server.arg("endCueMs").toInt();

  } else if (target == "fireup") {
    // Uplight colour held while a valve is open. Accepts either the colour-input
    // form ("#rrggbb") or explicit byte fields, so tests can post either shape.
    if (server.hasArg("fireUpColor"))
      parseHexColor(server.arg("fireUpColor"),
                    buttonConfig.fireUpR, buttonConfig.fireUpG, buttonConfig.fireUpB);
    if (server.hasArg("fireUpR")) buttonConfig.fireUpR = (uint8_t)server.arg("fireUpR").toInt();
    if (server.hasArg("fireUpG")) buttonConfig.fireUpG = (uint8_t)server.arg("fireUpG").toInt();
    if (server.hasArg("fireUpB")) buttonConfig.fireUpB = (uint8_t)server.arg("fireUpB").toInt();
    if (server.hasArg("fireUpW")) buttonConfig.fireUpW = (uint8_t)server.arg("fireUpW").toInt();

  } else if (target == "all") {
    String   themeName  = server.arg("theme");
    uint8_t  bright     = (uint8_t)server.arg("brightness").toInt();
    uint16_t speed      = (uint16_t)server.arg("speed").toInt();
    if (speed < 10 || speed > 400) speed = 100;
    uint8_t  flameLevel = (uint8_t)server.arg("flameLevel").toInt();
    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      towerConfigs[i].themeName  = themeName;
      towerConfigs[i].bright     = bright;
      towerConfigs[i].speed      = speed;
      towerConfigs[i].flameLevel = flameLevel;
    }

  } else {
    uint8_t idx = (uint8_t)target.toInt();
    if (idx < NUM_TOWERS) {
      uint16_t speed = (uint16_t)server.arg("speed").toInt();
      if (speed < 10 || speed > 400) speed = 100;
      towerConfigs[idx].connected  = server.hasArg("connected");
      towerConfigs[idx].themeName  = server.arg("theme");
      towerConfigs[idx].bright     = (uint8_t)server.arg("brightness").toInt();
      towerConfigs[idx].speed      = speed;
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

// Empty-accumulator purge: hold every valve open while the web UI button is
// held. Independent of the FSM — no duration limit, no cooldown.
static void handleApiPurgeStart() {
  LOG_I("[WEB] POST /api/purge/start");
  purgeStart();
  server.send(200);
}

static void handleApiPurgeStop() {
  LOG_I("[WEB] POST /api/purge/stop");
  purgeStop();
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

// Operator clicked "Open in real browser" in the web UI. Flip the in-RAM
// captive-portal flag off so subsequent OS connectivity probes succeed and
// the captive popup auto-closes. Resets to true on every boot.
static void handleApiCaptiveDismiss() {
  LOG_I("[WEB] POST /api/captive/dismiss");
  captivePortalActive = false;
  server.send(204);
}

// iOS connectivity probe. When the captive portal is active we redirect to
// the config page (which iOS opens in its popup). After dismiss, return the
// exact "Success" HTML body so iOS treats the network as having internet.
static void handleIosProbe() {
  if (captivePortalActive) {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302);
    return;
  }
  server.send(200, "text/html",
              F("<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"));
}

// Android connectivity probe. 204 No Content is the success signal.
static void handleAndroidProbe() {
  if (captivePortalActive) {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302);
    return;
  }
  server.send(204);
}

// Windows NCSI text probe.
static void handleWindowsNcsi() {
  if (captivePortalActive) {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302);
    return;
  }
  server.send(200, "text/plain", F("Microsoft NCSI"));
}

// Windows 10+ HTTPS-fallback connect-test probe.
static void handleWindowsConnectTest() {
  if (captivePortalActive) {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302);
    return;
  }
  server.send(200, "text/plain", F("Microsoft Connect Test"));
}


static void handleApiState() {
  String s;
  s.reserve(2048);
  s += '{';

  s += F("\"boot_id\":\"");
  // Render the 32-bit boot fingerprint as 8 lowercase hex chars.
  char hex[9];
  snprintf(hex, sizeof(hex), "%08x", (unsigned)bootId);
  s += hex;
  s += F("\",\"uptime_ms\":");
  s += millis();

  s += F(",\"fsm\":{\"state\":\"");
  s += fsmStateName(fsmState);
  s += F("\",\"elapsed_ms\":");
  s += fsmElapsedMs();
  s += '}';

  s += F(",\"purge\":");
  s += (purgeActive() ? F("true") : F("false"));

  s += F(",\"button\":{\"mode\":");
  s += buttonConfig.mode;
  s += F(",\"fireDurationMs\":");
  s += buttonConfig.fireDurationMs;
  s += F(",\"cooldownMs\":");
  s += buttonConfig.cooldownMs;
  s += F(",\"endCuePattern\":");
  s += buttonConfig.endCuePattern;
  s += F(",\"endCueMs\":");
  s += buttonConfig.endCueMs;
  s += F(",\"machineGunBurstMs\":");
  s += buttonConfig.machineGunBurstMs;
  s += '}';

  s += F(",\"fireUplight\":{\"r\":");
  s += buttonConfig.fireUpR;
  s += F(",\"g\":");
  s += buttonConfig.fireUpG;
  s += F(",\"b\":");
  s += buttonConfig.fireUpB;
  s += F(",\"w\":");
  s += buttonConfig.fireUpW;
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
    s += F(",\"theme\":\"");
    s += towerConfigs[i].themeName;
    s += F("\",\"brightness\":");
    s += towerConfigs[i].bright;
    s += F(",\"speed\":");
    s += towerConfigs[i].speed;
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
  // Per-boot fingerprint. Used by the web UI to invalidate the locally-stored
  // Test Fire armed state after a reboot (so the cover re-closes).
  bootId = esp_random();

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
  server.on("/api/state",            HTTP_GET,  handleApiState);
  server.on("/api/button/press",     HTTP_POST, handleApiPress);
  server.on("/api/button/release",   HTTP_POST, handleApiRelease);
  server.on("/api/button/reset",     HTTP_POST, handleApiReset);
  server.on("/api/purge/start",      HTTP_POST, handleApiPurgeStart);
  server.on("/api/purge/stop",       HTTP_POST, handleApiPurgeStop);
  server.on("/api/morse",            HTTP_POST, handleApiMorse);
  server.on("/api/morse/stop",       HTTP_POST, handleApiMorseStop);
  server.on("/api/captive/dismiss",  HTTP_POST, handleApiCaptiveDismiss);

  // OS captive-portal probe URLs. While captivePortalActive=true these redirect
  // to / so iOS/Android pop the captive UI. After /api/captive/dismiss the
  // handlers return success responses so the OS closes the popup.
  server.on("/hotspot-detect.html",        HTTP_GET, handleIosProbe);
  server.on("/library/test/success.html",  HTTP_GET, handleIosProbe);
  server.on("/generate_204",               HTTP_GET, handleAndroidProbe);
  server.on("/gen_204",                    HTTP_GET, handleAndroidProbe);
  server.on("/ncsi.txt",                   HTTP_GET, handleWindowsNcsi);
  server.on("/connecttest.txt",            HTTP_GET, handleWindowsConnectTest);

  // Catch-all. While captive portal is active, redirect to config page (this is
  // what triggers iOS/Android to surface the popup). Once dismissed, return a
  // generic 200 "Success" so any straggling OS probe also accepts the network.
  server.onNotFound([]() {
    if (captivePortalActive) {
      server.sendHeader("Location", "http://192.168.4.1/");
      server.send(302);
    } else {
      server.send(200, "text/plain", F("Success"));
    }
  });
  server.begin();
  LOG_I("HTTP server + captive portal DNS started");
}

void webTick() {
  dns.processNextRequest();
  server.handleClient();
}
