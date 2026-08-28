#pragma once
#include <Arduino.h>

// =================================================================
// Configuração persistente do Homeguard (gravada em NVS / Preferences)
// =================================================================
//
// Tudo que antes era hardcoded no .ino agora vem daqui, preenchido pelo
// portal de configuração (hotspot) na primeira vez. Ver provisioning.*
//
#define HG_MAX_CODES 16          // nº máximo de senhas do teclado
#define HG_CODE_LEN  12          // tamanho máx. de uma senha (+1 \0)
#define HG_LABEL_LEN 24          // rótulo da senha (ex.: "Gui", "teclado")

struct KeypadCode {
  char code[HG_CODE_LEN];
  char label[HG_LABEL_LEN];
};

// Estados do LED RGB de status de conexão (ver homeguard.ino, updateConnLed).
// Definido aqui (não no .ino) porque a geração automática de protótipos do
// Arduino insere a declaração da função no topo do arquivo, antes de onde o
// enum apareceria se ficasse no meio do .ino — e o tipo precisa já existir.
enum ConnLedState {
  CONN_LED_PROVISIONING,    // azul piscando rápido  — modo config (hotspot aberto)
  CONN_LED_CONNECTING_WIFI, // amarelo piscando       — conectando ao Wi-Fi
  CONN_LED_MQTT_DOWN,       // roxo pisca-duplo       — Wi-Fi ok, MQTT/HA fora
  CONN_LED_ALL_OK,          // verde "respirando"     — tudo conectado
  CONN_LED_TRIGGERED,       // vermelho piscando rápido — alarme disparado
  CONN_LED_FACTORY_RESET_HOLD // branco piscando, acelera — segurando o botão de reset
};

// Estados do LED RGB de feedback do alarme (ver homeguard.ino, updateAlarmLed).
// Separado do ConnLedState acima: esse aqui mostra armado/desarmado/disparado,
// não status de conexão — são dois LEDs físicos com papéis diferentes.
enum AlarmLedState {
  ALARM_LED_DISARMED, // verde fixo   — desarmado
  ALARM_LED_ARMED,    // vermelho fixo — armado
  ALARM_LED_SIREN      // amarelo piscando — sirene tocando (junto com ela)
};

struct Config {
  // Rede
  char ssid[33];
  char wifiPass[65];
  // MQTT / Home Assistant
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUser[33];
  char mqttPass[65];
  char masterCode[HG_CODE_LEN];// SENHA MESTRE (teclado: programar; tb usada como PIN do HA)
  char haPin[HG_CODE_LEN];     // PIN exigido p/ DESARMAR pelo HA (= masterCode por padrão)
  // Telegram (opcional)
  char botToken[64];
  char chatId[24];
  // Sirene
  bool sirenEnabled;           // gating: sirene física pode tocar?
  uint16_t sirenTimeoutSec;    // auto-corte (0 = nunca corta sozinha)
  // Flag
  bool configured;             // já passou pelo portal?
};
