//! Gravação pelo cabo USB (o que o flash.sh faz, sem Python nem esptool).
//!
//! Bootloader, tabela de partições e boot_app0 vêm embutidos neste programa
//! (assets/, copiados do build do firmware): são fixos para o projeto. Só o
//! firmware (homeguard.ino.bin) é escolhido na hora — o mesmo arquivo do OTA.
//!
//! A NVS (0x9000) não é tocada: configuração e senhas ficam, a não ser que se
//! peça para apagar tudo.

use std::sync::mpsc::Sender;
use std::thread;

use espflash::connection::{Connection, ResetAfterOperation, ResetBeforeOperation};
use espflash::flasher::Flasher;
use espflash::image_format::Segment;
use espflash::target::{Chip, ProgressCallbacks};
use serialport::{SerialPortType, UsbPortInfo};

use crate::net::NetEvent;

const BOOTLOADER: &[u8] = include_bytes!("../assets/bootloader.bin");
const PARTITIONS: &[u8] = include_bytes!("../assets/partitions.bin");
const BOOT_APP0: &[u8] = include_bytes!("../assets/boot_app0.bin");

/// chip_id no cabeçalho de imagem do ESP-IDF (esp_image_header_t, offset 12).
const ESP32C3_CHIP_ID: u16 = 5;
/// Slot de app do min_spiffs: 1,9 MB.
const APP_SLOT: usize = 0x1E_0000;

