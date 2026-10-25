#pragma once
#include <Arduino.h>

// Registro de eventos (armou, desarmou, disparo, senha errada...) gravado no
// LittleFS. Sobrevive a reinício, queda de energia e atualização por OTA.
// Guarda os últimos EVENTLOG_SIZE eventos num arquivo circular de registros fixos.
//
// Só pode ser chamado do loop() (não é seguro entre tarefas).
#define EVENTLOG_SIZE 200

namespace EventLog {
  bool   begin();                                   // monta o LittleFS e acha o fim do registro
  void   add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  String json(int max = EVENTLOG_SIZE);             // [{"t":epoch,"m":"..."}], mais novo primeiro
  void   clear();
  bool   timeValid();                               // o NTP já acertou o relógio?
}
