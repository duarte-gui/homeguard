//! Estado do programa e o que cada tecla faz. O desenho fica em ui.rs.

use std::fs;
use std::sync::mpsc::{self, Receiver, Sender};
use std::time::{Duration, Instant};

use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use serde_json::{json, Value};

use crate::flash::{self, FlashEvent, Port};
use crate::form::{Field, Form, FormKey, Kind};
use crate::net::{self, Board, Conn, NetEvent};
use crate::proto::{self, Admin, Event, State, WifiNet};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tab {
    Painel,
    Zonas,
    Senhas,
    Tempos,
    Sirene,
    Rede,
    Eventos,
    Sistema,
    Usb,
}

pub const TABS: [(Tab, &str); 9] = [
    (Tab::Painel, "Painel"),
    (Tab::Zonas, "Zonas"),
    (Tab::Senhas, "Senhas"),
    (Tab::Tempos, "Tempos e agenda"),
    (Tab::Sirene, "Sirene/Telegram"),
    (Tab::Rede, "Rede"),
    (Tab::Eventos, "Eventos"),
    (Tab::Sistema, "Sistema"),
    (Tab::Usb, "Gravar USB"),
];

impl Tab {
    /// Abas que só funcionam com a sessão de admin aberta.
    pub fn needs_admin(self) -> bool {
        !matches!(self, Tab::Painel | Tab::Usb)
    }
}

/// O que fazer depois de uma pergunta (texto, confirmação ou formulário).
#[derive(Clone)]
pub enum Act {
    Connect,
    ScanNetwork,
    AdminLogin,
    Disarm,
    ArmForce(String),
    SaveZone(u32),
    AddCode,
    RemoveCode(usize),
    SaveNetwork,
    ChangeMaster,
    Reboot,
    BackupPath,
    RestorePath,
    Restore(String),
    OtaPath,
    Ota(Vec<u8>),
    FlashPath,
    Flash,
}

pub enum Modal {
    Connect { sel: usize },
    Input { title: String, label: String, value: String, secret: bool, digits: bool, act: Act },
    Confirm { text: String, act: Act },
    Form { title: String, form: Form, act: Act },
    Wifi { nets: Vec<WifiNet>, sel: usize },
}

pub struct Status {
    pub text: String,
    pub ok: bool,
    pub at: Instant,
}

pub struct App {
    pub tx: Sender<NetEvent>,
    rx: Receiver<NetEvent>,
    pub conn: Option<Conn>,
    pub connected: bool,
    pub host: String,
    last_attempt: Option<Instant>,

    pub state: Option<State>,
    pub state_at: Instant,
    pub admin: Option<Admin>,

    pub tab: Tab,
    /// true: as setas andam no menu lateral; false: dentro da aba.
    pub menu_focus: bool,
    pub modal: Option<Modal>,
    pub status: Option<Status>,
    pub quit: bool,

    pub boards: Vec<Board>,
    pub searching: bool,
    pub scan_nets: Vec<std::net::Ipv4Addr>,

    pub zone_sel: usize,
    pub code_sel: usize,
    pub timing_form: Form,
    pub siren_form: Form,
    pub net_form: Form,
    pub events: Vec<Event>,
    pub events_scroll: usize,
    last_arm_mode: String,
    /// IP fixo recém-salvo: a próxima reconexão vai para ele.
    pending_host: Option<String>,
    backup_path: Option<String>,
    pub ota: Option<(u64, u64)>,

    pub ports: Vec<Port>,
    pub port_sel: usize,
    last_usb_poll: Instant,
    pub fw_path: String,
    pub fw_info: String,
    pub erase_all: bool,
    pub flashing: bool,
    pub flash_log: Vec<String>,
    pub flash_prog: Option<(&'static str, usize, usize)>,

    /// Último release no GitHub (consultado ao abrir; d baixa).
    pub release: Option<Result<net::Release, String>>,
    pub downloading: bool,
    /// Depois de baixar pela aba Sistema, já pergunta se manda por OTA.
    ota_after_download: bool,
}

impl App {
    pub fn new(host: Option<String>) -> Self {
        let (tx, rx) = mpsc::channel();
        let mut app = App {
            tx,
            rx,
            conn: None,
            connected: false,
            host: String::new(),
            last_attempt: None,
            state: None,
            state_at: Instant::now(),
            admin: None,
            tab: Tab::Painel,
            menu_focus: true,
            modal: None,
            status: None,
            quit: false,
            boards: Vec::new(),
            searching: false,
            scan_nets: Vec::new(),
            zone_sel: 0,
            code_sel: 0,
            timing_form: Form::default(),
            siren_form: Form::default(),
            net_form: Form::default(),
            events: Vec::new(),
            events_scroll: 0,
            last_arm_mode: "away".into(),
            pending_host: None,
            backup_path: None,
            ota: None,
            ports: flash::list_ports(),
            port_sel: 0,
            last_usb_poll: Instant::now(),
            fw_path: default_firmware_path(),
            fw_info: String::new(),
            erase_all: false,
            flashing: false,
            flash_log: Vec::new(),
            flash_prog: None,
            release: None,
            downloading: false,
            ota_after_download: false,
        };
        net::check_release(app.tx.clone());
        app.refresh_fw_info();
        match host {
            Some(h) => app.connect(h),
            None => app.open_connect(),
        }
        app
    }

    // ------------------------------------------------------------ conexão

    pub fn go_usb(&mut self) {
        self.tab = Tab::Usb;
        self.modal = None;
    }

    fn open_connect(&mut self) {
        self.search();
        self.modal = Some(Modal::Connect { sel: 0 });
    }

    /// Busca as placas (mDNS + varredura). Inclui a rede da placa atual e as
    /// que a pessoa já pediu para varrer.
    fn search(&mut self) {
        if self.searching {
            return;
        }
        self.searching = true;
        let mut extra = self.scan_nets.clone();
        if let Some(n) = net::parse_network(&self.host) {
            if !extra.contains(&n) {
                extra.push(n);
            }
        }
        net::discover(extra, self.tx.clone());
    }

    pub fn connect(&mut self, host: String) {
        let host = host.trim().trim_start_matches("http://").trim_end_matches('/').to_string();
        if host.is_empty() {
            return;
        }
        self.conn = None; // solta a anterior
        self.connected = false;
        self.admin = None;
        self.state = None;
        self.host = host.clone();
        self.last_attempt = Some(Instant::now());
        self.info(format!("conectando em {host}…"));
        self.conn = Some(net::connect(host, self.tx.clone()));
    }

