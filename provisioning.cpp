#include "provisioning.h"
#include "storage.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

static WebServer server(80);
static DNSServer dns;
static bool      g_done = false;

// ── HTML do portal ────────────────────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Configurar Homeguard</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#14151a;color:#fff;padding:18px;min-height:100vh}
  h1{color:#4ade80;font-size:1.4rem;margin-bottom:2px}
  .sub{color:#888;font-size:.82rem;margin-bottom:20px}
  .card{background:#1e1f29;border-radius:12px;padding:16px;margin-bottom:14px}
  .card h2{color:#4ade80;font-size:.92rem;margin-bottom:10px}
  label{display:block;font-size:.78rem;color:#aaa;margin:9px 0 3px}
  input{width:100%;padding:10px;border-radius:8px;border:1px solid #33344a;background:#262738;color:#fff;font-size:1rem}
  .row{display:flex;gap:8px} .row>div{flex:1}
  .net{display:flex;justify-content:space-between;padding:9px 11px;border-radius:8px;cursor:pointer;margin-bottom:6px;background:#262738;border:1px solid #33344a}
  .net-sig{color:#4ade80;font-size:.8rem}
  #sel-box{background:#262738;border:2px solid #4ade80;border-radius:8px;padding:9px 11px;cursor:pointer;display:flex;justify-content:space-between}
  #scan-status{text-align:center;padding:14px;color:#666;font-size:.85rem}
  details{margin-top:6px} summary{color:#4ade80;font-size:.85rem;cursor:pointer}
  button{width:100%;padding:14px;background:#4ade80;color:#0b3d1f;border:none;border-radius:10px;font-size:1rem;font-weight:700;cursor:pointer;margin-top:6px}
  #msg{text-align:center;margin-top:10px;font-size:.85rem;min-height:18px;color:#4ade80}
</style>
</head>
<body>
<h1>&#x1F6E1; Homeguard</h1>
<p class="sub">Configuração inicial &middot; acesse em http://setup.homeguard</p>

<div class="card">
  <h2>Rede Wi-Fi</h2>
  <div id="net-list"><div id="scan-status">Buscando redes...</div></div>
  <div id="sel-wrap" style="display:none">
    <div id="sel-box" onclick="clearSel()"><span id="sel-name"></span><span style="color:#888">trocar &#x2715;</span></div>
    <label>Senha do Wi-Fi</label>
    <input id="pass" type="password" placeholder="&bull;&bull;&bull;&bull;&bull;&bull;&bull;&bull;" autocomplete="new-password">
  </div>
</div>

<div class="card">
  <h2>Home Assistant / MQTT</h2>
  <label>Broker (IP do Home Assistant)</label>
  <input id="mqttHost" placeholder="192.168.1.90">
  <div class="row">
    <div><label>Porta</label><input id="mqttPort" type="number" value="1883"></div>
    <div><label>PIN p/ desarmar</label><input id="haPin" inputmode="numeric" placeholder="1234"></div>
  </div>
  <label>Usuário MQTT</label><input id="mqttUser" placeholder="mqtt">
  <label>Senha MQTT</label><input id="mqttPass" type="password" placeholder="&bull;&bull;&bull;&bull;">
</div>

<div class="card">
  <h2>Senha inicial do teclado</h2>
  <label>Código (4-8 dígitos)</label>
  <input id="code0" inputmode="numeric" placeholder="ex: 1234">
</div>

<div class="card">
  <details>
    <summary>Telegram (opcional)</summary>
    <label>Bot token</label><input id="botToken" placeholder="123456:ABC...">
    <label>Chat ID</label><input id="chatId" inputmode="numeric" placeholder="999999999">
  </details>
</div>

<button onclick="save()">Salvar e Reiniciar</button>
<p id="msg"></p>

<script>
var selSSID='';
function rssiBar(r){return r>-55?'&#x2582;&#x2584;&#x2586;&#x2588;':r>-65?'&#x2582;&#x2584;&#x2586;':r>-75?'&#x2582;&#x2584;':'&#x2582;';}
function escH(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}
function renderNets(nets){
  var el=document.getElementById('net-list');
  if(!nets.length){el.innerHTML='<div id="scan-status">Nenhuma rede. <span onclick="pollScan()" style="color:#4ade80;cursor:pointer">Tentar de novo</span></div>';return;}
  el.innerHTML='';
  nets.forEach(function(n){
    var d=document.createElement('div');d.className='net';
    d.innerHTML='<span>'+(n.open?'':'&#x1F512; ')+escH(n.ssid)+'</span><span class="net-sig">'+rssiBar(n.rssi)+'</span>';
    d.onclick=function(){pickNet(n.ssid,n.open);};
    el.appendChild(d);
  });
}
function pickNet(s,open){selSSID=s;document.getElementById('net-list').style.display='none';
  document.getElementById('sel-name').textContent=s;document.getElementById('sel-wrap').style.display='block';
  document.getElementById('pass').placeholder=open?'Rede aberta':'••••••••';document.getElementById('pass').focus();}
function clearSel(){selSSID='';document.getElementById('net-list').style.display='block';document.getElementById('sel-wrap').style.display='none';}
function pollScan(){fetch('/scan').then(function(r){if(r.status===202){setTimeout(pollScan,2000);return null;}return r.json();})
  .then(function(d){if(d)renderNets(d);}).catch(function(){setTimeout(pollScan,3000);});}
function val(id){return document.getElementById(id).value.trim();}
function save(){
  var msg=document.getElementById('msg');
  if(!selSSID){msg.textContent='Selecione a rede Wi-Fi.';return;}
  if(!val('mqttHost')){msg.textContent='Informe o broker MQTT.';return;}
  var d={ssid:selSSID,pass:document.getElementById('pass').value,
    mqttHost:val('mqttHost'),mqttPort:parseInt(val('mqttPort')||'1883'),
    mqttUser:val('mqttUser'),mqttPass:document.getElementById('mqttPass').value,
    haPin:val('haPin')||'1234',code0:val('code0'),
    botToken:val('botToken'),chatId:val('chatId')};
  msg.textContent='Salvando...';
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})
   .then(function(r){return r.json()}).then(function(){
     document.body.innerHTML='<div style="display:flex;min-height:100vh;align-items:center;justify-content:center;flex-direction:column;font-family:system-ui;color:#fff;background:#14151a;padding:24px"><div style="font-size:2rem">&#x2705;</div><h2 style="color:#4ade80;margin:8px 0">Configurado!</h2><p style="color:#aaa;text-align:center">O Homeguard vai reiniciar e conectar na sua rede.</p></div>';
   }).catch(function(){msg.textContent='Erro ao salvar.';});
}
pollScan();
</script>
</body>
</html>
)rawhtml";

// ── Handlers ──────────────────────────────────────────────────────
static void handleRoot()   { server.send_P(200, "text/html", PORTAL_HTML); }
static void handleRedirect(){ server.sendHeader("Location", "http://192.168.4.1/", true); server.send(302, "text/plain", ""); }

static void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING || n < 0) {
    if (n != WIFI_SCAN_RUNNING) WiFi.scanNetworks(true);
    server.send(202, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 20; i++) {
    JsonObject net = arr.add<JsonObject>();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleSave() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "vazio"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "json"); return; }

  Config cfg = Storage::loadConfig();   // preserva defaults (sirene etc.)
  strlcpy(cfg.ssid,     doc["ssid"]     | "", sizeof(cfg.ssid));
  strlcpy(cfg.wifiPass, doc["pass"]     | "", sizeof(cfg.wifiPass));
  strlcpy(cfg.mqttHost, doc["mqttHost"] | "", sizeof(cfg.mqttHost));
  cfg.mqttPort = doc["mqttPort"] | 1883;
  strlcpy(cfg.mqttUser, doc["mqttUser"] | "", sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass, doc["mqttPass"] | "", sizeof(cfg.mqttPass));
  strlcpy(cfg.haPin,    doc["haPin"]    | "1234", sizeof(cfg.haPin));
  strlcpy(cfg.botToken, doc["botToken"] | "", sizeof(cfg.botToken));
  strlcpy(cfg.chatId,   doc["chatId"]   | "", sizeof(cfg.chatId));
  cfg.configured = true;
  Storage::saveConfig(cfg);

  // Senha inicial do teclado (se informada)
  const char* code0 = doc["code0"] | "";
  if (strlen(code0) >= 4) {
    KeypadCode codes[HG_MAX_CODES];
    int n = Storage::loadCodes(codes, HG_MAX_CODES);
    bool exists = false;
    for (int i = 0; i < n; i++) if (strcmp(codes[i].code, code0) == 0) exists = true;
    if (!exists && n < HG_MAX_CODES) {
      strlcpy(codes[n].code,  code0,    sizeof(codes[n].code));
      strlcpy(codes[n].label, "inicial", sizeof(codes[n].label));
      Storage::saveCodes(codes, n + 1);
    }
  }

  server.send(200, "application/json", "{\"ok\":true}");
  g_done = true;
}

// ── API ───────────────────────────────────────────────────────────
void Provisioning::begin() {
  WiFi.mode(WIFI_AP_STA);  // STA p/ conseguir escanear redes
  String ap = "Homeguard-" + String((uint32_t)ESP.getEfuseMac(), HEX).substring(0, 4);
  ap.toUpperCase();
  WiFi.softAP(ap.c_str());
  WiFi.scanNetworks(true);
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/",     HTTP_GET,  handleRoot);
  server.on("/scan", HTTP_GET,  handleScan);
  server.on("/save", HTTP_POST, handleSave);
  // captive portal (iOS / Android / Windows)
  server.on("/hotspot-detect.html",       HTTP_GET, handleRedirect);
  server.on("/library/test/success.html", HTTP_GET, handleRedirect);
  server.on("/generate_204",              HTTP_GET, handleRedirect);
  server.on("/ncsi.txt",                  HTTP_GET, handleRedirect);
  server.on("/connecttest.txt",           HTTP_GET, handleRedirect);
  server.onNotFound(handleRedirect);
  server.begin();
  Serial.printf("[PROV] AP: %s  -> http://192.168.4.1\n", ap.c_str());
}

void Provisioning::handle() {
  dns.processNextRequest();
  server.handleClient();
}

bool Provisioning::isDone() { return g_done; }
