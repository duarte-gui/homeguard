


/*
 * ====================================================================================
 * ESP32 Homeguard Firmware - Versão Final (AsyncWebSocket + LittleFS + Wiegand + MQTT)
 * ====================================================================================
 * 
 * Este firmware foi atualizado para usar uma biblioteca Wiegand local (Wiegand.aga e
 * Wiegand.cepepe na mesma pasta do projeto), eliminando a necessidade de instalar
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
 * - Monitora 5 setores físicos e aciona a lógica de alarme.
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
 * 2. WIEGAND: Os arquivos Wiegand.aga e Wiegand.cepepe DEVEM estar na mesma pasta deste sketch.
 * 
 * 3. CONFIGURAÇÕES ABAIXO:
 *    - Preencha as credenciais de Wi-Fi, Telegram e MQTT.
 */

// =================================================================
// 1. INCLUSÃO DE BIBLIOTECAS
// =================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
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

// =================================================================
// 2. CONFIGURAÇÕES - AJUSTE ESTAS VARIÁVEIS
// =================================================================

// --- Configuração: carregada da NVS (preenchida pelo portal hotspot) ---
// Segredos NÃO ficam mais no código. Ver provisioning.* / storage.*
Config cfg;
const char* device_name = "ESP32 Homeguard"; // Nome que aparece no Home Assistant

// --- Teclado Wiegand (senhas agora no módulo Codes / NVS) ---
const int WIEGAND_D0_PIN = 18;
const int WIEGAND_D1_PIN = 19;

// --- Mapeamento de Pinos (GPIO) ---
const int SECTOR_1_PIN = 13;
const int SECTOR_2_PIN = 12;
const int SECTOR_3_PIN = 14;
const int SECTOR_4_PIN = 27;
const int SECTOR_5_PIN = 26;
const int SIREN_RELAY_PIN = 4;
const int STATUS_LED_PIN = 16;
const int GATE_LOCK_RELAY_PIN = 17;

// --- Nomes dos Setores ---
const char* sector_names[] = {
  "Portão Garagem", // Removido "Setor X" para nomes mais limpos no HA
  "Presença Garagem",
  "Portão Principal",
  "Porta Gui",
  "Porta Gu"
};

const int NUM_SECTORS = 5;
const int sector_pins[NUM_SECTORS] = {SECTOR_1_PIN, SECTOR_2_PIN, SECTOR_3_PIN, SECTOR_4_PIN, SECTOR_5_PIN};

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
int triggeringSectorId = 0;
unsigned long sirenStartMs = 0;      // p/ o auto-corte da sirene
String sector_status[NUM_SECTORS];
String last_sector_status[NUM_SECTORS];

// --- Teclado: máquina de estados (programação exige senha mestre) ---
// KP_AUTH = aguardando a senha mestre após "# com buffer vazio".
enum KeypadState { KP_IDLE, KP_AUTH, KP_MENU, KP_ADD, KP_DEL, KP_RESET };
KeypadState keypadState = KP_IDLE;

// --- Tópicos MQTT (availability / LWT) ---
const char* AVAILABILITY_TOPIC = "homeguard/availability";

// --- Controle de Tempo ---
unsigned long lastStatusCheck = 0;
const long checkInterval = 100;
unsigned long lastMqttReconnectAttempt = 0;

// --- Lógica do Teclado ---
String keypadBuffer = "";
unsigned long lastKeypadInputTime = 0;
const long keypadTimeout = 5000;

// =================================================================
// 4. FUNÇÕES DO SISTEMA
// =================================================================

void publishFullMqttState(); // Forward declaration
void publishCodesList();     // Forward declaration

