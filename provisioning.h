#pragma once
#include "config.h"

// Portal de configuração via hotspot (captive portal), estilo "abra no celular".
// Sobe um AP "Homeguard-XXXX"; o usuário escolhe a rede, digita MQTT/Telegram/PIN
// e uma senha inicial. Ao salvar, grava em NVS e o firmware reinicia.
// Página do portal (também servida em /setup/ pelo servidor do alarme).
extern const char PORTAL_HTML[];

namespace Provisioning {
  void begin();    // sobe o AP + portal
  void handle();   // chamar no loop enquanto !isDone()
  bool isDone();   // true depois que o usuário salvou (firmware deve reiniciar)
}
