#pragma once
#include "config.h"

// Gerência das senhas do teclado (cadastro local e remoto). Persiste em NVS.
namespace Codes {
  void   load();                                   // carrega do NVS p/ memória
  bool   check(const String& code);                // senha válida? (inclui as de pânico)
  bool   isDuress(const String& code);             // é senha de pânico?
  bool   add(const String& code, const char* label, bool duress = false);
                                                   // false se cheio/duplicado/curta
  bool   remove(const String& code);               // false se não existe
  bool   removeAt(int index);                      // pela posição na lista (admin web)
  int    count();
  String exportJson();                             // COM os valores: só para o backup do admin
  void   replaceAll(const KeypadCode* codes, int n); // restauração do backup
  String labelOf(const String& code);              // de quem é a senha ("" se não existe)
  String listJson(bool withDuress = false);        // rótulos + tamanho (NUNCA os valores).
                                                   // withDuress só para o admin: quem lê o
                                                   // MQTT não pode saber qual é a de pânico.
}
