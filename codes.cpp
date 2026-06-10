#include "codes.h"
#include "storage.h"
#include <ArduinoJson.h>

static KeypadCode g_codes[HG_MAX_CODES];
static int        g_count = 0;

void Codes::load() {
  g_count = Storage::loadCodes(g_codes, HG_MAX_CODES);
}

int Codes::count() { return g_count; }

bool Codes::check(const String& code) {
  for (int i = 0; i < g_count; i++) {
    if (code.equals(g_codes[i].code)) return true;
  }
  return false;
}

bool Codes::add(const String& code, const char* label) {
  if (code.length() < 4 || code.length() >= HG_CODE_LEN) return false;
  if (g_count >= HG_MAX_CODES) return false;
  for (int i = 0; i < g_count; i++)                // sem duplicar
    if (code.equals(g_codes[i].code)) return false;
  strlcpy(g_codes[g_count].code,  code.c_str(),            sizeof(g_codes[g_count].code));
  strlcpy(g_codes[g_count].label, (label && *label) ? label : "sem nome", sizeof(g_codes[g_count].label));
  g_count++;
  Storage::saveCodes(g_codes, g_count);
  return true;
}

bool Codes::remove(const String& code) {
  for (int i = 0; i < g_count; i++) {
    if (code.equals(g_codes[i].code)) {
      for (int j = i; j < g_count - 1; j++) g_codes[j] = g_codes[j + 1];
      g_count--;
      Storage::saveCodes(g_codes, g_count);
      return true;
    }
  }
  return false;
}

// Lista só rótulos e tamanho — NUNCA expõe o valor das senhas.
String Codes::listJson() {
  JsonDocument doc;
  doc["count"] = g_count;
  JsonArray arr = doc["codes"].to<JsonArray>();
  for (int i = 0; i < g_count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["label"]  = g_codes[i].label;
    o["digits"] = (int)strlen(g_codes[i].code);
  }
  String out; serializeJson(doc, out);
  return out;
}