    fn send(&mut self, cmd: Value) {
        match &self.conn {
            Some(c) if self.connected => c.send(cmd),
            _ => self.error("sem conexão com a placa (c: conectar)".into()),
        }
    }

    pub fn info(&mut self, text: String) {
        self.status = Some(Status { text, ok: true, at: Instant::now() });
    }
    pub fn error(&mut self, text: String) {
        self.status = Some(Status { text, ok: false, at: Instant::now() });
    }

    /// Chamado a cada volta do laço: eventos das threads, USB e reconexão automática.
    pub fn tick(&mut self) {
        while let Ok(ev) = self.rx.try_recv() {
            self.on_event(ev);
        }
        if self.last_usb_poll.elapsed() > Duration::from_secs(1) {
            self.last_usb_poll = Instant::now();
            self.poll_usb();
        }
        // Caiu (reinício da placa, Wi-Fi): tenta de novo a cada 3 s.
        if !self.connected && !self.host.is_empty() {
            if self.last_attempt.map(|t| t.elapsed() > Duration::from_secs(3)).unwrap_or(true) {
                let h = self.host.clone();
                self.last_attempt = Some(Instant::now());
                self.conn = Some(net::connect(h, self.tx.clone()));
            }
        }
    }

    /// Placa ligada ou tirada do USB: avisa e mantém a lista de portas em dia.
    /// Durante a gravação a porta some e volta (reset), então não mexe.
    fn poll_usb(&mut self) {
        if self.flashing {
            return;
        }
        let now = flash::list_ports();
        let antes: Vec<String> = self.ports.iter().map(|p| p.name.clone()).collect();
        let sel_name = self.ports.get(self.port_sel).map(|p| p.name.clone());
        for p in now.iter().filter(|p| p.is_esp() && !antes.contains(&p.name)) {
            let mac = p.mac().map(|m| format!(", MAC {m}")).unwrap_or_default();
            self.info(format!("ESP32 conectado na USB: {}{mac}", p.name));
        }
        for name in antes.iter().filter(|n| !now.iter().any(|p| &p.name == *n)) {
            if self.ports.iter().any(|p| &p.name == name && p.is_esp()) {
                self.info(format!("ESP32 desconectado da USB ({name})"));
            }
        }
        self.port_sel = sel_name.and_then(|n| now.iter().position(|p| p.name == n)).unwrap_or(0);
        self.ports = now;
    }

    pub fn usb_esp(&self) -> Option<&Port> {
        self.ports.iter().find(|p| p.is_esp())
    }

    fn on_event(&mut self, ev: NetEvent) {
        match ev {
            NetEvent::Connected(h) => {
                if h == self.host {
                    self.connected = true;
                    self.info(format!("conectado em {h}"));
                }
            }
            NetEvent::Disconnected(why) => {
                let was = self.connected;
                self.connected = false;
                self.admin = None;
                if was || self.status.as_ref().map(|s| s.ok).unwrap_or(true) {
                    if !why.is_empty() {
                        self.error(format!("{why} (tentando de novo…)"));
                    }
                }
            }
            NetEvent::Message(v) => self.on_message(v),
            NetEvent::OtaProgress(sent, total) => self.ota = Some((sent, total)),
            NetEvent::OtaDone(r) => {
                self.ota = None;
                match r {
                    Ok(()) => self.info("firmware enviado: a placa está reiniciando. Se a versão nova não subir, ela volta para a atual.".into()),
                    Err(e) => self.error(format!("atualização recusada: {e}")),
                }
            }
            NetEvent::RestoreDone(r) => match r {
                Ok(()) => self.info("backup restaurado".into()),
                Err(e) => self.error(format!("restauração recusada: {e}")),
            },
            NetEvent::Release(r) => self.release = Some(r),
            NetEvent::Downloaded(r) => {
                self.downloading = false;
                match r {
                    Ok(path) => {
                        self.fw_path = path.clone();
                        self.refresh_fw_info();
                        self.info(format!("baixado: {path} ({})", self.fw_info));
                        if std::mem::take(&mut self.ota_after_download) {
                            self.run(Act::OtaPath, path, None);
                        }
                    }
                    Err(e) => {
                        self.ota_after_download = false;
                        self.error(e);
                    }
                }
            }
            NetEvent::Found(b) => {
                self.searching = false;
                self.boards = b;
            }
            NetEvent::Flash(f) => match f {
                FlashEvent::Log(s) => self.flash_log.push(s),
                FlashEvent::Progress { part, current, total } => self.flash_prog = Some((part, current, total)),
                FlashEvent::Done(r) => {
                    self.flashing = false;
                    self.flash_prog = None;
                    match r {
                        Ok(()) => {
                            self.flash_log.push("✔ gravado e reiniciado".into());
                            self.info("gravação concluída".into());
                        }
                        Err(e) => {
                            self.flash_log.push(format!("✘ {e}"));
                            self.error(e);
                        }
                    }
                }
            },
        }
    }

    fn on_message(&mut self, v: Value) {
        match v["type"].as_str() {
            Some("result") => self.on_result(v),
            Some("admin") => {
                if let Ok(a) = serde_json::from_value::<Admin>(v) {
                    self.admin = Some(a);
                    self.load_forms(false);
                }
            }
            _ => {
                if let Ok(s) = serde_json::from_value::<State>(v) {
                    self.state = Some(s);
                    self.state_at = Instant::now();
                }
            }
        }
    }

