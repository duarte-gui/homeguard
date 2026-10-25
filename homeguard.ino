


/*
 * ====================================================================================
 * ESP32 Homeguard Firmware - Versão Final (AsyncWebSocket + LittleFS + Wiegand + MQTT)
 * ====================================================================================
 *
 * ALVO: ESP32-C3 (RISC-V, single core, 4MB de flash embutida).
 * Compilar com o FQBN:
 *   esp32:esp32:esp32c3:JTAGAdapter=default,CDCOnBoot=cdc,PartitionScheme=min_spiffs,
 *   CPUFreq=160,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,
 *   DebugLevel=none,EraseFlash=none,ZigbeeMode=default
 * CDCOnBoot=cdc é OBRIGATÓRIO: o Serial precisa sair pelo USB porque os pinos
 * da UART0 (GPIO 20/21) foram usados como GPIO. Ver o mapa de pinos abaixo.
 * min_spiffs = dois slots de 1,9 MB para o firmware (atualização por OTA, com
 * volta automática para a versão anterior se a nova não subir) e 128 KB de
 * LittleFS para o registro de eventos. A interface web vai DENTRO do firmware
 * (web_assets.h, gerado por tools/embed_web.py).
 * ====================================================================================
 * 
 * Este firmware foi atualizado para usar uma biblioteca Wiegand local (Wiegand.h e
 * Wiegand.cpp na mesma pasta do projeto), eliminando a necessidade de instalar
 * bibliotecas externas e resolvendo os problemas de compilação.
 *
 * NOVA FUNCIONALIDADE: Integração com Home Assistant via MQTT com Auto-Discovery.
 *
 * Funcionalidades Principais:
 * - Serve os arquivos da interface web (HTML, TSX, etc.) a partir do LittleFS.
 * - Comunicação WebSocket no endpoint "/ws" para atualizações instantâneas.
 * - Comunicação MQTT para integração com Home Assistant (Arme/Desarme, Status dos Setores).
 * - Sincroniza o estado completo com todos os clientes (Web, HA, etc.).
 * - Recebe comandos da interface web e do Home Assistant.
 * - Monitora 4 setores físicos e aciona a lógica de alarme.
 * - Envia notificações de alerta para o Telegram.
 * - Lê senhas de um teclado Wiegand para armar/desarmar o sistema fisicamente.
 *
 * --- INSTRUÇÕES DE SETUP ---
 * 1. BIBLIOTECAS: Garanta que as seguintes bibliotecas estão instaladas e atualizadas:
 *    - ESPAsyncWebServer
 *    - AsyncTCP
 *    - UniversalTelegramBot by Brian Lough
 *    - ArduinoJson by Benoit Blanchon
 *    - LittleFS_esp32
 *    - PubSubClient by Nick O'Leary (NOVO)
 * 
 * 2. WIEGAND: Os arquivos Wiegand.h e Wiegand.cpp DEVEM estar na mesma pasta deste sketch.
 * 
 * 3. CONFIGURAÇÕES ABAIXO:
 *    - Preencha as credenciais de Wi-Fi, Telegram e MQTT.
 */

// =================================================================
// 1. INCLUSÃO DE BIBLIOTECAS
// =================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>      // acesso por http://homeguard.local, sem depender do IP
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include "FS.h"
#include "LittleFS.h"
#include "Wiegand.h"
#include <PubSubClient.h> // NOVO: Biblioteca para MQTT
#include "config.h"       // struct Config (NVS)
#include "storage.h"      // persistência NVS
#include "provisioning.h" // portal de configuração (hotspot)
#include "codes.h"        // senhas do teclado
#include "eventlog.h"     // registro de eventos (LittleFS)
#include <Update.h>       // atualização por OTA
#include <esp_ota_ops.h>
#include <time.h>
#include <DNSServer.h>     // hotspot de resgate (captive portal)
#if __has_include("web_assets.h")
#include "web_assets.h"   // interface web embutida (tools/embed_web.py)
#define HG_HAS_WEB 1
#endif
#include <vector>
#include <algorithm>

// =================================================================
// 2. CONFIGURAÇÕES - AJUSTE ESTAS VARIÁVEIS
// =================================================================

// --- Configuração: carregada da NVS (preenchida pelo portal hotspot) ---
// Segredos NÃO ficam mais no código. Ver provisioning.* / storage.*
Config cfg;

// Nome de rede da placa. Vira:
//   - http://<nome>.local  (mDNS/Bonjour)
//   - o hostname DHCP que aparece na lista de clientes do roteador
// O padrão leva o mesmo sufixo do hotspot do portal (HOMEGUARD-208F ->
// homeguard-208f): com duas placas na mesma casa, "homeguard" fixo fazia as
// duas responderem pelo mesmo nome. Dá para trocar na página de admin.
char HOSTNAME[33];

// --- Mapeamento de Pinos (GPIO) — ESP32-C3 ---
//
// Restrições do ESP32-C3 (QFN32, flash embutida) que ditam este mapa:
//   GPIO 11 ......... VDD_SPI            -> inutilizável
//   GPIO 12 a 17 .... flash SPI interna  -> inutilizáveis
//   GPIO 18 e 19 .... USB D- / D+        -> reservados p/ o USB-Serial-JTAG
//                                           (é por onde grava e sai o Serial)
//   GPIO 2, 8, 9 .... strapping          -> precisam estar em nível ALTO no boot.
//                                           Um setor FECHADO puxa o pino p/ GND
//                                           (INPUT_PULLUP), então travaria o boot.
//   GPIO 8 .......... LED RGB embutido   -> usado via rgbLedWrite(RGB_BUILTIN,...).
//                                           É strapping, mas a própria devkit já o
//                                           liga ao LED, então continua bootando.
//   GPIO 20 e 21 .... UART0 RX / TX      -> livres porque o Serial vai pelo USB
//                                           (build com CDCOnBoot=cdc)
//
// GPIO 20 ficou livre: era o 5º setor, removido. O 21 continua sendo escrito pelo
// firmware (liga/desliga conforme armado), mas não precisa de nada ligado nele —
// o status em cores sai do pixel RGB pendurado na linha de dados do GPIO 8.
const int SECTOR_1_PIN = 0;
const int SECTOR_2_PIN = 1;
const int SECTOR_3_PIN = 3;
const int SECTOR_4_PIN = 4;
const int GATE_LOCK_RELAY_PIN = 5;
const int SIREN_RELAY_PIN = 10;
const int STATUS_LED_PIN = 21; // UART0 TX: pisca durante o log de boot da ROM

// --- Teclado Wiegand (senhas agora no módulo Codes / NVS) ---
// D0/D1 saíram de 18/19 (que no C3 são as linhas do USB) para 6/7.
const int WIEGAND_D0_PIN = 6;
const int WIEGAND_D1_PIN = 7;

// --- Reset de fábrica pelo botão BOOT (GPIO 9, já com pull-up na placa) ---
// Segurar 10 s com o sistema JÁ rodando. Segurar na energização não serve:
// GPIO 9 em nível baixo no reset põe o C3 em modo de gravação.
const int BOOT_BTN_PIN = 9;
const unsigned long FACTORY_RESET_HOLD_MS = 10000;

// --- Nomes dos Setores ---
const char* sector_names[] = {
  "Portão Garagem", // Removido "Setor X" para nomes mais limpos no HA
  "Presença Garagem",
  "Portão Principal",
  "Porta Gui"
};

#define HG_FW_VERSION "2.2.1"
const char* FW_VERSION = HG_FW_VERSION;
// Marca procurada pelo homeguard-admin (tools/) no .bin, para conferir que o
// arquivo é mesmo deste projeto e mostrar a versão antes de gravar. Vai para o
// Serial no boot: sem nenhuma referência, o linker a jogava fora.
const char FW_MARKER[] = "HOMEGUARD_FW_VERSION=" HG_FW_VERSION;

// Fica aqui em cima porque o Arduino gera os protótipos das funções antes da
// primeira função do arquivo, e tryDisarm() devolve este tipo.
enum DisarmResult { DISARM_OK, DISARM_WRONG, DISARM_LOCKED };
// Os mesmos modos do painel de alarme do Home Assistant.
enum ArmMode : uint8_t { MODE_AWAY = 0, MODE_HOME = 1 };

// Sufixo de 4 dígitos que identifica a placa: o mesmo do hotspot do portal
// (HOMEGUARD-208F). Vai no nome de rede, nos tópicos MQTT e no nome do HA.
String boardSuffix() {
  String s = String((uint32_t)ESP.getEfuseMac(), HEX).substring(0, 4);
  s.toLowerCase();
  return s;
}
String defaultHostname()   { return "homeguard-" + boardSuffix(); }
String defaultMqttId()     { return "homeguard-" + boardSuffix(); }
String defaultDeviceName() { String s = boardSuffix(); s.toUpperCase(); return "Homeguard " + s; }
const char* mqttId()  { static String d = defaultMqttId();     return cfg.mqttId[0] ? cfg.mqttId : d.c_str(); }
const char* devName() { static String d = defaultDeviceName(); return cfg.deviceName[0] ? cfg.deviceName : d.c_str(); }

// Nome válido para DNS: 1-32 de [a-z0-9-], sem '-' nas pontas.
bool validHostname(const String& h) {
  if (h.isEmpty() || h.length() > 32 || h[0] == '-' || h[h.length() - 1] == '-') return false;
  for (char ch : h) {
    if (!isdigit(ch) && !(ch >= 'a' && ch <= 'z') && ch != '-') return false;
  }
  return true;
}

// ---------------------------------------------------------------- IP fixo
String ipStr(uint32_t v) { return v ? IPAddress(v).toString() : String(); }

bool parseIp(const String& s, uint32_t& out) {
  IPAddress ip;
  if (!ip.fromString(s.c_str())) return false;
  out = (uint32_t)ip;
  return true;
}

// Máscara válida = uns seguidos de zeros (255.255.255.0 sim, 255.0.255.0 não).
bool validMask(uint32_t m) {
  uint32_t h = ntohl(m);
  return h != 0 && ((~h + 1) & ~h) == 0;
}

// "" = ok. IP e gateway têm de estar na mesma rede, senão a placa conecta no
// Wi-Fi mas ninguém chega nela.
String checkStaticIp(uint32_t ip, uint32_t gw, uint32_t mask) {
  if (!validMask(mask)) return "Máscara inválida (ex.: 255.255.255.0).";
  if ((ip & mask) != (gw & mask)) return "O IP e o gateway precisam estar na mesma rede.";
  if ((ip & ~mask) == 0 || (ip & ~mask) == ~mask) return "Esse IP é o endereço da rede ou de broadcast.";
  if (ip == gw) return "O IP não pode ser o mesmo do gateway.";
  return "";
}

// Antes do WiFi.begin(): IP fixo ou DHCP.
void applyIpConfig() {
  if (cfg.staticIp && cfg.ipAddr && cfg.ipGateway && cfg.ipMask) {
    WiFi.config(IPAddress(cfg.ipAddr), IPAddress(cfg.ipGateway), IPAddress(cfg.ipMask),
                IPAddress(cfg.ipDns ? cfg.ipDns : cfg.ipGateway));
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }
}

const int NUM_SECTORS = HG_ZONES;
const int sector_pins[NUM_SECTORS] = {SECTOR_1_PIN, SECTOR_2_PIN, SECTOR_3_PIN, SECTOR_4_PIN};

// Setores já publicados no Home Assistant em versões anteriores. A configuração
// de descoberta vai retida no broker, então tirar um setor do array não basta:
// sem apagar o tópico, a entidade fica para sempre no HA marcada como indisponível.
const int MAX_SECTORS_EVER = 8;

// =================================================================
// 3. OBJETOS GLOBAIS E VARIÁVEIS DE ESTADO
// =================================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr; // instanciado no setup, após carregar a config
Wiegand wiegand;

// --- MQTT (NOVO) ---
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// --- Estado do Sistema ---
bool isArmed = false;
bool isSirenActive = false;
bool isTriggered = false;            // alarme disparado (independe da sirene física)
bool apActive = false;               // hotspot de resgate no ar (ver maintainWifi)
int triggeringSectorId = 0;
ArmMode armMode = MODE_AWAY;
// Contagens de saída (logo depois de armar) e de entrada (zona de entrada aberta).
bool exitRunning = false, entryRunning = false;
unsigned long exitEndsAt = 0, entryEndsAt = 0;
int entryZone = 0;
uint8_t tempBypass = 0;              // zonas abertas na hora do arme forçado: inibidas até desarmar
unsigned long openSinceMs[HG_ZONES] = {};
bool openAlerted[HG_ZONES] = {};
unsigned long sirenStartMs = 0;      // p/ o auto-corte da sirene
String sector_status[NUM_SECTORS];
String last_sector_status[NUM_SECTORS];

// Nome dado na página de admin; vazio = nome padrão de sector_names.
const char* zoneName(int i) {
  return cfg.zoneNames[i][0] ? cfg.zoneNames[i] : sector_names[i];
}
// Zona inibida não dispara o alarme, mas continua aparecendo aberta/fechada.
bool zoneBypassed(int i) { return cfg.zoneBypass & (1 << i); }

