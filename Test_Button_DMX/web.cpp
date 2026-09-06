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
#include "audio.h"
#include "morse.h"
#include "storage.h"
#include "secrets.h"
#include "dmx.h"
#include "ota.h"
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

// A form checkbox. Note the semantics the /set handler relies on: a browser
// submits nothing at all for an unchecked box, so handleSet() reads these with
// hasArg() and absent means false. Any client posting to /set must therefore
// send every checkbox it wants to keep on — see docs/spec-solenoid-binary.md.
static String checkBox(const String& name, const String& id, const String& label, bool checked) {
  String s = F("<div class='cr'><input type='checkbox' name='");
  s += name;
  s += F("' id='");
  s += id;
  s += '\'';
  if (checked) s += F(" checked");
  s += F("><label for='");
  s += id;
  s += F("'>");
  s += label;
  s += F("</label></div>");
  return s;
}

static String connectedCheck(const String& id, bool checked) {
  return checkBox(F("connected"), id, F("Connected"), checked);
}

// The per-fixture propane isolator. Replaces the old flame/fire level sliders:
// a solenoid has no level, so the only thing left to offer is on or off.
static String fireEnabledCheck(const String& id, bool checked) {
  return checkBox(F("fireEnabled"), id, F("Fire enabled"), checked);
}