    fn on_result(&mut self, v: Value) {
        let cmd = v["command"].as_str().unwrap_or("").to_string();
        let ok = v["ok"].as_bool().unwrap_or(false);
        let err = v["error"].as_str().unwrap_or("").to_string();
        if !ok && err.starts_with("Sessão de administrador") {
            self.admin = None;
        }
        match cmd.as_str() {
            "arm" if !ok && v["openZones"].as_array().map(|a| !a.is_empty()).unwrap_or(false) => {
                let zonas: Vec<&str> = v["openZones"].as_array().unwrap().iter().filter_map(|z| z.as_str()).collect();
                self.modal = Some(Modal::Confirm {
                    text: format!(
                        "Zona aberta: {}.\n\nArmar ({}) mesmo assim? A zona fica inibida até desarmar.",
                        zonas.join(", "),
                        proto::mode_label(&self.last_arm_mode)
                    ),
                    act: Act::ArmForce(self.last_arm_mode.clone()),
                });
            }
            "getEvents" if ok => {
                self.events = serde_json::from_value(v["events"].clone()).unwrap_or_default();
                self.events_scroll = 0;
            }
            "getBackup" if ok => {
                if let Some(path) = self.backup_path.take() {
                    let text = serde_json::to_string_pretty(&v["backup"]).unwrap_or_default();
                    match fs::write(&path, text) {
                        Ok(()) => self.info(format!("backup salvo em {path} (tem as senhas: guarde com cuidado)")),
                        Err(e) => self.error(format!("não deu para gravar {path}: {e}")),
                    }
                }
            }
            "scanWifi" if ok => {
                let nets: Vec<WifiNet> = serde_json::from_value(v["networks"].clone()).unwrap_or_default();
                if nets.is_empty() {
                    self.info("nenhuma rede encontrada".into());
                } else {
                    self.modal = Some(Modal::Wifi { nets, sel: 0 });
                }
            }
            _ if ok => {
                let msg = match cmd.as_str() {
                    "arm" => "armado",
                    "disarm" => "desarmado",
                    "silenceSiren" => "sirene silenciada (o alarme segue disparado até desarmar)",
                    "adminLogin" => "sessão de admin aberta (fecha sozinha após 10 min sem uso)",
                    "adminLogout" => "saiu do admin",
                    "setZone" => "zona salva",
                    "addCode" => "senha cadastrada",
                    "removeCode" => "senha removida",
                    "setTiming" => "tempos salvos",
                    "setAutoArm" => "arme automático salvo",
                    "setSiren" => "sirene salva",
                    "setTelegram" => "Telegram salvo",
                    "testTelegram" => "mensagem de teste enviada",
                    "testOutput" => "saída acionada por 1 s",
                    "setMaster" => "senha mestre alterada",
                    "setNetwork" => "rede salva: a placa está reiniciando",
                    "reboot" => "reiniciando a placa…",
                    _ => "ok",
                };
                match cmd.as_str() {
                    "setTiming" | "setAutoArm" => self.timing_form.dirty = false,
                    "setSiren" | "setTelegram" => self.siren_form.dirty = false,
                    "setNetwork" => {
                        self.net_form.dirty = false;
                        if let Some(h) = self.pending_host.take() {
                            if !h.is_empty() {
                                self.host = h;
                            }
                        }
                    }
                    "adminLogout" => self.admin = None,
                    // Entrou no admin a partir do menu: já vai para dentro da aba.
                    "adminLogin" if self.tab.needs_admin() => self.menu_focus = false,
                    _ => {}
                }
                self.info(msg.into());
            }
            _ => self.error(if err.is_empty() { format!("{cmd}: a placa recusou") } else { err }),
        }
    }

    /// Preenche os formulários com o que está na placa (só os que não foram mexidos).
    fn load_forms(&mut self, force: bool) {
        let Some(a) = self.admin.clone() else { return };
        if force || !self.timing_form.dirty {
            let sel = self.timing_form.sel;
            self.timing_form = Form::new(vec![
                Field::new("entry", "Entrada: tempo para desarmar (s)", Kind::Number { min: 0, max: 300 }, a.timing.entry_delay_sec.to_string()),
                Field::new("exit", "Saída: tempo para sair (s)", Kind::Number { min: 0, max: 300 }, a.timing.exit_delay_sec.to_string()),
                Field::new("open", "Avisar zona aberta há (min, 0 = não)", Kind::Number { min: 0, max: 1440 }, a.timing.open_alert_min.to_string()),
                Field::new("aa_on", "Arme automático", Kind::Bool, a.auto_arm.enabled.to_string()),
                Field::new("aa_time", "Horário (HH:MM)", Kind::Time, format!("{:02}:{:02}", a.auto_arm.minute / 60, a.auto_arm.minute % 60)),
                Field::new("aa_days", "Dias", Kind::Days, a.auto_arm.days.to_string()),
                Field::new("aa_mode", "Modo", Kind::Choice(vec![("away".into(), "Ausente".into()), ("home".into(), "Em casa".into())]), a.auto_arm.mode.clone()),
                Field::new("tz", "Fuso horário", Kind::Choice(tz_options(&a.auto_arm.tz)), a.auto_arm.tz.clone()),
            ]);
            self.timing_form.sel = sel;
        }
        if force || !self.siren_form.dirty {
            let sel = self.siren_form.sel;
            self.siren_form = Form::new(vec![
                Field::new("siren_on", "Sirene física habilitada", Kind::Bool, a.siren.enabled.to_string()),
                Field::new("siren_to", "Desliga sozinha após (s, 0 = nunca)", Kind::Number { min: 0, max: 1800 }, a.siren.timeout_sec.to_string()),
                Field::new("chat", "Telegram: ID do chat", Kind::Text(23), a.telegram.chat_id.clone()),
                Field::new("token", "Telegram: token do bot", Kind::Secret(63), "")
                    .hint(if a.telegram.has_token { "salvo (em branco mantém)" } else { "123456:ABC…" }),
            ]);
            self.siren_form.sel = sel;
        }
        if force || !self.net_form.dirty {
            let n = &a.network;
            let sel = self.net_form.sel;
            self.net_form = Form::new(vec![
                Field::new("ssid", "Wi-Fi (só 2,4 GHz)", Kind::Text(32), n.ssid.clone()),
                Field::new("pass", "Senha do Wi-Fi", Kind::Secret(63), "").hint("(em branco mantém)"),
                Field::new("hostname", "Nome na rede (.local)", Kind::Text(32), n.hostname.clone()).hint(&n.default_hostname),
                Field::new("deviceName", "Nome da central (HA/Telegram)", Kind::Text(32), n.device_name.clone()).hint(&n.default_device_name),
                Field::new("mqttId", "ID no MQTT", Kind::Text(32), n.mqtt_id.clone()).hint(&n.default_mqtt_id),
                Field::new("mqttHost", "Broker MQTT", Kind::Text(63), n.mqtt_host.clone()),
                Field::new("mqttPort", "Porta", Kind::Number { min: 1, max: 65535 }, n.mqtt_port.to_string()),
                Field::new("mqttUser", "Usuário MQTT", Kind::Text(32), n.mqtt_user.clone()),
                Field::new("mqttPass", "Senha MQTT", Kind::Secret(64), "").hint(if n.has_mqtt_pass { "(em branco mantém)" } else { "" }),
                Field::new(
                    "ipMode",
                    "Endereço",
                    Kind::Choice(vec![("dhcp".into(), "Automático (DHCP)".into()), ("static".into(), "IP fixo".into())]),
                    if n.ip_mode.is_empty() { "dhcp".to_string() } else { n.ip_mode.clone() },
                ),
                // Sem IP fixo salvo, sugere o que a placa está usando agora.
                Field::new("ip", "IP fixo", Kind::Text(15), or(&n.ip, &n.current.ip)),
                Field::new("gateway", "Gateway", Kind::Text(15), or(&n.gateway, &n.current.gateway)),
                Field::new("subnet", "Máscara", Kind::Text(15), or(&n.subnet, &n.current.subnet)),
                Field::new("dns", "DNS", Kind::Text(15), n.dns.clone()).hint("(em branco = o gateway)"),
            ]);
            self.net_form.sel = sel;
        }
        self.zone_sel = self.zone_sel.min(a.zones.len().saturating_sub(1));
        self.code_sel = self.code_sel.min(a.codes.len().saturating_sub(1));
    }

