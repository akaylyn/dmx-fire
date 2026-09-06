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
    INFO("Tower %d: connected=%-5s  fire=%-5s  bright=%3d  speed=%3d%%  theme=%s",
         i,
         towerConfigs[i].connected   ? "true" : "false",
         towerConfigs[i].fireEnabled ? "on"   : "OFF",
         towerConfigs[i].bright,
         towerConfigs[i].speed,
         towerConfigs[i].themeName.c_str());
    if (towerConfigs[i].connected) anyConnected = true;
    if (!towerConfigs[i].connected)
      FAIL("Tower " + String(i) + " is marked disconnected — skipped in DMX loop");
    if (!towerConfigs[i].fireEnabled)
      FAIL("Tower " + String(i) + " has fire disabled — its valve will not open");
    if (towerConfigs[i].bright < 8)
      FAIL("Tower " + String(i) + " brightness=" + String(towerConfigs[i].bright) + " — may be invisible; try ≥32");
  }
  if (anyConnected) PASS("At least one tower connected");
}

// ---- Confluence config --------------------------------------------------

static void testConfluenceConfig() {
  HEAD("Confluence config");
  INFO("connected=%s  fire=%s",
       confluenceConfig.connected   ? "true" : "false",
       confluenceConfig.fireEnabled ? "on"   : "OFF");
  if (!confluenceConfig.connected) FAIL("Confluence marked disconnected — solenoid will not fire");
  if (!confluenceConfig.fireEnabled) FAIL("Confluence fire disabled — the solenoid will not open even when the FSM fires");
  if (confluenceConfig.connected && confluenceConfig.fireEnabled) PASS("Confluence config OK");
}

// ---- DMX address map ----------------------------------------------------

static void testDmxAddresses() {
  HEAD("DMX universe map");
  INFO("Confluence : ch  1 –  4  (only ch 1 = solenoid)");
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    uint16_t base = 4 + (uint16_t)i * 15;
    INFO("Tower %d    : ch %2d – %2d decoder (R/G/B/FIRE),  ch %2d – %2d uplight (R/G/B/W),  ch %2d – %2d unclaimed",
         i, base + 1, base + 4, base + 5, base + 8, base + 9, base + 15);
  }
  PASS("Address map printed — verify fixtures are set to these channels");
  INFO("NOTE: uplights must be in 4-channel mode (R/G/B/W); start addresses unchanged");
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
  state.r  = state.g  = state.b  = 255;  // accumulator strips (capped in towerWrite)
  state.ur = state.ug = state.ub = 255;  // uplight RGB — separate fields since the fire-look split
  state.white    = 255;    // uplight white channel
  state.fireOpen = false;  // NEVER open the propane valves during a boot diagnostic

  uint32_t deadline = millis() + 500;
  while (millis() < deadline) {
    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      towerWrite(i, state);
    }
    confluenceWrite(false);  // don't open central solenoid during test
    dmxUpdate();
    delay(20);
  }

  // Return all to zero
  TowerState off = {};
  for (uint8_t i = 0; i < NUM_TOWERS; i++) towerWrite(i, off);
  confluenceWrite(false);
  dmxUpdate();

  PASS("Visual test complete — did the fixtures light up?");
}

// ---- Valve channel registry --------------------------------------------
// The valve list in dmx.cpp and the tower stride in towers.cpp are two
// independent statements of the same fact. This checks they still agree, so a
// future change to CHANNELS_PER_TOWER or NUM_TOWERS cannot silently leave a
// solenoid outside the set of channels the binary guard protects.