pub enum FlashEvent {
    Log(String),
    Progress { part: &'static str, current: usize, total: usize },
    Done(Result<(), String>),
}

#[derive(Clone)]
pub struct Port {
    pub name: String,
    pub desc: String,
    info: Option<UsbPortInfo>,
}

impl Port {
    /// USB nativo da Espressif (C3/S3/C6): é a placa, não um adaptador qualquer.
    pub fn is_esp(&self) -> bool {
        self.info.as_ref().map(|i| i.vid == 0x303a).unwrap_or(false)
    }
    /// No USB nativo, o número de série é o MAC do chip.
    pub fn mac(&self) -> Option<String> {
        self.info.as_ref().filter(|i| i.vid == 0x303a).and_then(|i| i.serial_number.clone())
    }
}

pub fn list_ports() -> Vec<Port> {
    let mut ports: Vec<Port> = serialport::available_ports()
        .unwrap_or_default()
        .into_iter()
        .filter_map(|p| match p.port_type {
            SerialPortType::UsbPort(info) => {
                let desc = match (info.vid, info.pid) {
                    (0x303a, 0x1001) => "ESP32-C3/S3 (USB nativo)".to_string(),
                    _ => format!(
                        "{} {:04x}:{:04x}",
                        info.product.clone().unwrap_or_else(|| "USB".into()),
                        info.vid,
                        info.pid
                    ),
                };
                Some(Port { name: p.port_name, desc, info: Some(info) })
            }
            SerialPortType::Unknown if p.port_name.contains("ttyACM") || p.port_name.contains("ttyUSB") => {
                Some(Port { name: p.port_name, desc: "serial".into(), info: None })
            }
            _ => None,
        })
        .collect();
    // A placa (USB nativo da Espressif) primeiro.
    ports.sort_by_key(|p| (p.info.as_ref().map(|i| i.vid != 0x303a).unwrap_or(true), p.name.clone()));
    ports
}

const MARKER: &[u8] = b"HOMEGUARD_FW_VERSION=";

/// Confere se o arquivo é um firmware de ESP32-C3 que cabe no slot. Devolve a
/// versão do Homeguard (FW_MARKER no .ino), ou None se a marca não estiver lá
/// (firmware de antes da 2.2.0, ou de outro projeto).
pub fn check_firmware(bin: &[u8]) -> Result<Option<String>, String> {
    if bin.len() < 0x100 || bin[0] != 0xE9 {
        return Err("o arquivo não é um firmware de ESP32 (falta o cabeçalho 0xE9)".into());
    }
    let chip = u16::from_le_bytes([bin[12], bin[13]]);
    if chip != ESP32C3_CHIP_ID {
        return Err(format!("o firmware é de outro chip (chip_id {chip}, esperado {ESP32C3_CHIP_ID} = ESP32-C3)"));
    }
    if bin.len() > APP_SLOT {
        return Err(format!("o firmware tem {} KB e não cabe no slot de {} KB", bin.len() / 1024, APP_SLOT / 1024));
    }
    let ver = bin.windows(MARKER.len()).position(|w| w == MARKER).map(|i| {
        let rest = &bin[i + MARKER.len()..];
        let end = rest.iter().position(|&b| b == 0).unwrap_or(0).min(32);
        String::from_utf8_lossy(&rest[..end]).to_string()
    });
    Ok(ver)
}

/// Nomes na ordem em que as partes são gravadas (as que já estão iguais na
/// placa são puladas sem passar pelo init, então o nome vem da posição).
const PARTS: [&str; 4] = ["bootloader", "partições", "boot_app0", "firmware"];

struct Progress {
    part: &'static str,
    idx: usize,
    total: usize,
    events: Sender<NetEvent>,
}

impl ProgressCallbacks for Progress {
    fn init(&mut self, addr: u32, total: usize) {
        self.total = total;
        self.part = match addr {
            0x0 => PARTS[0],
            0x8000 => PARTS[1],
            0xe000 => PARTS[2],
            _ => PARTS[3],
        };
        self.send(0);
    }
    fn update(&mut self, current: usize) {
        self.send(current);
    }
    fn verifying(&mut self) {
        let _ = self.events.send(NetEvent::Flash(FlashEvent::Log(format!("conferindo {}…", self.part))));
    }
    fn finish(&mut self, skipped: bool) {
        let part = PARTS.get(self.idx).copied().unwrap_or("?");
        self.idx += 1;
        if skipped {
            let _ = self.events.send(NetEvent::Flash(FlashEvent::Log(format!("{part}: igual ao da placa, pulado"))));
        } else {
            self.send(self.total);
        }
    }
}

impl Progress {
    fn send(&self, current: usize) {
        let _ = self.events.send(NetEvent::Flash(FlashEvent::Progress { part: self.part, current, total: self.total }));
    }
}

pub fn flash(port: Port, firmware: Vec<u8>, erase_all: bool, events: Sender<NetEvent>) {
    thread::spawn(move || {
        let r = run(&port, &firmware, erase_all, &events);
        let _ = events.send(NetEvent::Flash(FlashEvent::Done(r)));
    });
}

fn run(port: &Port, firmware: &[u8], erase_all: bool, events: &Sender<NetEvent>) -> Result<(), String> {
    let log = |s: String| {
        let _ = events.send(NetEvent::Flash(FlashEvent::Log(s)));
    };
    log(format!("abrindo {}…", port.name));
    let serial = serialport::new(&port.name, 115_200)
        .flow_control(serialport::FlowControl::None)
        .open_native()
        .map_err(|e| format!("não abriu {}: {e} (permissão? grupo dialout/uucp)", port.name))?;
    // Sem a informação do USB, assume o USB nativo do C3 (é o que escolhe o
    // jeito certo de resetar a placa para o modo de gravação).
    let info = port.info.clone().unwrap_or(UsbPortInfo {
        vid: 0x303a,
        pid: 0x1001,
        serial_number: None,
        manufacturer: None,
        product: None,
    });
    let conn = Connection::new(serial, info, ResetAfterOperation::HardReset, ResetBeforeOperation::DefaultReset, 115_200);
    log("conectando no bootloader da placa…".into());
    let mut flasher = Flasher::connect(conn, true, true, true, None, Some(460_800))
        .map_err(|e| format!("a placa não entrou no modo de gravação: {e}"))?;
    let chip = flasher.chip();
    if chip != Chip::Esp32c3 {
        return Err(format!("a placa é um {chip}, não um ESP32-C3"));
    }
    if let Ok(info) = flasher.device_info() {
        log(format!("{} rev {:?}, flash {}", info.chip, info.revision.unwrap_or_default(), info.flash_size));
    }
    if erase_all {
        log("apagando a flash inteira (configuração e senhas também)…".into());
        flasher.erase_flash().map_err(|e| format!("falha ao apagar: {e}"))?;
    }
    let segments = [
        Segment::new(0x0, BOOTLOADER),
        Segment::new(0x8000, PARTITIONS),
        Segment::new(0xe000, BOOT_APP0),
        Segment::new(0x1_0000, firmware),
    ];
    let mut progress = Progress { part: "", idx: 0, total: 0, events: events.clone() };
    flasher
        .write_bins_to_flash(&segments, &mut progress)
        .map_err(|e| format!("falha na gravação: {e}"))?;
    log("reiniciando a placa…".into());
    flasher
        .connection()
        .reset_after(true, chip)
        .map_err(|e| format!("gravou, mas não reiniciou sozinha (aperte RST): {e}"))?;
    Ok(())
}
