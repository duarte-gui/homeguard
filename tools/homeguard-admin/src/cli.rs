//! Modo linha de comando (sem a interface): para scripts e para gravar rápido.

use std::sync::mpsc;
use std::time::Duration;

use crate::flash::{self, FlashEvent};
use crate::net::{self, NetEvent};

pub fn scan(extra: Option<&str>) -> anyhow::Result<()> {
    let mut nets = Vec::new();
    if let Some(e) = extra {
        nets.push(net::parse_network(e).ok_or_else(|| anyhow::anyhow!("rede inválida: {e} (use algo como 10.55.9.0/24)"))?);
    }
    eprintln!("procurando (mDNS e varredura das redes locais)…");
    let (tx, rx) = mpsc::channel();
    net::discover(nets, tx);
    while let Ok(ev) = rx.recv() {
        if let NetEvent::Found(boards) = ev {
            if boards.is_empty() {
                println!("nenhuma placa encontrada");
            }
            for b in boards {
                println!("{:<16} {:<24} {}", b.addr, b.name, b.detail);
            }
            break;
        }
    }
    Ok(())
}

pub fn flash(path: &str, port: Option<&str>) -> anyhow::Result<()> {
    let bin = std::fs::read(path)?;
    match flash::check_firmware(&bin).map_err(|e| anyhow::anyhow!(e))? {
        Some(v) => println!("{path}: Homeguard v{v}, {} KB", bin.len() / 1024),
        None => println!("{path}: ESP32-C3, {} KB (sem a marca do Homeguard)", bin.len() / 1024),
    }
    let ports = flash::list_ports();
    let port = match port {
        Some(p) => ports.into_iter().find(|x| x.name == p).ok_or_else(|| anyhow::anyhow!("porta {p} não encontrada"))?,
        None => ports.into_iter().next().ok_or_else(|| anyhow::anyhow!("nenhuma porta serial: a placa está no USB?"))?,
    };
    println!("porta: {} ({})", port.name, port.desc);
    let (tx, rx) = mpsc::channel();
    flash::flash(port, bin, false, tx);
    let mut last = ("", 101);
    loop {
        match rx.recv_timeout(Duration::from_secs(120)) {
            Ok(NetEvent::Flash(FlashEvent::Log(s))) => println!("{s}"),
            Ok(NetEvent::Flash(FlashEvent::Progress { part, current, total })) if total > 0 => {
                let pct = current * 100 / total;
                if part != last.0 || pct / 10 != last.1 / 10 {
                    println!("  {part}: {pct}%");
                    last = (part, pct);
                }
            }
            Ok(NetEvent::Flash(FlashEvent::Done(r))) => {
                return match r {
                    Ok(()) => {
                        println!("gravado e reiniciado");
                        Ok(())
                    }
                    Err(e) => anyhow::bail!(e),
                };
            }
            Ok(_) => {}
            Err(_) => anyhow::bail!("a gravação parou de responder"),
        }
    }
}

pub fn download() -> anyhow::Result<()> {
    let r = net::latest_release().map_err(|e| anyhow::anyhow!(e))?;
    println!("último release: {} {}", r.tag, r.page);
    let path = net::download_release(&r).map_err(|e| anyhow::anyhow!(e))?;
    println!("{path}");
    Ok(())
}
