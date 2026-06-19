#include <Arduino.h>
#include <WiFi.h>
#include "log.h"
#include "towers.h"
#include "confluence.h"
#include "button_fsm.h"
#include "storage.h"
#include "dmx.h"
#include "tests.h"

#define PASS(msg)       LOG_I("  [PASS] %s", msg)
#define FAIL(msg)       LOG_E("  [FAIL] %s", msg)
#define INFO(fmt, ...)  LOG_I("  " fmt, ##__VA_ARGS__)
#define HEAD(msg)       LOG_I("\n-- %s --", msg)

// ---- WiFi ---------------------------------------------------------------

static void testWifi() {
  HEAD("WiFi AP");
  IPAddress ip = WiFi.softAPIP();
  INFO("SSID : %s", WiFi.softAPSSID().c_str());
  INFO("IP   : %s", ip.toString().c_str());
  INFO("Clients: %d", WiFi.softAPgetStationNum());
  if (ip[0] != 0)
    PASS("AP running — connect to the WiFi network above, then open http://192.168.4.1");
  else
    FAIL("AP IP is 0.0.0.0 — softAP did not start");
}

// ---- Tower config -------------------------------------------------------

static void testTowerConfig() {
  HEAD("Tower config (loaded from NVS)");
  bool anyConnected = false;
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    INFO("Tower %d: connected=%-5s  bright=%3d  speed=%3d%%  flameLevel=%3d  theme=%s",
         i,
         towerConfigs[i].connected ? "true" : "false",
         towerConfigs[i].bright,
         towerConfigs[i].speed,
         towerConfigs[i].flameLevel,
         towerConfigs[i].themeName.c_str());
    if (towerConfigs[i].connected) anyConnected = true;
    if (!towerConfigs[i].connected)
      FAIL("Tower " + String(i) + " is marked disconnected — skipped in DMX loop");
    if (towerConfigs[i].bright < 8)
      FAIL("Tower " + String(i) + " brightness=" + String(towerConfigs[i].bright) + " — may be invisible; try ≥32");
  }
  if (anyConnected) PASS("At least one tower connected");
}

// ---- Confluence config --------------------------------------------------

static void testConfluenceConfig() {
  HEAD("Confluence config");
  INFO("connected=%s  fireLevel=%d",
       confluenceConfig.connected ? "true" : "false",
       confluenceConfig.fireLevel);
  if (!confluenceConfig.connected) FAIL("Confluence marked disconnected — solenoid will not fire");
  if (confluenceConfig.fireLevel == 0) FAIL("fireLevel=0 — solenoid will not open even when FSM fires");
  if (confluenceConfig.connected && confluenceConfig.fireLevel > 0) PASS("Confluence config OK");
}

// ---- DMX address map ----------------------------------------------------

static void testDmxAddresses() {
  HEAD("DMX universe map");
  INFO("Confluence : ch  1 –  4  (only ch 4 = solenoid)");
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    uint16_t base    = 4 + (uint16_t)i * 15;
    uint16_t decEnd  = base + 4;
    uint16_t strobeS = base + 5;
    uint16_t strobeE = base + 15;
    INFO("Tower %d    : ch %2d – %2d decoder (R/G/B/W),  ch %2d – %2d strobe",
         i, base + 1, decEnd, strobeS, strobeE);
  }
  PASS("Address map printed — verify fixtures are DIP-switched to these channels");
  INFO("NOTE: towers moved +4 from previous sketch to make room for Confluence on ch 1-4");
}

// ---- Storage round-trip -------------------------------------------------

static void testStorage() {
  HEAD("Storage (NVS round-trip)");
  uint8_t saved = towerConfigs[0].bright;
  towerConfigs[0].bright = 99;
  storageSave();
  towerConfigs[0].bright = 0;
  storageLoad();
  if (towerConfigs[0].bright == 99) {
    PASS("NVS write→read round-trip OK");
  } else {
    FAIL("NVS round-trip FAILED — expected 99, got " + String(towerConfigs[0].bright));
  }
  // Restore original value
  towerConfigs[0].bright = saved;
  storageSave();
}

// ---- FSM transitions ----------------------------------------------------

static void testFsm() {
  HEAD("Button FSM");
  FsmState before = fsmState;

  // Reset only the FSM state — NOT buttonConfig, which was just loaded from NVS.
  buttonInjectReset();
  if (fsmState == FSM_IDLE) PASS("FSM initialises to IDLE");
  else                       FAIL("FSM did not initialise to IDLE");

  // Press → FIRE_ACTIVE
  buttonFsmTick(/*wasPressed=*/true, false, true);
  if (fsmState == FSM_FIRE_ACTIVE) PASS("IDLE + press → FSM_FIRE_ACTIVE");
  else                              FAIL("Press did not transition to FIRE_ACTIVE");

  // Press during FIRE_ACTIVE — should stay
  buttonFsmTick(true, false, true);
  if (fsmState == FSM_FIRE_ACTIVE) PASS("Extra press during FIRE_ACTIVE is ignored");
  else                              FAIL("FIRE_ACTIVE broken by extra press");

  // Reset FSM — restore to IDLE for normal operation
  buttonInjectReset();
  INFO("FSM reset to IDLE for normal operation");
}

// ---- DMX visual test ----------------------------------------------------
// Writes full-brightness white to all towers for ~500 ms so you can see
// whether the fixtures actually respond to the new DMX addresses.

static void testDmxVisual() {
  HEAD("DMX visual test (500 ms full-white on all towers)");
  INFO("Watch fixtures — if none respond, they are likely DIP-switched to old addresses");

  TowerState state = {};
  state.masterDim = 255;
  state.r = state.g = state.b = 255;
  state.white = 255;   // uplight white channel
  state.fire  = 0;     // NEVER open the propane valves during a boot diagnostic

  uint32_t deadline = millis() + 500;
  while (millis() < deadline) {
    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      towerWrite(i, state);
    }
    confluenceWrite(0);  // don't open central solenoid during test
    dmxDevice.update();
    delay(20);
  }

  // Return all to zero
  TowerState off = {};
  for (uint8_t i = 0; i < NUM_TOWERS; i++) towerWrite(i, off);
  confluenceWrite(0);
  dmxDevice.update();

  PASS("Visual test complete — did the fixtures light up?");
}

// ---- Public entry point -------------------------------------------------

void runDiagnostics() {
  LOG_I("========== DMX Fire Diagnostics ==========");
  testWifi();
  testTowerConfig();
  testConfluenceConfig();
  testDmxAddresses();
  testStorage();
  testFsm();
  testDmxVisual();
  LOG_I("========== Diagnostics complete ==========");
}
