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
