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
  cfg.zoneHomeMask    = 0xFF;
  cfg.entryDelaySec   = 30;
  cfg.exitDelaySec    = 60;
  cfg.autoArmMin      = 23 * 60;
  cfg.autoArmDays     = 0x7F;  // todos os dias
  strlcpy(cfg.tz, "<-03>3", sizeof(cfg.tz));
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
    p.getString("master",   cfg.masterCode, sizeof(cfg.masterCode));
    p.getString("haPin",    cfg.haPin,    sizeof(cfg.haPin));
    p.getString("botToken", cfg.botToken, sizeof(cfg.botToken));
    p.getString("chatId",   cfg.chatId,   sizeof(cfg.chatId));
    cfg.sirenEnabled    = p.getBool("sirenEn", true);
    cfg.sirenTimeoutSec = p.getUShort("sirenTo", 180);
    for (int i = 0; i < HG_ZONES; i++) {
      char k[6]; snprintf(k, sizeof(k), "zn%d", i);
      if (p.isKey(k)) p.getString(k, cfg.zoneNames[i], sizeof(cfg.zoneNames[i]));
    }
    cfg.zoneBypass      = p.getUChar("zbyp", 0);
    if (p.isKey("host")) p.getString("host", cfg.hostname, sizeof(cfg.hostname));
    p.getBytes("ztype", cfg.zoneType, sizeof(cfg.zoneType));   // sem a chave: tudo instantânea
    cfg.zoneHomeMask    = p.getUChar("zhome", 0xFF);
    cfg.entryDelaySec   = p.getUShort("entryS", 30);
    cfg.exitDelaySec    = p.getUShort("exitS", 60);
    cfg.openAlertMin    = p.getUShort("openMin", 0);
    cfg.autoArmEnabled  = p.getBool("aaOn", false);
    cfg.autoArmMin      = p.getUShort("aaMin", 23 * 60);
    cfg.autoArmDays     = p.getUChar("aaDays", 0x7F);
    cfg.autoArmMode     = p.getUChar("aaMode", 0);
    if (p.isKey("tz")) p.getString("tz", cfg.tz, sizeof(cfg.tz));
    cfg.staticIp  = p.getBool("sip", false);
    cfg.ipAddr    = p.getULong("ipA", 0);
    cfg.ipGateway = p.getULong("ipG", 0);
    cfg.ipMask    = p.getULong("ipM", 0);
    cfg.ipDns     = p.getULong("ipD", 0);
    if (p.isKey("mqttId")) {
      p.getString("mqttId", cfg.mqttId, sizeof(cfg.mqttId));
      p.getString("devName", cfg.deviceName, sizeof(cfg.deviceName));
    } else {
      // Placa configurada antes de existir o ID: mantém os tópicos e entidades
      // de sempre, senão o Home Assistant ganharia um aparelho novo e as
      // automações apontando para o antigo parariam.
      strlcpy(cfg.mqttId, "homeguard", sizeof(cfg.mqttId));
      strlcpy(cfg.deviceName, "ESP32 Homeguard", sizeof(cfg.deviceName));
    }
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
  p.putString("master",   cfg.masterCode);
  p.putString("haPin",    cfg.haPin);
  p.putString("botToken", cfg.botToken);
  p.putString("chatId",   cfg.chatId);
  p.putBool("sirenEn",    cfg.sirenEnabled);
  p.putUShort("sirenTo",  cfg.sirenTimeoutSec);
  for (int i = 0; i < HG_ZONES; i++) {
    char k[6]; snprintf(k, sizeof(k), "zn%d", i);
    p.putString(k, cfg.zoneNames[i]);
  }
  p.putUChar("zbyp",      cfg.zoneBypass);
  p.putString("host",     cfg.hostname);
  p.putBytes("ztype",     cfg.zoneType, sizeof(cfg.zoneType));
  p.putUChar("zhome",     cfg.zoneHomeMask);
  p.putUShort("entryS",   cfg.entryDelaySec);
  p.putUShort("exitS",    cfg.exitDelaySec);
  p.putUShort("openMin",  cfg.openAlertMin);
  p.putBool("aaOn",       cfg.autoArmEnabled);
  p.putUShort("aaMin",    cfg.autoArmMin);
  p.putUChar("aaDays",    cfg.autoArmDays);
  p.putUChar("aaMode",    cfg.autoArmMode);
  p.putString("tz",       cfg.tz);
  p.putString("mqttId",   cfg.mqttId);
  p.putString("devName",  cfg.deviceName);
  p.putBool("sip",        cfg.staticIp);
  p.putULong("ipA",       cfg.ipAddr);
  p.putULong("ipG",       cfg.ipGateway);
  p.putULong("ipM",       cfg.ipMask);
  p.putULong("ipD",       cfg.ipDns);
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
    out[n].duress = o["d"] | false;
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
    if (codes[i].duress) o["d"] = true;
  }
  String json;
  serializeJson(doc, json);

  Preferences p;
  p.begin(NVS_NS, false);
  p.putString("codes", json);
  p.end();
}

void Storage::saveArmState(bool armed, uint8_t mode, uint8_t tempBypass) {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putUChar("armSt", armed ? (0x80 | mode) : 0);
  p.putUChar("armTb", armed ? tempBypass : 0);
  p.end();
}

bool Storage::loadArmState(uint8_t& mode, uint8_t& tempBypass) {
  Preferences p;
  p.begin(NVS_NS, true);
  uint8_t st = p.getUChar("armSt", 0);
  tempBypass = p.getUChar("armTb", 0);
  p.end();
  mode = st & 0x7F;
  return st & 0x80;
}
