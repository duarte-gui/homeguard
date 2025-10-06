


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

// =================================================================
// 2. CONFIGURAÇÕES - AJUSTE ESTAS VARIÁVEIS
// =================================================================

// --- Configurações de Rede ---
const char* ssid = "seu_ssid";
const char* password = "sua_senha";

// --- Configurações do Telegram ---
#define BOT_TOKEN "12345678:GGHHSSYE7785kjgGgK"
#define CHAT_ID "999999999"

// --- Configurações do MQTT (NOVO) ---
const char* mqtt_server = "192.168.1.90"; // IP do seu Home Assistant ou broker MQTT
const int mqtt_port = 1883;
const char* mqtt_user = "usuario_MQTT"; // CORRIGIDO: Usuário detectado nos logs
const char* mqtt_password = "senha_MQTT"; // IMPORTANTE: Coloque a senha do usuário 'casa' aqui
const char* device_name = "ESP32 Homeguard"; // Nome que aparecerá no Home Assistant

// --- Configurações do Home Assistant ---
const char* HOME_ASSISTANT_PIN_CODE = "1234"; // Defina o código a ser usado no HA

// --- Configurações do Teclado Wiegand ---
const int WIEGAND_D0_PIN = 18;
const int WIEGAND_D1_PIN = 19;
const int NUM_MASTER_CODES = 3;
const String MASTER_CODES[NUM_MASTER_CODES] = {"1234", "4321", "0987"};

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
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
Wiegand wiegand;

// --- MQTT (NOVO) ---
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// --- Estado do Sistema ---
bool isArmed = false;
bool isSirenActive = false;
int triggeringSectorId = 0;
String sector_status[NUM_SECTORS];
String last_sector_status[NUM_SECTORS];

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

String getFullStateJson() {
  JsonDocument doc;
  doc["isArmed"] = isArmed;
  doc["isSirenActive"] = isSirenActive;
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
  if (WiFi.status() == WL_CONNECTED) {
    bot.sendMessage(CHAT_ID, message, "");
  }
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
        isSirenActive = false;
        triggeringSectorId = 0;
        updateSiren();
    }
    broadcastFullState();
}

void handleActivateSiren() {
    isSirenActive = true;
    Serial.println("Sirene ativada via MQTT.");
    updateSiren();
    broadcastFullState();
}

void handleSilenceSiren() {
    isSirenActive = false;
    triggeringSectorId = 0;
    Serial.println("Sirene silenciada.");
    updateSiren();
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
  doc["payload_arm_away"] = "ARM_AWAY";
  doc["payload_disarm"] = "DISARM";
  // ATUALIZAÇÃO: Exige código apenas para desarmar.
  doc["code_arm_required"] = false;
  doc["code_disarm_required"] = true;
  doc["code"] = HOME_ASSISTANT_PIN_CODE;
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
  // Publica estado do alarme
  mqttClient.publish("homeguard/alarm/state", isArmed ? "armed_away" : "disarmed", true);

  // Publica estado da sirene
  mqttClient.publish("homeguard/siren/state", isSirenActive ? "ON" : "OFF", true);

  // Publica estado de cada setor
  for (int i = 0; i < NUM_SECTORS; i++) {
    char topic[64];
    snprintf(topic, sizeof(topic), "homeguard/sector/%d/state", i + 1);
    mqttClient.publish(topic, sector_status[i].c_str(), true);
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
  }
}

void reconnectMqtt() {
  if (!mqttClient.connected()) {
    Serial.print("Tentando conexão MQTT...");
    if (mqttClient.connect(device_name, mqtt_user, mqtt_password)) {
      Serial.println("Conectado!");
      publishDiscoveryMessages();
      publishFullMqttState();
      // Subscreve aos tópicos de comando
      mqttClient.subscribe("homeguard/alarm/command");
      mqttClient.subscribe("homeguard/siren/command");
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
      data[len] = 0;
      JsonDocument doc;
      deserializeJson(doc, (char*)data);
      const char* command = doc["command"];
      if (strcmp(command, "toggleArm") == 0) {
        handleArmDisarm(!isArmed);
      } else if (strcmp(command, "silenceSiren") == 0) {
        handleSilenceSiren();
      }
    }
  }
}

// =================================================================
// 7. SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\nINICIALIZANDO FIRMWare ESP32 HOMEGUARD (COM MQTT)");

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

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado! IP: " + WiFi.localIP().toString());

  // --- MQTT Setup (NOVO) ---
  mqttClient.setServer(mqtt_server, mqtt_port);
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
  if (wiegand.available()) {
    uint32_t key = wiegand.getCode(); 
    lastKeypadInputTime = currentMillis;

    if (key >= 0 && key <= 9) {
      keypadBuffer += String(key);
    } 
    else if (key == 11) { // '#' (Enter)
      bool isCodeValid = false;
      for (int i = 0; i < NUM_MASTER_CODES; i++) {
        if (keypadBuffer.equals(MASTER_CODES[i])) {
          isCodeValid = true;
          break;
        }
      }
      if (isCodeValid) {
        handleArmDisarm(!isArmed);
      }
      keypadBuffer = "";
    } 
    else if (key == 10) { // '*' (Cancelar)
      keypadBuffer = "";
    }
  }

  // --- Timeout do Teclado ---
  if (keypadBuffer.length() > 0 && (currentMillis - lastKeypadInputTime > keypadTimeout)) {
    keypadBuffer = "";
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
          if (!isSirenActive) {
            isSirenActive = true;
            triggeringSectorId = i + 1;
            updateSiren();
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
