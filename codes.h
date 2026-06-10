#pragma once
#include "config.h"

// Gerência das senhas do teclado (cadastro local e remoto). Persiste em NVS.
namespace Codes {
  void   load();                                   // carrega do NVS p/ memória
  bool   check(const String& code);                // senha válida?
  bool   add(const String& code, const char* label); // false se cheio/duplicado/curta
  bool   remove(const String& code);               // false se não existe
  int    count();
  String listJson();                               // rótulos + tamanho (NUNCA os valores)
}
