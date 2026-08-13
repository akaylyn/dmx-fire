#include <Update.h>
#include "ota.h"
#include "dmx.h"
#include "towers.h"
#include "confluence.h"
#include "button_fsm.h"
#include "morse.h"
#include "log.h"

static bool   g_inProgress = false;
static String g_lastError;
static size_t g_received = 0;

bool otaInProgress()      { return g_inProgress; }
const char* otaLastError() { return g_lastError.c_str(); }

// An OTA is only safe from a fully idle rig. Anything that could have a valve
// open — or open one while the main loop is stalled — blocks the upload.
static bool safeToStart(String& why) {
  if (fsmState != FSM_IDLE) {
    why = String("FSM is ") + fsmStateName(fsmState) + ", must be IDLE";
    return false;
  }
  if (purgeActive()) { why = "purge is active"; return false; }
  if (morseActive()) { why = "morse is playing"; return false; }
  return true;
}

// Drive every channel to 0 and push real frames before the loop stalls.
//
// DMX fixtures hold their last commanded value when the signal stops, and an
// upload stops it for many seconds. Zeroing the shadow buffer alone is not
// enough — the bytes have to reach the wire, so this emits several frames,
// paced past the TX-drain guard in dmxUpdate().
static void forceEverythingClosed() {
  TowerState off = {};
  for (uint8_t i = 0; i < NUM_TOWERS; i++) towerWrite(i, off);
  confluenceWrite(0);
  for (uint8_t i = 0; i < 4; i++) {
    dmxUpdate();
    delay(DMX_FRAME_INTERVAL_MS);
  }
  LOG_I("[OTA] all valves driven to 0 and flushed to the bus");
}

void otaRegister(WebServer& server) {
  // POST /api/update — multipart upload of Test_Button_DMX.ino.bin.
  // The second callback streams the body; the first runs once it completes.
  server.on(
    "/api/update", HTTP_POST,
    [&server]() {
      // Completion handler. Runs after the upload callback has seen the body.
      bool ok = !Update.hasError() && g_lastError.isEmpty();
      if (ok) {
        server.sendHeader("Connection", "close");
        server.send(200, "application/json",
                    String("{\"ok\":true,\"bytes\":") + g_received + "}");
        LOG_I("[OTA] update OK (%u bytes) — restarting", (unsigned)g_received);
        delay(200);          // let the response actually leave the socket
        ESP.restart();
      } else {
        server.sendHeader("Connection", "close");
        server.send(500, "application/json",
                    String("{\"ok\":false,\"error\":\"") + g_lastError + "\"}");
        LOG_E("[OTA] update FAILED: %s", g_lastError.c_str());
      }
      g_inProgress = false;
    },
    [&server]() {
      HTTPUpload& up = server.upload();

      if (up.status == UPLOAD_FILE_START) {
        g_lastError = "";
        g_received  = 0;

        String why;
        if (!safeToStart(why)) {
          g_lastError = why;
          LOG_E("[OTA] refused: %s", why.c_str());
          return;            // never call Update.begin(); body is discarded
        }

        g_inProgress = true;
        forceEverythingClosed();

        LOG_I("[OTA] starting update from %s", up.filename.c_str());
        // Size is unknown up front with chunked multipart; Update rolls into
        // whichever OTA slot is not currently running.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          g_lastError = "Update.begin failed (no free OTA slot?)";
          LOG_E("[OTA] %s", g_lastError.c_str());
          g_inProgress = false;
        }

      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!g_lastError.isEmpty()) return;   // rejected — swallow the rest
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
          g_lastError = "flash write failed";
          LOG_E("[OTA] %s at %u bytes", g_lastError.c_str(), (unsigned)g_received);
          return;
        }
        g_received += up.currentSize;
        // One line per ~128 KB: enough to watch progress, not enough to flood.
        static size_t lastLogged = 0;
        if (g_received - lastLogged >= 131072) {
          lastLogged = g_received;
          LOG_I("[OTA] %u KB received", (unsigned)(g_received / 1024));
        }

      } else if (up.status == UPLOAD_FILE_END) {
        if (!g_lastError.isEmpty()) return;
        if (!Update.end(true)) {                  // true = set the boot slot
          g_lastError = String("Update.end failed: ") + Update.errorString();
          LOG_E("[OTA] %s", g_lastError.c_str());
        }

      } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        g_lastError  = "upload aborted";
        g_inProgress = false;
        LOG_E("[OTA] aborted after %u bytes", (unsigned)g_received);
      }
    });
}