String getFullStateJson() {
  JsonDocument doc;
  doc["isArmed"] = isArmed;
  doc["isSirenActive"] = isSirenActive;
  doc["isTriggered"] = isTriggered;
  doc["triggeringSectorId"] = triggeringSectorId;
  JsonArray sectors = doc["sectors"].to<JsonArray>();
  for (int i = 0; i < NUM_SECTORS; i++) {
    JsonObject sector = sectors.add<JsonObject>();
    sector["id"] = i + 1;
    char fullName[50];
    snprintf(fullName, sizeof(fullName), "Setor %d - %s", i + 1, sector_names[i]);
    sector["name"] = fullName;
    sector["status"] = sector_status[i];
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

void sendTelegramAlert(String message) {
  if (bot != nullptr && cfg.chatId[0] != '\0' && WiFi.status() == WL_CONNECTED) {
    bot->sendMessage(cfg.chatId, message, "");
  }
}

// Estado do painel de alarme para o Home Assistant.
const char* alarmStateStr() {
  if (isTriggered) return "triggered";
  return isArmed ? "armed_away" : "disarmed";
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

String readSensorStatus(int pin) {
  return (digitalRead(pin) == HIGH) ? "Aberto" : "Fechado";
}

void updateSiren() {
  digitalWrite(SIREN_RELAY_PIN, isSirenActive ? HIGH : LOW);
}

void updateHardwareState() {
  if (isArmed) {
    digitalWrite(STATUS_LED_PIN, LOW);
    digitalWrite(GATE_LOCK_RELAY_PIN, HIGH);
  } else {
    digitalWrite(STATUS_LED_PIN, HIGH);
    digitalWrite(GATE_LOCK_RELAY_PIN, LOW);
  }
}

void handleArmDisarm(bool arm) {
    isArmed = arm;
    Serial.printf("Sistema agora está: %s\n", isArmed ? "ARMADO" : "DESARMADO");
    updateHardwareState();
    if (!isArmed) {
        // Desarmar limpa o disparo e silencia a sirene.
        isTriggered = false;
        isSirenActive = false;
        triggeringSectorId = 0;
        updateSiren();
    }
    broadcastFullState();
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

void publishDiscoveryMessages() {
  JsonDocument doc;
  char topic[128];
  char payload[1024];

  Serial.println("--- Publicando mensagens de descoberta para o Home Assistant ---");

  // --- Entidade: Painel de Alarme ---
  doc.clear();
  doc["name"] = device_name;
  doc["unique_id"] = "esp32_homeguard_alarm_panel";
  doc["state_topic"] = "homeguard/alarm/state";
  doc["command_topic"] = "homeguard/alarm/command";
  doc["availability_topic"] = AVAILABILITY_TOPIC;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  doc["payload_arm_away"] = "ARM_AWAY";
  doc["payload_disarm"] = "DISARM";
  // ATUALIZAÇÃO: Exige código apenas para desarmar.
  doc["code_arm_required"] = false;
  doc["code_disarm_required"] = true;
  doc["code"] = cfg.haPin;
  JsonObject device_panel = doc["device"].to<JsonObject>();
  // CORREÇÃO FINAL: O Home Assistant exige que 'identifiers' seja um array.
  device_panel["identifiers"].add("esp32_homeguard_alarm");
  device_panel["name"] = device_name;
  device_panel["manufacturer"] = "DIY";
  device_panel["model"] = "ESP32";
  serializeJson(doc, payload);
  snprintf(topic, sizeof(topic), "homeassistant/alarm_control_panel/esp32_homeguard/config");
  Serial.printf("Publicando no tópico: %s\n", topic);
  if (mqttClient.publish(topic, payload, true)) {
    Serial.println("  -> Publicação da descoberta do Painel de Alarme OK.");
  } else {
    Serial.println("  -> ERRO ao publicar a descoberta do Painel de Alarme.");
  }

  // --- Entidade: Switch da Sirene ---
  doc.clear();
  doc["name"] = String(device_name) + " Siren";
  doc["unique_id"] = "esp32_homeguard_siren";
  doc["state_topic"] = "homeguard/siren/state";
  doc["command_topic"] = "homeguard/siren/command"; // Usado para ativar (ON) ou silenciar (OFF)
  doc["availability_topic"] = AVAILABILITY_TOPIC;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:alarm-bell";
  JsonObject device_siren = doc["device"].to<JsonObject>();
  // CORREÇÃO FINAL: O Home Assistant exige que 'identifiers' seja um array.
  device_siren["identifiers"].add("esp32_homeguard_alarm");
  device_siren["name"] = device_name;
  device_siren["manufacturer"] = "DIY";
  device_siren["model"] = "ESP32";
  serializeJson(doc, payload);
  snprintf(topic, sizeof(topic), "homeassistant/switch/esp32_homeguard_siren/config");
  Serial.printf("Publicando no tópico: %s\n", topic);
  if (mqttClient.publish(topic, payload, true)) {
    Serial.println("  -> Publicação da descoberta da Sirene OK.");
  } else {
    Serial.println("  -> ERRO ao publicar a descoberta da Sirene.");
  }

  // --- Entidade: Switch "Sirene habilitada" (gating do disparo) ---
  doc.clear();
  doc["name"] = String(device_name) + " Sirene habilitada";
  doc["unique_id"] = "esp32_homeguard_siren_enabled";
  doc["state_topic"] = "homeguard/siren_enabled/state";
  doc["command_topic"] = "homeguard/siren_enabled/command";
  doc["availability_topic"] = AVAILABILITY_TOPIC;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["icon"] = "mdi:bell-cog";
  doc["entity_category"] = "config";
  {
    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"].add("esp32_homeguard_alarm");
    dev["name"] = device_name;
    dev["manufacturer"] = "DIY";
    dev["model"] = "ESP32";
  }
  serializeJson(doc, payload);
  snprintf(topic, sizeof(topic), "homeassistant/switch/esp32_homeguard_siren_enabled/config");
  if (mqttClient.publish(topic, payload, true)) {
    Serial.println("  -> Publicação da descoberta da 'Sirene habilitada' OK.");
  } else {
    Serial.println("  -> ERRO ao publicar a descoberta da 'Sirene habilitada'.");
  }

  // --- Entidades: Sensores Binários (Setores) ---
  for (int i = 0; i < NUM_SECTORS; i++) {
    doc.clear();
    doc["name"] = String(device_name) + " " + sector_names[i];
    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "esp32_homeguard_sector_%d", i + 1);
    doc["unique_id"] = unique_id;
    char state_topic[64];
    snprintf(state_topic, sizeof(state_topic), "homeguard/sector/%d/state", i + 1);
    doc["state_topic"] = state_topic;
    doc["availability_topic"] = AVAILABILITY_TOPIC;
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";
    doc["payload_on"] = "Aberto";
    doc["payload_off"] = "Fechado";
    doc["device_class"] = "door";
    JsonObject device_sensor = doc["device"].to<JsonObject>();
    // CORREÇÃO FINAL: O Home Assistant exige que 'identifiers' seja um array.
    device_sensor["identifiers"].add("esp32_homeguard_alarm");
    device_sensor["name"] = device_name;
    device_sensor["manufacturer"] = "DIY";
    device_sensor["model"] = "ESP32";
    serializeJson(doc, payload);
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s/config", unique_id);
    Serial.printf("Publicando no tópico: %s\n", topic);
    if (mqttClient.publish(topic, payload, true)) {
        Serial.printf("  -> Publicação da descoberta do Setor %d OK.\n", i + 1);
    } else {
        Serial.printf("  -> ERRO ao publicar a descoberta do Setor %d.\n", i + 1);
    }
  }
  Serial.println("--- Fim da publicação das mensagens de descoberta ---");
}


void publishFullMqttState() {
  // Publica estado do alarme (disarmed / armed_away / triggered)
  mqttClient.publish("homeguard/alarm/state", alarmStateStr(), true);

  // Publica estado da sirene física e do gating
  mqttClient.publish("homeguard/siren/state", isSirenActive ? "ON" : "OFF", true);
  mqttClient.publish("homeguard/siren_enabled/state", cfg.sirenEnabled ? "ON" : "OFF", true);

  // Publica estado de cada setor
  for (int i = 0; i < NUM_SECTORS; i++) {
    char topic[64];
    snprintf(topic, sizeof(topic), "homeguard/sector/%d/state", i + 1);
    mqttClient.publish(topic, sector_status[i].c_str(), true);
  }
}

// Publica a LISTA de senhas (apenas rótulos + tamanho, NUNCA os valores).
void publishCodesList() {
  if (mqttClient.connected()) {
    mqttClient.publish("homeguard/codes/list", Codes::listJson().c_str(), true);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Mensagem recebida no tópico [");
  Serial.print(topic);
  Serial.print("]: ");
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  Serial.println(message);

  if (strcmp(topic, "homeguard/alarm/command") == 0) {
    if (strcmp(message, "ARM_AWAY") == 0) {
      handleArmDisarm(true);
    } else if (strcmp(message, "DISARM") == 0) {
      handleArmDisarm(false);
    }
  } else if (strcmp(topic, "homeguard/siren/command") == 0) {
    if (strcmp(message, "OFF") == 0) { // Usado para silenciar a sirene
      handleSilenceSiren();
    } else if (strcmp(message, "ON") == 0) { // Usado para ativar a sirene
      handleActivateSiren();
    }
  } else if (strcmp(topic, "homeguard/siren_enabled/command") == 0) {
    handleSirenEnabled(strcmp(message, "ON") == 0);
  } else if (strcmp(topic, "homeguard/codes/set") == 0) {
    // Cadastro REMOTO de senha: {"action":"add"|"remove","code":"1234","label":"Gui"}
    JsonDocument cmd;
    if (!deserializeJson(cmd, message)) {
      const char* action = cmd["action"] | "";
      const char* code   = cmd["code"]   | "";
      const char* label  = cmd["label"]  | "remoto";
      bool ok = false;
      if (strcmp(action, "add") == 0)    ok = Codes::add(String(code), label);
      else if (strcmp(action, "remove") == 0) ok = Codes::remove(String(code));
      Serial.printf("[CODES] %s '%s' -> %s\n", action, code, ok ? "OK" : "FALHOU");
      publishCodesList();
    }
  } else if (strcmp(topic, "homeguard/system/command") == 0) {
    if (strcmp(message, "reboot") == 0) {
      Serial.println("[SYS] Reboot solicitado via MQTT."); delay(200); ESP.restart();
    } else if (strcmp(message, "factory_reset") == 0) {
      Serial.println("[SYS] FACTORY RESET via MQTT."); Storage::clearAll(); delay(200); ESP.restart();
    }
  }
}

void reconnectMqtt() {
  if (!mqttClient.connected()) {
    Serial.print("Tentando conexão MQTT...");
    // Last Will: se o ESP cair, o broker publica "offline" → entidades ficam unavailable.
    if (mqttClient.connect(device_name, cfg.mqttUser, cfg.mqttPass,
                           AVAILABILITY_TOPIC, 0, true, "offline")) {
      Serial.println("Conectado!");
      mqttClient.publish(AVAILABILITY_TOPIC, "online", true); // ficou online
      publishDiscoveryMessages();
      publishFullMqttState();
      publishCodesList();
      // Subscreve aos tópicos de comando
      mqttClient.subscribe("homeguard/alarm/command");
      mqttClient.subscribe("homeguard/siren/command");
      mqttClient.subscribe("homeguard/siren_enabled/command");
      mqttClient.subscribe("homeguard/codes/set");
      mqttClient.subscribe("homeguard/system/command");
    } else {
      Serial.print("Falhou, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" Tentando novamente em 5 segundos");
    }
  }
}

// =================================================================
// 6. EVENTOS WEBSOCKET
// =================================================================
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("Cliente WebSocket #%u conectado\n", client->id());
    client->text(getFullStateJson());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("Cliente WebSocket #%u desconectado\n", client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      JsonDocument doc;
      deserializeJson(doc, (char*)data, len);
      const char* command = doc["command"];
      if (command == nullptr) return;
      if (strcmp(command, "toggleArm") == 0) {
        handleArmDisarm(!isArmed);
      } else if (strcmp(command, "silenceSiren") == 0) {
        handleSilenceSiren();
      } else if (strcmp(command, "setSirenEnabled") == 0) {
        handleSirenEnabled(doc["value"] | true);
      } else if (strcmp(command, "addCode") == 0) {
        const char* c = doc["code"]; const char* l = doc["label"] | "web";
        if (c) { Codes::add(String(c), l); publishCodesList(); }
      } else if (strcmp(command, "removeCode") == 0) {
        const char* c = doc["code"];
        if (c) { Codes::remove(String(c)); publishCodesList(); }
      }
    }
  }
}

// =================================================================
// 7. SETUP
// =================================================================

// Abre o portal de configuração (hotspot) e NÃO retorna — reinicia ao salvar.
void runProvisioningPortal() {
  Serial.println("[PROV] Sem configuracao valida — abrindo portal (AP Homeguard-XXXX).");
  Provisioning::begin();
  while (!Provisioning::isDone()) {
    Provisioning::handle();
    delay(5);
  }
  Serial.println("[PROV] Salvo. Reiniciando...");
  delay(800);
  ESP.restart();
}

// Conecta no WiFi com timeout. Retorna false se não conseguir.
bool connectWiFi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.wifiPass);
  Serial.print("Conectando ao WiFi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado! IP: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("\nFalha ao conectar no WiFi.");
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nINICIALIZANDO FIRMWare ESP32 HOMEGUARD (COM MQTT)");

  // Carrega configuração e senhas da NVS
  cfg = Storage::loadConfig();
  Codes::load();

  wiegand.begin(WIEGAND_D0_PIN, WIEGAND_D1_PIN);

  for (int i = 0; i < NUM_SECTORS; i++) {
    pinMode(sector_pins[i], INPUT_PULLUP);
    last_sector_status[i] = readSensorStatus(sector_pins[i]);
    sector_status[i] = last_sector_status[i];
  }
  
  pinMode(SIREN_RELAY_PIN, OUTPUT);
  digitalWrite(SIREN_RELAY_PIN, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(GATE_LOCK_RELAY_PIN, OUTPUT);
  updateHardwareState();

  if (!LittleFS.begin()) {
      Serial.println("Falha ao montar LittleFS. O sistema será interrompido.");
      while(1);
  }

  // Sem configuração → portal hotspot (não retorna; reinicia ao salvar).
  if (!cfg.configured) {
    runProvisioningPortal();
  }

  // Conecta no WiFi; se falhar (credencial errada/rede fora), cai no portal.
  if (!connectWiFi(20000)) {
    runProvisioningPortal();
  }

  // --- Telegram (token vem da config) ---
  bot = new UniversalTelegramBot(cfg.botToken, secured_client);

  // --- MQTT Setup ---
  mqttClient.setServer(cfg.mqttHost, cfg.mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048); // AUMENTADO: Resolve falhas de publicação com payloads grandes.

  secured_client.setInsecure();
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  server.serveStatic("/", LittleFS, "/");
  server.begin();
  
  Serial.println("Servidores Web e WebSocket iniciados. Sistema pronto.");
}

// =================================================================
// 8. LOOP PRINCIPAL
// =================================================================
void loop() {
  unsigned long currentMillis = millis();

  // --- LÓGICA MQTT (NOVO) ---
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      long now = millis();
      if (now - lastMqttReconnectAttempt > 5000) {
        lastMqttReconnectAttempt = now;
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
          } else if (isOperatingCode(keypadBuffer)) {
            handleArmDisarm(!isArmed);
          }
          break;
        case KP_AUTH:
          if (isMaster(keypadBuffer)) {
            keypadState = KP_MENU;
            Serial.println("[KP] MENU: 1=cadastrar  2=remover  0=reset  (* cancela)");
          } else { keypadState = KP_IDLE; Serial.println("[KP] senha mestre incorreta"); }
          break;
        case KP_ADD:
          if (Codes::add(keypadBuffer, "teclado")) { Serial.println("[KP] senha cadastrada"); publishCodesList(); }
          else Serial.println("[KP] falha (curta/duplicada/cheia)");
          keypadState = KP_IDLE;
          break;
        case KP_DEL:
          if (Codes::remove(keypadBuffer)) { Serial.println("[KP] senha removida"); publishCodesList(); }
          else Serial.println("[KP] senha nao encontrada");
          keypadState = KP_IDLE;
          break;
        case KP_RESET:
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
    Serial.println("[SIREN] Auto-corte por timeout (alarme segue disparado ate desarmar).");
    broadcastFullState();
  }

  // --- VERIFICA SENSORES ---
  if (currentMillis - lastStatusCheck >= checkInterval) {
    lastStatusCheck = currentMillis;
    bool stateChanged = false;
    for (int i = 0; i < NUM_SECTORS; i++) {
      sector_status[i] = readSensorStatus(sector_pins[i]);
      if (sector_status[i] != last_sector_status[i]) {
        stateChanged = true;
        last_sector_status[i] = sector_status[i];
        if (isArmed && sector_status[i] == "Aberto") {
          if (!isTriggered) {
            // Dispara o alarme (estado lógico) — independe da sirene física.
            isTriggered = true;
            triggeringSectorId = i + 1;
            // Sirene física só toca se o gating permitir (cfg.sirenEnabled).
            if (cfg.sirenEnabled) {
              isSirenActive = true;
              sirenStartMs = currentMillis;
              updateSiren();
            }
            char alertMessage[128];
            snprintf(alertMessage, sizeof(alertMessage), "🚨 *ALARME DISPARADO!* 🚨\nSetor violado: %s", sector_names[i]);
            sendTelegramAlert(alertMessage);
          }
        }
      }
    }
    if (stateChanged) {
      broadcastFullState();
    }
  }
}
