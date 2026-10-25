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
#define HG_ZONES     4           // zonas com fio (ver sector_pins no .ino)
#define HG_ZONE_NAME_LEN 24
#define HG_ID_LEN    33          // ID MQTT, nome do aparelho no HA, hostname

// Como cada zona reage com o sistema armado.
enum ZoneType : uint8_t {
  ZT_INSTANT  = 0,   // dispara na hora
  ZT_DELAYED  = 1,   // entrada/saída: dá o tempo de entrada para desarmar
  ZT_FOLLOWER = 2,   // seguidora: instantânea, mas espera se a contagem de
                     // entrada ou saída já estiver correndo (ex.: presença no caminho)
  ZT_24H      = 3,   // dispara mesmo desarmado (ex.: botão de pânico, tamper)
};

struct KeypadCode {
  char code[HG_CODE_LEN];
  char label[HG_LABEL_LEN];
  bool duress;                 // senha de pânico: desarma, mas avisa em silêncio
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
  // Zonas
  char zoneNames[HG_ZONES][HG_ZONE_NAME_LEN]; // vazio = nome padrão do .ino
  uint8_t zoneBypass;          // bit i = zona i+1 inibida (não dispara)
  uint8_t zoneType[HG_ZONES];  // ZoneType
  uint8_t zoneHomeMask;        // bit i = zona ativa no modo "Em casa"
  uint16_t entryDelaySec;      // tempo para desarmar ao entrar (zonas de entrada/saída)
  uint16_t exitDelaySec;       // tempo para sair depois de armar
  uint16_t openAlertMin;       // avisa se uma zona ficar aberta tanto tempo (0 = não avisa)
  // Arme automático
  bool autoArmEnabled;
  uint16_t autoArmMin;         // minuto do dia (0-1439), horário local
  uint8_t autoArmDays;         // bit 0 = domingo ... bit 6 = sábado
  uint8_t autoArmMode;         // ArmMode do .ino (0 ausente, 1 em casa)
  char tz[40];                 // fuso no formato POSIX (Brasília = "<-03>3")
  // Identidade (várias placas na mesma casa / no mesmo HA)
  char hostname[HG_ID_LEN];    // nome .local e DHCP (vazio = homeguard-XXXX)
  // Endereço na rede: DHCP (padrão) ou fixo. Valores no formato do IPAddress (uint32).
  bool staticIp;
  uint32_t ipAddr, ipGateway, ipMask, ipDns;  // ipDns 0 = usa o gateway
  char mqttId[HG_ID_LEN];      // prefixo dos tópicos e IDs no HA (vazio = homeguard-XXXX)
  char deviceName[HG_ID_LEN];  // nome do aparelho no HA e no Telegram (vazio = Homeguard XXXX)
  // Flag
  bool configured;             // já passou pelo portal?
};