// --- Teclado: máquina de estados (programação exige senha mestre) ---
// KP_AUTH = aguardando a senha mestre após "# com buffer vazio".
enum KeypadState { KP_IDLE, KP_AUTH, KP_MENU, KP_ADD, KP_DEL, KP_RESET };
KeypadState keypadState = KP_IDLE;

// --- MQTT: tudo sai do ID da placa, para duas placas não se misturarem ---
//   tópicos:   <id>/alarm/state, <id>/sector/1/state ...
//   HA:        esp32_<id>_alarm_panel ... ('-' vira '_')
// Placas antigas ficam com id "homeguard", que dá exatamente os nomes de antes.
String topicBase, uidBase;
String T(const char* suffix) { return topicBase + "/" + suffix; }

// --- Controle de Tempo ---
unsigned long lastStatusCheck = 0;
const long checkInterval = 100;
unsigned long lastMqttReconnectAttempt = 0;
// Intervalo entre tentativas: dobra a cada falha (até 1 min) e volta a 5 s ao conectar.
// Cada tentativa trava o loop() até o timeout; com o broker fora do ar, tentar
// sempre a cada 5 s deixava teclado, zonas e página web lentos.
unsigned long mqttRetryMs = 5000;

// --- Lógica do Teclado ---
String keypadBuffer = "";
unsigned long lastKeypadInputTime = 0;
const long keypadTimeout = 5000;

// =================================================================
// 4. FUNÇÕES DO SISTEMA
// =================================================================

void publishFullMqttState(); // Forward declaration
void publishCodesList();     // Forward declaration
void publishNotice(const char* type, const char* zone); // Forward declaration
void updateHardwareState();  // Forward declaration
void updateSiren();          // Forward declaration

const char* modeName(ArmMode m) {
  return m == MODE_HOME ? "em casa" : "ausente";
}
const char* modeKey(ArmMode m) {
  return m == MODE_HOME ? "home" : "away";
}
ArmMode parseMode(const char* s) {
  if (!strcmp(s, "home"))  return MODE_HOME;
  return MODE_AWAY;
}
uint8_t modeMask(ArmMode m) {
  return m == MODE_HOME ? cfg.zoneHomeMask : 0xFF;
}
bool zoneOpen(int i) { return sector_status[i] == "Aberto"; }
// Zona vigiada neste modo de arme (24 h é tratada à parte: vale sempre).
bool zoneActive(int i, ArmMode m) {
  if (zoneBypassed(i) || (tempBypass & (1 << i))) return false;
  return modeMask(m) & (1 << i);
}
String zoneList(uint8_t mask) {
  String s;
  for (int i = 0; i < NUM_SECTORS; i++) {
    if (!(mask & (1 << i))) continue;
    if (s.length()) s += ", ";
    s += zoneName(i);
  }
  return s;
}
long remainingS(bool running, unsigned long endsAt) {
  if (!running) return 0;
  long r = (long)(endsAt - millis());
  return r > 0 ? (r + 999) / 1000 : 0;
}

String getFullStateJson() {
  JsonDocument doc;
  doc["type"] = "state";
  doc["isArmed"] = isArmed;
  doc["isSirenActive"] = isSirenActive;
  doc["isTriggered"] = isTriggered;
  doc["triggeringSectorId"] = triggeringSectorId;
  doc["mode"] = modeKey(armMode);
  doc["exitRemaining"] = remainingS(exitRunning, exitEndsAt);
  doc["entryRemaining"] = remainingS(entryRunning, entryEndsAt);
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
  doc["hostname"] = HOSTNAME;
  JsonArray sectors = doc["sectors"].to<JsonArray>();
  for (int i = 0; i < NUM_SECTORS; i++) {
    JsonObject sector = sectors.add<JsonObject>();
    sector["id"] = i + 1;
    sector["name"] = zoneName(i);
    sector["status"] = sector_status[i];
    sector["bypass"] = zoneBypassed(i);
    sector["tempBypass"] = (bool)(tempBypass & (1 << i));
    sector["is24h"] = cfg.zoneType[i] == ZT_24H;
  }
  String output;
  serializeJson(doc, output);
  return output;
}

void broadcastFullState() {
  String jsonState = getFullStateJson();
  ws.textAll(jsonState);
  if (mqttClient.connected()) {
    publishFullMqttState();
  }
  Serial.println("Estado enviado para os clientes: " + jsonState);
}

// (Re)cria o bot quando o token muda pela página de admin.
void setupTelegram() {
  delete bot;
  bot = nullptr;
  if (cfg.botToken[0] != '\0') bot = new UniversalTelegramBot(cfg.botToken, secured_client);
}

// O nome da central vai na primeira linha: com duas placas, dá para saber de qual veio.
// Sem Wi-Fi, o aviso espera numa fila (com a hora em que aconteceu) e sai quando
// a placa reconectar: um disparo durante a queda da rede não se perde.
std::vector<String> pendingAlerts;
const size_t MAX_PENDING_ALERTS = 10;

bool sendTelegramAlert(const String& message) {
  if (bot == nullptr || cfg.chatId[0] == '\0') return false;
  if (WiFi.status() == WL_CONNECTED) {
    return bot->sendMessage(cfg.chatId, String(devName()) + "\n" + message, "");
  }
  String when;
  struct tm t;
  if (time(nullptr) > 1700000000 && getLocalTime(&t, 0)) {
    char buf[24];
    strftime(buf, sizeof(buf), " (%d/%m %H:%M)", &t);
    when = buf;
  }
  if (pendingAlerts.size() >= MAX_PENDING_ALERTS) pendingAlerts.erase(pendingAlerts.begin());
  pendingAlerts.push_back(message + when);
  return false;
}

void flushPendingAlerts() {
  if (pendingAlerts.empty() || bot == nullptr || cfg.chatId[0] == '\0') return;
  std::vector<String> fila;
  fila.swap(pendingAlerts);
  for (const String& m : fila) {
    bot->sendMessage(cfg.chatId, String(devName()) + "\n" + m + "\n(enviado com atraso: a placa estava sem Wi-Fi)", "");
  }
}

// Estado do painel de alarme para o Home Assistant.
const char* alarmStateStr() {
  if (isTriggered) return "triggered";
  if (!isArmed) return "disarmed";
  if (entryRunning) return "pending";
  if (exitRunning) return "arming";
  return armMode == MODE_HOME ? "armed_home" : "armed_away";
}

// --- Autoridade das senhas ---
// A senha MESTRE (definida no setup) é a ÚNICA que pode PROGRAMAR o teclado.
bool isMaster(const String& c) {
  return cfg.masterCode[0] != '\0' && c.equals(cfg.masterCode);
}
// Qualquer senha cadastrada (ou a mestre) pode apenas ARMAR/DESARMAR.
bool isOperatingCode(const String& c) {
  return isMaster(c) || Codes::check(c);
}

// --- Limite de tentativas ---
// 5 senhas erradas seguidas bloqueiam novas tentativas por 1 minuto: numa
// senha de 4 dígitos, isso leva o chute por força bruta de minutos para dias.
// Teclado e web/HA têm contadores separados: alguém chutando senha pela rede
// não pode travar o teclado do dono (e vice-versa).
const uint8_t AUTH_MAX_FAILS = 5;
const unsigned long AUTH_LOCK_MS = 60000;

struct AuthLimiter {
  const char* origem;
  uint8_t fails;
  unsigned long lockUntil;           // 0 = não bloqueado

  long remainingMs() {
    if (!lockUntil) return 0;
    long r = (long)(lockUntil - millis());
    if (r <= 0) { lockUntil = 0; return 0; }
    return r;
  }
  void record(bool ok) {
    if (ok) { fails = 0; return; }
    if (++fails < AUTH_MAX_FAILS) return;
    fails = 0;
    lockUntil = millis() + AUTH_LOCK_MS;
    if (!lockUntil) lockUntil = 1;
    EventLog::add("5 senhas erradas (%s): bloqueado 60 s", origem);
    sendTelegramAlert(String("⚠️ 5 senhas erradas seguidas (") + origem + "). Novas tentativas bloqueadas por 1 minuto.");
    publishNotice("lockout", origem);
  }
  String message() {
    return "Muitas tentativas erradas. Tente de novo em " + String((remainingMs() + 999) / 1000) + " s.";
  }
};
AuthLimiter authRemote = {"web/HA", 0, 0};
AuthLimiter authKeypad = {"teclado", 0, 0};

// Quem usou a senha, para o registro: "Ana (teclado)".
String whoUsed(const String& code, const char* origem) {
  String label = isMaster(code) ? String("senha mestre") : Codes::labelOf(code);
  return label + " (" + origem + ")";
}

// ---------------------------------------------------------------- arme / desarme
void saveArmState() { Storage::saveArmState(isArmed, armMode, tempBypass); }

// Zonas instantâneas abertas que impedem armar neste modo. Zonas de entrada/saída
// e seguidoras podem estar abertas: é por elas que se sai.
uint8_t blockingZones(ArmMode m) {
  uint8_t mask = 0;
  for (int i = 0; i < NUM_SECTORS; i++) {
    if (cfg.zoneType[i] == ZT_INSTANT && zoneOpen(i) && !zoneBypassed(i) && (modeMask(m) & (1 << i)))
      mask |= 1 << i;
  }
  return mask;
}

// Arma. Devolve as zonas abertas que impediram (0 = armou ou já estava armado).
// force: arma mesmo assim, deixando essas zonas inibidas até desarmar.
uint8_t armSystem(ArmMode m, bool force, const String& who) {
  if (isArmed) return 0;
  uint8_t open = blockingZones(m);
  if (open && !force) return open;

  isArmed = true;
  armMode = m;
  isTriggered = false;
  triggeringSectorId = 0;
  entryRunning = false;
  tempBypass = open;
  bool hasEntryZone = false;
  for (int i = 0; i < NUM_SECTORS; i++)
    if (cfg.zoneType[i] == ZT_DELAYED && zoneActive(i, m)) hasEntryZone = true;
  // Sem zona de entrada/saída não há por onde sair: arma direto.
  exitRunning = hasEntryZone && cfg.exitDelaySec > 0;
  exitEndsAt = millis() + cfg.exitDelaySec * 1000UL;

  EventLog::add("Armado (%s) por %s", modeName(m), who.c_str());
  if (open) {
    EventLog::add("Inibida ate desarmar: %s", zoneList(open).c_str());
    sendTelegramAlert("⚠️ Armado (" + String(modeName(m)) + ") com zona aberta, inibida até desarmar: " + zoneList(open));
  }
  saveArmState();
  updateHardwareState();
  broadcastFullState();
  return 0;
}

void disarmSystem(const String& who) {
  if (!isArmed && !isTriggered) return;
  isArmed = false;
  isTriggered = false;
  isSirenActive = false;
  triggeringSectorId = 0;
  exitRunning = entryRunning = false;
  tempBypass = 0;
  EventLog::add("Desarmado por %s", who.c_str());
  saveArmState();
  updateSiren();
  updateHardwareState();
  broadcastFullState();
}

void triggerAlarm(int i) {
  isTriggered = true;
  exitRunning = entryRunning = false;
  triggeringSectorId = i + 1;
  // Sirene física só toca se o gating permitir (cfg.sirenEnabled).
  if (cfg.sirenEnabled) {
    isSirenActive = true;
    sirenStartMs = millis();
    updateSiren();
  }
  EventLog::add("DISPARO: %s", zoneName(i));
  sendTelegramAlert(String("🚨 ALARME DISPARADO 🚨\nZona: ") + zoneName(i));
}

// Senha de pânico: o alarme desarma normalmente (quem coagiu vê tudo normal),
// mas sai um alerta silencioso. Sem sirene e sem nenhuma diferença no LED.
void raiseDuress(const String& who) {
  EventLog::add("COACAO: senha de panico de %s", who.c_str());
  sendTelegramAlert("🆘 COAÇÃO: o alarme foi desarmado com a senha de pânico de " + who + ".");
  if (mqttClient.connected()) {
    mqttClient.publish(T("event").c_str(), "{\"event_type\":\"duress\"}", false);
  }
}

// Desarme com senha pela web ou pelo Home Assistant. Vale qualquer senha
// cadastrada no teclado, inclusive a mestre e as de pânico.
DisarmResult tryDisarm(const String& code, const char* origem) {
  if (authRemote.remainingMs() > 0) return DISARM_LOCKED;
  bool ok = isOperatingCode(code);
  if (!ok) EventLog::add("Senha errada para desarmar (%s)", origem);
  authRemote.record(ok);
  if (!ok) return DISARM_WRONG;
  String who = whoUsed(code, origem);
  disarmSystem(who);
  if (Codes::isDuress(code)) raiseDuress(who);
  return DISARM_OK;
}

String readSensorStatus(int pin) {
  return (digitalRead(pin) == HIGH) ? "Aberto" : "Fechado";
}

void updateSiren() {
  digitalWrite(SIREN_RELAY_PIN, isSirenActive ? HIGH : LOW);
}