    // ------------------------------------------------------------ teclas

    pub fn key(&mut self, k: KeyEvent) {
        if k.modifiers.contains(KeyModifiers::CONTROL) && k.code == KeyCode::Char('c') {
            self.quit = true;
            return;
        }
        if self.modal.is_some() {
            self.modal_key(k);
            return;
        }
        if self.menu_focus {
            match k.code {
                KeyCode::Up => return self.cycle_tab(-1),
                KeyCode::Down => return self.cycle_tab(1),
                KeyCode::Right | KeyCode::Enter | KeyCode::Tab => {
                    if self.tab.needs_admin() && self.admin.is_none() {
                        if self.connected {
                            self.ask_login();
                        }
                    } else {
                        self.menu_focus = false;
                    }
                    return;
                }
                _ => {}
            }
        } else if let Some(form) = self.tab_form() {
            // O formulário da aba tem prioridade (texto sendo digitado, ←/→ nas
            // opções, dias 1..7).
            // Na edição o formulário fica com todas as teclas (o key() devolve
            // Handled). Antes um `form.editing ||` aqui engolia as teclas sem
            // passá-las: só o primeiro caractere entrava.
            if matches!(form.key(k), FormKey::Handled) {
                return;
            }
        }
        match k.code {
            KeyCode::Char('q') => self.quit = true,
            KeyCode::Esc | KeyCode::Left if !self.menu_focus => self.menu_focus = true,
            // Escolher a aba pelo número já entra nela: antes o foco ficava no
            // menu, e ↓ andava pelas abas em vez dos campos.
            KeyCode::Char(c @ '1'..='9') => {
                self.tab = TABS[(c as u8 - b'1') as usize].0;
                self.menu_focus = self.tab.needs_admin() && self.admin.is_none();
                self.on_tab_enter();
            }
            KeyCode::BackTab => self.cycle_tab(-1),
            KeyCode::Char(']') => self.cycle_tab(1),
            KeyCode::Char('[') => self.cycle_tab(-1),
            KeyCode::Char('c') => self.open_connect(),
            KeyCode::Char('L') | KeyCode::Char('l') if self.admin.is_none() && self.tab != Tab::Usb => self.ask_login(),
            _ => self.tab_key(k),
        }
    }

    fn cycle_tab(&mut self, d: i32) {
        let i = TABS.iter().position(|t| t.0 == self.tab).unwrap_or(0) as i32;
        self.tab = TABS[((i + d).rem_euclid(TABS.len() as i32)) as usize].0;
        self.on_tab_enter();
    }

    fn on_tab_enter(&mut self) {
        if self.tab == Tab::Eventos && self.admin.is_some() && self.events.is_empty() {
            self.send(json!({"command": "getEvents"}));
        }
    }

    fn tab_form(&mut self) -> Option<&mut Form> {
        if self.admin.is_none() {
            return None;
        }
        match self.tab {
            Tab::Tempos => Some(&mut self.timing_form),
            Tab::Sirene => Some(&mut self.siren_form),
            Tab::Rede => Some(&mut self.net_form),
            _ => None,
        }
    }

    fn ask_login(&mut self) {
        self.modal = Some(Modal::Input {
            title: "Administração".into(),
            label: "Senha mestre".into(),
            value: String::new(),
            secret: true,
            digits: true,
            act: Act::AdminLogin,
        });
    }

