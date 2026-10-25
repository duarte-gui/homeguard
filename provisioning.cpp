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
// Também servida pelo .ino em /setup/ (hotspot de resgate, com o alarme rodando):
// por isso os fetch usam caminho relativo ('scan', 'save', 'current').
const char PORTAL_HTML[] PROGMEM = R"rawhtml(
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
<p class="sub" id="sub">Configuração inicial</p>
<div class="card" id="rescue-note" style="display:none;border:1px solid #f59e0b"><p style="font-size:.85rem;color:#fbbf24">A placa está sem Wi-Fi, mas o alarme continua funcionando normalmente. Ajuste o que mudou e salve: o que ficar em branco continua como está.</p></div>

<div class="card">
  <h2>Rede Wi-Fi</h2>
  <div id="net-list"><div id="scan-status">Buscando redes...</div></div>
  <div id="manual-wrap" style="display:none">
    <label>Nome exato da rede (SSID)</label>
    <input id="manual-ssid" placeholder="MinhaRede" autocapitalize="off" autocorrect="off" spellcheck="false">
    <button type="button" onclick="useManual()">Usar esta rede</button>
  </div>
  <div id="sel-wrap" style="display:none">
    <div id="sel-box" onclick="clearSel()"><span id="sel-name"></span><span style="color:#888">trocar &#x2715;</span></div>
    <label>Senha do Wi-Fi <span id="pass-tog" onclick="togglePass()" style="float:right;color:#4ade80;cursor:pointer">mostrar</span></label>
    <input id="pass" type="password" placeholder="&bull;&bull;&bull;&bull;&bull;&bull;&bull;&bull;" autocomplete="new-password" autocapitalize="off" autocorrect="off" spellcheck="false">
  </div>
</div>

<div class="card" id="dhcp-card" style="display:none">
  <h2>Endereço na rede</h2>
  <label style="display:flex;gap:8px;align-items:center;color:#fff;font-size:.9rem">
    <input type="checkbox" id="dhcp" style="width:auto"> Voltar para IP automático (DHCP)</label>
  <p style="color:#aaa;font-size:.78rem;margin-top:6px">A placa está com IP fixo <b id="sip"></b>. Se ninguém consegue acessá-la pela rede, marque aqui.</p>
</div>

<div class="card">
  <h2>Home Assistant / MQTT</h2>
  <label>Broker (IP do Home Assistant)</label>
  <input id="mqttHost" placeholder="192.168.1.90">
  <label>Porta</label><input id="mqttPort" type="number" value="1883">
  <label>Usuário MQTT</label><input id="mqttUser" placeholder="mqtt">
  <label>Senha MQTT</label><input id="mqttPass" type="password" placeholder="&bull;&bull;&bull;&bull;">
</div>

<div class="card">
  <h2>&#x1F511; Senha mestre</h2>
  <label id="master-label">4 a 8 dígitos &mdash; usada para PROGRAMAR o teclado (cadastrar/remover senhas, reset) e para desarmar no Home Assistant. Guarde com cuidado.</label>
  <input id="master" inputmode="numeric" placeholder="ex: 1234">
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
var selSSID='', seen={};
function rssiBar(r){return r>-55?'&#x2582;&#x2584;&#x2586;&#x2588;':r>-65?'&#x2582;&#x2584;&#x2586;':r>-75?'&#x2582;&#x2584;':'&#x2582;';}
function escH(s){var d=document.createElement('div');d.textContent=s;return d.innerHTML;}
function row(html,sig,fn){
  var d=document.createElement('div');d.className='net';
  var a=document.createElement('span');a.innerHTML=html;
  var b=document.createElement('span');b.className='net-sig';b.innerHTML=sig;
  d.appendChild(a);d.appendChild(b);d.onclick=fn;return d;
}
// Acumula o que cada varredura encontrou. O rádio divide tempo com o hotspot,
// entao uma rede fraca aparece numa passada e some na seguinte: sem acumular,
// a lista fica piscando e a rede certa nunca da para clicar.
function mergeNets(nets){
  nets.forEach(function(n){ if(n.ssid) seen[n.ssid]=n; });
  renderNets();
}
function renderNets(){
  var el=document.getElementById('net-list'), arr=[];
  for(var k in seen) arr.push(seen[k]);
  arr.sort(function(a,b){return b.rssi-a.rssi;});
  el.innerHTML='';
  if(!arr.length){
    var s=document.createElement('div');s.id='scan-status';
    s.textContent='Buscando redes...';el.appendChild(s);
  }
  arr.forEach(function(n){
    el.appendChild(row((n.open?'':'&#x1F512; ')+escH(n.ssid),rssiBar(n.rssi),
      function(){pickNet(n.ssid,n.open);}));
  });
  el.appendChild(row('Outra rede ou oculta','digitar',askManual));
  el.appendChild(row('Procurar de novo','&#x21bb;',function(){fetch('scan?again=1');}));
}
var selOpen=false, cur={configured:false};
function pickNet(s,open){selSSID=s;selOpen=open;document.getElementById('net-list').style.display='none';
  document.getElementById('sel-name').textContent=s;document.getElementById('sel-wrap').style.display='block';
  document.getElementById('pass').placeholder=open?'Rede aberta':(cur.configured&&s===cur.ssid?'em branco mantém a senha salva':'••••••••');document.getElementById('pass').focus();}
