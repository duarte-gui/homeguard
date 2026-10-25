//! O que a placa manda pelo WebSocket. Mesmos nomes do firmware (homeguard.ino:
//! getFullStateJson e getAdminJson); campos que faltarem viram o padrão.

use serde::Deserialize;

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct State {
    pub is_armed: bool,
    pub is_siren_active: bool,
    pub is_triggered: bool,
    pub triggering_sector_id: u32,
    pub mode: String,
    pub exit_remaining: u32,
    pub entry_remaining: u32,
    pub sectors: Vec<Sector>,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Sector {
    pub id: u32,
    pub name: String,
    pub status: String,
    pub bypass: bool,
    pub temp_bypass: bool,
    pub is24h: bool,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Admin {
    pub token: String,
    pub codes: Vec<Code>,
    pub zones: Vec<Zone>,
    pub timing: Timing,
    pub auto_arm: AutoArm,
    pub telegram: Telegram,
    pub network: Network,
    pub siren: Siren,
    pub system: System,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Code {
    pub label: String,
    pub digits: u32,
    pub duress: bool,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Zone {
    pub id: u32,
    pub name: String,
    pub default_name: String,
    pub bypass: bool,
    #[serde(rename = "type")]
    pub kind: u8,
    pub home: bool,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Timing {
    pub entry_delay_sec: u32,
    pub exit_delay_sec: u32,
    pub open_alert_min: u32,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct AutoArm {
    pub enabled: bool,
    pub minute: u32,
    pub days: u8,
    pub mode: String,
    pub tz: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Telegram {
    pub chat_id: String,
    pub has_token: bool,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Network {
    pub ssid: String,
    pub hostname: String,
    pub default_hostname: String,
    pub mqtt_id: String,
    pub default_mqtt_id: String,
    pub device_name: String,
    pub default_device_name: String,
    pub mqtt_host: String,
    pub mqtt_port: u32,
    pub mqtt_user: String,
    pub has_mqtt_pass: bool,
    pub ip_mode: String,
    pub ip: String,
    pub gateway: String,
    pub subnet: String,
    pub dns: String,
    pub current: CurrentIp,
}

/// O que a placa está usando agora (do DHCP ou do IP fixo).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct CurrentIp {
    pub ip: String,
    pub gateway: String,
    pub subnet: String,
    pub dns: String,
    pub mac: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Siren {
    pub enabled: bool,
    pub timeout_sec: u32,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct System {
    pub version: String,
    pub ip: String,
    pub rssi: i32,
    pub uptime_s: u64,
    pub free_heap: u64,
    pub mqtt: bool,
    pub clock: u64,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Event {
    pub t: u64,
    pub m: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct WifiNet {
    pub ssid: String,
    pub rssi: i32,
    pub open: bool,
}

pub const ZONE_TYPES: [(&str, &str); 4] = [
    ("0", "Instantânea"),
    ("1", "Entrada/saída"),
    ("2", "Seguidora"),
    ("3", "24 horas"),
];

pub fn zone_type_label(t: u8) -> &'static str {
    ZONE_TYPES.get(t as usize).map(|z| z.1).unwrap_or("?")
}

pub fn mode_label(m: &str) -> &'static str {
    if m == "home" { "Em casa" } else { "Ausente" }
}

pub const TIMEZONES: [(&str, &str); 4] = [
    ("<-02>2", "Fernando de Noronha (UTC-2)"),
    ("<-03>3", "Brasília (UTC-3)"),
    ("<-04>4", "Amazonas, MT, MS, RO, RR (UTC-4)"),
    ("<-05>5", "Acre (UTC-5)"),
];