    fn tab_key(&mut self, k: KeyEvent) {
        let ch = match k.code {
            KeyCode::Char(c) => Some(c),
            _ => None,
        };
        if self.tab.needs_admin() && self.admin.is_none() {
            return;
        }
        match self.tab {
            Tab::Painel => match ch {
                Some('a') => self.arm("away"),
                Some('h') => self.arm("home"),
                Some('d') => {
                    self.modal = Some(Modal::Input {
                        title: "Desarmar".into(),
                        label: "Senha cadastrada".into(),
                        value: String::new(),
                        secret: true,
                        digits: true,
                        act: Act::Disarm,
                    })
                }
                Some('s') => self.send(json!({"command": "silenceSiren"})),
                _ => {}
            },
            Tab::Zonas => {
                let n = self.admin.as_ref().map(|a| a.zones.len()).unwrap_or(0);
                match k.code {
                    KeyCode::Up if !self.menu_focus => self.zone_sel = self.zone_sel.saturating_sub(1),
                    KeyCode::Down if !self.menu_focus => self.zone_sel = (self.zone_sel + 1).min(n.saturating_sub(1)),
                    KeyCode::Enter => self.edit_zone(),
                    _ => {}
                }
            }
            Tab::Senhas => {
                let n = self.admin.as_ref().map(|a| a.codes.len()).unwrap_or(0);
                match k.code {
                    KeyCode::Up => self.code_sel = self.code_sel.saturating_sub(1),
                    KeyCode::Down => self.code_sel = (self.code_sel + 1).min(n.saturating_sub(1)),
                    KeyCode::Char('n') => {
                        self.modal = Some(Modal::Form {
                            title: "Nova senha".into(),
                            form: Form::new(vec![
                                Field::new("label", "Nome", Kind::Text(23), "").hint("ex.: Ana"),
                                Field::new("code", "Senha (4 a 8 números)", Kind::Digits(8), ""),
                                Field::new("duress", "Senha de pânico", Kind::Bool, "false"),
                            ]),
                            act: Act::AddCode,
                        })
                    }
                    KeyCode::Char('x') | KeyCode::Delete => {
                        if let Some(c) = self.admin.as_ref().and_then(|a| a.codes.get(self.code_sel)) {
                            let label = c.label.clone();
                            self.modal = Some(Modal::Confirm {
                                text: format!("Remover a senha de {label}?"),
                                act: Act::RemoveCode(self.code_sel),
                            });
                        }
                    }
                    _ => {}
                }
            }
            Tab::Tempos => match ch {
                Some('s') => self.save_timing(),
                Some('r') => self.load_forms(true),
                _ => {}
            },
            Tab::Sirene => match ch {
                Some('s') => self.save_siren(),
                Some('r') => self.load_forms(true),
                Some('t') => self.send(json!({"command": "testOutput", "target": "siren"})),
                Some('k') => self.send(json!({"command": "testOutput", "target": "lock"})),
                Some('g') => self.send(json!({"command": "testTelegram"})),
                _ => {}
            },
            Tab::Rede => match ch {
                Some('s') => {
                    let mut text = "A placa vai reiniciar com a rede nova. Se não conectar, ela abre o portal de configuração sozinha.".to_string();
                    if self.net_form.get("ipMode") == "static" {
                        let ip = self.net_form.get("ip").trim().to_string();
                        text.push_str(&format!(
                            "\n\nCom IP fixo, a placa passa a ser {ip}. Se o IP estiver errado, ela some da rede: aí é o teclado (#, senha mestre, 3) que abre o hotspot para voltar ao DHCP."
                        ));
                        // Depois de reiniciar, reconecta no IP novo.
                        self.pending_host = Some(ip);
                    } else {
                        self.pending_host = None;
                    }
                    text.push_str("\n\nSalvar?");
                    self.modal = Some(Modal::Confirm { text, act: Act::SaveNetwork })
                }
                Some('r') => self.load_forms(true),
                Some('w') => {
                    self.info("procurando redes (uns 4 s)…".into());
                    self.send(json!({"command": "scanWifi"}));
                }
                _ => {}
            },
            Tab::Eventos => match k.code {
                KeyCode::Char('r') => {
                    self.send(json!({"command": "getEvents"}));
                }
                KeyCode::Up => self.events_scroll = self.events_scroll.saturating_sub(1),
                KeyCode::Down => self.events_scroll = (self.events_scroll + 1).min(self.events.len().saturating_sub(1)),
                KeyCode::PageUp => self.events_scroll = self.events_scroll.saturating_sub(15),
                KeyCode::PageDown => self.events_scroll = (self.events_scroll + 15).min(self.events.len().saturating_sub(1)),
                _ => {}
            },
            Tab::Sistema => match ch {
                Some('m') => {
                    self.modal = Some(Modal::Form {
                        title: "Trocar a senha mestre".into(),
                        form: Form::new(vec![
                            Field::new("cur", "Senha atual", Kind::Secret(8), ""),
                            Field::new("new", "Nova (4 a 8 números)", Kind::Secret(8), ""),
                            Field::new("conf", "Repita a nova", Kind::Secret(8), ""),
                        ]),
                        act: Act::ChangeMaster,
                    })
                }
                Some('b') => {
                    let id = self.admin.as_ref().map(|a| a.network.mqtt_id.clone()).unwrap_or_default();
                    let dia = chrono::Local::now().format("%Y-%m-%d");
                    self.ask_path("Baixar backup", "Salvar em", format!("homeguard-backup-{id}-{dia}.json"), Act::BackupPath);
                }
                Some('o') => self.ask_path("Restaurar backup", "Arquivo", String::new(), Act::RestorePath),
                Some('u') => {
                    let p = self.fw_path.clone();
                    self.ask_path("Atualizar firmware pela rede (OTA)", "Arquivo .bin", p, Act::OtaPath)
                }
                Some('r') => {
                    self.modal = Some(Modal::Confirm { text: "Reiniciar a placa agora?".into(), act: Act::Reboot })
                }
                Some('x') => self.send(json!({"command": "adminLogout"})),
                Some('d') => self.download_release(true),
                Some('a') => self.send(json!({"command": "getAdmin"})),
                _ => {}
            },
            Tab::Usb => match k.code {
                _ if self.flashing => {}
                KeyCode::Up => self.port_sel = self.port_sel.saturating_sub(1),
                KeyCode::Down => self.port_sel = (self.port_sel + 1).min(self.ports.len().saturating_sub(1)),
                KeyCode::Char('r') => {
                    self.ports = flash::list_ports();
                    self.port_sel = 0;
                }
                KeyCode::Char('f') => {
                    let p = self.fw_path.clone();
                    self.ask_path("Firmware para gravar", "Arquivo .bin", p, Act::FlashPath)
                }
                KeyCode::Char('e') => self.erase_all = !self.erase_all,
                KeyCode::Char('d') => self.download_release(false),
                KeyCode::Char('g') | KeyCode::Enter => self.confirm_flash(),
                _ => {}
            },
        }
    }

    /// Baixa o firmware do último release. Pela aba Sistema (ota=true), em
    /// seguida pergunta se manda para a placa pela rede.
    fn download_release(&mut self, ota: bool) {
        if self.downloading {
            return;
        }
        match self.release.clone() {
            Some(Ok(r)) => {
                self.downloading = true;
                self.ota_after_download = ota;
                self.info(format!("baixando o firmware {} do GitHub…", r.tag));
                net::fetch_release(r, self.tx.clone());
            }
            Some(Err(_)) | None => {
                self.info("consultando o GitHub…".into());
                self.release = None;
                net::check_release(self.tx.clone());
            }
        }
    }