function clearSel(){selSSID='';document.getElementById('net-list').style.display='block';document.getElementById('sel-wrap').style.display='none';}
function askManual(){
  document.getElementById('net-list').style.display='none';
  document.getElementById('manual-wrap').style.display='block';
  document.getElementById('manual-ssid').focus();
}
function useManual(){
  var s=document.getElementById('manual-ssid').value.trim();
  if(!s){document.getElementById('manual-ssid').focus();return;}
  document.getElementById('manual-wrap').style.display='none';
  pickNet(s,false);
}
// Nunca para: cada volta junta o que a varredura nova achou. Antes a busca
// encerrava na primeira resposta 200, entao uma varredura parcial (o caso comum,
// porque a primeira roda enquanto o hotspot ainda esta subindo) virava definitiva.
function pollScan(){
  fetch('scan')
    .then(function(r){return r.status===202?null:r.json();})
    .then(function(d){if(d)mergeNets(d);})
    .catch(function(){})
    .then(function(){setTimeout(pollScan,2500);});
}
function val(id){return document.getElementById(id).value.trim();}
// O teclado do celular capitaliza a primeira letra e o autocorretor troca
// palavras sem avisar: ver a senha antes de salvar evita o reason=15 no boot.
function togglePass(){
  var p=document.getElementById('pass'),t=document.getElementById('pass-tog');
  if(p.type==='password'){p.type='text';t.textContent='ocultar';}
  else{p.type='password';t.textContent='mostrar';}
}
var spaceWarned=false;
function save(){
  var msg=document.getElementById('msg');
  if(!selSSID){msg.textContent='Selecione a rede Wi-Fi.';return;}
  if(!val('mqttHost')){msg.textContent='Informe o broker MQTT.';return;}
  if(val('master').length<4){msg.textContent=cur.configured?'Digite a senha mestre atual para confirmar.':'Defina a senha mestre (mín. 4 dígitos).';return;}
  // Espaço no começo/fim é quase sempre acidente, mas pode ser de verdade: avisa
  // uma vez e salva no segundo toque. A senha nunca é cortada automaticamente.
  var pw=document.getElementById('pass').value;
  if(pw!==pw.trim()&&!spaceWarned){spaceWarned=true;
    msg.textContent='A senha do Wi-Fi começa ou termina com espaço. Confira e toque em Salvar de novo para manter assim.';return;}
  var d={ssid:selSSID,pass:document.getElementById('pass').value,
    mqttHost:val('mqttHost'),mqttPort:parseInt(val('mqttPort')||'1883'),
    mqttUser:val('mqttUser'),mqttPass:document.getElementById('mqttPass').value,
    master:val('master'),
    botToken:val('botToken'),chatId:val('chatId'),open:selOpen,dhcp:document.getElementById('dhcp').checked};
  msg.textContent='Salvando...';
  fetch('save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})
   .then(function(r){return r.json()}).then(function(r){
     if(r.ok===false){msg.textContent=r.error||'A placa recusou.';return;}
     document.body.innerHTML='<div style="display:flex;min-height:100vh;align-items:center;justify-content:center;flex-direction:column;font-family:system-ui;color:#fff;background:#14151a;padding:24px"><div style="font-size:2rem">&#x2705;</div><h2 style="color:#4ade80;margin:8px 0">Configurado!</h2><p style="color:#aaa;text-align:center">O Homeguard vai reiniciar e conectar na sua rede.</p></div>';
   }).catch(function(){msg.textContent='Erro ao salvar.';});
}
// Placa já configurada (hotspot de resgate): preenche com o que está salvo.
// Senhas e token nunca vêm; em branco = mantém.
fetch('current').then(function(r){return r.json();}).then(function(c){
  cur=c; if(!c.configured) return;
  document.getElementById('sub').textContent=c.name||'Homeguard';
  document.getElementById('rescue-note').style.display='block';
  document.getElementById('mqttHost').value=c.mqttHost||'';
  document.getElementById('mqttPort').value=c.mqttPort||1883;
  document.getElementById('mqttUser').value=c.mqttUser||'';
  document.getElementById('mqttPass').placeholder=c.hasMqttPass?'em branco mantém':'';
  document.getElementById('chatId').value=c.chatId||'';
  document.getElementById('botToken').placeholder=c.hasToken?'salvo (em branco mantém)':'123456:ABC...';
  document.getElementById('master-label').textContent='Senha mestre ATUAL, para confirmar a mudança (não troca a senha).';
  document.getElementById('master').type='password';
  if(c.staticIp){document.getElementById('dhcp-card').style.display='block';document.getElementById('sip').textContent=c.staticIp;}
  if(c.ssid) pickNet(c.ssid,false);
}).catch(function(){});
pollScan();
</script>
</body>
</html>
)rawhtml";