static String buildPage() {
  String s;
  s.reserve(50000);  // grown by the Firmware/OTA tab, the Audio tab and the live readouts

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
         ".audlink{font-family:ui-monospace,Menlo,monospace;font-size:.82rem;line-height:1.5;white-space:pre-wrap;color:var(--muted)}"
         ".audlink b{color:var(--fg)}"
         ".audlink .ok{color:#0a0;font-weight:700}"
         ".audlink .bad{color:var(--accent);font-weight:700}"
         ".audbar{height:10px;border-radius:5px;background:var(--line);overflow:hidden;margin-top:12px}"
         ".audbar>div{height:100%;width:0;background:var(--accent);transition:width .2s}"
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
         // Live device readout under each fixture's address line. Config forms are
         // built once here at page load, so without this the page silently lies
         // whenever anything else changes config. See notes.md Session 5.
         ".live{font:12px/1.5 ui-monospace,Menlo,monospace;background:var(--field-bg);"
         "border:1px solid var(--line);border-radius:8px;padding:8px 10px;margin:0 0 12px;"
         "color:var(--muted);white-space:pre-wrap}"
         ".live.bad{border-color:#b3402f;color:#8c2f20;background:#fdeeeb}"
         ".live .warn{color:#b3402f;font-weight:600}"
         ".live .stale{color:#8a6d1f}"
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
         "<button type='button' data-tab='audio' role='tab'>Audio</button>"
         "<button type='button' data-tab='firmware' role='tab'>Firmware</button>"
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
         "<option value='3'");
  if (buttonConfig.mode == 3) s += F(" selected");
  s += F(">Audio: beat pop</option>"
         "<option value='4'");
  if (buttonConfig.mode == 4) s += F(" selected");
  s += F(">Audio: sustained bass</option>"
         "<option value='5'");
  if (buttonConfig.mode == 5) s += F(" selected");
  s += F(">Audio: drop only</option>"
         "<option value='6'");
  if (buttonConfig.mode == 6) s += F(" selected");
  s += F(">Audio: machine gun on beat</option>"
         "</select></label>"
         "<p class='testfire-hint' id='audModeHint' style='display:none'>"
         "Audio mode &mdash; also needs a live audio link and ARM on the Audio tab. "
         "Limits live there too, not here.</p>");
  s += rangeSlider("Fire duration (ms)", "fireDurationMs",
                   buttonConfig.fireDurationMs, 10, 10000, 10);
  s += F("<div id='mgRow'");
  if (buttonConfig.mode != 2 && buttonConfig.mode != 6) s += F(" style='display:none'");
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
         "<div class='live' data-live='cf'>reading device\u2026</div>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='confluence'>");
  s += connectedCheck("cf", confluenceConfig.connected);
  s += fireEnabledCheck("cffe", confluenceConfig.fireEnabled);
  s += F("<button type='submit'>Save</button></form></fieldset></section>");

  // --- Towers tab (sub-tabs + Apply-to-All + per-tower loop) ---
  s += F("<section class='tab' data-tab='towers' role='tabpanel' hidden>"
         "<h2>Tower Configs</h2>"
         "<fieldset><legend>Bus handover</legend>"
         "<p class='dmx-addr'>Stop transmitting DMX so a <strong>manual console or the Enttec "
         "can drive the fixtures</strong>. DMX has no arbitration &mdash; two transmitters on "
         "one pair garble each other &mdash; so this is how you hand the bus over without "
         "unplugging the controller. Every valve is driven shut and flushed to the wire before "
         "the transmitter goes silent, and the rig must be idle to start. "
         "<strong>This is global:</strong> a frame carries all 64 slots, so there is no "
         "per-tower quiet &mdash; untick <em>Connected</em> to zero one tower's channels "
         "instead. Quiet mode is never saved; a power cycle always restores normal output.</p>"
         "<div class='live' data-live='bus'>reading device\u2026</div>"
         "<div class='row'>"
         "<button type='button' id='quietBtn'>Go quiet &mdash; hand over the bus</button>"
         "<button type='button' id='resumeBtn' hidden>Resume transmitting</button>"
         "</div></fieldset>"
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
  // No Fire enabled here, for the same reason there is no Connected: an
  // unchecked box submits nothing, so an Apply-to-All would silently clear the
  // flag on all four towers at once.
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
           "<div class='live' data-live='t");
    s += i;
    s += F("'>reading device\u2026</div>"
           "<form method='POST' action='/set'>"
           "<input type='hidden' name='target' value='");
    s += i;
    s += F("'>");
    s += connectedCheck("c" + String(i), towerConfigs[i].connected);
    s += fireEnabledCheck("f" + String(i), towerConfigs[i].fireEnabled);
    s += themeSelect("theme", towerConfigs[i].themeName);
    s += rangeSlider("Brightness", "brightness", towerConfigs[i].bright, 0, 255);
    s += rangeSlider("Speed (%)", "speed", towerConfigs[i].speed, 10, 400, 10);
    s += F("<button type='submit'>Save</button></form></fieldset></div>");
  }
  s += F("</section>");

  // --- Firmware (OTA) tab ---
  // --- Audio tab ---
  s += F("<section class='tab' data-tab='audio' role='tabpanel' hidden>"
         "<h2>Audio</h2>"
         "<fieldset><legend>Link</legend>"
         "<p class='dmx-addr'>Audio node streams UDP feature packets to port "
         "<code>4210</code>. Lights react whenever the link is fresh; fire also "
         "needs ARM and an audio button mode (3-6).</p>"
         "<div id='audLink' class='audlink'>waiting for packets...</div>"
         "<div class='audbar'><div id='audBudget'></div></div>"
         "<p class='testfire-hint' id='audBudgetTxt'></p>"
         "</fieldset>"
         "<fieldset><legend>Arm</legend>"
         "<div class='arm-row'>"
         "<span class='arm-state' id='audArmState'>Disarmed</span>"
         "</div>"
         "<button type='button' id='audArmBtn'>ARM AUDIO FIRE</button>"
         "<button type='button' id='audDisarmBtn' class='disarmed'>DISARM</button>"
         "<p class='testfire-hint'>Arming persists until you disarm or the device "
         "reboots. There is no timer &mdash; the dead-man is the audio link itself: "
         "no fresh packets for <span id='audStaleTxt'>500</span> ms means no fire.</p>"
         "</fieldset>"
         "<fieldset><legend>Safety limits</legend>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='audio'>");
  s += rangeSlider("Duty ceiling %",      "audDutyPct",   audioConfig.dutyPct,   0, 50);
  s += rangeSlider("Duty window (ms)",    "audDutyWinMs", audioConfig.dutyWinMs, 2000, 60000, 1000);
  s += rangeSlider("Min gap (ms)",        "audMinGapMs",  audioConfig.minGapMs,  100, 5000, 50);
  s += rangeSlider("Max single open (ms)","audMaxOpenMs", audioConfig.maxOpenMs, 50, 3000, 50);
  s += rangeSlider("Shot length (ms)",    "audShotMs",    audioConfig.shotMs,    50, 2000, 10);
  s += rangeSlider("Lead / pre-fire (ms)","audLeadMs",    audioConfig.leadMs,    0, 500, 10);
  s += rangeSlider("Stale timeout (ms)",  "audStaleMs",   audioConfig.staleMs,   100, 2000, 50);
  s += F("<p class='testfire-hint'>Burst is hard-capped at 3000 ms whatever the "
         "duty window allows &mdash; no audio shot may put out more propane at once "
         "than one manual FIREBALL press. Lead should be MEASURED: film a shot at "
         "120fps and count frames from the LED to visible flame.</p>"
         "</form></fieldset>"
         "<fieldset><legend>Response</legend>"
         "<form method='POST' action='/set'>"
         "<input type='hidden' name='target' value='audio'>");
  s += rangeSlider("Bass open",   "audBassOn",     audioConfig.bassOn,     0, 255);
  s += rangeSlider("Bass close",  "audBassOff",    audioConfig.bassOff,    0, 255);
  s += rangeSlider("Beat min",    "audBeatMin",    audioConfig.beatMin,    0, 255);
  s += rangeSlider("Drop thresh", "audDropMin",    audioConfig.dropMin,    0, 255);
  s += rangeSlider("Drop gap ms", "audDropGapMs",  audioConfig.dropGapMs,  500, 30000, 500);
  s += rangeSlider("Drop shot ms","audDropShotMs", audioConfig.dropShotMs, 50, 2000, 10);
  s += F("<label>Light mode<select name='audLightMode'>");
  {
    const char* lm[][2] = {{"0","Off"},{"1","Pulse"},{"2","Bands"}};
    for (auto& o : lm) {
      s += F("<option value='"); s += o[0]; s += '\'';
      if (audioConfig.lightMode == (uint8_t)atoi(o[0])) s += F(" selected");
      s += '>'; s += o[1]; s += F("</option>");
    }
  }
  s += F("</select></label>");
  s += rangeSlider("Light depth", "audLightDepth", audioConfig.lightDepth, 0, 255);
  s += F("</form></fieldset></section>");

  s += F("<section class='tab' data-tab='firmware' role='tabpanel' hidden>"
         "<h2>Firmware Update</h2>"
         "<fieldset><legend>Upload over WiFi</legend>"
         "<p class='dmx-addr'>Upload <code>Test_Button_DMX.ino.bin</code> straight to the "
         "device over WiFi &mdash; no USB cable. Takes seconds. The device reboots itself "
         "when the upload finishes.</p>"
         "<p class='testfire-hint'><strong>The rig must be idle.</strong> An upload stops the "
         "DMX loop, so every valve is driven closed before it starts and the device refuses "
         "the upload unless the FSM is IDLE and no purge or morse is running.</p>"
         "<label>Firmware file<input type='file' id='fwFile' accept='.bin'></label>"
         "<div class='btn-row'>"
         "<button type='button' class='btn' id='fwUpload'>Upload firmware</button>"
         "</div>"
         "<div class='sr'><progress id='fwProgress' value='0' max='100' style='width:100%'>"
         "</progress><span class='val' id='fwPct'>0%</span></div>"
         "<p class='dmx-addr' id='fwStatus'>Idle.</p>"
         "</fieldset></section>");

  s += F("</main>");

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
         "var hint=document.getElementById('audModeHint');""function upd(){var v=sel.value;row.style.display=(v==='2'||v==='6')?'':'none';if(hint)hint.style.display=(v>='3'&&v<='6')?'':'none';}""sel.addEventListener('change',upd);upd();})();"
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
         // OTA upload. XMLHttpRequest, not fetch: fetch exposes no upload
         // progress, and the device reboots the instant it accepts the image,
         // so a dropped connection at ~100% is success rather than an error.
         "(function(){var f=document.getElementById('fwFile');"
         "var g=document.getElementById('fwUpload');"
         "var bar=document.getElementById('fwProgress');"
         "var pc=document.getElementById('fwPct');"
         "var st=document.getElementById('fwStatus');"
         "if(!f||!g)return;function say(m){st.textContent=m;}"
         "g.addEventListener('click',function(){"
         "if(!f.files||!f.files.length){say('Pick a .bin file first.');return;}"
         "var fl=f.files[0];say('Checking device state...');"
         "fetch('/api/state').then(function(r){return r.json();}).then(function(s){"
         "if(s.fsm.state!=='IDLE'||s.purge){"
         "say('Refused: device is not idle (fsm='+s.fsm.state+', purge='+s.purge+').');return;}"
         "var fd=new FormData();fd.append('firmware',fl,fl.name);"
         "var x=new XMLHttpRequest();x.open('POST','/api/update');"
         "x.upload.addEventListener('progress',function(e){if(!e.lengthComputable)return;"
         "var p=Math.round(e.loaded*100/e.total);bar.value=p;pc.textContent=p+'%';});"
         "x.addEventListener('load',function(){if(x.status===200){bar.value=100;pc.textContent='100%';"
         "say('Upload complete — device is rebooting. Reload in a few seconds.');}"
         "else{say('Failed (HTTP '+x.status+'): '+x.responseText);}g.disabled=false;});"
         "x.addEventListener('error',function(){if(bar.value>=99){"
         "say('Upload complete — device rebooting (connection dropped, expected).');}"
         "else{say('Upload failed at '+bar.value+'% — connection lost.');}g.disabled=false;});"
         "g.disabled=true;say('Uploading '+Math.round(fl.size/1024)+' KB...');x.send(fd);"
         "}).catch(function(e){say('Cannot reach device: '+e.message);});});})();"
         // --- Audio tab: arm latch + live link readout ---
         // Arming is a server-side LATCH, not press-and-hold, so this deliberately
         // does not reuse the setup() hold factory used by Test Fire and Purge.
         "(function(){"
         "var arm=document.getElementById('audArmBtn'),dis=document.getElementById('audDisarmBtn');"
         "var st=document.getElementById('audArmState'),lk=document.getElementById('audLink');"
         "var bar=document.getElementById('audBudget'),btx=document.getElementById('audBudgetTxt');"
         "var stale=document.getElementById('audStaleTxt');"
         "if(!arm)return;"
         "function post(u){return fetch(u,{method:'POST'}).then(refresh);}"
         "arm.onclick=function(){post('/api/audio/arm');};"
         "dis.onclick=function(){post('/api/audio/disarm');};"
         "function refresh(){return fetch('/api/state').then(function(r){return r.json();})"
         ".then(function(j){var a=j.audio;if(!a)return;"
         "st.textContent=a.armed?'ARMED':'Disarmed';"
         "st.className='arm-state'+(a.armed?' bad':'');"
         "if(stale)stale.textContent=a.cfg.staleMs;"
         "lk.innerHTML='link  '+(a.fresh?'<span class=ok>FRESH</span>':'<span class=bad>STALE</span>')"
         "+'   peer '+a.peer+'\\n'"
         "+'pkts '+a.packets+'  '+a.pps+'/s   gaps '+a.gaps+'   bad '+a.bad+'\\n'"
         "+'bass '+a.bass+'  mid '+a.mid+'  treb '+a.treble+'  lvl '+a.level+'\\n'"
         "+'bpm '+(a.bpm||'--')+'  beat '+(a.beatMs||'--')+'ms  '"
         "+(a.confident?'<span class=ok>grid locked</span>':'grid unlocked')"
         "+(a.beat?'   <b>BEAT</b>':'');"
         "var cap=a.dutyCapMs||0,used=a.dutyUsedMs||0;"
         "bar.style.width=(cap?Math.min(100,used*100/cap):0)+'%';"
         "btx.textContent='propane budget '+used+' / '+cap+' ms';"
         "});}"
         // Only poll while the Audio tab is actually visible: each request is
         // handled synchronously inside webTick(), so background polling would add
         // jitter to the DMX frame gate for no benefit.
         "setInterval(function(){"
         "var t=document.querySelector(\"section.tab[data-tab='audio']\");"
         "if(t&&!t.hidden)refresh();},500);"
         "refresh();"
         "})();"

         // ---- Live fixture state ----------------------------------------
         // Config forms above are rendered ONCE, at page build. Anything that
         // changes config afterwards (the API, a pytest run, another phone on
         // the AP) leaves them stale — and because a form posts EVERY field,
         // saving a stale form writes the stale values straight back. This
         // syncs each form from /api/state and prints what the device is
         // really doing. A tower with connected=false is skipped entirely in
         // the frame loop (.ino:209) and its channels are never written; on
         // the wire that is indistinguishable from a dead decoder, so it gets
         // a loud banner. notes.md Session 5 cost a replaced decoder to that.
         "(function(){"
         "var F=50,fm={},lv={};"
         "document.querySelectorAll('[data-live]').forEach(function(e){"
         "var k=e.getAttribute('data-live');lv[k]=e;"
         "var f=e.parentElement.querySelector(\"form[action='/set']\");if(!f)return;"
         "fm[k]=f;"
         "f.addEventListener('input',function(){f.dataset.dirty='1'});"
         "f.addEventListener('change',function(){f.dataset.dirty='1'});"
         "f.addEventListener('submit',function(){delete f.dataset.dirty});});"
         "function sf(f,n,v){"
         "var e=f.querySelector(\"[name='\"+n+\"']\");"
         "if(!e||e===document.activeElement)return;"
         "if(e.type==='checkbox'){e.checked=!!v;return}"
         "e.value=v;var b=e.nextElementSibling;"
         "if(b&&b.classList&&b.classList.contains('val'))b.textContent=v;}"
         "function by(c,s,n){"
         "return(!c||c.length<s-1+n)?'?':c.slice(s-1,s-1+n).join('/');}"
         // vf() — a solenoid channel carries 0 or 255 and nothing else, because
         // dmxShadowWrite() refuses anything in between. A mid-scale byte here is
         // therefore not an operator setting to explain but a firmware fault to
         // report. See docs/spec-solenoid-binary.md.
         "function vf(c,vc){"
         "if(!c||c.length<vc)return '';"
         "var v=c[vc-1];if(v===0||v===255)return '';"
         "return \"\\n<span class='warn'>CH\"+vc+' = '+v+' - a valve channel must be 0 or 255. '"
         "+'This should be impossible; the binary guard in dmx.cpp has been bypassed.</span>';}"
         "function rd(k,cfg,body,bad){"
         "var e=lv[k];if(!e)return;e.innerHTML=body;e.classList.toggle('bad',!!bad);"
         "var f=fm[k];if(!f)return;"
         "if(f.dataset.dirty){e.innerHTML+=\"\\n<span class='stale'>Form has unsaved edits \""
         "+\"- not syncing from the device. Save, or reload to discard.</span>\";return}"
         "for(var n in cfg)sf(f,n,cfg[n]);}"
         "function tick(s){"
         "var c=(s.dmx||{}).ch,b=s.button||{},q=!!(s.dmx||{}).quiet;"
         "var qb=document.getElementById('quietBtn'),rb=document.getElementById('resumeBtn');"
         "if(qb&&rb){qb.hidden=q;rb.hidden=!q;}"
         "if(lv.bus){lv.bus.innerHTML=q?"
         "\"<span class='warn'>TRANSMITTER QUIET - nothing is being sent. The bus is free for \""
         "+'another controller. Fixtures are holding the all-zero frame that was flushed '"
         "+'before going silent.</span>'"
         ":'transmitting   '+F+' ms frames, 64 slots';"
         "lv.bus.classList.toggle('bad',q);}"
         "(s.towers||[]).forEach(function(t,i){"
         "var B=4+i*15,l;"
         "if(!t.connected){"
         "l=\"<span class='warn'>DISCONNECTED - this tower is skipped in the DMX loop, so \""
         "+'channels '+(B+1)+'-'+(B+8)+' are never written and stay at 0. '"
         "+'Tick Connected and press Save.</span>';}"
         "else if(q){"
         "l=\"<span class='stale'>transmitter quiet - this tower's channels are composed but \""
         "+'not sent.</span>';}"
         "else{"
         "l='on air   decoder '+by(c,B+1,4)+'   uplight '+by(c,B+5,4);"
         "if(t.brightness===0)l+=\"\\n<span class='warn'>brightness 0 - strips stay black \""
         "+'(the uplight still lights during fire).</span>';"
         "if(!t.fireEnabled)l+=\"\\n<span class='warn'>fire disabled - CH\"+(B+4)"
         "+' stays shut. The lights keep running; only this tower propane is isolated.'"
         "+'</span>';"
         "l+=vf(c,B+4);}"
         "rd('t'+i,{connected:t.connected,fireEnabled:t.fireEnabled,theme:t.theme,"
         "brightness:t.brightness,speed:t.speed},l,"
         "!t.connected||t.brightness===0||!t.fireEnabled);});"
         "var q=s.confluence;"
         "if(q){var m,bad=false;"
         "if(!q.connected){"
         "m=\"<span class='warn'>DISCONNECTED - confluenceWrite() is skipped, so the central \""
         "+'solenoid on CH1 never fires.</span>';bad=true;}"
         "else if(q){"
         "m=\"<span class='stale'>transmitter quiet - CH1 is composed but not sent.</span>\";}"
         "else{m='on air   CH1 '+by(c,1,1);"
         "if(!q.fireEnabled){"
         "m+=\"\\n<span class='warn'>fire disabled - the central solenoid never opens.\""
         "+'</span>';bad=true;}"
         "m+=vf(c,1);}"
         // Fire duration belongs to the button, but it gates every valve on the
         // rig, so it is surfaced next to the solenoid it silences.
         "if(b.fireDurationMs!==undefined&&b.fireDurationMs<F){"
         "m+=\"\\n<span class='warn'>fire duration \"+b.fireDurationMs+' ms is shorter than one '"
         "+F+' ms DMX frame - a shot can only reach the wire as a single frame, far too '"
         "+'brief to light.</span>';bad=true;}"
         "rd('cf',{connected:q.connected,fireEnabled:q.fireEnabled},m,bad);}}"
         "function poll(){"
         "fetch('/api/state').then(function(r){return r.json()}).then(tick).catch(function(){"
         "Object.keys(lv).forEach(function(k){"
         "lv[k].textContent='device unreachable';lv[k].classList.add('bad');});});}"
         "var q0=document.getElementById('quietBtn'),r0=document.getElementById('resumeBtn');"
         "function qp(u,e){e.disabled=true;"
         "fetch(u,{method:'POST'}).then(function(r){"
         "return r.json().catch(function(){return{}}).then(function(j){"
         "if(!r.ok){lv.bus.innerHTML=\"<span class='warn'>refused - \"+(j.error||r.status)"
         "+'</span>';lv.bus.classList.add('bad');}});})"
         ".catch(function(){}).then(function(){e.disabled=false;poll();});}"
         "if(q0)q0.onclick=function(){qp('/api/dmx/quiet/start',q0)};"
         "if(r0)r0.onclick=function(){qp('/api/dmx/quiet/stop',r0)};"

         // Poll only while a tab holding a readout is visible — every request is
         // handled synchronously in webTick(), so background polling adds DMX
         // frame jitter for nothing.
         "setInterval(function(){"
         "var v=Array.prototype.some.call(document.querySelectorAll("
         "\"section.tab[data-tab='towers'],section.tab[data-tab='confluence']\"),"
         "function(t){return !t.hidden});"
         "if(v)poll();},1000);"
         "poll();"
         "})();"
         "</script></body></html>");
  return s;
}