    /// "v2.2.1 disponível" / "você já tem a v2.2.1" para mostrar nas abas.
    pub fn release_line(&self) -> String {
        match &self.release {
            None => "GitHub: consultando…".into(),
            Some(Err(e)) => format!("GitHub: {e} (d tenta de novo)"),
            Some(Ok(r)) => {
                let novo = r.tag.trim_start_matches('v');
                let atual = self.admin.as_ref().map(|a| a.system.version.clone()).unwrap_or_default();
                if self.downloading {
                    format!("GitHub: baixando {}…", r.tag)
                } else if !atual.is_empty() && atual == novo {
                    format!("GitHub: {} é a última versão — a placa já está nela", r.tag)
                } else {
                    format!("GitHub: {} disponível{} — d baixa", r.tag, if r.name.is_empty() || r.name == r.tag { String::new() } else { format!(" ({})", r.name) })
                }
            }
        }
    }

    fn ask_path(&mut self, title: &str, label: &str, value: String, act: Act) {
        self.modal = Some(Modal::Input { title: title.into(), label: label.into(), value, secret: false, digits: false, act });
    }

    fn arm(&mut self, mode: &str) {
        self.last_arm_mode = mode.into();
        self.send(json!({"command": "arm", "mode": mode}));
    }

    fn edit_zone(&mut self) {
        let Some(z) = self.admin.as_ref().and_then(|a| a.zones.get(self.zone_sel)).cloned() else { return };
        let tipos = proto::ZONE_TYPES.iter().map(|(v, l)| (v.to_string(), l.to_string())).collect();
        self.modal = Some(Modal::Form {
            title: format!("Zona {}", z.id),
            form: Form::new(vec![
                Field::new("name", "Nome", Kind::Text(23), z.name.clone()).hint(&z.default_name),
                Field::new("type", "Tipo", Kind::Choice(tipos), z.kind.to_string()),
                Field::new("home", "Vigia no modo Em casa", Kind::Bool, z.home.to_string()),
                Field::new("bypass", "Inibida (não dispara)", Kind::Bool, z.bypass.to_string()),
            ]),
            act: Act::SaveZone(z.id),
        });
    }

    fn save_timing(&mut self) {
        let f = &self.timing_form;
        let Some(minute) = f.minutes("aa_time") else {
            self.error("horário inválido: use HH:MM".into());
            return;
        };
        let timing = json!({
            "command": "setTiming",
            "entryDelaySec": f.num("entry"), "exitDelaySec": f.num("exit"), "openAlertMin": f.num("open"),
        });
        let auto = json!({
            "command": "setAutoArm",
            "enabled": f.bool("aa_on"), "minute": minute, "days": f.num("aa_days"),
            "mode": f.get("aa_mode"), "tz": f.get("tz"),
        });
        self.send(timing);
        self.send(auto);
    }

    fn save_siren(&mut self) {
        let f = &self.siren_form;
        let siren = json!({"command": "setSiren", "enabled": f.bool("siren_on"), "timeoutSec": f.num("siren_to")});
        let tg = json!({"command": "setTelegram", "chatId": f.get("chat").trim(), "token": f.get("token").trim()});
        self.send(siren);
        self.send(tg);
    }

    fn confirm_flash(&mut self) {
        let Some(port) = self.ports.get(self.port_sel) else {
            self.error("nenhuma porta serial: conecte a placa pelo USB e aperte r".into());
            return;
        };
        let firmware = match fs::read(&self.fw_path) {
            Ok(b) => b,
            Err(e) => {
                self.error(format!("não deu para ler {}: {e} (f: escolher o arquivo)", self.fw_path));
                return;
            }
        };
        if let Err(e) = flash::check_firmware(&firmware) {
            self.error(e);
            return;
        }
        let mut text = format!("Gravar {} em {}?\n\n{}", self.fw_path, port.name, self.fw_info);
        if self.erase_all {
            text.push_str("\n\n⚠ APAGAR TUDO está ligado: Wi-Fi, senhas, zonas e registro serão apagados.");
        } else {
            text.push_str("\n\nConfiguração e senhas ficam como estão.");
        }
        self.modal = Some(Modal::Confirm { text, act: Act::Flash });
    }

    pub fn refresh_fw_info(&mut self) {
        self.fw_info = match fs::read(&self.fw_path) {
            Err(_) => "arquivo não encontrado".into(),
            Ok(b) => match flash::check_firmware(&b) {
                Ok(Some(v)) => format!("Homeguard v{v}, {} KB", b.len() / 1024),
                Ok(None) => format!("ESP32-C3, {} KB, sem marca do Homeguard (versão antiga ou outro projeto)", b.len() / 1024),
                Err(e) => e,
            },
        };
    }

    // ------------------------------------------------------------ janelas

