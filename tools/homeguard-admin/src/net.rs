//! Conversa com a placa: WebSocket (estado e comandos), HTTP (OTA e restauração
//! do backup) e a busca de placas na rede por mDNS. Tudo em threads próprias,
//! que avisam a interface por um canal.

use std::io::{self, Read};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use serde_json::Value;
use tungstenite::stream::MaybeTlsStream;
use tungstenite::{Message, WebSocket};

/// Tudo que as threads mandam para a interface.
pub enum NetEvent {
    Connected(String),
    Disconnected(String),
    Message(Value),
    OtaProgress(u64, u64),
    OtaDone(Result<(), String>),
    RestoreDone(Result<(), String>),
    Found(Vec<Board>),
    Flash(crate::flash::FlashEvent),
    Release(Result<Release, String>),
    Downloaded(Result<String, String>),
}

#[derive(Debug, Clone)]
pub struct Board {
    pub name: String,
    pub addr: String,
    pub detail: String,
}

/// Lado da interface da conexão WebSocket: manda comandos; fechar = soltar.
pub struct Conn {
    tx: Sender<String>,
}

impl Conn {
    pub fn send(&self, cmd: Value) {
        let _ = self.tx.send(cmd.to_string());
    }
}

pub fn connect(host: String, events: Sender<NetEvent>) -> Conn {
    let (tx, rx) = mpsc::channel::<String>();
    let h = host.clone();
    thread::spawn(move || {
        match open_ws(&h) {
            Ok(ws) => {
                let _ = events.send(NetEvent::Connected(h.clone()));
                let why = run_ws(ws, rx, &events);
                let _ = events.send(NetEvent::Disconnected(why));
            }
            Err(e) => {
                let _ = events.send(NetEvent::Disconnected(format!("não conectou em {h}: {e}")));
            }
        }
    });
    Conn { tx }
}

fn open_ws(host: &str) -> anyhow::Result<WebSocket<MaybeTlsStream<TcpStream>>> {
    let addr = (host, 80)
        .to_socket_addrs()?
        .next()
        .ok_or_else(|| anyhow::anyhow!("endereço não encontrado"))?;
    let stream = TcpStream::connect_timeout(&addr, Duration::from_secs(4))?;
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;   // só para o handshake
    stream.set_nodelay(true)?;
    let (mut ws, _) = tungstenite::client(format!("ws://{host}/ws"), MaybeTlsStream::Plain(stream))
        .map_err(|e| anyhow::anyhow!("{e}"))?;
    // Depois do handshake, timeout curto: a mesma thread alterna entre ler e
    // enviar. Antes dele, o timeout curto interrompia o próprio handshake.
    if let MaybeTlsStream::Plain(s) = ws.get_mut() {
        s.set_read_timeout(Some(Duration::from_millis(50)))?;
    }
    Ok(ws)
}

fn run_ws(
    mut ws: WebSocket<MaybeTlsStream<TcpStream>>,
    rx: Receiver<String>,
    events: &Sender<NetEvent>,
) -> String {
    let mut last_rx = Instant::now();
    let mut pinged = false;
    loop {
        match ws.read() {
            Ok(Message::Text(t)) => {
                last_rx = Instant::now();
                pinged = false;
                if let Ok(v) = serde_json::from_str::<Value>(t.as_str()) {
                    if events.send(NetEvent::Message(v)).is_err() {
                        return String::new();
                    }
                }
            }
            Ok(Message::Close(_)) => return "a placa fechou a conexão".into(),
            Ok(_) => {
                last_rx = Instant::now();
                pinged = false;
            }
            Err(tungstenite::Error::Io(e))
                if e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut => {}
            Err(e) => return format!("conexão caiu: {e}"),
        }
        loop {
            match rx.try_recv() {
                Ok(text) => {
                    if let Err(e) = ws.send(Message::text(text)) {
                        return format!("falha ao enviar: {e}");
                    }
                }
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    let _ = ws.close(None);
                    return String::new();
                }
            }
        }
        // Placa sumiu sem fechar (energia, Wi-Fi): o ping descobre.
        if last_rx.elapsed() > Duration::from_secs(15) {
            if pinged {
                return "a placa parou de responder".into();
            }
            if ws.send(Message::Ping(Default::default())).is_err() {
                return "a placa parou de responder".into();
            }
            pinged = true;
            last_rx = Instant::now();
        }
    }
}

