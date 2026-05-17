#include <Preferences.h>
#include "towers.h"
#include "confluence.h"
#include "button_fsm.h"
#include "palettes.h"
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

    snprintf(key, sizeof(key), "t%df", i);
    towerConfigs[i].flameLevel = prefs.getUChar(key, 255);

    // palName must be fetched into a String; pal is derived from it
    snprintf(key, sizeof(key), "t%dp", i);
    towerConfigs[i].palName = prefs.getString(key, "green");
    towerConfigs[i].pal     = palFromName(towerConfigs[i].palName);
  }

  confluenceConfig.connected = prefs.getBool("cfcon", true);
  confluenceConfig.fireLevel = prefs.getUChar("cffl",  255);

  buttonConfig.mode              = prefs.getUChar("btnmode",    0);
  buttonConfig.fireDurationMs    = prefs.getUShort("btnfire",   3000);
  buttonConfig.cooldownMs        = prefs.getUShort("btncool",   10000);
  buttonConfig.endCuePattern     = prefs.getUChar("btncue",     0);
  buttonConfig.machineGunBurstMs = prefs.getUShort("btnmgburst", 200);

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

    snprintf(key, sizeof(key), "t%df", i);
    prefs.putUChar(key, towerConfigs[i].flameLevel);

    snprintf(key, sizeof(key), "t%dp", i);
    prefs.putString(key, towerConfigs[i].palName);
  }

  prefs.putBool("cfcon",    confluenceConfig.connected);
  prefs.putUChar("cffl",    confluenceConfig.fireLevel);

  prefs.putUChar("btnmode",     buttonConfig.mode);
  prefs.putUShort("btnfire",    buttonConfig.fireDurationMs);
  prefs.putUShort("btncool",    buttonConfig.cooldownMs);
  prefs.putUChar("btncue",      buttonConfig.endCuePattern);
  prefs.putUShort("btnmgburst", buttonConfig.machineGunBurstMs);

  prefs.end();
}
