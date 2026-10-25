#pragma once
#include "config.h"

// Persistência em NVS (Preferences). Namespace "homeguard".
namespace Storage {
  // Config geral
  Config loadConfig();
  void   saveConfig(const Config& cfg);
  void   clearAll();              // apaga tudo (factory reset)

  // Senhas do teclado (lista). Retorna a quantidade lida em `out`.
  int    loadCodes(KeypadCode* out, int maxCodes);
  void   saveCodes(const KeypadCode* codes, int count);

  // Estado armado, para voltar armado depois de uma queda de energia.
  // armed=false apaga o resto. tempBypass = zonas inibidas só até desarmar.
  void   saveArmState(bool armed, uint8_t mode, uint8_t tempBypass);
  bool   loadArmState(uint8_t& mode, uint8_t& tempBypass);
}