// ------------------------------------------------------------------ HTTP

/// Leitor que conta quantos bytes já foram enviados (barra de progresso do OTA).
struct Counting<R> {
    inner: R,
    sent: u64,
    total: u64,
    last: Instant,
    events: Sender<NetEvent>,
}

impl<R: Read> Read for Counting<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.sent += n as u64;
        if self.last.elapsed() > Duration::from_millis(100) || self.sent == self.total {
            self.last = Instant::now();
            let _ = self.events.send(NetEvent::OtaProgress(self.sent, self.total));
        }
        Ok(n)
    }
}

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(5))
        .timeout_read(Duration::from_secs(90))
        .build()
}

/// Lê {"ok":..,"error":..} da resposta, inclusive das de erro (400).
fn board_reply(r: Result<ureq::Response, ureq::Error>) -> Result<(), String> {
    let resp = match r {
        Ok(resp) => resp,
        Err(ureq::Error::Status(_, resp)) => resp,
        Err(e) => return Err(format!("não deu para enviar: {e}")),
    };
    let v: Value = resp.into_json().map_err(|e| format!("resposta estranha da placa: {e}"))?;
    if v["ok"].as_bool() == Some(true) {
        Ok(())
    } else {
        Err(v["error"].as_str().unwrap_or("a placa recusou").to_string())
    }
}

/// Atualização por OTA: o mesmo multipart que a página de admin manda.
pub fn ota(host: String, token: String, firmware: Vec<u8>, events: Sender<NetEvent>) {
    thread::spawn(move || {
        let boundary = "----homeguard-admin-7d1e4b";
        let head = format!(
            "--{boundary}\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"homeguard.ino.bin\"\r\n\
             Content-Type: application/octet-stream\r\n\r\n"
        );
        let tail = format!("\r\n--{boundary}--\r\n");
        let total = (head.len() + firmware.len() + tail.len()) as u64;
        let body = io::Cursor::new(head.into_bytes())
            .chain(io::Cursor::new(firmware))
            .chain(io::Cursor::new(tail.into_bytes()));
        let reader = Counting { inner: body, sent: 0, total, last: Instant::now(), events: events.clone() };
        let r = agent()
            .post(&format!("http://{host}/api/ota?t={token}"))
            .set("Content-Type", &format!("multipart/form-data; boundary={boundary}"))
            .set("Content-Length", &total.to_string())
            .send(reader);
        let _ = events.send(NetEvent::OtaDone(board_reply(r)));
    });
}

pub fn restore(host: String, token: String, json: String, events: Sender<NetEvent>) {
    thread::spawn(move || {
        let r = agent()
            .post(&format!("http://{host}/api/restore?t={token}"))
            .set("Content-Type", "application/json")
            .send_string(&json);
        let _ = events.send(NetEvent::RestoreDone(board_reply(r)));
    });
}

// ------------------------------------------------------------------ busca

/// Acha placas de dois jeitos ao mesmo tempo, em ~4 s:
///   - mDNS (homeguard-XXXX.local): rápido, mas só na mesma rede local;
///   - varredura das redes /24 deste PC (e das que a pessoa digitar), batendo
///     em /api/info de quem tiver a porta 80 aberta. Acha a placa mesmo em outra
///     sub-rede roteada, onde o mDNS não chega.
pub fn discover(extra: Vec<Ipv4Addr>, events: Sender<NetEvent>) {
    thread::spawn(move || {
        let found = Arc::new(Mutex::new(Vec::<Board>::new()));
        let mdns_found = found.clone();
        let mdns = thread::spawn(move || {
            for ip in mdns_addresses() {
                if let Some(b) = probe(ip) {
                    add_board(&mdns_found, b);
                }
            }
        });

        let mut nets: Vec<Ipv4Addr> = local_networks();
        for n in extra {
            if !nets.contains(&n) {
                nets.push(n);
            }
        }
        let queue: Vec<Ipv4Addr> = nets
            .iter()
            .flat_map(|n| (1..=254u8).map(move |h| { let o = n.octets(); Ipv4Addr::new(o[0], o[1], o[2], h) }))
            .collect();
        let queue = Arc::new(Mutex::new(queue));
        let workers: Vec<_> = (0..64)
            .map(|_| {
                let (queue, found) = (queue.clone(), found.clone());
                thread::spawn(move || loop {
                    let Some(ip) = queue.lock().unwrap().pop() else { break };
                    let addr = SocketAddr::from((ip, 80));
                    if TcpStream::connect_timeout(&addr, Duration::from_millis(400)).is_ok() {
                        if let Some(b) = probe(ip) {
                            add_board(&found, b);
                        }
                    }
                })
            })
            .collect();
        for w in workers {
            let _ = w.join();
        }
        let _ = mdns.join();
        let mut list = found.lock().unwrap().clone();
        list.sort_by(|a, b| a.name.cmp(&b.name));
        let _ = events.send(NetEvent::Found(list));
    });
}