// ── Lista de redes ────────────────────────────────────────────────
// Montada ANTES de o softAP subir. Com o hotspot no ar e um celular associado a
// ele, o radio fica presso ao canal do AP para servir esse cliente e a varredura
// volta quase vazia — era por isso que o portal mostrava "Buscando redes..." para
// sempre. Medido na bancada: em modo estacao puro, 9 a 16 redes; com o AP no ar
// e o celular conectado, zero.
struct ScanNet { String ssid; int32_t rssi; bool open; };
static ScanNet g_nets[24];
static int     g_netCount = 0;

static void mergeScanResults(int n) {
  const int cap = (int)(sizeof(g_nets) / sizeof(g_nets[0]));
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;           // oculta: so da para chegar digitando
    int at = -1;
    for (int j = 0; j < g_netCount; j++) if (g_nets[j].ssid == ssid) { at = j; break; }
    if (at < 0) {
      if (g_netCount >= cap) continue;
      at = g_netCount++;
      g_nets[at].ssid = ssid;
    }
    g_nets[at].rssi = WiFi.RSSI(i);
    g_nets[at].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
  }
}

// Duas passadas: uma varredura sozinha ja perde rede fraca. Acumulando, o que
// aparece numa passada fica na lista. ~9 s no pior caso, antes do AP aparecer.
static void scanBeforeAP() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);
  for (int round = 0; round < 2; round++) {
    int n = WiFi.scanNetworks(false, true, false, 350);
    if (n > 0) mergeScanResults(n);
    WiFi.scanDelete();
  }
  Serial.printf("[PROV] varredura previa: %d redes\n", g_netCount);
}

// ── Handlers ──────────────────────────────────────────────────────
static void handleRoot()   { server.send_P(200, "text/html", PORTAL_HTML); }
static void handleRedirect(){ server.sendHeader("Location", "http://192.168.4.1/", true); server.send(302, "text/plain", ""); }

// Serve a lista montada antes do AP. Um rescan pedido pelo usuario (?again=1)
// roda assincrono e so pode ACRESCENTAR: com o celular no hotspot ele vem fraco,
// entao substituir a lista por ele seria piorar.
static void handleScan() {
  int n = WiFi.scanComplete();
  if (n >= 0) { mergeScanResults(n); WiFi.scanDelete(); }

  if (server.hasArg("again") && n != WIFI_SCAN_RUNNING) {
    WiFi.scanNetworks(true, true, false, 350);
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < g_netCount; i++) {
    JsonObject net = arr.add<JsonObject>();
    net["ssid"] = g_nets[i].ssid;
    net["rssi"] = g_nets[i].rssi;
    net["open"] = g_nets[i].open;
  }
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
  // Senha MESTRE (autoridade do teclado) — também vira o PIN de desarme no HA.
  const char* master = doc["master"] | "1234";
  strlcpy(cfg.masterCode, master, sizeof(cfg.masterCode));
  strlcpy(cfg.haPin,      master, sizeof(cfg.haPin));
  strlcpy(cfg.botToken,   doc["botToken"] | "", sizeof(cfg.botToken));
  strlcpy(cfg.chatId,     doc["chatId"]   | "", sizeof(cfg.chatId));
  cfg.configured = true;
  Storage::saveConfig(cfg);

  server.send(200, "application/json", "{\"ok\":true}");
  g_done = true;
}

// ── API ───────────────────────────────────────────────────────────
void Provisioning::begin() {
  // Varre PRIMEIRO, com o radio livre. Depois sobe o hotspot. A ordem e o que
  // faz a lista existir: depois do AP no ar, a varredura nao presta mais.
  scanBeforeAP();

  WiFi.mode(WIFI_AP_STA);
  String ap = "Homeguard-" + String((uint32_t)ESP.getEfuseMac(), HEX).substring(0, 4);
  ap.toUpperCase();
  WiFi.softAP(ap.c_str());
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/",     HTTP_GET,  handleRoot);
  server.on("/scan", HTTP_GET,  handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/current", HTTP_GET, []() { server.send(200, "application/json", "{\"configured\":false}"); });
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
