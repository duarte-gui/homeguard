#include "storage.h"
#include <Preferences.h>
#include <ArduinoJson.h>

static const char* NVS_NS = "homeguard";

Config Storage::loadConfig() {
  Config cfg{};
  // Padrões seguros
  cfg.mqttPort        = 1883;
  cfg.sirenEnabled    = true;
  cfg.sirenTimeoutSec = 180;   // 3 min por padrão
  cfg.configured      = false;

  Preferences p;
  p.begin(NVS_NS, true);
  if (p.getBool("ok", false)) {
    p.getString("ssid",     cfg.ssid,     sizeof(cfg.ssid));
    p.getString("wifiPass", cfg.wifiPass, sizeof(cfg.wifiPass));
    p.getString("mqttHost", cfg.mqttHost, sizeof(cfg.mqttHost));
    cfg.mqttPort = p.getUShort("mqttPort", 1883);
    p.getString("mqttUser", cfg.mqttUser, sizeof(cfg.mqttUser));
    p.getString("mqttPass", cfg.mqttPass, sizeof(cfg.mqttPass));
    p.getString("haPin",    cfg.haPin,    sizeof(cfg.haPin));
    p.getString("botToken", cfg.botToken, sizeof(cfg.botToken));
    p.getString("chatId",   cfg.chatId,   sizeof(cfg.chatId));
    cfg.sirenEnabled    = p.getBool("sirenEn", true);
    cfg.sirenTimeoutSec = p.getUShort("sirenTo", 180);
    cfg.configured      = (cfg.ssid[0] != '\0');
  }
  p.end();
  return cfg;
}

void Storage::saveConfig(const Config& cfg) {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putString("ssid",     cfg.ssid);
  p.putString("wifiPass", cfg.wifiPass);
  p.putString("mqttHost", cfg.mqttHost);
  p.putUShort("mqttPort", cfg.mqttPort);
  p.putString("mqttUser", cfg.mqttUser);
  p.putString("mqttPass", cfg.mqttPass);
  p.putString("haPin",    cfg.haPin);
  p.putString("botToken", cfg.botToken);
  p.putString("chatId",   cfg.chatId);
  p.putBool("sirenEn",    cfg.sirenEnabled);
  p.putUShort("sirenTo",  cfg.sirenTimeoutSec);
  p.putBool("ok",         true);
  p.end();
}

void Storage::clearAll() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.clear();
  p.end();
}

// --- Senhas: armazenadas como JSON array em uma única chave "codes" ---
int Storage::loadCodes(KeypadCode* out, int maxCodes) {
  Preferences p;
  p.begin(NVS_NS, true);
  String json = p.getString("codes", "[]");
  p.end();

  JsonDocument doc;
  if (deserializeJson(doc, json)) return 0;
  JsonArray arr = doc.as<JsonArray>();
  int n = 0;
  for (JsonObject o : arr) {
    if (n >= maxCodes) break;
    strlcpy(out[n].code,  o["code"]  | "", sizeof(out[n].code));
    strlcpy(out[n].label, o["label"] | "", sizeof(out[n].label));
    if (out[n].code[0] != '\0') n++;
  }
  return n;
}

void Storage::saveCodes(const KeypadCode* codes, int count) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < count; i++) {
    if (codes[i].code[0] == '\0') continue;
    JsonObject o = arr.add<JsonObject>();
    o["code"]  = codes[i].code;
    o["label"] = codes[i].label;
  }
  String json;
  serializeJson(doc, json);

  Preferences p;
  p.begin(NVS_NS, false);
  p.putString("codes", json);
  p.end();
}