// =================================================================
// LED RGB EMBUTIDO (GPIO 8) — complementa o LED externo do GPIO 21
// =================================================================
// O LED externo só distingue armado/desarmado. O RGB acrescenta os estados que
// antes só apareciam no Serial: disparo, Wi-Fi caído e modo de configuração.
//
//   verde fraco ....... desarmado, tudo certo
//   vermelho fixo ..... armado
//   vermelho piscando . ALARME DISPARADO, sirene tocando
//   magenta piscando .. ALARME DISPARADO, sirene muda (silenciada, tempo esgotado
//                       ou desativada): Telegram e HA continuam avisando
//   amarelo rápido .... contagem de ENTRADA: desarme agora
//   amarelo lento ..... contagem de SAÍDA: saia e feche a porta
//   laranja piscando .. sem Wi-Fi
//   azul piscando ..... portal de configuração aberto
//
// Brilho baixo de propósito: um WS2812 no máximo é ofuscante de perto.
#ifdef RGB_BUILTIN
// Só reescreve quando a cor muda — o rgbLedWrite() usa o RMT e não precisa ser
// chamado a cada volta do loop.
void rgbShow(uint8_t r, uint8_t g, uint8_t b) {
  static int last = -1;
  int now = (r << 16) | (g << 8) | b;
  if (now == last) return;
  last = now;
  rgbLedWrite(RGB_BUILTIN, r, g, b);
}

void updateRgbLed(unsigned long nowMs) {
  uint8_t r = 0, g = 0, b = 0;
  unsigned long period = 0;   // 0 = cor fixa

  if (isTriggered && isSirenActive) {             // prioridade máxima
    r = 60; period = 300;
  } else if (isTriggered) {                       // disparo silencioso
    r = 45; b = 45; period = 300;                 // magenta
  } else if (entryRunning) {
    r = 45; g = 30; period = 150;                 // amarelo rápido
  } else if (exitRunning) {
    r = 45; g = 30; period = 600;                 // amarelo lento
  } else if (apActive) {
    b = 45; period = 1200;                        // azul lento: hotspot de configuração aberto
  } else if (WiFi.status() != WL_CONNECTED) {
    r = 45; g = 12; period = 1200;                // laranja
  } else if (isArmed) {
    r = 40;
  } else {
    g = 18;
  }

  if (period == 0) { rgbShow(r, g, b); return; }
  if ((nowMs / period) % 2) rgbShow(0, 0, 0); else rgbShow(r, g, b);
}
#else
void rgbShow(uint8_t, uint8_t, uint8_t) {}
void updateRgbLed(unsigned long) {}
#endif

void updateHardwareState() {
  if (isArmed) {
    digitalWrite(STATUS_LED_PIN, LOW);
    digitalWrite(GATE_LOCK_RELAY_PIN, HIGH);
  } else {
    digitalWrite(STATUS_LED_PIN, HIGH);
    digitalWrite(GATE_LOCK_RELAY_PIN, LOW);
  }
}

// Reset de fábrica pelo botão. Devolve true enquanto o botão está pressionado,
// para quem chama não sobrescrever o LED com o status normal durante a contagem.
bool checkFactoryResetButton(unsigned long nowMs) {
  static bool down = false;
  static unsigned long pressedAt = 0;

  if (digitalRead(BOOT_BTN_PIN) == HIGH) {
    if (down) Serial.println("[RESET] BOOT solto antes de 10 s — cancelado.");
    down = false;
    return false;
  }
  if (!down) {
    down = true;
    pressedAt = nowMs;
    Serial.println("[RESET] BOOT pressionado: segure 10 s para o reset de fábrica.");
  }

  unsigned long held = nowMs - pressedAt;
  unsigned long period = held < 7000 ? 250 : 80;   // acelera perto do fim
  if ((nowMs / period) % 2) rgbShow(0, 0, 0); else rgbShow(60, 0, 0);

  if (held >= FACTORY_RESET_HOLD_MS) {
    Serial.println("[RESET] FACTORY RESET pelo botão BOOT.");
    rgbShow(0, 0, 60);              // azul fixo: apagou, vai subir no portal
    Storage::clearAll();
    delay(500);
    ESP.restart();
  }
  return true;
}

void handleActivateSiren() {
    isSirenActive = true;
    sirenStartMs = millis();
    Serial.println("Sirene ativada via MQTT.");
    updateSiren();
    broadcastFullState();
}

// Silencia a sirene física, mas o alarme CONTINUA "triggered" até desarmar.
void handleSilenceSiren() {
    if (isSirenActive) EventLog::add("Sirene silenciada");
    isSirenActive = false;
    Serial.println("Sirene silenciada (alarme segue disparado até desarmar).");
    updateSiren();
    broadcastFullState();
}

// Liga/desliga o GATING da sirene (se a sirene física pode tocar no disparo).
void handleSirenEnabled(bool enabled) {
    cfg.sirenEnabled = enabled;
    Storage::saveConfig(cfg);
    Serial.printf("Sirene habilitada: %s\n", enabled ? "SIM" : "NAO");
    if (!enabled && isSirenActive) { isSirenActive = false; updateSiren(); }
    broadcastFullState();
}


// =================================================================
// 5. FUNÇÕES MQTT (NOVO)
// =================================================================

// Todas as entidades ficam no mesmo aparelho do HA.
void addDevice(JsonDocument& doc) {
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"].add(uidBase + "_alarm");   // o HA exige array
  dev["name"] = devName();
  dev["manufacturer"] = "DIY";
  dev["model"] = "Homeguard ESP32-C3";
  dev["sw_version"] = FW_VERSION;
  dev["configuration_url"] = String("http://") + HOSTNAME + ".local";
}

void addAvailability(JsonDocument& doc) {
  doc["availability_topic"] = T("availability");
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
}

bool publishConfig(const char* component, const String& objectId, JsonDocument& doc) {
  String topic = String("homeassistant/") + component + "/" + objectId + "/config";
  String payload;
  serializeJson(doc, payload);
  bool ok = mqttClient.publish(topic.c_str(), payload.c_str(), true);
  Serial.printf("  [%s] %s\n", ok ? "OK" : "ERRO", topic.c_str());
  return ok;
}

// Tópicos de descoberta desta placa. Payload vazio e retido = "esqueça esta
// entidade" para o HA: é como se limpa ao trocar o ID ou tirar um setor.
void clearDiscovery() {
  mqttClient.publish(("homeassistant/alarm_control_panel/" + uidBase + "/config").c_str(), "", true);
  mqttClient.publish(("homeassistant/switch/" + uidBase + "_siren/config").c_str(), "", true);
  mqttClient.publish(("homeassistant/switch/" + uidBase + "_siren_enabled/config").c_str(), "", true);
  mqttClient.publish(("homeassistant/event/" + uidBase + "_duress/config").c_str(), "", true);
  mqttClient.publish(("homeassistant/event/" + uidBase + "_notice/config").c_str(), "", true);
  for (int i = 0; i < MAX_SECTORS_EVER; i++) {
    mqttClient.publish(("homeassistant/binary_sensor/" + uidBase + "_sector_" + (i + 1) + "/config").c_str(), "", true);
  }
  mqttClient.publish(T("availability").c_str(), "", true);
}

void publishDiscoveryMessages() {
  JsonDocument doc;
  Serial.println("--- Publicando a descoberta do Home Assistant ---");

  // --- Painel de alarme ---
  doc["name"] = devName();
  doc["unique_id"] = uidBase + "_alarm_panel";
  doc["state_topic"] = T("alarm/state");
  doc["command_topic"] = T("alarm/command");
  addAvailability(doc);
  doc["payload_arm_away"] = "ARM_AWAY";
  doc["payload_arm_home"] = "ARM_HOME";
  doc["payload_disarm"] = "DISARM";
  // Só os modos que o firmware implementa: sem isto o HA mostra também
  // "Férias", "Personalizado" etc., que mandariam comandos ignorados aqui.
  JsonArray features = doc["supported_features"].to<JsonArray>();
  features.add("arm_away");
  features.add("arm_home");
  doc["code_arm_required"] = false;
  doc["code_disarm_required"] = true;
  // O código NUNCA vai no payload: ele ficava retido no broker em texto puro.
  // Com REMOTE_CODE o HA pede o código e o manda junto do comando; quem confere
  // é o firmware, com as mesmas senhas do teclado.
  doc["code"] = "REMOTE_CODE";
  doc["command_template"] = "{\"action\":\"{{ action }}\",\"code\":\"{{ code }}\"}";
  addDevice(doc);
  publishConfig("alarm_control_panel", uidBase, doc);

  // --- Sirene (liga / silencia) ---
  doc.clear();
  doc["name"] = String(devName()) + " Siren";
  doc["unique_id"] = uidBase + "_siren";
  doc["state_topic"] = T("siren/state");
  doc["command_topic"] = T("siren/command");
  addAvailability(doc);
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:alarm-bell";
  addDevice(doc);
  publishConfig("switch", uidBase + "_siren", doc);

  // --- "Sirene habilitada" (gating do disparo) ---
  doc.clear();
  doc["name"] = String(devName()) + " Sirene habilitada";
  doc["unique_id"] = uidBase + "_siren_enabled";
  doc["state_topic"] = T("siren_enabled/state");
  doc["command_topic"] = T("siren_enabled/command");
  addAvailability(doc);
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:bell-cog";
  doc["entity_category"] = "config";
  addDevice(doc);
  publishConfig("switch", uidBase + "_siren_enabled", doc);

  // --- Zonas ---
  for (int i = 0; i < NUM_SECTORS; i++) {
    doc.clear();
    String uid = uidBase + "_sector_" + (i + 1);
    doc["name"] = String(devName()) + " " + zoneName(i);
    doc["unique_id"] = uid;
    doc["state_topic"] = T(("sector/" + String(i + 1) + "/state").c_str());
    addAvailability(doc);
    doc["payload_on"] = "Aberto";
    doc["payload_off"] = "Fechado";
    doc["device_class"] = "door";
    addDevice(doc);
    publishConfig("binary_sensor", uid, doc);
  }

  // --- Coação (senha de pânico): "event" no HA, para uma automação silenciosa ---
  doc.clear();
  doc["name"] = String(devName()) + " Coação";
  doc["unique_id"] = uidBase + "_duress";
  doc["state_topic"] = T("event");
  addAvailability(doc);
  doc["event_types"].to<JsonArray>().add("duress");
  addDevice(doc);
  publishConfig("event", uidBase + "_duress", doc);

  // --- Avisos: zona aberta há muito tempo, arme automático que falhou, bloqueio ---
  doc.clear();
  doc["name"] = String(devName()) + " Avisos";
  doc["unique_id"] = uidBase + "_notice";
  doc["state_topic"] = T("notice");
  addAvailability(doc);
  JsonArray types = doc["event_types"].to<JsonArray>();
  types.add("zone_open_long");
  types.add("auto_arm_failed");
  types.add("lockout");
  doc["icon"] = "mdi:alert-circle-outline";
  addDevice(doc);
  publishConfig("event", uidBase + "_notice", doc);

  // Setores que existiam em versões antigas (até 8): apaga a descoberta.
  for (int i = NUM_SECTORS; i < MAX_SECTORS_EVER; i++) {
    mqttClient.publish(("homeassistant/binary_sensor/" + uidBase + "_sector_" + (i + 1) + "/config").c_str(), "", true);
  }
  Serial.println("--- Fim da descoberta ---");
}

void publishNotice(const char* type, const char* zone) {
  if (!mqttClient.connected()) return;
  JsonDocument d;
  d["event_type"] = type;
  if (zone && *zone) d["detail"] = zone;
  String out;
  serializeJson(d, out);
  mqttClient.publish(T("notice").c_str(), out.c_str(), false);
}

void publishFullMqttState() {
  // disarmed / arming / armed_away / armed_home / pending / triggered
  mqttClient.publish(T("alarm/state").c_str(), alarmStateStr(), true);
  mqttClient.publish(T("siren/state").c_str(), isSirenActive ? "ON" : "OFF", true);
  mqttClient.publish(T("siren_enabled/state").c_str(), cfg.sirenEnabled ? "ON" : "OFF", true);
  for (int i = 0; i < NUM_SECTORS; i++) {
    mqttClient.publish(T(("sector/" + String(i + 1) + "/state").c_str()).c_str(), sector_status[i].c_str(), true);
  }
}

