#include <Preferences.h>
#include "towers.h"
#include "confluence.h"
#include "button_fsm.h"
#include "audio.h"
#include "storage.h"

static Preferences prefs;

void storageLoad() {
  prefs.begin("dmxfire", true);  // read-only

  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    char key[4];

    snprintf(key, sizeof(key), "t%dc", i);
    towerConfigs[i].connected = prefs.getBool(key, true);

    snprintf(key, sizeof(key), "t%db", i);
    towerConfigs[i].bright = prefs.getUChar(key, 128);

    // Key is t<N>v, NOT the old t<N>f. That key holds a UChar (the retired
    // flameLevel byte); getBool() on a type-mismatched key silently returns the
    // default, which is too quiet a thing to depend on for a propane setting.
    // storageSave() removes the stale key. See docs/spec-solenoid-binary.md.
    snprintf(key, sizeof(key), "t%dv", i);
    towerConfigs[i].fireEnabled = prefs.getBool(key, true);

    snprintf(key, sizeof(key), "t%ds", i);
    towerConfigs[i].speed = prefs.getUShort(key, 100);

    // Key renamed t%dp → t%dh as part of the palette→theme migration; any
    // previously-saved palette name is intentionally not migrated and the
    // tower resets to the default "green" theme on first boot after upload.
    snprintf(key, sizeof(key), "t%dh", i);
    towerConfigs[i].themeName = prefs.getString(key, "green");
  }

  confluenceConfig.connected = prefs.getBool("cfcon", true);
  confluenceConfig.fireEnabled = prefs.getBool("cffe", true);   // was UChar "cffl"

  buttonConfig.mode              = prefs.getUChar("btnmode",    0);
  buttonConfig.fireDurationMs    = prefs.getUShort("btnfire",   3000);
  buttonConfig.cooldownMs        = prefs.getUShort("btncool",   10000);
  buttonConfig.endCuePattern     = prefs.getUChar("btncue",     0);
  buttonConfig.endCueMs          = prefs.getUShort("btncuems",  1000);
  buttonConfig.machineGunBurstMs = prefs.getUShort("btnmgburst", 200);

  // Uplight fire look — global, defaults to amber with white off.
  buttonConfig.fireUpR = prefs.getUChar("fireupr", 255);
  buttonConfig.fireUpG = prefs.getUChar("fireupg", 110);
  buttonConfig.fireUpB = prefs.getUChar("fireupb", 0);
  buttonConfig.fireUpW = prefs.getUChar("fireupw", 0);

  // mode now selects propane behaviour (3–6 are audio-driven), so an out-of-range
  // value from an older build or a bad POST must not survive a reboot.
  if (buttonConfig.mode > AUDIO_MODE_MAX) buttonConfig.mode = 0;

  // Audio node. `armed` is deliberately absent — it is RAM-only and false every boot.
  audioConfig.shotMs     = prefs.getUShort("audshot",     150);
  audioConfig.minGapMs   = prefs.getUShort("audgap",      200);
  audioConfig.dutyPct    = prefs.getUChar("audduty",      40);
  audioConfig.dutyWinMs  = prefs.getUShort("audwin",      10000);
  audioConfig.maxOpenMs  = prefs.getUShort("audmaxopen",  1000);
  audioConfig.leadMs     = prefs.getUShort("audlead",     120);
  audioConfig.staleMs    = prefs.getUShort("audstale",    500);
  audioConfig.bassOn     = prefs.getUChar("audbasson",    170);
  audioConfig.bassOff    = prefs.getUChar("audbassoff",   140);
  audioConfig.beatMin    = prefs.getUChar("audbeatmin",   90);
  audioConfig.dropMin    = prefs.getUChar("auddropmin",   200);
  audioConfig.dropGapMs  = prefs.getUShort("auddropgap",  3000);
  audioConfig.dropShotMs = prefs.getUShort("auddropshot", 400);
  audioConfig.lightMode  = prefs.getUChar("audlmode",     1);
  audioConfig.lightDepth = prefs.getUChar("audldepth",    150);

  prefs.end();
}

void storageSave() {
  prefs.begin("dmxfire", false);  // read-write

  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    char key[4];

    snprintf(key, sizeof(key), "t%dc", i);
    prefs.putBool(key, towerConfigs[i].connected);

    snprintf(key, sizeof(key), "t%db", i);
    prefs.putUChar(key, towerConfigs[i].bright);

    snprintf(key, sizeof(key), "t%dv", i);
    prefs.putBool(key, towerConfigs[i].fireEnabled);

    // Shed the retired flameLevel byte on the first config write after upload,
    // so a rig that has run the old firmware does not need scripts/flash.sh
    // --erase to clear it. remove() on an absent key is a no-op.
    snprintf(key, sizeof(key), "t%df", i);
    prefs.remove(key);

    snprintf(key, sizeof(key), "t%ds", i);
    prefs.putUShort(key, towerConfigs[i].speed);

    snprintf(key, sizeof(key), "t%dh", i);
    prefs.putString(key, towerConfigs[i].themeName);
  }

  prefs.putBool("cfcon",    confluenceConfig.connected);
  prefs.putBool("cffe",     confluenceConfig.fireEnabled);
  prefs.remove("cffl");   // retired fireLevel byte — see the tower loop above

  prefs.putUChar("btnmode",     buttonConfig.mode);
  prefs.putUShort("btnfire",    buttonConfig.fireDurationMs);
  prefs.putUShort("btncool",    buttonConfig.cooldownMs);
  prefs.putUChar("btncue",      buttonConfig.endCuePattern);
  prefs.putUShort("btncuems",   buttonConfig.endCueMs);
  prefs.putUShort("btnmgburst", buttonConfig.machineGunBurstMs);

  prefs.putUChar("fireupr", buttonConfig.fireUpR);
  prefs.putUChar("fireupg", buttonConfig.fireUpG);
  prefs.putUChar("fireupb", buttonConfig.fireUpB);
  prefs.putUChar("fireupw", buttonConfig.fireUpW);

  prefs.putUShort("audshot",     audioConfig.shotMs);
  prefs.putUShort("audgap",      audioConfig.minGapMs);
  prefs.putUChar("audduty",      audioConfig.dutyPct);
  prefs.putUShort("audwin",      audioConfig.dutyWinMs);
  prefs.putUShort("audmaxopen",  audioConfig.maxOpenMs);
  prefs.putUShort("audlead",     audioConfig.leadMs);
  prefs.putUShort("audstale",    audioConfig.staleMs);
  prefs.putUChar("audbasson",    audioConfig.bassOn);
  prefs.putUChar("audbassoff",   audioConfig.bassOff);
  prefs.putUChar("audbeatmin",   audioConfig.beatMin);
  prefs.putUChar("auddropmin",   audioConfig.dropMin);
  prefs.putUShort("auddropgap",  audioConfig.dropGapMs);
  prefs.putUShort("auddropshot", audioConfig.dropShotMs);
  prefs.putUChar("audlmode",     audioConfig.lightMode);
  prefs.putUChar("audldepth",    audioConfig.lightDepth);

  prefs.end();
}
