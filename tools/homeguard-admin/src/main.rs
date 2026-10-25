//! homeguard-admin — administração do alarme ESP32 Homeguard pelo terminal.
//!
//!   homeguard-admin                 procura as placas na rede
//!   homeguard-admin 192.168.1.18    conecta direto
//!   homeguard-admin --usb           abre direto na gravação pelo cabo
//!   homeguard-admin --scan [rede]   só lista as placas encontradas
//!   homeguard-admin --flash <arquivo.bin> [porta]   grava pelo cabo, sem a interface

mod app;
mod cli;
mod flash;
mod form;
mod net;
mod proto;
mod ui;

use std::time::Duration;

use crossterm::event::{self, Event, KeyEventKind};

fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "-h" || a == "--help") {
        println!("homeguard-admin {}\n", env!("CARGO_PKG_VERSION"));
        println!("Uso:\n  homeguard-admin                procura as placas na rede");
        println!("  homeguard-admin <endereço>     conecta direto (IP ou nome .local)");
        println!("  homeguard-admin --usb          abre na gravação pelo cabo USB");
        println!("  homeguard-admin --scan [rede]  lista as placas (rede extra: ex. 10.55.9.0/24)");
        println!("  homeguard-admin --download     baixa o firmware do último release no GitHub");
        println!("  homeguard-admin --flash <arquivo.bin> [porta]");
        println!("                                 grava pelo cabo sem a interface (configuração fica)");
        return Ok(());
    }
    if let Some(i) = args.iter().position(|a| a == "--scan") {
        return cli::scan(args.get(i + 1).map(String::as_str));
    }
    if args.iter().any(|a| a == "--download") {
        return cli::download();
    }
    if let Some(i) = args.iter().position(|a| a == "--flash") {
        let Some(bin) = args.get(i + 1) else { anyhow::bail!("--flash precisa do arquivo .bin") };
        return cli::flash(bin, args.get(i + 2).map(String::as_str));
    }
    let usb = args.iter().any(|a| a == "--usb");
    let host = args.iter().find(|a| !a.starts_with('-')).cloned();

    let mut app = app::App::new(host);
    if usb {
        app.go_usb();
    }

    let mut terminal = ratatui::init();
    let result = (|| -> anyhow::Result<()> {
        while !app.quit {
            app.tick();
            terminal.draw(|f| ui::draw(f, &app))?;
            if event::poll(Duration::from_millis(100))? {
                if let Event::Key(k) = event::read()? {
                    if k.kind == KeyEventKind::Press {
                        app.key(k);
                    }
                }
            }
        }
        Ok(())
    })();
    ratatui::restore();
    result
}

#[cfg(test)]
mod tests {
    use ratatui::backend::TestBackend;
    use ratatui::Terminal;

    /// Desenha a tela de verdade num buffer (sem terminal), conectado à placa
    /// de HG_TEST_HOST. `HG_TEST_HOST=10.55.9.17 cargo test -- --ignored --nocapture`
    #[test]
    #[ignore]
    fn screenshot() {
        let host = std::env::var("HG_TEST_HOST").expect("HG_TEST_HOST");
        let mut app = crate::app::App::new(Some(host));
        let t0 = std::time::Instant::now();
        while t0.elapsed().as_secs() < 3 {
            app.tick();
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        use crossterm::event::{KeyCode, KeyEvent};
        // Painel; ↓ três vezes (Tempos, sem admin); ↑↑ volta a Zonas; 9 + → entra no USB.
        let steps: [(&str, Vec<KeyCode>); 3] = [
            ("painel", vec![]),
            ("seta ↓↓↓ e ↑↑", vec![KeyCode::Down, KeyCode::Down, KeyCode::Down, KeyCode::Up, KeyCode::Up]),
            ("9 e →", vec![KeyCode::Char('9'), KeyCode::Right]),
        ];
        for (tab, keys) in steps {
            for k in keys {
                app.key(KeyEvent::from(k));
            }
            app.tick();
            let mut term = Terminal::new(TestBackend::new(110, 30)).unwrap();
            term.draw(|f| crate::ui::draw(f, &app)).unwrap();
            let buf = term.backend().buffer().clone();
            println!("===== aba {tab}");
            for y in 0..buf.area.height {
                let line: String = (0..buf.area.width).map(|x| buf[(x, y)].symbol().to_string()).collect();
                println!("{}", line.trim_end());
            }
        }
    }
}
