#include "morse.h"
#include "confluence.h"
#include "log.h"

uint16_t morseUnitMs = 150;

// Sequence is one char per Morse unit: '1' = fire on, '0' = fire off.
// A dot is "1", a dash is "111", elements within a letter are separated
// by "0", letters by "000", words by "0000000".
static String   g_seq;
static uint32_t g_startMs;
static bool     g_active = false;

static const char* lookup(char c) {
  static const char* letters[26] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",
    "....", "..",   ".---", "-.-",  ".-..", "--",   "-.",
    "---",  ".--.", "--.-", ".-.",  "...",  "-",    "..-",
    "...-", ".--",  "-..-", "-.--", "--..",
  };
  static const char* digits[10] = {
    "-----", ".----", "..---", "...--", "....-",
    ".....", "-....", "--...", "---..", "----.",
  };
  if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
  if (c >= '0' && c <= '9') return digits[c - '0'];
  return nullptr;
}

bool morseStart(const String& text) {
  g_seq = "";
  g_seq.reserve(text.length() * 12);

  for (uint16_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c >= 'a' && c <= 'z') c -= 32;

    if (c == ' ') {
      // Word gap = 7 units total. If we already wrote a letter-gap, extend it.
      while (g_seq.length() && g_seq[g_seq.length()-1] == '0') g_seq.remove(g_seq.length()-1);
      if (g_seq.length()) g_seq += "0000000";
      continue;
    }

    const char* code = lookup(c);
    if (!code) continue;
    for (uint16_t j = 0; code[j]; j++) {
      g_seq += (code[j] == '-') ? "111" : "1";
      if (code[j+1]) g_seq += "0";
    }
    g_seq += "000";
  }

  while (g_seq.length() && g_seq[g_seq.length()-1] == '0') g_seq.remove(g_seq.length()-1);

  if (g_seq.length() == 0) {
    LOG_I("[MORSE] no codable characters in input");
    return false;
  }

  g_startMs = millis();
  g_active  = true;
  LOG_I("[MORSE] starting: %u units * %u ms = %lu ms total",
        (unsigned)g_seq.length(), (unsigned)morseUnitMs,
        (unsigned long)g_seq.length() * morseUnitMs);
  return true;
}

void morseStop() {
  if (g_active) { LOG_I("[MORSE] stopped"); g_active = false; }
}

bool morseActive() { return g_active; }

uint8_t morseTick() {
  if (!g_active) return 0;
  uint32_t elapsed = millis() - g_startMs;
  uint32_t idx     = elapsed / morseUnitMs;
  if (idx >= g_seq.length()) {
    LOG_I("[MORSE] playback complete");
    g_active = false;
    return 0;
  }
  return (g_seq[idx] == '1') ? confluenceConfig.fireLevel : 0;
}