// Publica a LISTA de senhas (apenas rótulos + tamanho, NUNCA os valores).
void publishCodesList() {
  if (mqttClient.connected()) {
    mqttClient.publish(T("codes/list").c_str(), Codes::listJson().c_str(), true);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  String t(topic);
  // Estes dois tópicos carregam senhas: não vão para o log.
  bool sensivel = t == T("alarm/command") || t == T("codes/set");
  Serial.printf("MQTT [%s]: %s\n", topic, sensivel ? "(conteudo omitido)" : message);

  // Payload vazio é alguém apagando uma mensagem retida, não um comando. Sem
  // este retorno, um vazio em siren_enabled/command DESABILITAVA a sirene
  // ("não é ON" -> false).
  if (length == 0) return;

  if (t == T("alarm/command")) {
    // Formato do HA com REMOTE_CODE: {"action":"ARM_AWAY|ARM_HOME|DISARM","code":"..."}.
    // "ARM_AWAY" puro ainda arma (armar não exige senha); "DISARM" sem código
    // é recusado.
    JsonDocument cmd;
    const char* action = message;
    String code;
    if (message[0] == '{' && !deserializeJson(cmd, message)) {
      action = cmd["action"] | "";
      code   = cmd["code"]   | "";
    }
    if (!strncmp(action, "ARM_", 4)) {
      // Outros modos do HA (noite, férias...) não existem aqui e não aparecem no painel.
      ArmMode m = !strcmp(action, "ARM_HOME") ? MODE_HOME : MODE_AWAY;
      // O HA não tem como perguntar "armar mesmo assim?": arma e inibe a zona
      // aberta até desarmar, avisando no Telegram (ver armSystem).
      armSystem(m, true, "Home Assistant");
      publishFullMqttState();       // já armado em outro modo: devolve o estado real
    } else if (strcmp(action, "DISARM") == 0) {
      if (tryDisarm(code, "Home Assistant") != DISARM_OK) publishFullMqttState();
    }
  } else if (t == T("siren/command")) {
    if (strcmp(message, "OFF") == 0) handleSilenceSiren();
    else if (strcmp(message, "ON") == 0) handleActivateSiren();
  } else if (t == T("siren_enabled/command")) {
    handleSirenEnabled(strcmp(message, "ON") == 0);
  } else if (t == T("codes/set")) {
    // Cadastro REMOTO de senha: {"action":"add"|"remove","code":"1234","label":"Gui"}
    JsonDocument cmd;
    if (!deserializeJson(cmd, message)) {
      const char* action = cmd["action"] | "";
      const char* code   = cmd["code"]   | "";
      const char* label  = cmd["label"]  | "remoto";
      bool ok = false;
      if (strcmp(action, "add") == 0)    ok = Codes::add(String(code), label);
      else if (strcmp(action, "remove") == 0) ok = Codes::remove(String(code));
      if (ok) EventLog::add("Senha %s por MQTT: %s", action, label);
      publishCodesList();
    }
  } else if (t == T("system/command")) {
    if (strcmp(message, "reboot") == 0) {
      EventLog::add("Reinicio pedido pelo MQTT");
      delay(200); ESP.restart();
    } else if (strcmp(message, "factory_reset") == 0) {
      Serial.println("[SYS] FACTORY RESET via MQTT."); Storage::clearAll(); delay(200); ESP.restart();
    }
  }
}

void reconnectMqtt() {
  if (!mqttClient.connected()) {
    Serial.print("Tentando conexão MQTT...");
    // Client ID sem espaços (o padrão MQTT só garante [0-9a-zA-Z]) e único por placa.
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "homeguard-%06lX",
             (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));
    // Broker anônimo: usuário/senha vazios precisam virar NULL. Passando "" o
    // PubSubClient envia credenciais em branco e o broker recusa a conexão.
    const char* mqttUser = cfg.mqttUser[0] ? cfg.mqttUser : nullptr;
    const char* mqttPass = cfg.mqttPass[0] ? cfg.mqttPass : nullptr;
    String availability = T("availability");
    // Last Will: se o ESP cair, o broker publica "offline" → entidades ficam unavailable.
    if (mqttClient.connect(clientId, mqttUser, mqttPass,
                           availability.c_str(), 0, true, "offline")) {
      Serial.println("Conectado!");
      mqttRetryMs = 5000;
      mqttClient.publish(availability.c_str(), "online", true); // ficou online
      publishDiscoveryMessages();
      publishFullMqttState();
      publishCodesList();
      // Comando vale no instante em que é enviado, nunca depois. Uma mensagem
      // RETIDA num tópico de comando é reentregue a cada (re)conexão: a sirene
      // disparava sozinha em todo boot, e um "factory_reset" retido apagaria a
      // configuração a cada reinício. Por isso cada tópico tem a retida apagada
      // (payload vazio + retain) ANTES do subscribe. O MQTT processa em ordem
      // dentro da mesma conexão, então o subscribe já não recebe nada velho.
      static const char* const COMMAND_TOPICS[] = {
        "alarm/command",
        "siren/command",
        "siren_enabled/command",
        "codes/set",
        "system/command",
      };
      for (const char* suffix : COMMAND_TOPICS) {
        String t = T(suffix);
        mqttClient.publish(t.c_str(), "", true);
        mqttClient.subscribe(t.c_str());
      }
    } else {
      Serial.print("Falhou, rc=");
      Serial.print(mqttClient.state());
      mqttRetryMs = min(mqttRetryMs * 2, 60000UL);
      Serial.printf(" Tentando novamente em %lu segundos\n", mqttRetryMs / 1000);
    }
  }
}

// =================================================================
// 6. EVENTOS WEBSOCKET
// =================================================================
// =================================================================
// PÁGINA WEB: comandos e sessão de administrador
// =================================================================
//
// Acesso:  ver estado, armar e silenciar a sirene ....... livre
//          desarmar ..................................... qualquer senha cadastrada
//          admin ........................................ senha mestre
//
// O callback do WebSocket roda na tarefa do AsyncTCP. MQTT (PubSubClient),
// Telegram e NVS não são seguros fora do loop(): o callback só enfileira o
// texto do comando, e o loop() executa.
struct WebCmd { uint32_t client; char json[640]; };
QueueHandle_t webCmdQueue = nullptr;

// Sessão de admin presa à conexão WebSocket: some quando a página fecha ou
// depois de 10 min sem uso. O token vale para os envios por HTTP (OTA e
// restauração do backup), que não passam pelo WebSocket.
const unsigned long ADMIN_IDLE_MS = 10UL * 60UL * 1000UL;
struct AdminSession { uint32_t client; unsigned long lastMs; char token[33]; };
AdminSession adminSessions[4] = {};

bool isAdmin(uint32_t client) {
  for (auto& a : adminSessions) {
    if (a.client != client || client == 0) continue;
    if (millis() - a.lastMs < ADMIN_IDLE_MS) { a.lastMs = millis(); return true; }
    a.client = 0;
    return false;
  }
  return false;
}

void grantAdmin(uint32_t client) {
  AdminSession* slot = &adminSessions[0];
  for (auto& a : adminSessions) {
    if (a.client == client || a.client == 0) { slot = &a; break; }
    if (a.lastMs < slot->lastMs) slot = &a;   // sem vaga: substitui a mais antiga
  }
  slot->client = client;
  slot->lastMs = millis();
  snprintf(slot->token, sizeof(slot->token), "%08lx%08lx%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random());
}

void revokeAdmin(uint32_t client) {
  for (auto& a : adminSessions) if (a.client == client) { a.client = 0; a.token[0] = '\0'; }
}

const char* adminToken(uint32_t client) {
  for (auto& a : adminSessions) if (a.client == client && client) return a.token;
  return "";
}

// Chamado pelas rotas HTTP (tarefa do AsyncTCP): só lê.
bool adminTokenValid(const String& t) {
  if (t.length() != 32) return false;
  for (auto& a : adminSessions) {
    if (a.client && t.equals(a.token) && millis() - a.lastMs < ADMIN_IDLE_MS) return true;
  }
  return false;
}

// Pulsos de teste e reinício pedidos pelo admin (encerrados no loop()).
unsigned long testSirenUntil = 0, testLockUntil = 0;
volatile unsigned long pendingRestartAt = 0;
uint32_t wifiScanClient = 0;         // cliente que pediu a varredura de redes (0 = nenhuma)

// Responde a varredura assíncrona pedida pelo admin: uma entrada por nome de
// rede (a mais forte, se houver vários APs), da mais forte para a mais fraca.
void finishWifiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  struct Net { String ssid; int rssi; bool open; };
  std::vector<Net> nets;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;                      // rede oculta: digitar à mão
    int rssi = WiFi.RSSI(i);
    auto it = std::find_if(nets.begin(), nets.end(), [&](const Net& x) { return x.ssid == ssid; });
    if (it != nets.end()) { it->rssi = max(it->rssi, rssi); continue; }
    nets.push_back({ssid, rssi, WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net& x, const Net& y) { return x.rssi > y.rssi; });

  JsonDocument r;
  r["type"] = "result";
  r["command"] = "scanWifi";
  r["ok"] = n >= 0;
  if (n < 0) r["error"] = "A varredura falhou. Tente de novo.";
  JsonArray arr = r["networks"].to<JsonArray>();
  for (const Net& x : nets) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = x.ssid;
    o["rssi"] = x.rssi;
    o["open"] = x.open;
  }
  String out;
  serializeJson(r, out);
  ws.text(wifiScanClient, out);
  wifiScanClient = 0;
}

void wsReply(uint32_t client, const char* command, bool ok, const String& error = "") {
  JsonDocument d;
  d["type"] = "result";
  d["command"] = command;
  d["ok"] = ok;
  if (error.length()) d["error"] = error;
  String out; serializeJson(d, out);
  ws.text(client, out);
}

String getAdminJson(uint32_t client) {
  JsonDocument d;
  d["type"] = "admin";
  d["token"] = adminToken(client);
  JsonDocument codes;
  deserializeJson(codes, Codes::listJson(true));
  d["codes"] = codes["codes"];
  JsonArray zones = d["zones"].to<JsonArray>();
  for (int i = 0; i < NUM_SECTORS; i++) {
    JsonObject z = zones.add<JsonObject>();
    z["id"] = i + 1;
    z["name"] = zoneName(i);
    z["defaultName"] = sector_names[i];
    z["bypass"] = zoneBypassed(i);
    z["type"] = cfg.zoneType[i];
    z["home"] = (bool)(cfg.zoneHomeMask & (1 << i));
  }
  JsonObject tm = d["timing"].to<JsonObject>();
  tm["entryDelaySec"] = cfg.entryDelaySec;
  tm["exitDelaySec"] = cfg.exitDelaySec;
  tm["openAlertMin"] = cfg.openAlertMin;
  JsonObject aa = d["autoArm"].to<JsonObject>();
  aa["enabled"] = cfg.autoArmEnabled;
  aa["minute"] = cfg.autoArmMin;
  aa["days"] = cfg.autoArmDays;
  aa["mode"] = modeKey((ArmMode)cfg.autoArmMode);
  aa["tz"] = cfg.tz;
  JsonObject tg = d["telegram"].to<JsonObject>();
  tg["chatId"] = cfg.chatId;
  tg["hasToken"] = cfg.botToken[0] != '\0';
  JsonObject net = d["network"].to<JsonObject>();
  net["ssid"] = cfg.ssid;
  net["hostname"] = HOSTNAME;
  net["defaultHostname"] = defaultHostname();
  net["mqttId"] = mqttId();
  net["defaultMqttId"] = defaultMqttId();
  net["deviceName"] = devName();
  net["defaultDeviceName"] = defaultDeviceName();
  net["mqttHost"] = cfg.mqttHost;
  net["mqttPort"] = cfg.mqttPort;
  net["mqttUser"] = cfg.mqttUser;
  net["hasMqttPass"] = cfg.mqttPass[0] != '\0';
  net["ipMode"] = cfg.staticIp ? "static" : "dhcp";
  net["ip"] = ipStr(cfg.ipAddr);
  net["gateway"] = ipStr(cfg.ipGateway);
  net["subnet"] = ipStr(cfg.ipMask);
  net["dns"] = ipStr(cfg.ipDns);
  JsonObject now = net["current"].to<JsonObject>();
  now["ip"] = WiFi.localIP().toString();
  now["gateway"] = WiFi.gatewayIP().toString();
  now["subnet"] = WiFi.subnetMask().toString();
  now["dns"] = WiFi.dnsIP().toString();
  now["mac"] = WiFi.macAddress();
  JsonObject sir = d["siren"].to<JsonObject>();
  sir["enabled"] = cfg.sirenEnabled;
  sir["timeoutSec"] = cfg.sirenTimeoutSec;
  JsonObject sys = d["system"].to<JsonObject>();
  sys["version"] = FW_VERSION;
  sys["ip"] = WiFi.localIP().toString();
  sys["rssi"] = WiFi.RSSI();
  sys["uptimeS"] = millis() / 1000;
  sys["freeHeap"] = ESP.getFreeHeap();
  sys["mqtt"] = mqttClient.connected();
  sys["clock"] = EventLog::timeValid() ? (uint32_t)time(nullptr) : 0;
  String out; serializeJson(d, out);
  return out;
}

// Depois de uma mudança que vale para todos (ex.: backup restaurado).
void pushAdminToAll() {
  for (auto& a : adminSessions) if (a.client) ws.text(a.client, getAdminJson(a.client));
}

bool soDigitos(const String& s) {
  if (s.isEmpty()) return false;
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}

// ---------------------------------------------------------------- backup
// Tudo que é da casa (zonas, senhas, tempos, Telegram). Fica de fora o que é
// da placa e da rede (Wi-Fi, MQTT, nome e ID): restaurar numa placa nova na
// mesma casa não pode fazer as duas responderem pelo mesmo ID.
void fillBackup(JsonObject b) {
  b["homeguardBackup"] = 1;
  b["version"] = FW_VERSION;
  b["created"] = EventLog::timeValid() ? (uint32_t)time(nullptr) : 0;
  b["device"] = devName();
  JsonArray zones = b["zones"].to<JsonArray>();
  for (int i = 0; i < NUM_SECTORS; i++) {
    JsonObject z = zones.add<JsonObject>();
    z["name"] = zoneName(i);
    z["type"] = cfg.zoneType[i];
    z["home"] = (bool)(cfg.zoneHomeMask & (1 << i));
    z["bypass"] = zoneBypassed(i);
  }
  b["entryDelaySec"] = cfg.entryDelaySec;
  b["exitDelaySec"] = cfg.exitDelaySec;
  b["openAlertMin"] = cfg.openAlertMin;
  b["sirenEnabled"] = cfg.sirenEnabled;
  b["sirenTimeoutSec"] = cfg.sirenTimeoutSec;
  JsonObject aa = b["autoArm"].to<JsonObject>();
  aa["enabled"] = cfg.autoArmEnabled;
  aa["minute"] = cfg.autoArmMin;
  aa["days"] = cfg.autoArmDays;
  aa["mode"] = modeKey((ArmMode)cfg.autoArmMode);
  b["tz"] = cfg.tz;
  b["telegramToken"] = cfg.botToken;
  b["telegramChatId"] = cfg.chatId;
  b["master"] = cfg.masterCode;
  JsonDocument codes;
  deserializeJson(codes, Codes::exportJson());
  b["codes"] = codes;
}