fn add_board(found: &Mutex<Vec<Board>>, b: Board) {
    let mut f = found.lock().unwrap();
    if !f.iter().any(|x| x.addr == b.addr) {
        f.push(b);
    }
}

/// É um Homeguard? A partir da 2.2 responde /api/info; antes disso, a página
/// tem "Homeguard" no título.
fn probe(ip: Ipv4Addr) -> Option<Board> {
    let agent = ureq::AgentBuilder::new().timeout(Duration::from_millis(2500)).build();
    if let Ok(resp) = agent.get(&format!("http://{ip}/api/info")).call() {
        if let Ok(v) = resp.into_json::<Value>() {
            if v["homeguard"].as_bool() == Some(true) {
                return Some(Board {
                    name: v["name"].as_str().unwrap_or("Homeguard").to_string(),
                    addr: ip.to_string(),
                    detail: format!(
                        "v{} · {}.local",
                        v["version"].as_str().unwrap_or("?"),
                        v["hostname"].as_str().unwrap_or("?")
                    ),
                });
            }
        }
    }
    let body = agent.get(&format!("http://{ip}/")).call().ok()?.into_string().ok()?;
    body.contains("Homeguard").then(|| Board {
        name: "Homeguard".into(),
        addr: ip.to_string(),
        detail: "firmware antigo (antes da 2.2)".into(),
    })
}

/// Redes /24 das interfaces deste PC (só endereços privados: não varre a internet).
fn local_networks() -> Vec<Ipv4Addr> {
    let mut nets = Vec::new();
    for iface in if_addrs::get_if_addrs().unwrap_or_default() {
        if let IpAddr::V4(ip) = iface.ip() {
            if ip.is_private() && !iface.is_loopback() {
                let o = ip.octets();
                let n = Ipv4Addr::new(o[0], o[1], o[2], 0);
                if !nets.contains(&n) {
                    nets.push(n);
                }
            }
        }
    }
    nets
}

/// "10.55.9.0/24", "10.55.9.7" ou "10.55.9" -> 10.55.9.0. Só /24.
pub fn parse_network(s: &str) -> Option<Ipv4Addr> {
    let s = s.trim().trim_end_matches("/24");
    let parts: Vec<&str> = s.split('.').filter(|p| !p.is_empty()).collect();
    if parts.len() < 3 || parts.len() > 4 {
        return None;
    }
    let o: Vec<u8> = parts[..3].iter().map(|p| p.parse().ok()).collect::<Option<_>>()?;
    Some(Ipv4Addr::new(o[0], o[1], o[2], 0))
}

fn mdns_addresses() -> Vec<Ipv4Addr> {
    let mut ips = Vec::new();
    let Ok(mdns) = mdns_sd::ServiceDaemon::new() else { return ips };
    if let Ok(rx) = mdns.browse("_http._tcp.local.") {
        let end = Instant::now() + Duration::from_secs(3);
        while let Some(left) = end.checked_duration_since(Instant::now()) {
            let Ok(ev) = rx.recv_timeout(left) else { break };
            if let mdns_sd::ServiceEvent::ServiceResolved(info) = ev {
                if info.get_hostname().starts_with("homeguard") {
                    for ip in info.get_addresses_v4() {
                        if !ips.contains(ip) {
                            ips.push(*ip);
                        }
                    }
                }
            }
        }
    }
    let _ = mdns.shutdown();
    ips
}

