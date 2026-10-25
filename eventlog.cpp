#include "eventlog.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

// Registro de tamanho fixo: achar o i-ésimo evento é só um seek, e regravar um
// slot não mexe nos outros. O seq crescente diz qual é o mais novo depois de
// dar a volta no arquivo.
struct Rec {
  uint32_t seq;      // 0 = slot vazio
  uint32_t ts;       // epoch (UTC); 0 = relógio ainda não acertado
  char msg[56];
};
static_assert(sizeof(Rec) == 64, "registro deve ter 64 bytes");

static const char* PATH = "/events.bin";
static bool     g_ok = false;
static uint32_t g_nextSeq = 1;
static int      g_nextSlot = 0;

bool EventLog::timeValid() {
  return time(nullptr) > 1700000000;   // depois de nov/2023 = NTP já respondeu
}

// Arquivo do tamanho certo, preenchido com slots vazios.
static bool createFile() {
  File f = LittleFS.open(PATH, "w");
  if (!f) return false;
  Rec empty{};
  for (int i = 0; i < EVENTLOG_SIZE; i++) f.write((const uint8_t*)&empty, sizeof(empty));
  f.close();
  return true;
}

bool EventLog::begin() {
  // Na primeira vez a partição vem vazia ou com restos da imagem antiga da
  // interface web. O begin(true) do core só formata para um código de erro
  // específico (e não para "Corrupted dir pair"), então formata aqui.
  if (!LittleFS.begin(false)) {
    Serial.println("[LOG] LittleFS sem formato valido: formatando");
    if (!LittleFS.format() || !LittleFS.begin(false)) return false;
  }
  File f = LittleFS.open(PATH, "r");
  if (!f || f.size() != EVENTLOG_SIZE * sizeof(Rec)) {
    if (f) f.close();
    if (!createFile()) return false;
    f = LittleFS.open(PATH, "r");
    if (!f) return false;
  }
  uint32_t maxSeq = 0;
  int maxSlot = -1;
  Rec r;
  for (int i = 0; i < EVENTLOG_SIZE; i++) {
    if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
    if (r.seq > maxSeq) { maxSeq = r.seq; maxSlot = i; }
  }
  f.close();
  g_nextSeq = maxSeq + 1;
  g_nextSlot = (maxSlot + 1) % EVENTLOG_SIZE;
  g_ok = true;
  return true;
}

void EventLog::add(const char* fmt, ...) {
  Rec r{};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r.msg, sizeof(r.msg), fmt, ap);
  va_end(ap);
  r.ts = timeValid() ? (uint32_t)time(nullptr) : 0;
  Serial.printf("[EVENTO] %s\n", r.msg);
  if (!g_ok) return;

  r.seq = g_nextSeq++;
  File f = LittleFS.open(PATH, "r+");
  if (!f) return;
  f.seek(g_nextSlot * sizeof(Rec));
  f.write((const uint8_t*)&r, sizeof(r));
  f.close();
  g_nextSlot = (g_nextSlot + 1) % EVENTLOG_SIZE;
}

String EventLog::json(int max) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  if (g_ok) {
    File f = LittleFS.open(PATH, "r");
    if (f) {
      Rec r;
      // Do mais novo para o mais velho, andando para trás a partir do último gravado.
      for (int n = 0, slot = g_nextSlot; n < EVENTLOG_SIZE && n < max; n++) {
        slot = (slot - 1 + EVENTLOG_SIZE) % EVENTLOG_SIZE;
        f.seek(slot * sizeof(Rec));
        if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r) || r.seq == 0) break;
        r.msg[sizeof(r.msg) - 1] = '\0';
        JsonObject o = arr.add<JsonObject>();
        o["t"] = r.ts;
        o["m"] = r.msg;
      }
      f.close();
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void EventLog::clear() {
  if (!g_ok) return;
  createFile();
  g_nextSeq = 1;
  g_nextSlot = 0;
}