// Confere o backup inteiro antes de mexer em qualquer coisa. "" = válido.
String validateBackup(JsonDocument& b) {
  if ((b["homeguardBackup"] | 0) != 1) return "Este arquivo não é um backup do Homeguard.";
  String master = b["master"] | "";
  if (!soDigitos(master) || master.length() < 4 || master.length() > 8) return "Senha mestre inválida no backup.";
  if (!b["zones"].is<JsonArray>() || !b["codes"].is<JsonArray>()) return "Backup incompleto.";
  if (b["codes"].size() > HG_MAX_CODES) return "Backup com senhas demais.";
  for (JsonObject c : b["codes"].as<JsonArray>()) {
    String code = c["code"] | "";
    if (!soDigitos(code) || code.length() < 4 || code.length() > 8) return "Senha inválida no backup.";
  }
  return "";
}

void applyBackup(JsonDocument& b) {
  int i = 0;
  for (JsonObject z : b["zones"].as<JsonArray>()) {
    if (i >= NUM_SECTORS) break;
    String name = z["name"] | "";
    name.trim();
    strlcpy(cfg.zoneNames[i], name.equals(sector_names[i]) ? "" : name.c_str(), sizeof(cfg.zoneNames[i]));
    cfg.zoneType[i] = constrain((int)(z["type"] | 0), 0, 3);
    bitWrite(cfg.zoneHomeMask, i, z["home"] | true);
    bitWrite(cfg.zoneBypass, i, z["bypass"] | false);
    i++;
  }
  cfg.entryDelaySec = constrain((int)(b["entryDelaySec"] | 30), 0, 300);
  cfg.exitDelaySec = constrain((int)(b["exitDelaySec"] | 60), 0, 300);
  cfg.openAlertMin = constrain((int)(b["openAlertMin"] | 0), 0, 1440);
  cfg.sirenEnabled = b["sirenEnabled"] | true;
  cfg.sirenTimeoutSec = constrain((int)(b["sirenTimeoutSec"] | 180), 0, 1800);
  cfg.autoArmEnabled = b["autoArm"]["enabled"] | false;
  cfg.autoArmMin = constrain((int)(b["autoArm"]["minute"] | 1380), 0, 1439);
  cfg.autoArmDays = (b["autoArm"]["days"] | 0x7F) & 0x7F;
  cfg.autoArmMode = parseMode(b["autoArm"]["mode"] | "away");
  strlcpy(cfg.tz, b["tz"] | "<-03>3", sizeof(cfg.tz));
  strlcpy(cfg.botToken, b["telegramToken"] | "", sizeof(cfg.botToken));
  strlcpy(cfg.chatId, b["telegramChatId"] | "", sizeof(cfg.chatId));
  strlcpy(cfg.masterCode, b["master"] | "", sizeof(cfg.masterCode));
  strlcpy(cfg.haPin, cfg.masterCode, sizeof(cfg.haPin));
  Storage::saveConfig(cfg);

  KeypadCode list[HG_MAX_CODES];
  int n = 0;
  for (JsonObject c : b["codes"].as<JsonArray>()) {
    if (n >= HG_MAX_CODES) break;
    strlcpy(list[n].code, c["code"] | "", sizeof(list[n].code));
    strlcpy(list[n].label, c["label"] | "sem nome", sizeof(list[n].label));
    list[n].duress = c["duress"] | false;
    n++;
  }
  Codes::replaceAll(list, n);

  setupTelegram();
  configTzTime(cfg.tz, "a.st1.ntp.br", "pool.ntp.org");
  if (mqttClient.connected()) publishDiscoveryMessages();
  publishCodesList();
  EventLog::add("Backup restaurado (%d senhas)", n);
  broadcastFullState();
  pushAdminToAll();
}

// Backup recebido por HTTP: a rota só valida e entrega; quem grava é o loop().
JsonDocument* volatile pendingRestore = nullptr;

// ---------------------------------------------------------------- comandos
void handleWebCommand(uint32_t client, JsonDocument& doc) {
  const char* c = doc["command"] | "";

  // ---------------- livres ----------------
  if (!strcmp(c, "_bye")) { revokeAdmin(client); return; }
  if (!strcmp(c, "arm") || !strcmp(c, "toggleArm")) {   // toggleArm: interface antiga, só arma
    if (isArmed) { wsReply(client, "arm", true); return; }
    uint8_t open = armSystem(parseMode(doc["mode"] | "away"), doc["force"] | false, "pagina web");
    if (!open) { wsReply(client, "arm", true); return; }
    // Zona instantânea aberta: a página pergunta se arma mesmo assim.
    JsonDocument d;
    d["type"] = "result";
    d["command"] = "arm";
    d["ok"] = false;
    d["error"] = "Zona aberta: " + zoneList(open);
    JsonArray z = d["openZones"].to<JsonArray>();
    for (int i = 0; i < NUM_SECTORS; i++) if (open & (1 << i)) z.add(zoneName(i));
    String out; serializeJson(d, out);
    ws.text(client, out);
    return;
  }
  if (!strcmp(c, "silenceSiren")) {          // só a sirene física; o disparo continua
    handleSilenceSiren();
    wsReply(client, c, true);
    return;
  }
  if (!strcmp(c, "disarm")) {
    DisarmResult r = tryDisarm(String(doc["code"] | ""), "web");
    wsReply(client, c, r == DISARM_OK,
            r == DISARM_LOCKED ? authRemote.message() : r == DISARM_WRONG ? String("Senha incorreta.") : String());
    return;
  }
  if (!strcmp(c, "adminLogin")) {
    if (authRemote.remainingMs() > 0) { wsReply(client, c, false, authRemote.message()); return; }
    bool ok = isMaster(String(doc["code"] | ""));
    if (!ok) EventLog::add("Senha mestre errada no admin");
    authRemote.record(ok);
    if (!ok) { wsReply(client, c, false, "Senha mestre incorreta."); return; }
    grantAdmin(client);
    EventLog::add("Admin aberto pela pagina web");
    wsReply(client, c, true);
    ws.text(client, getAdminJson(client));
    return;
  }

  // ---------------- só admin ----------------
  if (!isAdmin(client)) {
    wsReply(client, c, false, "Sessão de administrador encerrada. Entre de novo com a senha mestre.");
    return;
  }

  if (!strcmp(c, "adminLogout")) { revokeAdmin(client); wsReply(client, c, true); return; }
  if (!strcmp(c, "getAdmin")) { ws.text(client, getAdminJson(client)); return; }
  if (!strcmp(c, "getEvents")) {
    JsonDocument d;
    d["type"] = "result";
    d["command"] = c;
    d["ok"] = true;
    JsonDocument ev;
    deserializeJson(ev, EventLog::json());
    d["events"] = ev;
    String out; serializeJson(d, out);
    ws.text(client, out);
    return;
  }
  if (!strcmp(c, "getBackup")) {
    JsonDocument d;
    d["type"] = "result";
    d["command"] = c;
    d["ok"] = true;
    fillBackup(d["backup"].to<JsonObject>());
    String out; serializeJson(d, out);
    ws.text(client, out);
    EventLog::add("Backup baixado pela pagina web");
    return;
  }

  bool ok = true;
  String err;

  if (!strcmp(c, "addCode")) {
    String code = doc["code"] | "";
    String label = doc["label"] | "";
    label.trim();
    if (!soDigitos(code) || code.length() < 4 || code.length() > 8) {
      ok = false; err = "A senha precisa ter de 4 a 8 números.";
    } else if (isMaster(code)) {
      ok = false; err = "Essa é a senha mestre.";
    } else if (!Codes::add(code, label.c_str(), doc["duress"] | false)) {
      ok = false; err = Codes::count() >= HG_MAX_CODES ? "Limite de senhas atingido." : "Essa senha já está cadastrada.";
    } else {
      EventLog::add("Senha cadastrada: %s", label.c_str());
      publishCodesList();
    }

  } else if (!strcmp(c, "removeCode")) {
    ok = Codes::removeAt(doc["index"] | -1);
    if (ok) { EventLog::add("Senha removida (posicao %d)", (int)(doc["index"] | -1) + 1); publishCodesList(); }
    else err = "Senha não encontrada.";

  } else if (!strcmp(c, "setZone")) {
    int i = (doc["id"] | 0) - 1;
    if (i < 0 || i >= NUM_SECTORS) { ok = false; err = "Zona inválida."; }
    else {
      String name = doc["name"] | "";
      name.trim();
      bool renomeou = !name.equals(zoneName(i));
      // nome igual ao padrão volta a seguir o padrão do firmware
      strlcpy(cfg.zoneNames[i], name.equals(sector_names[i]) ? "" : name.c_str(), sizeof(cfg.zoneNames[i]));
      bitWrite(cfg.zoneBypass, i, doc["bypass"] | false);
      cfg.zoneType[i] = constrain((int)(doc["type"] | (int)cfg.zoneType[i]), 0, 3);
      bitWrite(cfg.zoneHomeMask, i, doc["home"] | true);
      Storage::saveConfig(cfg);
      EventLog::add("Zona %d alterada no admin", i + 1);
      if (renomeou && mqttClient.connected()) publishDiscoveryMessages();   // nome novo no HA
      broadcastFullState();
    }

  } else if (!strcmp(c, "setTiming")) {
    cfg.entryDelaySec = constrain((int)(doc["entryDelaySec"] | 30), 0, 300);
    cfg.exitDelaySec = constrain((int)(doc["exitDelaySec"] | 60), 0, 300);
    cfg.openAlertMin = constrain((int)(doc["openAlertMin"] | 0), 0, 1440);
    Storage::saveConfig(cfg);

  } else if (!strcmp(c, "setAutoArm")) {
    cfg.autoArmEnabled = doc["enabled"] | false;
    cfg.autoArmMin = constrain((int)(doc["minute"] | 1380), 0, 1439);
    cfg.autoArmDays = (doc["days"] | 0x7F) & 0x7F;
    cfg.autoArmMode = parseMode(doc["mode"] | "away");
    String tz = doc["tz"] | cfg.tz;
    strlcpy(cfg.tz, tz.c_str(), sizeof(cfg.tz));
    Storage::saveConfig(cfg);
    configTzTime(cfg.tz, "a.st1.ntp.br", "pool.ntp.org");

  } else if (!strcmp(c, "testOutput")) {
    const char* alvo = doc["target"] | "";
    if (isArmed || isTriggered) { ok = false; err = "Desarme o sistema antes de testar."; }
    else if (!strcmp(alvo, "siren")) { digitalWrite(SIREN_RELAY_PIN, HIGH); testSirenUntil = millis() + 1000; }
    else if (!strcmp(alvo, "lock"))  { digitalWrite(GATE_LOCK_RELAY_PIN, HIGH); testLockUntil = millis() + 1000; }
    else { ok = false; err = "Saída inválida."; }

  } else if (!strcmp(c, "setTelegram")) {
    String token = doc["token"] | "";
    String chat = doc["chatId"] | "";
    token.trim(); chat.trim();
    if (token.length()) strlcpy(cfg.botToken, token.c_str(), sizeof(cfg.botToken));  // vazio = mantém
    strlcpy(cfg.chatId, chat.c_str(), sizeof(cfg.chatId));
    Storage::saveConfig(cfg);
    setupTelegram();

  } else if (!strcmp(c, "testTelegram")) {
    if (!bot || cfg.chatId[0] == '\0') { ok = false; err = "Preencha o token e o ID antes."; }
    else if (!sendTelegramAlert("✅ Teste: as notificações estão funcionando.")) {
      ok = false; err = "O Telegram recusou. Confira o token e o ID, e se você já mandou uma mensagem para o bot.";
    }

  } else if (!strcmp(c, "setSiren")) {
    int to = doc["timeoutSec"] | (int)cfg.sirenTimeoutSec;
    cfg.sirenTimeoutSec = constrain(to, 0, 1800);
    handleSirenEnabled(doc["enabled"] | cfg.sirenEnabled);   // grava e avisa todo mundo

  } else if (!strcmp(c, "setMaster")) {
    String atual = doc["current"] | "";
    String nova = doc["newCode"] | "";
    if (authRemote.remainingMs() > 0) { ok = false; err = authRemote.message(); }
    else {
      bool confere = isMaster(atual);
      authRemote.record(confere);
      if (!confere) { ok = false; err = "A senha mestre atual não confere."; }
      else if (!soDigitos(nova) || nova.length() < 4 || nova.length() > 8) { ok = false; err = "A senha mestre precisa ter de 4 a 8 números."; }
      else if (Codes::check(nova)) { ok = false; err = "Essa senha já é de um usuário."; }
      else {
        strlcpy(cfg.masterCode, nova.c_str(), sizeof(cfg.masterCode));
        strlcpy(cfg.haPin, nova.c_str(), sizeof(cfg.haPin));
        Storage::saveConfig(cfg);
        EventLog::add("Senha mestre alterada");
      }
    }

  } else if (!strcmp(c, "setNetwork")) {
    String ssid = doc["ssid"] | "";
    ssid.trim();
    String host = doc["hostname"] | "";               // vazio = volta ao padrão
    host.trim();
    host.toLowerCase();
    String id = doc["mqttId"] | "";                   // vazio = volta ao padrão
    id.trim();
    id.toLowerCase();
    String name = doc["deviceName"] | "";
    name.trim();
    uint32_t ipA = 0, ipG = 0, ipM = 0, ipD = 0;
    if (ssid.isEmpty() || ssid.length() > 32) { ok = false; err = "Informe o nome da rede Wi-Fi."; }
    else if (host.length() && !validHostname(host)) {
      ok = false; err = "Nome na rede: só letras, números e hífen (até 32, sem hífen nas pontas).";
    }
    else if (id.length() && !validHostname(id)) {
      ok = false; err = "ID no MQTT: só letras, números e hífen (até 32, sem hífen nas pontas).";
    }
    else if (name.length() > 32) { ok = false; err = "Nome da central: até 32 caracteres."; }
    else if (!strcmp(doc["ipMode"] | "dhcp", "static") && !(
               parseIp(doc["ip"] | "", ipA) && parseIp(doc["gateway"] | "", ipG) && parseIp(doc["subnet"] | "", ipM))) {
      ok = false; err = "IP fixo: preencha IP, gateway e máscara (ex.: 192.168.1.50).";
    }
    else if (!strcmp(doc["ipMode"] | "dhcp", "static") && (err = checkStaticIp(ipA, ipG, ipM)).length()) {
      ok = false;
    }
    else if (strlen(doc["dns"] | "") && !parseIp(doc["dns"] | "", ipD)) { ok = false; err = "DNS inválido."; }
    else {
      cfg.staticIp = !strcmp(doc["ipMode"] | "dhcp", "static");
      if (cfg.staticIp) { cfg.ipAddr = ipA; cfg.ipGateway = ipG; cfg.ipMask = ipM; cfg.ipDns = ipD; }
      // ID novo: o HA esquece as entidades do ID antigo (senão ficam lá, indisponíveis).
      String novoId = id.length() ? id : defaultMqttId();
      if (novoId != mqttId() && mqttClient.connected()) clearDiscovery();
      strlcpy(cfg.mqttId, novoId == defaultMqttId() ? "" : novoId.c_str(), sizeof(cfg.mqttId));
      strlcpy(cfg.deviceName, name == defaultDeviceName() ? "" : name.c_str(), sizeof(cfg.deviceName));
      strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
      strlcpy(cfg.hostname, host == defaultHostname() ? "" : host.c_str(), sizeof(cfg.hostname));
      String pass = doc["pass"] | "";                  // vazio = mantém a senha atual
      if (pass.length()) strlcpy(cfg.wifiPass, pass.c_str(), sizeof(cfg.wifiPass));
      strlcpy(cfg.mqttHost, (doc["mqttHost"] | ""), sizeof(cfg.mqttHost));
      cfg.mqttPort = doc["mqttPort"] | 1883;
      strlcpy(cfg.mqttUser, (doc["mqttUser"] | ""), sizeof(cfg.mqttUser));
      String mp = doc["mqttPass"] | "";
      if (mp.length()) strlcpy(cfg.mqttPass, mp.c_str(), sizeof(cfg.mqttPass));
      Storage::saveConfig(cfg);
      EventLog::add("Rede alterada no admin; reiniciando");
      // Se a rede nova não conectar, o boot cai no portal sozinho.
      pendingRestartAt = millis() + 1500;
    }

  } else if (!strcmp(c, "scanWifi")) {
    // Assíncrona: a placa continua conectada e o loop() segue rodando; a
    // resposta sai em finishWifiScan() quando a varredura termina (~4 s).
    if (wifiScanClient) { wsReply(client, c, false, "Já existe uma varredura em andamento."); return; }
    if (WiFi.scanNetworks(true, false, false, 300) == WIFI_SCAN_FAILED) {
      wsReply(client, c, false, "Não foi possível iniciar a varredura.");
      return;
    }
    wifiScanClient = client;
    return;

  } else if (!strcmp(c, "reboot")) {
    EventLog::add("Reinicio pedido pelo admin");
    pendingRestartAt = millis() + 1500;

  } else {
    ok = false; err = "Comando desconhecido.";
  }

  wsReply(client, c, ok, err);
  ws.text(client, getAdminJson(client));
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("Cliente WebSocket #%u conectado\n", client->id());
    client->text(getFullStateJson());
    return;
  }
  WebCmd cmd;
  cmd.client = client->id();
  if (type == WS_EVT_DISCONNECT) {
    Serial.printf("Cliente WebSocket #%u desconectado\n", client->id());
    strlcpy(cmd.json, "{\"command\":\"_bye\"}", sizeof(cmd.json));
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)) return;
    if (len >= sizeof(cmd.json)) return;
    memcpy(cmd.json, data, len);
    cmd.json[len] = '\0';
  } else {
    return;
  }
  if (!webCmdQueue || xQueueSend(webCmdQueue, &cmd, 0) != pdTRUE) {
    if (type == WS_EVT_DATA) {
      // Responde com o nome do comando, senão a página fica esperando até o timeout.
      JsonDocument d, r;
      deserializeJson(d, cmd.json);
      r["type"] = "result";
      r["command"] = d["command"] | "";
      r["ok"] = false;
      r["error"] = "Placa ocupada, tente de novo.";
      String out;
      serializeJson(r, out);
      client->text(out);
    }
  }
}