    fn modal_key(&mut self, k: KeyEvent) {
        let Some(mut modal) = self.modal.take() else { return };
        let mut close = false;
        let mut done: Option<(Act, String, Option<Form>)> = None;
        match &mut modal {
            Modal::Connect { sel } => {
                let n = self.boards.len() + 2;
                match k.code {
                    KeyCode::Esc => close = !self.host.is_empty() || self.tab == Tab::Usb,
                    KeyCode::Char('q') if self.host.is_empty() => self.quit = true,
                    KeyCode::Char('u') => {
                        self.tab = Tab::Usb;
                        close = true;
                    }
                    KeyCode::Up => *sel = sel.saturating_sub(1),
                    KeyCode::Down => *sel = (*sel + 1).min(n - 1),
                    KeyCode::Char('r') => self.search(),
                    KeyCode::Enter => {
                        if let Some(b) = self.boards.get(*sel) {
                            let addr = b.addr.clone();
                            self.connect(addr);
                            close = true;
                        } else if *sel == self.boards.len() + 1 {
                            modal = Modal::Input {
                                title: "Varrer outra rede".into(),
                                label: "Rede /24 (ex.: 10.55.9.0/24)".into(),
                                value: String::new(),
                                secret: false,
                                digits: false,
                                act: Act::ScanNetwork,
                            };
                        } else {
                            modal = Modal::Input {
                                title: "Conectar".into(),
                                label: "Endereço (IP ou nome .local)".into(),
                                value: self.host.clone(),
                                secret: false,
                                digits: false,
                                act: Act::Connect,
                            };
                        }
                    }
                    _ => {}
                }
            }
            Modal::Input { value, digits, act, .. } => match k.code {
                KeyCode::Esc => close = true,
                KeyCode::Enter => done = Some((act.clone(), value.clone(), None)),
                KeyCode::Backspace => {
                    value.pop();
                }
                KeyCode::Char(c) if !*digits || c.is_ascii_digit() => value.push(c),
                _ => {}
            },
            Modal::Confirm { act, .. } => match k.code {
                KeyCode::Char('s') | KeyCode::Char('S') | KeyCode::Char('y') | KeyCode::Enter => {
                    done = Some((act.clone(), String::new(), None))
                }
                KeyCode::Char('n') | KeyCode::Char('N') | KeyCode::Esc => close = true,
                _ => {}
            },
            Modal::Form { form, act, .. } => {
                if form.editing {
                    form.key(k);
                } else {
                    match k.code {
                        KeyCode::Esc => close = true,
                        KeyCode::Char('s') => done = Some((act.clone(), String::new(), Some(form.clone()))),
                        _ => {
                            form.key(k);
                        }
                    }
                }
            }
            Modal::Wifi { nets, sel } => match k.code {
                KeyCode::Esc => close = true,
                KeyCode::Up => *sel = sel.saturating_sub(1),
                KeyCode::Down => *sel = (*sel + 1).min(nets.len().saturating_sub(1)),
                KeyCode::Enter => {
                    if let Some(n) = nets.get(*sel) {
                        let ssid = n.ssid.clone();
                        if let Some(f) = self.net_form.fields.iter_mut().find(|f| f.key == "ssid") {
                            f.value = ssid;
                            self.net_form.dirty = true;
                        }
                    }
                    close = true;
                }
                _ => {}
            },
        }
        if let Some((act, input, form)) = done {
            // A ação pode abrir outra janela (ex.: confirmação depois de escolher o arquivo).
            self.run(act, input, form);
            return;
        }
        if !close {
            self.modal = Some(modal);
        }
    }

    fn run(&mut self, act: Act, input: String, form: Option<Form>) {
        let input = input.trim().to_string();
        match act {
            Act::Connect => self.connect(input),
            Act::ScanNetwork => match net::parse_network(&input) {
                None => {
                    self.error(format!("\"{input}\" não é uma rede: use algo como 10.55.9.0/24"));
                    self.modal = Some(Modal::Connect { sel: 0 });
                }
                Some(n) => {
                    if !self.scan_nets.contains(&n) {
                        self.scan_nets.push(n);
                    }
                    self.info(format!("varrendo {n}/24…"));
                    self.search();
                    self.modal = Some(Modal::Connect { sel: 0 });
                }
            },
            Act::AdminLogin => self.send(json!({"command": "adminLogin", "code": input})),
            Act::Disarm => self.send(json!({"command": "disarm", "code": input})),
            Act::ArmForce(mode) => self.send(json!({"command": "arm", "mode": mode, "force": true})),
            Act::SaveZone(id) => {
                let f = form.unwrap_or_default();
                if f.get("name").trim().is_empty() {
                    self.error("a zona precisa de um nome".into());
                    return;
                }
                self.send(json!({
                    "command": "setZone", "id": id, "name": f.get("name").trim(),
                    "type": f.num("type"), "home": f.bool("home"), "bypass": f.bool("bypass"),
                }));
            }
            Act::AddCode => {
                let f = form.unwrap_or_default();
                let code = f.get("code");
                if f.get("label").trim().is_empty() || !(4..=8).contains(&code.len()) {
                    self.error("informe o nome e uma senha de 4 a 8 números".into());
                    return;
                }
                self.send(json!({"command": "addCode", "label": f.get("label").trim(), "code": code, "duress": f.bool("duress")}));
            }
            Act::RemoveCode(i) => self.send(json!({"command": "removeCode", "index": i})),
            Act::SaveNetwork => {
                let f = &self.net_form;
                let mut cmd = json!({"command": "setNetwork"});
                for k in ["ssid", "pass", "hostname", "deviceName", "mqttId", "mqttHost", "mqttUser", "mqttPass", "ipMode", "ip", "gateway", "subnet", "dns"] {
                    cmd[k] = json!(f.get(k));
                }
                cmd["mqttPort"] = json!(f.num("mqttPort"));
                self.send(cmd);
            }
            Act::ChangeMaster => {
                let f = form.unwrap_or_default();
                let (cur, new, conf) = (f.get("cur"), f.get("new"), f.get("conf"));
                if new != conf {
                    self.error("as duas senhas novas não batem".into());
                } else if !(4..=8).contains(&new.len()) || !new.chars().all(|c| c.is_ascii_digit()) {
                    self.error("a senha mestre precisa ter de 4 a 8 números".into());
                } else {
                    self.send(json!({"command": "setMaster", "current": cur, "newCode": new}));
                }
            }
            Act::Reboot => self.send(json!({"command": "reboot"})),
            Act::BackupPath => {
                if input.is_empty() {
                    return;
                }
                self.backup_path = Some(input);
                self.send(json!({"command": "getBackup"}));
            }
            Act::RestorePath => match fs::read_to_string(&input) {
                Err(e) => self.error(format!("não deu para ler {input}: {e}")),
                Ok(text) => match serde_json::from_str::<Value>(&text) {
                    Ok(v) if v["homeguardBackup"].as_i64() == Some(1) => {
                        let from = v["device"].as_str().unwrap_or("?").to_string();
                        self.modal = Some(Modal::Confirm {
                            text: format!(
                                "Restaurar o backup de \"{from}\"?\n\nZonas, senhas, senha mestre, tempos, agenda e Telegram desta placa serão substituídos."
                            ),
                            act: Act::Restore(text),
                        });
                    }
                    _ => self.error(format!("{input} não é um backup do Homeguard")),
                },
            },
            Act::Restore(text) => {
                let Some(a) = &self.admin else { return };
                net::restore(self.host.clone(), a.token.clone(), text, self.tx.clone());
                self.info("enviando o backup…".into());
            }
            Act::OtaPath => match fs::read(&input) {
                Err(e) => self.error(format!("não deu para ler {input}: {e}")),
                Ok(bin) => match flash::check_firmware(&bin) {
                    Err(e) => self.error(e),
                    Ok(ver) => {
                        self.fw_path = input.clone();
                        self.refresh_fw_info();
                        let atual = self.admin.as_ref().map(|a| a.system.version.clone()).unwrap_or_default();
                        let nova = ver.map(|v| format!("v{v}")).unwrap_or_else(|| "versão desconhecida".into());
                        self.modal = Some(Modal::Confirm {
                            text: format!(
                                "Atualizar de v{atual} para {nova} ({} KB)?\n\nA placa reinicia no fim (uns 10 s sem alarme). Se a versão nova não subir, ela volta sozinha para a atual.",
                                bin.len() / 1024
                            ),
                            act: Act::Ota(bin),
                        });
                    }
                },
            },
            Act::Ota(bin) => {
                let Some(a) = &self.admin else { return };
                self.ota = Some((0, bin.len() as u64));
                net::ota(self.host.clone(), a.token.clone(), bin, self.tx.clone());
            }
            Act::FlashPath => {
                if !input.is_empty() {
                    self.fw_path = input;
                    self.refresh_fw_info();
                }
            }
            Act::Flash => {
                let Some(port) = self.ports.get(self.port_sel).cloned() else { return };
                let Ok(bin) = fs::read(&self.fw_path) else { return };
                // A placa vai reiniciar: a conexão de rede cai e volta sozinha.
                self.flashing = true;
                self.flash_log.clear();
                self.flash_prog = None;
                flash::flash(port, bin, self.erase_all, self.tx.clone());
            }
        }
    }