// ------------------------------------------------------------------ GitHub

/// Repositório com os releases do firmware (HOMEGUARD_REPO troca, ex.: um fork).
pub fn repo() -> String {
    std::env::var("HOMEGUARD_REPO").unwrap_or_else(|_| "duarte-gui/homeguard".into())
}

#[derive(Debug, Clone)]
pub struct Release {
    pub tag: String,
    pub name: String,
    pub url: String,       // download do homeguard.ino.bin
    pub size: u64,
    pub page: String,      // página do release, para ler as notas
}

/// Último release (não pré-release) que tenha o homeguard.ino.bin anexado.
pub fn latest_release() -> Result<Release, String> {
    let agent = ureq::AgentBuilder::new().timeout(Duration::from_secs(15)).build();
    let v: Value = agent
        .get(&format!("https://api.github.com/repos/{}/releases/latest", repo()))
        .set("User-Agent", "homeguard-admin")
        .set("Accept", "application/vnd.github+json")
        .call()
        .map_err(|e| match e {
            ureq::Error::Status(404, _) => "nenhum release publicado ainda no GitHub".to_string(),
            e => format!("não deu para consultar o GitHub: {e}"),
        })?
        .into_json()
        .map_err(|e| format!("resposta estranha do GitHub: {e}"))?;
    let asset = v["assets"]
        .as_array()
        .and_then(|a| a.iter().find(|x| x["name"].as_str() == Some("homeguard.ino.bin")))
        .ok_or_else(|| format!("o release {} não tem o homeguard.ino.bin", v["tag_name"].as_str().unwrap_or("?")))?;
    Ok(Release {
        tag: v["tag_name"].as_str().unwrap_or("").to_string(),
        name: v["name"].as_str().unwrap_or("").to_string(),
        url: asset["browser_download_url"].as_str().unwrap_or("").to_string(),
        size: asset["size"].as_u64().unwrap_or(0),
        page: v["html_url"].as_str().unwrap_or("").to_string(),
    })
}

/// Pasta de cache dos firmwares baixados (~/.cache/homeguard-admin).
pub fn cache_dir() -> std::path::PathBuf {
    let base = std::env::var_os("XDG_CACHE_HOME")
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("LOCALAPPDATA").map(std::path::PathBuf::from))
        .or_else(|| std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".cache")))
        .unwrap_or_else(std::env::temp_dir);
    base.join("homeguard-admin")
}

/// Baixa o firmware do release e confere antes de deixar usar. Devolve o caminho.
pub fn download_release(r: &Release) -> Result<String, String> {
    let dir = cache_dir();
    std::fs::create_dir_all(&dir).map_err(|e| format!("não deu para criar {}: {e}", dir.display()))?;
    let path = dir.join(format!("homeguard-{}.bin", r.tag.trim_start_matches('v')));
    let agent = ureq::AgentBuilder::new().timeout(Duration::from_secs(120)).build();
    let resp = agent
        .get(&r.url)
        .set("User-Agent", "homeguard-admin")
        .call()
        .map_err(|e| format!("falha no download: {e}"))?;
    let mut bin = Vec::with_capacity(r.size as usize);
    resp.into_reader()
        .take(4 * 1024 * 1024)
        .read_to_end(&mut bin)
        .map_err(|e| format!("falha no download: {e}"))?;
    if r.size > 0 && bin.len() as u64 != r.size {
        return Err(format!("download incompleto ({} de {} bytes)", bin.len(), r.size));
    }
    crate::flash::check_firmware(&bin)?;
    std::fs::write(&path, &bin).map_err(|e| format!("não deu para gravar {}: {e}", path.display()))?;
    Ok(path.to_string_lossy().to_string())
}

pub fn check_release(events: Sender<NetEvent>) {
    thread::spawn(move || {
        let _ = events.send(NetEvent::Release(latest_release()));
    });
}

pub fn fetch_release(r: Release, events: Sender<NetEvent>) {
    thread::spawn(move || {
        let _ = events.send(NetEvent::Downloaded(download_release(&r)));
    });
}