// ---------------------------------------------------------------- rotas HTTP
// A interface vem embutida no firmware, comprimida (web_assets.h).
void setupWebRoutes() {
#ifdef HG_HAS_WEB
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    const WebAsset* a = &WEB_ASSETS[i];
    bool isIndex = !strcmp(a->path, "/index.html");
    auto handler = [a, isIndex](AsyncWebServerRequest* req) {
      // Quem entrou pelo hotspot de resgate quer a configuração da rede.
      if (isIndex && apActive && req->client()->localIP() == WiFi.softAPIP()) {
        req->redirect("/setup/");
        return;
      }
      AsyncWebServerResponse* r = req->beginResponse(200, a->mime, a->data, a->len);
      r->addHeader("Content-Encoding", "gzip");
      // Os .js/.css têm hash no nome: podem ficar em cache para sempre. O
      // index.html não, senão o navegador continua na interface velha após o OTA.
      r->addHeader("Cache-Control", isIndex ? "no-cache" : "public, max-age=31536000, immutable");
      req->send(r);
    };
    server.on(a->path, HTTP_GET, handler);
    if (isIndex) server.on("/", HTTP_GET, handler);
  }
#else
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html; charset=utf-8",
              "<h1>Homeguard</h1><p>Firmware compilado sem a interface. Rode "
              "<code>npm run build</code> em data/ e <code>tools/embed_web.py</code>, e recompile.</p>");
  });
#endif

  setupRescueRoutes();

  // --- Identificação (pública): o homeguard-admin varre a rede procurando isto ---
  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument d;
    d["homeguard"] = true;
    d["version"] = FW_VERSION;
    d["name"] = devName();
    d["hostname"] = HOSTNAME;
    d["mqttId"] = mqttId();
    d["ip"] = WiFi.localIP().toString();
    d["ipMode"] = cfg.staticIp ? "static" : "dhcp";
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
  });

  // --- Atualização do firmware (OTA) ---
  // O token da sessão de admin vai na URL (?t=). O arquivo é gravado no slot
  // livre; só no fim, se a imagem for válida, o boot passa para ele.
  static String otaError;
  static bool otaAllowed = false;
  server.on("/api/ota", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      bool ok = otaAllowed && otaError.isEmpty() && !Update.isRunning() && !Update.hasError();
      JsonDocument d;
      d["ok"] = ok;
      if (!ok) d["error"] = otaError.length() ? otaError : String("O envio não terminou.");
      String out; serializeJson(d, out);
      req->send(ok ? 200 : 400, "application/json", out);
      // Zera para o próximo envio: um POST sem arquivo não pode herdar o "ok" deste.
      otaAllowed = false;
      otaError = "";
      if (ok) pendingRestartAt = millis() + 2000;
    },
    [](AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
      if (index == 0) {
        otaError = "";
        otaAllowed = req->hasParam("t") && adminTokenValid(req->getParam("t")->value());
        if (!otaAllowed) { otaError = "Sessão de administrador inválida. Entre de novo."; return; }
        if (Update.isRunning()) Update.abort();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
          otaError = String("Não deu para começar: ") + Update.errorString();
          return;
        }
        Serial.printf("[OTA] recebendo %s\n", filename.c_str());
      }
      if (!otaAllowed || otaError.length()) return;
      if (len && Update.write(data, len) != len) {
        otaError = String("Falha ao gravar: ") + Update.errorString();
        Update.abort();
        return;
      }
      if (final && !Update.end(true)) {
        otaError = String("Arquivo recusado (não é um firmware desta placa?): ") + Update.errorString();
      }
    });

  // --- Restauração do backup ---
  server.on("/api/restore", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      String err;
      if (!req->hasParam("t") || !adminTokenValid(req->getParam("t")->value())) err = "Sessão de administrador inválida. Entre de novo.";
      else if (!req->_tempObject) err = "Arquivo vazio.";
      else if (pendingRestore) err = "Já existe uma restauração em andamento.";
      else {
        JsonDocument* b = new JsonDocument();
        if (deserializeJson(*b, (const char*)req->_tempObject)) err = "O arquivo não é um JSON válido.";
        else err = validateBackup(*b);
        if (err.length()) delete b; else pendingRestore = b;   // o loop() aplica
      }
      JsonDocument d;
      d["ok"] = err.isEmpty();
      if (err.length()) d["error"] = err;
      String out; serializeJson(d, out);
      req->send(err.isEmpty() ? 200 : 400, "application/json", out);
    },
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (total > 8192) return;                        // backup real tem ~2 KB
      if (index == 0) req->_tempObject = calloc(total + 1, 1);   // liberado pelo servidor
      if (req->_tempObject) memcpy((uint8_t*)req->_tempObject + index, data, len);
    });
}

// =================================================================
// 7. SETUP
// =================================================================

// OTA: o firmware novo só vira "o oficial" depois de rodar 1 minuto conectado.
// Se ele travar, reiniciar ou não conseguir conectar antes disso, o bootloader
// volta sozinho para a versão anterior no próximo boot.
// verifyRollbackLater() fica em rollback.cpp (ver o comentário lá).
bool otaPendingVerify() {
  esp_ota_img_states_t st;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
         st == ESP_OTA_IMG_PENDING_VERIFY;
}

// Abre o portal de configuração (hotspot) e NÃO retorna — reinicia ao salvar.
void runProvisioningPortal() {
  // A mensagem precisa dizer QUAL dos dois casos e: sem config salva, ou config
  // salva que nao conectou. Dizer sempre "sem configuracao" manda quem esta
  // depurando investigar a NVS quando o problema e o Wi-Fi.
  Serial.printf("[PROV] %s — abrindo portal.\n",
                cfg.configured ? "Configuracao existe, mas o Wi-Fi nao conectou"
                               : "Sem configuracao salva");
  Provisioning::begin();
  bool emTeste = otaPendingVerify();
  while (!Provisioning::isDone()) {
    Provisioning::handle();
    // Firmware recém-atualizado que não conectou no Wi-Fi: em vez de esperar
    // alguém no portal, reinicia e deixa o bootloader voltar à versão anterior.
    if (emTeste && millis() > 5UL * 60UL * 1000UL) {
      Serial.println("[OTA] firmware novo nao conectou: voltando para o anterior");
      ESP.restart();
    }
    if (!checkFactoryResetButton(millis()))
      rgbShow(0, 0, ((millis() / 600) % 2) ? 0 : 45);   // azul piscando
    delay(5);
  }
  Serial.println("[PROV] Salvo. Reiniciando...");
  delay(800);
  ESP.restart();
}

// Conecta no WiFi com timeout. Retorna false se não conseguir.
// Último motivo de desconexão informado pelo driver (0 = nenhum). É o número que
// separa senha errada (15/204) de rede inexistente (201) de AP que nem respondeu.
static volatile uint8_t lastWifiReason = 0;