    /// Segundos restantes de uma contagem, descontando o tempo desde o último estado.
    pub fn left(&self, secs: u32) -> u32 {
        secs.saturating_sub(self.state_at.elapsed().as_secs() as u32)
    }
}

fn or(a: &str, b: &str) -> String {
    if a.is_empty() { b.to_string() } else { a.to_string() }
}

fn tz_options(current: &str) -> Vec<(String, String)> {
    let mut v: Vec<(String, String)> = proto::TIMEZONES.iter().map(|(t, l)| (t.to_string(), l.to_string())).collect();
    if !v.iter().any(|o| o.0 == current) && !current.is_empty() {
        v.push((current.to_string(), current.to_string()));
    }
    v
}

/// Onde costuma estar o firmware: ao lado do programa ou na pasta atual.
fn default_firmware_path() -> String {
    for p in ["homeguard.ino.bin", "build/homeguard.ino.bin"] {
        if std::path::Path::new(p).exists() {
            return p.into();
        }
    }
    "homeguard.ino.bin".into()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossterm::event::{KeyCode, KeyEvent};

    fn admin_json() -> Value {
        json!({"type":"admin","token":"x","codes":[],"zones":[],
               "timing":{"entryDelaySec":30,"exitDelaySec":60,"openAlertMin":0},
               "autoArm":{"enabled":false,"minute":1380,"days":127,"mode":"away","tz":"<-03>3"},
               "telegram":{"chatId":"","hasToken":false},"network":{"ssid":"x","current":{}},
               "siren":{"enabled":true,"timeoutSec":180},"system":{"version":"2.2.1"}})
    }

    fn press(app: &mut App, keys: &[KeyCode]) {
        for k in keys {
            app.key(KeyEvent::from(*k));
        }
    }

    /// O caminho de quem usa: escolhe a aba e vai até o horário.
    #[test]
    fn editar_horario_pelo_menu() {
        let mut app = App::new(Some("127.0.0.1".into()));
        app.modal = None;
        app.on_message(admin_json());
        // Pelo número da aba.
        press(&mut app, &[KeyCode::Char('4')]);
        let tab_apos_4 = app.tab == Tab::Tempos;
        press(&mut app, &[KeyCode::Down, KeyCode::Down, KeyCode::Down, KeyCode::Down]);
        let sel = app.timing_form.sel;
        press(&mut app, &[KeyCode::Char('2'), KeyCode::Char('2'), KeyCode::Char('3'), KeyCode::Char('0'), KeyCode::Enter]);
        println!("aba após '4' é Tempos: {tab_apos_4}; campo após 4×↓: {sel}; aba agora: {:?}; horário: {:?}; foco no menu: {}",
                 TABS.iter().find(|t| t.0 == app.tab).map(|t| t.1), app.timing_form.get("aa_time"), app.menu_focus);
        assert_eq!(app.tab, Tab::Tempos);
        assert_eq!(app.timing_form.get("aa_time"), "22:30");
    }

    /// Texto: Enter edita, letras entram (inclusive 's', que fora da edição é
    /// "salvar"), Enter termina; depois o 's' abre a confirmação de salvar.
    #[test]
    fn editar_texto_na_rede() {
        let mut app = App::new(Some("127.0.0.1".into()));
        app.modal = None;
        app.on_message(admin_json());
        press(&mut app, &[KeyCode::Char('6'), KeyCode::Enter, KeyCode::Backspace]);
        for c in "Casa 2s".chars() {
            press(&mut app, &[KeyCode::Char(c)]);
        }
        press(&mut app, &[KeyCode::Enter]);
        assert_eq!(app.net_form.get("ssid"), "Casa 2s");
        press(&mut app, &[KeyCode::Char('s')]);
        assert!(matches!(app.modal, Some(Modal::Confirm { .. })));
    }

    /// Pelo menu: ↓ até a aba, Enter entra, e aí ↓ anda nos campos.
    #[test]
    fn editar_horario_pelas_setas() {
        let mut app = App::new(Some("127.0.0.1".into()));
        app.modal = None;
        app.on_message(admin_json());
        press(&mut app, &[KeyCode::Down, KeyCode::Down, KeyCode::Down, KeyCode::Enter]);
        assert_eq!(app.tab, Tab::Tempos);
        assert!(!app.menu_focus);
        press(&mut app, &[KeyCode::Down, KeyCode::Down, KeyCode::Down, KeyCode::Down]);
        press(&mut app, &[KeyCode::Char('0'), KeyCode::Char('6'), KeyCode::Char('4'), KeyCode::Char('5'), KeyCode::Enter]);
        assert_eq!(app.tab, Tab::Tempos);
        assert_eq!(app.timing_form.get("aa_time"), "06:45");
        // Esc volta ao menu, e lá ↓ troca de aba.
        press(&mut app, &[KeyCode::Esc, KeyCode::Down]);
        assert_eq!(app.tab, Tab::Sirene);
    }
}