static void testValveChannelMap() {
  HEAD("Valve channel registry");

  String list;
  for (uint8_t i = 0; i < NUM_VALVE_CHANNELS; i++) {
    if (i) list += ", ";
    list += String(VALVE_CHANNELS[i]);
  }
  INFO("Registered valve channels: %s", list.c_str());

  bool ok = true;

  // Every tower valve derived from the stride must be in the registry.
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    uint16_t ch = towerValveChannel(i);
    if (!dmxIsValveChannel(ch)) {
      FAIL("Tower " + String(i) + " valve CH" + String(ch) +
           " is NOT in VALVE_CHANNELS — the stride and the registry disagree");
      ok = false;
    }
  }
  if (!dmxIsValveChannel(1)) {
    FAIL("Confluence CH1 is not registered as a valve channel");
    ok = false;
  }

  // ...and nothing else may be. A colour channel wrongly marked as a valve
  // would have its dimming refused, which is a visible fault, not a safe one.
  const uint16_t notValves[] = { 2, 3, 4, 7, 9, 12, 22, 24, 37, 39, 52, 54, 64 };
  for (uint8_t i = 0; i < sizeof(notValves) / sizeof(notValves[0]); i++) {
    if (dmxIsValveChannel(notValves[i])) {
      FAIL("CH" + String(notValves[i]) + " is wrongly registered as a valve");
      ok = false;
    }
  }

  if (ok) PASS("Valve registry matches the tower stride; no colour channel is flagged");
}

// ---- Binary valve guard -------------------------------------------------
// The unit proof that a solenoid channel cannot carry a partial byte. Runs
// against the real shadow buffer, on a valve channel, and leaves it closed.
//
// Safe despite briefly setting CH8 to 255: nothing here calls dmxUpdate(), and
// runDiagnostics() is invoked at the end of setup() before loop() starts, so no
// frame can be emitted while the buffer holds the open byte. Do not add a
// dmxUpdate() to this function.

static void testValveGuardRefusesPartial() {
  HEAD("Binary valve guard (0 or 255 only)");

  const uint16_t ch  = towerValveChannel(0);   // CH8
  const uint16_t idx = ch - 1;                 // dmxLastFrame is 0-indexed

  dmxValveWrite(ch, false);
  if (dmxLastFrame[idx] != VALVE_CLOSED) {
    FAIL("dmxValveWrite(ch, false) left CH" + String(ch) + " at " + String(dmxLastFrame[idx]));
  }

  // The whole point: a mid-scale byte must not reach a valve channel.
  // Expect one "[DMX] refused" line on the console for each of these.
  const uint8_t partials[] = { 1, 64, 127, 128, 200, 254 };
  bool refused = true;
  for (uint8_t i = 0; i < sizeof(partials) / sizeof(partials[0]); i++) {
    dmxShadowWrite(partials[i], ch);
    if (dmxLastFrame[idx] != VALVE_CLOSED) {
      FAIL("CH" + String(ch) + " accepted partial value " + String(partials[i]));
      refused = false;
      dmxShadowWrite(VALVE_CLOSED, ch);   // put it back before the next attempt
    }
  }
  if (refused) PASS("Valve channel refused every partial byte (1/64/127/128/200/254)");

  // Both legal values still get through.
  dmxValveWrite(ch, true);
  if (dmxLastFrame[idx] == VALVE_OPEN) PASS("dmxValveWrite(ch, true) -> 255");
  else FAIL("dmxValveWrite(ch, true) left CH" + String(ch) + " at " + String(dmxLastFrame[idx]));

  dmxValveWrite(ch, false);
  if (dmxLastFrame[idx] == VALVE_CLOSED) PASS("dmxValveWrite(ch, false) -> 0");
  else FAIL("dmxValveWrite(ch, false) left CH" + String(ch) + " at " + String(dmxLastFrame[idx]));

  // A colour channel in the same block must still dim normally.
  dmxShadowWrite(128, ch - 1);   // decoder CH3 (Blue)
  if (dmxLastFrame[ch - 2] == 128) PASS("Non-valve channel still accepts mid-scale values");
  else FAIL("Guard leaked onto a colour channel — CH" + String(ch - 1) + " rejected 128");
  dmxShadowWrite(0, ch - 1);

  // Leave the bus as we found it: valve shut.
  dmxValveWrite(ch, false);
}

// ---- Public entry point -------------------------------------------------

void runDiagnostics() {
  LOG_I("========== DMX Fire Diagnostics ==========");
  testWifi();
  testTowerConfig();
  testConfluenceConfig();
  testDmxAddresses();
  testValveChannelMap();
  testValveGuardRefusesPartial();
  testStorage();
  testFsm();
  testDmxVisual();
  LOG_I("========== Diagnostics complete ==========");
}
