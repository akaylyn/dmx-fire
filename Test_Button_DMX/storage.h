#pragma once

// Load all config from NVS into towerConfigs, confluenceConfig, buttonConfig.
// Call in setup() before webSetup() so the UI reflects persisted state.
void storageLoad();

// Persist all config to NVS. Call after any web config change.
void storageSave();