bool connectWiFi(uint32_t timeoutMs) {
  static bool handlerSet = false;
  if (!handlerSet) {
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
      lastWifiReason = info.wifi_sta_disconnected.reason;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    handlerSet = true;
  }

  // Antes do mode(): no core 3.x o nome é aplicado quando a interface STA é
  // criada. É o que vai para o DHCP e aparece na lista de clientes do roteador.
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  // As novas tentativas ficam por conta do maintainWifi(): o reconectar
  // automático do core brigava com elas e com o hotspot de resgate.
  WiFi.setAutoReconnect(false);
  lastWifiReason = 0;
  applyIpConfig();
  WiFi.begin(cfg.ssid, cfg.wifiPass);
  Serial.print("Conectando ao WiFi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConectado! IP: %s  |  http://%s.local\n",
                  WiFi.localIP().toString().c_str(), HOSTNAME);
    return true;
  }

  // Sem isto, "Falha ao conectar" não distingue rede inexistente de senha errada
  // de AP que recusa a placa — e o diagnóstico vira adivinhação. O código de
  // motivo do driver é mais preciso que o status; o status só vale quando o
  // driver não reportou motivo nenhum (o AP nem respondeu).
  const char* motivo;
  switch (lastWifiReason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      motivo = "senha errada"; break;
    case WIFI_REASON_AUTH_FAIL:              motivo = "autenticação recusada (senha errada ou modo de segurança)"; break;
    case WIFI_REASON_NO_AP_FOUND:            motivo = "rede não encontrada (SSID errado, oculta ou só 5 GHz)"; break;
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
                                             motivo = "a rede usa um modo de segurança que a placa não aceita"; break;
    case WIFI_REASON_ASSOC_FAIL:             motivo = "o AP recusou a associação (filtro de MAC ou limite de clientes)"; break;
    case WIFI_REASON_AUTH_EXPIRE:            motivo = "o AP não respondeu à autenticação (sinal fraco?)"; break;
    default:
      switch (WiFi.status()) {
        case WL_NO_SSID_AVAIL: motivo = "rede não encontrada (SSID errado, oculta ou só 5 GHz)"; break;
        case WL_IDLE_STATUS:   motivo = "o AP não respondeu à placa (filtro de MAC, PMF ou sinal fraco)"; break;
        default:               motivo = "não completou a conexão dentro do tempo"; break;
      }
  }
  Serial.printf("\nFalha ao conectar no WiFi \"%s\": %s (status=%d, reason=%d)\n",
                cfg.ssid, motivo, (int)WiFi.status(), (int)lastWifiReason);
  return false;
}

// Por que a placa ligou. Falta de energia, travamento e watchdog avisam no
// Telegram (energia cortada pode ser sabotagem); reinício pedido não.
String bootAlert;
void logBoot() {
  const char* motivo;
  bool avisa = true;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  motivo = "energia ligada / voltou"; break;
    case ESP_RST_BROWNOUT: motivo = "queda de tensao"; break;
    case ESP_RST_PANIC:    motivo = "travamento (erro no firmware)"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      motivo = "watchdog (firmware travou)"; break;
    case ESP_RST_SW:       motivo = "reinicio pedido"; avisa = false; break;
    case ESP_RST_EXT:      motivo = "botao de reset"; break;
    case ESP_RST_USB:      motivo = "reset pela USB (gravacao)"; avisa = false; break;
    default:               motivo = "outro"; break;
  }
  EventLog::add("Central ligada (v%s): %s", FW_VERSION, motivo);
  if (otaPendingVerify()) EventLog::add("Firmware novo em teste: v%s", FW_VERSION);
  if (avisa) bootAlert = String("🔌 Central ligada: ") + motivo + ".";
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\n\nINICIALIZANDO FIRMWARE ESP32 HOMEGUARD (%s)\n", FW_MARKER);

  // Carrega configuração e senhas da NVS
  cfg = Storage::loadConfig();
  strlcpy(HOSTNAME, cfg.hostname[0] ? cfg.hostname : defaultHostname().c_str(), sizeof(HOSTNAME));
  topicBase = mqttId();
  uidBase = "esp32_" + topicBase;
  uidBase.replace('-', '_');
  Codes::load();

  wiegand.begin(WIEGAND_D0_PIN, WIEGAND_D1_PIN);

  for (int i = 0; i < NUM_SECTORS; i++) {
    pinMode(sector_pins[i], INPUT_PULLUP);
    last_sector_status[i] = readSensorStatus(sector_pins[i]);
    sector_status[i] = last_sector_status[i];
  }
  
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  pinMode(SIREN_RELAY_PIN, OUTPUT);
  digitalWrite(SIREN_RELAY_PIN, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(GATE_LOCK_RELAY_PIN, OUTPUT);
  updateHardwareState();
  rgbShow(0, 0, 0);   // apaga o RGB até o primeiro updateRgbLed()

  // O alarme não depende do LittleFS: sem ele, só o registro de eventos não grava.
  if (!EventLog::begin()) {
    Serial.println("AVISO: falha ao montar o LittleFS — o registro de eventos nao sera gravado.");
  }
  logBoot();

  // Voltou de uma queda de energia armado: continua armado, sem contagem de saída.
  uint8_t savedMode, savedTb;
  if (Storage::loadArmState(savedMode, savedTb)) {
    isArmed = true;
    armMode = savedMode == MODE_HOME ? MODE_HOME : MODE_AWAY;
    tempBypass = savedTb;
    updateHardwareState();
    EventLog::add("Continua armado (%s) apos reinicio", modeName(armMode));
  }

  // Sem configuração → portal hotspot (não retorna; reinicia ao salvar).
  if (!cfg.configured) {
    runProvisioningPortal();
  }

  // Já configurada: tenta o Wi-Fi e segue com o alarme de qualquer jeito. Depois
  // de uma queda de energia o roteador demora 1-2 min para voltar, e antes a placa
  // desistia em 20 s e ficava presa no portal — a casa ficava sem alarme até
  // alguém configurar. Agora o loop() continua tentando (maintainWifi) e, se a
  // rede não voltar em 3 min, abre o hotspot de configuração junto com o alarme.
  if (!connectWiFi(10000)) {
    EventLog::add("Sem Wi-Fi no boot: alarme ativo, tentando reconectar");
  }

  // --- Relógio (NTP), para o registro de eventos e o arme automático ---
  configTzTime(cfg.tz, "a.st1.ntp.br", "pool.ntp.org");

  // --- Telegram (token vem da config) ---
  setupTelegram();
  if (bootAlert.length()) sendTelegramAlert(bootAlert);

  // --- MQTT Setup ---
  mqttClient.setServer(cfg.mqttHost, cfg.mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048); // AUMENTADO: Resolve falhas de publicação com payloads grandes.
  // O broker fica na rede local: se não atender em 1,5 s, não vai atender. O padrão
  // (3 s no connect + 15 s esperando o CONNACK) congelava o loop() a cada tentativa.
  wifiClient.setConnectionTimeout(1500);
  mqttClient.setSocketTimeout(5);

  // --- mDNS: permite acessar por http://homeguard.local em vez de decorar o IP ---
  // (nativo em iOS/macOS, Windows 10+ e Linux com avahi; no Android é irregular)
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS ativo: http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("AVISO: falha ao iniciar o mDNS — use o IP.");
  }

  secured_client.setInsecure();
  webCmdQueue = xQueueCreate(6, sizeof(WebCmd));
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);
  setupWebRoutes();
  server.begin();
  
  Serial.println("Servidores Web e WebSocket iniciados. Sistema pronto.");
}

// =================================================================
// 8. LOOP PRINCIPAL
// =================================================================

// Lê as zonas e decide, conforme o tipo de cada uma:
//   instantânea ... dispara na hora
//   entrada/saída . ignora durante a saída; ao entrar, começa a contagem de entrada
//   seguidora ..... ignora enquanto alguma contagem corre; senão, instantânea
//   24 horas ...... dispara ao abrir, armado ou não
void checkSensors(unsigned long now) {
  bool changed = false;
  for (int i = 0; i < NUM_SECTORS; i++) {
    String st = readSensorStatus(sector_pins[i]);
    bool opened = st == "Aberto" && last_sector_status[i] != "Aberto";
    if (st != last_sector_status[i]) { changed = true; last_sector_status[i] = st; }
    sector_status[i] = st;
    bool open = zoneOpen(i);

    if (!open) { openSinceMs[i] = 0; openAlerted[i] = false; }
    else if (!openSinceMs[i]) openSinceMs[i] = now | 1;

    if (zoneBypassed(i)) continue;
    checkOpenTooLong(i, now);

    if (cfg.zoneType[i] == ZT_24H) {
      if (opened && !isTriggered) { triggerAlarm(i); changed = true; }
      continue;
    }
    // O disparo é avaliado pelo estado ATUAL, não só na transição: uma zona que
    // já estava aberta quando a contagem de saída acabou também dispara.
    if (!isArmed || isTriggered || !open || !zoneActive(i, armMode)) continue;
    switch (cfg.zoneType[i]) {
      case ZT_DELAYED:
        if (exitRunning || entryRunning) break;
        if (cfg.entryDelaySec == 0) { triggerAlarm(i); changed = true; break; }
        entryRunning = true;
        entryEndsAt = now + cfg.entryDelaySec * 1000UL;
        entryZone = i;
        EventLog::add("Entrada por %s: aguardando senha", zoneName(i));
        changed = true;
        break;
      case ZT_FOLLOWER:
        if (exitRunning || entryRunning) break;
        triggerAlarm(i); changed = true;
        break;
      default:
        triggerAlarm(i); changed = true;
        break;
    }
  }

  if (exitRunning && (long)(now - exitEndsAt) >= 0) { exitRunning = false; changed = true; }
  if (entryRunning && (long)(now - entryEndsAt) >= 0) { triggerAlarm(entryZone); changed = true; }
  if (changed) broadcastFullState();
}

// Zona aberta há mais de N minutos sem estar sendo vigiada (portão esquecido).
// Um aviso por abertura.
void checkOpenTooLong(int i, unsigned long now) {
  if (!cfg.openAlertMin || !openSinceMs[i] || openAlerted[i]) return;
  if (isArmed && zoneActive(i, armMode)) return;          // aí é disparo, não aviso
  if (now - openSinceMs[i] < cfg.openAlertMin * 60000UL) return;
  openAlerted[i] = true;
  EventLog::add("%s aberta ha %u min", zoneName(i), cfg.openAlertMin);
  sendTelegramAlert(String("🚪 ") + zoneName(i) + " está aberta há " + cfg.openAlertMin + " min.");
  publishNotice("zone_open_long", zoneName(i));
}

// Arme automático no horário configurado. Com zona aberta não arma: avisa.
void checkAutoArm(unsigned long now) {
  static unsigned long lastCheck = 0;
  static int lastDay = -1;
  if (!cfg.autoArmEnabled || now - lastCheck < 5000) return;
  lastCheck = now;
  struct tm t;
  if (!EventLog::timeValid() || !getLocalTime(&t, 10)) return;
  if (t.tm_hour * 60 + t.tm_min != cfg.autoArmMin) return;
  if (!(cfg.autoArmDays & (1 << t.tm_wday)) || lastDay == t.tm_yday) return;
  lastDay = t.tm_yday;
  if (isArmed) return;
  ArmMode m = (ArmMode)cfg.autoArmMode;
  uint8_t open = armSystem(m, false, "agenda");
  if (open) {
    EventLog::add("Arme automatico cancelado: %s aberta", zoneList(open).c_str());
    sendTelegramAlert("⚠️ O arme automático (" + String(modeName(m)) + ") não aconteceu. Zona aberta: " + zoneList(open));
    publishNotice("auto_arm_failed", zoneList(open).c_str());
  } else {
    sendTelegramAlert("🔒 Armado automaticamente (" + String(modeName(m)) + ").");
  }
}

// ---------------------------------------------------------------- Wi-Fi e hotspot de resgate
//
// Sem Wi-Fi, o alarme continua (zonas, teclado, sirene, registro). A placa tenta
// a rede salva a cada 15 s; depois de 3 min sem conseguir, sobe o hotspot
// HOMEGUARD-XXXX com a página de configuração (/setup/), sem parar o alarme.
// Quando a rede volta, o hotspot fecha sozinho. Pelo teclado (#, mestre, 3) ele
// abre na hora, por 10 min, mesmo com Wi-Fi.
const unsigned long RESCUE_AFTER_MS = 3UL * 60UL * 1000UL;
const unsigned long RESCUE_MANUAL_MS = 10UL * 60UL * 1000UL;
DNSServer rescueDns;
unsigned long wifiDownSince = 0, lastWifiTry = 0, apManualUntil = 0;
String rescueNetsJson = "[]";
volatile bool rescueRescan = false;
volatile bool rescueAuthFail = false;
JsonDocument* volatile pendingSetup = nullptr;

String apName() {
  String s = boardSuffix();
  s.toUpperCase();
  return "HOMEGUARD-" + s;
}

// Lista de redes para a página. Varre ANTES de subir o hotspot: com ele no ar e
// um celular conectado, a varredura volta vazia (ver provisioning.cpp).
void scanForRescue(int n) {
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    bool dup = false;
    for (JsonObject o : arr) if (ssid == (const char*)o["ssid"]) { dup = true; break; }
    if (dup) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  if (arr.size()) { rescueNetsJson = ""; serializeJson(d, rescueNetsJson); }
}

void openRescueAp(bool manual) {
  if (manual) apManualUntil = millis() + RESCUE_MANUAL_MS;
  if (apActive) return;
  scanForRescue(WiFi.scanNetworks(false, false, false, 300));
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName().c_str());
  rescueDns.start(53, "*", WiFi.softAPIP());
  apActive = true;
  EventLog::add("Hotspot %s aberto (%s)", apName().c_str(), manual ? "teclado" : "sem Wi-Fi ha 3 min");
}