// --- Handlers ---

static void handleRoot() {
  LOG_I("[WEB] GET /  client=%s", server.client().remoteIP().toString().c_str());
  server.send(200, "text/html", buildPage());
}

// Every audio field is clamped, unlike target=button. These govern propane duty, so a
// bad POST must not be able to widen the safety envelope.
static uint16_t clampU16(long v, uint16_t lo, uint16_t hi) {
  if (v < (long)lo) return lo;
  if (v > (long)hi) return hi;
  return (uint16_t)v;
}

static void applyAudioTarget() {
  if (server.hasArg("audShotMs"))
    audioConfig.shotMs = clampU16(server.arg("audShotMs").toInt(), AUD_SHOT_MIN, AUD_SHOT_MAX);
  if (server.hasArg("audMinGapMs"))
    audioConfig.minGapMs = clampU16(server.arg("audMinGapMs").toInt(), AUD_GAP_MIN, AUD_GAP_MAX);
  if (server.hasArg("audDutyPct"))
    audioConfig.dutyPct = (uint8_t)clampU16(server.arg("audDutyPct").toInt(), 0, AUD_DUTY_MAX);
  if (server.hasArg("audDutyWinMs"))
    audioConfig.dutyWinMs = clampU16(server.arg("audDutyWinMs").toInt(), AUD_WIN_MIN, AUD_WIN_MAX);
  if (server.hasArg("audMaxOpenMs"))
    audioConfig.maxOpenMs = clampU16(server.arg("audMaxOpenMs").toInt(), AUD_MAXOPEN_MIN, AUD_MAXOPEN_MAX);
  if (server.hasArg("audLeadMs"))
    audioConfig.leadMs = clampU16(server.arg("audLeadMs").toInt(), 0, AUD_LEAD_MAX);
  if (server.hasArg("audStaleMs"))
    audioConfig.staleMs = clampU16(server.arg("audStaleMs").toInt(), AUD_STALE_MIN, AUD_STALE_MAX);
  if (server.hasArg("audBassOn"))
    audioConfig.bassOn = (uint8_t)clampU16(server.arg("audBassOn").toInt(), 0, 255);
  if (server.hasArg("audBassOff"))
    audioConfig.bassOff = (uint8_t)clampU16(server.arg("audBassOff").toInt(), 0, 255);
  if (server.hasArg("audBeatMin"))
    audioConfig.beatMin = (uint8_t)clampU16(server.arg("audBeatMin").toInt(), 0, 255);
  if (server.hasArg("audDropMin"))
    audioConfig.dropMin = (uint8_t)clampU16(server.arg("audDropMin").toInt(), 0, 255);
  if (server.hasArg("audDropGapMs"))
    audioConfig.dropGapMs = clampU16(server.arg("audDropGapMs").toInt(), AUD_DROPGAP_MIN, AUD_DROPGAP_MAX);
  if (server.hasArg("audDropShotMs"))
    audioConfig.dropShotMs = clampU16(server.arg("audDropShotMs").toInt(), AUD_DROPSHOT_MIN, AUD_DROPSHOT_MAX);
  if (server.hasArg("audLightMode"))
    audioConfig.lightMode = (uint8_t)clampU16(server.arg("audLightMode").toInt(), 0, 2);
  if (server.hasArg("audLightDepth"))
    audioConfig.lightDepth = (uint8_t)clampU16(server.arg("audLightDepth").toInt(), 0, 255);

  // Hysteresis only works if the close threshold sits below the open threshold;
  // equal values would chatter the valve at the boundary.
  if (audioConfig.bassOff >= audioConfig.bassOn)
    audioConfig.bassOff = (audioConfig.bassOn > 8) ? (audioConfig.bassOn - 8) : 0;
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
    confluenceConfig.connected   = server.hasArg("connected");
    // Checkbox semantics, same as `connected`: absent means off. A legacy
    // client still posting fireLevel= gets no valve level, because there is no
    // longer one to set — see docs/spec-solenoid-binary.md.
    confluenceConfig.fireEnabled = server.hasArg("fireEnabled");

  } else if (target == "button") {
    // Clamped: mode now selects propane behaviour (3–6 are audio-driven), so an
    // out-of-range value is not a cosmetic bug. Unknown values fall back to FIREBALL.
    uint8_t mode = (uint8_t)server.arg("mode").toInt();
    buttonConfig.mode              = (mode > AUDIO_MODE_MAX) ? 0 : mode;
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
    // Apply-to-All covers the look only. fireEnabled is deliberately absent for
    // the same reason `connected` is: it is a per-fixture safety flag, and a
    // form that omits a checkbox would clear it for all four towers at once.
    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      towerConfigs[i].themeName = themeName;
      towerConfigs[i].bright    = bright;
      towerConfigs[i].speed     = speed;
    }

  } else if (target == "audio") {
    applyAudioTarget();

  } else {
    uint8_t idx = (uint8_t)target.toInt();
    if (idx < NUM_TOWERS) {
      uint16_t speed = (uint16_t)server.arg("speed").toInt();
      if (speed < 10 || speed > 400) speed = 100;
      towerConfigs[idx].connected   = server.hasArg("connected");
      towerConfigs[idx].fireEnabled = server.hasArg("fireEnabled");
      towerConfigs[idx].themeName   = server.arg("theme");
      towerConfigs[idx].bright      = (uint8_t)server.arg("brightness").toInt();
      towerConfigs[idx].speed       = speed;
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
  // Drop any audio-owned shot too, otherwise audio would still think it holds the
  // valve and would not re-trigger. Deliberately NOT a disarm: the test suite resets
  // between every test and would otherwise have to re-arm each time.
  audioAbortShot();
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

// DMX quiet mode — stop transmitting so a manual console or the Enttec can drive
// the bus. DMX has no arbitration, so two transmitters on one pair garble each
// other; this is how you hand the bus over without unplugging the M5.
//
// Same safety contract as OTA, and for the same reason: stopping frames makes every
// fixture latch its last commanded value, so a valve open at that moment would stay
// open with nothing left running to close it. Refuse unless the rig is provably
// idle, then drive everything to 0 ON THE WIRE before going silent.
static void handleApiDmxQuietStart() {
  LOG_I("[WEB] POST /api/dmx/quiet/start");
  String why;
  if (!rigSafeToStall(why)) {
    LOG_W("[DMX] quiet refused — %s", why.c_str());
    server.send(409, "application/json",
                String("{\"ok\":false,\"error\":\"") + why + "\"}");
    return;
  }
  rigForceEverythingClosed();
  dmxSetQuiet(true);
  LOG_I("[DMX] transmitter QUIET — bus handed over; power-cycle or /stop to resume");
  server.send(200, "application/json", "{\"ok\":true,\"quiet\":true}");
}

static void handleApiDmxQuietStop() {
  LOG_I("[WEB] POST /api/dmx/quiet/stop");
  dmxSetQuiet(false);
  LOG_I("[DMX] transmitter resumed");
  server.send(200, "application/json", "{\"ok\":true,\"quiet\":false}");
}

// Audio arming. A latch, not a hold — unlike the fire and purge buttons there is no
// release timer here, so STALENESS is the dead-man: no fresh packets means no fire
// regardless of this flag. RAM-only, false on every boot.
static void handleApiAudioArm() {
  LOG_I("[WEB] POST /api/audio/arm");
  audioArm();
  server.send(200);
}

static void handleApiAudioDisarm() {
  LOG_I("[WEB] POST /api/audio/disarm");
  audioDisarm();
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
  s.reserve(3072);  // grown by the audio block; exceeding this reallocs on a fragmenting heap
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

  s += F(",\"ota\":{\"inProgress\":");
  s += (otaInProgress() ? F("true") : F("false"));
  s += F(",\"lastError\":\"");
  s += otaLastError();
  s += F("\"}");

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
  s += F(",\"fireEnabled\":");
  s += confluenceConfig.fireEnabled ? F("true") : F("false");
  s += '}';

  // Audio block sits BEFORE the dmx block so the "]}}" terminator at the end of
  // this function stays untouched.
  {
    const AudioFeatures& a = audioSnapshot();
    uint32_t age = audioAgeMs();
    s += F(",\"audio\":{\"armed\":");
    s += (audioArmed() ? F("true") : F("false"));
    s += F(",\"fresh\":");
    s += (audioFresh() ? F("true") : F("false"));
    s += F(",\"peer\":\"");
    s += audioPeer().toString();
    s += F("\",\"port\":");
    s += AUDIO_UDP_PORT;
    s += F(",\"ageMs\":");
    s += (age == UINT32_MAX) ? -1 : (long)age;   // -1 = never heard from
    s += F(",\"pps\":");
    s += audioPps();
    s += F(",\"packets\":");
    s += audioPackets();
    s += F(",\"gaps\":");
    s += audioSeqGaps();
    s += F(",\"bad\":");
    s += audioBadPackets();
    s += F(",\"floods\":");
    s += audioFloodTicks();
    s += F(",\"bass\":");
    s += a.bass;
    s += F(",\"mid\":");
    s += a.mid;
    s += F(",\"treble\":");
    s += a.treble;
    s += F(",\"level\":");
    s += a.level;
    s += F(",\"bpm\":");
    s += a.bpm;
    s += F(",\"beatMs\":");
    s += a.beatIntervalMs;
    s += F(",\"beat\":");
    s += (audioBeatRecent(120) ? F("true") : F("false"));
    s += F(",\"confident\":");
    s += (audioBeatGridConfident() ? F("true") : F("false"));
    s += F(",\"shotActive\":");
    s += (audioShotActive() ? F("true") : F("false"));
    s += F(",\"dutyUsedMs\":");
    s += audioDutyUsedMs();
    s += F(",\"dutyCapMs\":");
    s += audioDutyCapMs();
    s += F(",\"cfg\":{\"shotMs\":");
    s += audioConfig.shotMs;
    s += F(",\"minGapMs\":");
    s += audioConfig.minGapMs;
    s += F(",\"dutyPct\":");
    s += audioConfig.dutyPct;
    s += F(",\"dutyWinMs\":");
    s += audioConfig.dutyWinMs;
    s += F(",\"maxOpenMs\":");
    s += audioConfig.maxOpenMs;
    s += F(",\"leadMs\":");
    s += audioConfig.leadMs;
    s += F(",\"staleMs\":");
    s += audioConfig.staleMs;
    s += F(",\"bassOn\":");
    s += audioConfig.bassOn;
    s += F(",\"bassOff\":");
    s += audioConfig.bassOff;
    s += F(",\"beatMin\":");
    s += audioConfig.beatMin;
    s += F(",\"dropMin\":");
    s += audioConfig.dropMin;
    s += F(",\"dropGapMs\":");
    s += audioConfig.dropGapMs;
    s += F(",\"dropShotMs\":");
    s += audioConfig.dropShotMs;
    s += F(",\"lightMode\":");
    s += audioConfig.lightMode;
    s += F(",\"lightDepth\":");
    s += audioConfig.lightDepth;
    s += F("}}");
  }

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
    s += F(",\"fireEnabled\":");
    s += towerConfigs[i].fireEnabled ? F("true") : F("false");
    s += '}';
  }
  s += ']';

  // `quiet` must be read alongside `ch`: the shadow buffer keeps composing frames
  // while the transmitter is muted, so `ch` is "what would be sent", not what is on
  // the wire. The UI labels it accordingly.
  s += F(",\"dmx\":{\"quiet\":");
  s += (dmxQuiet() ? F("true") : F("false"));
  s += F(",\"ch\":[");
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
  server.on("/api/dmx/quiet/start",  HTTP_POST, handleApiDmxQuietStart);
  server.on("/api/dmx/quiet/stop",   HTTP_POST, handleApiDmxQuietStop);
  // Registered explicitly as HTTP_POST: onNotFound 302s unknown routes to "/", so a
  // typo'd or unregistered path would look like success to fetch() rather than 404.
  server.on("/api/audio/arm",        HTTP_POST, handleApiAudioArm);
  server.on("/api/audio/disarm",     HTTP_POST, handleApiAudioDisarm);
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
  // OTA firmware upload — registered last so its route wins over onNotFound.
  otaRegister(server);

  server.begin();
  LOG_I("HTTP server + captive portal DNS started");
}

void webTick() {
  dns.processNextRequest();
  server.handleClient();
}
