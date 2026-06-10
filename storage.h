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
}