void closeRescueAp() {
  if (!apActive) return;
  rescueDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  apManualUntil = 0;
  EventLog::add("Hotspot de configuracao fechado");
}

void maintainWifi(unsigned long now) {
  if (apActive) rescueDns.processNextRequest();
  if (rescueAuthFail) {
    rescueAuthFail = false;
    EventLog::add("Senha mestre errada no hotspot");
    authRemote.record(false);
  }
  if (pendingSetup) {
    JsonDocument* d = pendingSetup;
    applySetup(*d);
    delete d;
    pendingSetup = nullptr;
  }
  if (rescueRescan && apActive && WiFi.softAPgetStationNum() == 0) {
    rescueRescan = false;
    scanForRescue(WiFi.scanNetworks(false, false, false, 300));
  }

  bool manual = apManualUntil && (long)(apManualUntil - now) > 0;
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDownSince) {
      EventLog::add("Wi-Fi conectado (%s)", WiFi.localIP().toString().c_str());
      wifiDownSince = 0;
    }
    if (apActive && !manual) closeRescueAp();
    flushPendingAlerts();
    return;
  }
  if (!wifiDownSince) {
    wifiDownSince = now | 1;
    lastWifiTry = now;              // o core acabou de tentar; espera antes da próxima
  }
  if (!apActive && (manual || now - wifiDownSince > RESCUE_AFTER_MS)) openRescueAp(false);
  if (apActive && apManualUntil && !manual && now - wifiDownSince < RESCUE_AFTER_MS) closeRescueAp();
  // Tentar a rede faz o rádio trocar de canal e derruba quem estiver no
  // hotspot: com alguém conectado nele, só tenta depois que sair.
  unsigned long every = apActive ? 60000UL : 15000UL;
  if (now - lastWifiTry > every && (!apActive || WiFi.softAPgetStationNum() == 0)) {
    lastWifiTry = now;
    WiFi.disconnect(false);
    WiFi.begin(cfg.ssid, cfg.wifiPass);
  }
}

// Salvo pela página do hotspot (já com a senha mestre conferida): grava e reinicia.
void applySetup(JsonDocument& d) {
  String ssid = d["ssid"] | "";
  ssid.trim();
  String pass = d["pass"] | "";
  bool trocouRede = ssid != cfg.ssid;
  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  // Em branco mantém a senha salva; rede aberta escolhida na lista zera.
  if (pass.length() || (d["open"] | false)) strlcpy(cfg.wifiPass, pass.c_str(), sizeof(cfg.wifiPass));
  strlcpy(cfg.mqttHost, d["mqttHost"] | cfg.mqttHost, sizeof(cfg.mqttHost));
  cfg.mqttPort = d["mqttPort"] | (int)cfg.mqttPort;
  strlcpy(cfg.mqttUser, d["mqttUser"] | "", sizeof(cfg.mqttUser));
  String mp = d["mqttPass"] | "";
  if (mp.length()) strlcpy(cfg.mqttPass, mp.c_str(), sizeof(cfg.mqttPass));
  String tk = d["botToken"] | "";
  tk.trim();
  if (tk.length()) strlcpy(cfg.botToken, tk.c_str(), sizeof(cfg.botToken));
  strlcpy(cfg.chatId, d["chatId"] | "", sizeof(cfg.chatId));
  if (d["dhcp"] | false) cfg.staticIp = false;      // saída de um IP fixo errado
  Storage::saveConfig(cfg);
  EventLog::add("Rede salva pelo hotspot%s; reiniciando", trocouRede ? " (Wi-Fi novo)" : "");
  pendingRestartAt = millis() + 1500;
}

// Rotas da página de configuração no servidor do alarme. Funcionam pelo hotspot
// e também pela rede normal; salvar sempre exige a senha mestre atual.
void setupRescueRoutes() {
  server.on("/setup", HTTP_GET, [](AsyncWebServerRequest* req) { req->redirect("/setup/"); });
  server.on("/setup/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html; charset=utf-8", (const uint8_t*)PORTAL_HTML, strlen_P(PORTAL_HTML));
  });
  server.on("/setup/current", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument d;
    d["configured"] = true;
    d["name"] = devName();
    d["ssid"] = cfg.ssid;
    d["mqttHost"] = cfg.mqttHost;
    d["mqttPort"] = cfg.mqttPort;
    d["mqttUser"] = cfg.mqttUser;
    d["hasMqttPass"] = cfg.mqttPass[0] != '\0';
    d["hasToken"] = cfg.botToken[0] != '\0';
    d["chatId"] = cfg.chatId;
    d["staticIp"] = cfg.staticIp ? ipStr(cfg.ipAddr) : String();
    String out; serializeJson(d, out);
    req->send(200, "application/json", out);
  });
  server.on("/setup/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("again")) rescueRescan = true;   // o loop varre quando ninguém estiver no hotspot
    req->send(200, "application/json", rescueNetsJson);
  });
  server.on("/setup/save", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      String err;
      JsonDocument* d = new JsonDocument();
      if (!req->_tempObject || deserializeJson(*d, (const char*)req->_tempObject)) err = "Dados inválidos.";
      else if (authRemote.remainingMs() > 0) err = authRemote.message();
      else if (!isMaster(String((*d)["master"] | ""))) { err = "Senha mestre incorreta."; rescueAuthFail = true; }
      else if (String((*d)["ssid"] | "").isEmpty()) err = "Escolha a rede Wi-Fi.";
      else if (pendingSetup) err = "Já está salvando.";
      if (err.length()) {
        delete d;
        JsonDocument r;
        r["ok"] = false;
        r["error"] = err;
        String out; serializeJson(r, out);
        req->send(400, "application/json", out);
        return;
      }
      pendingSetup = d;                                // o loop grava e reinicia
      req->send(200, "application/json", "{\"ok\":true}");
    },
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (total > 2048) return;
      if (index == 0) req->_tempObject = calloc(total + 1, 1);
      if (req->_tempObject) memcpy((uint8_t*)req->_tempObject + index, data, len);
    });
  // Captive portal: pelo hotspot, qualquer endereço cai na configuração.
  server.onNotFound([](AsyncWebServerRequest* req) {
    if (apActive) req->redirect("http://192.168.4.1/setup/");
    else req->send(404, "text/plain", "Não encontrado");
  });
}

// Confirma o firmware novo (ver verifyRollbackLater) depois de 1 min no ar e conectado.
void confirmFirmware(unsigned long now) {
  static bool done = false;
  if (done || now < 60000 || WiFi.status() != WL_CONNECTED) return;
  done = true;
  if (otaPendingVerify()) {
    esp_ota_mark_app_valid_cancel_rollback();
    EventLog::add("Firmware v%s confirmado", FW_VERSION);
  }
}
void loop() {
  unsigned long currentMillis = millis();

  if (!checkFactoryResetButton(currentMillis)) updateRgbLed(currentMillis);

  // --- Comandos da página web (enfileirados pelo callback do WebSocket) ---
  WebCmd webCmd;
  while (webCmdQueue && xQueueReceive(webCmdQueue, &webCmd, 0) == pdTRUE) {
    JsonDocument doc;
    if (!deserializeJson(doc, webCmd.json)) handleWebCommand(webCmd.client, doc);
  }
  if (pendingRestore) {
    JsonDocument* b = pendingRestore;
    applyBackup(*b);
    delete b;
    pendingRestore = nullptr;
  }
  confirmFirmware(currentMillis);
  maintainWifi(currentMillis);
  checkAutoArm(currentMillis);
  if (testSirenUntil && (long)(currentMillis - testSirenUntil) >= 0) { testSirenUntil = 0; updateSiren(); }
  if (testLockUntil && (long)(currentMillis - testLockUntil) >= 0) { testLockUntil = 0; updateHardwareState(); }
  if (pendingRestartAt && (long)(currentMillis - pendingRestartAt) >= 0) ESP.restart();
  if (wifiScanClient) finishWifiScan();

  // --- LÓGICA MQTT (NOVO) ---
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      // currentMillis e' unsigned long: guardar millis() em `long` virava negativo
      // depois de ~24,8 dias e travava a reconexao MQTT para sempre.
      if (currentMillis - lastMqttReconnectAttempt > mqttRetryMs) {
        lastMqttReconnectAttempt = currentMillis;
        reconnectMqtt();
      }
    } else {
      mqttClient.loop();
    }
  }


  // --- LÓGICA TECLADO WIEGAND ---
  // '*' SEMPRE limpa os dígitos (função preservada). Com o buffer já vazio, cancela a programação.
  // Operar (qualquer senha cadastrada OU a mestre):
  //   [*] <senha> #            -> arma / desarma
  // Programar (SÓ a senha MESTRE):
  //   #  (com o buffer vazio)  -> pede a mestre:  <mestre> #
  //     1 -> CADASTRAR: <nova senha> #
  //     2 -> REMOVER:   <senha> #
  //     0 -> RESET:     <mestre> #     (factory reset -> volta ao portal)
  if (wiegand.available()) {
    uint32_t key = wiegand.getCode();
    lastKeypadInputTime = currentMillis;

    if (key == 10) {                 // '*' -> LIMPA os dígitos (nunca muda)
      if (keypadBuffer.length() == 0) keypadState = KP_IDLE; // '*' com buffer vazio -> cancela
      keypadBuffer = "";
    }
    else if (keypadState == KP_MENU) {   // escolha da operação (uma tecla)
      if (key == 1)      { keypadState = KP_ADD;   Serial.println("[KP] CADASTRAR: nova senha + #"); }
      else if (key == 2) { keypadState = KP_DEL;   Serial.println("[KP] REMOVER: senha + #"); }
      else if (key == 0) { keypadState = KP_RESET; Serial.println("[KP] RESET: digite a MESTRE + #"); }
      else if (key == 3) { keypadState = KP_IDLE; openRescueAp(true); }
      keypadBuffer = "";
    }
    else if (key <= 9) {             // dígito
      keypadBuffer += String(key);
    }
    else if (key == 11) {            // '#'
      switch (keypadState) {
        case KP_IDLE:
          if (keypadBuffer.length() == 0) {
            keypadState = KP_AUTH;   // '#' com buffer vazio -> entra na programação
            Serial.println("[KP] PROGRAMACAO: digite a senha MESTRE + #");
          } else if (authKeypad.remainingMs() > 0) {
            Serial.println("[KP] bloqueado: muitas senhas erradas");
          } else if (isOperatingCode(keypadBuffer)) {
            authKeypad.record(true);
            String who = whoUsed(keypadBuffer, "teclado");
            if (isArmed || isTriggered) {
              disarmSystem(who);
              if (Codes::isDuress(keypadBuffer)) raiseDuress(who);
            } else {
              // O teclado não tem como perguntar: zona aberta fica inibida até desarmar.
              armSystem(MODE_AWAY, true, who);
            }
          } else {
            EventLog::add("Senha errada no teclado");
            authKeypad.record(false);
          }
          break;
        case KP_AUTH:
          if (authKeypad.remainingMs() > 0) { keypadState = KP_IDLE; break; }
          authKeypad.record(isMaster(keypadBuffer));
          if (isMaster(keypadBuffer)) {
            keypadState = KP_MENU;
            Serial.println("[KP] MENU: 1=cadastrar  2=remover  3=hotspot de configuracao  0=reset  (* cancela)");
          } else { keypadState = KP_IDLE; EventLog::add("Senha mestre errada no teclado"); }
          break;
        case KP_ADD:
          if (Codes::add(keypadBuffer, "teclado")) { EventLog::add("Senha cadastrada pelo teclado"); publishCodesList(); }
          else Serial.println("[KP] falha (curta/duplicada/cheia)");
          keypadState = KP_IDLE;
          break;
        case KP_DEL:
          if (Codes::remove(keypadBuffer)) { EventLog::add("Senha removida pelo teclado"); publishCodesList(); }
          else Serial.println("[KP] senha nao encontrada");
          keypadState = KP_IDLE;
          break;
        case KP_RESET:
          if (authKeypad.remainingMs() > 0) { keypadState = KP_IDLE; break; }
          authKeypad.record(isMaster(keypadBuffer));
          if (isMaster(keypadBuffer)) {
            Serial.println("[KP] FACTORY RESET pelo teclado.");
            Storage::clearAll(); delay(300); ESP.restart();
          } else { Serial.println("[KP] mestre incorreta - reset cancelado"); }
          keypadState = KP_IDLE;
          break;
        default: break;
      }
      keypadBuffer = "";
    }
  }

  // --- Timeout do Teclado (volta ao estado ocioso) ---
  if ((keypadBuffer.length() > 0 || keypadState != KP_IDLE) &&
      (currentMillis - lastKeypadInputTime > keypadTimeout)) {
    keypadBuffer = "";
    keypadState = KP_IDLE;
  }

  // --- AUTO-CORTE DA SIRENE (timeout) ---
  if (isSirenActive && cfg.sirenTimeoutSec > 0 &&
      (currentMillis - sirenStartMs > (unsigned long)cfg.sirenTimeoutSec * 1000UL)) {
    isSirenActive = false;
    updateSiren();
    EventLog::add("Sirene desligada pelo tempo limite");
    broadcastFullState();
  }

  // --- VERIFICA SENSORES ---
  if (currentMillis - lastStatusCheck >= checkInterval) {
    lastStatusCheck = currentMillis;
    checkSensors(currentMillis);
  }
}
