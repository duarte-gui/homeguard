//! Desenho da tela. Só lê o App; quem muda o estado é app.rs.

use chrono::{DateTime, Local};
use ratatui::layout::{Alignment, Constraint, Flex, Layout, Rect};
use ratatui::style::{Color, Modifier, Style, Stylize};
use ratatui::text::{Line, Span, Text};
use ratatui::widgets::{Block, Cell, Clear, Gauge, List, ListItem, ListState, Paragraph, Row, Table, TableState, Wrap};
use ratatui::Frame;

use crate::app::{App, Modal, Tab, TABS};
use crate::proto;

pub fn draw(f: &mut Frame, app: &App) {
    let keys = shortcuts(app);
    let [header, main, help, status] = Layout::vertical([
        Constraint::Length(1),
        Constraint::Min(5),
        Constraint::Length(help_height(&keys, f.area().width)),
        Constraint::Length(1),
    ])
    .areas(f.area());
    let [menu, body] = Layout::horizontal([Constraint::Length(20), Constraint::Min(30)]).areas(main);

    draw_header(f, app, header);
    draw_menu(f, app, menu);
    if app.tab.needs_admin() && app.admin.is_none() {
        draw_locked(f, app, body);
    } else {
        match app.tab {
            Tab::Painel => draw_panel(f, app, body),
            Tab::Zonas => draw_zones(f, app, body),
            Tab::Senhas => draw_codes(f, app, body),
            Tab::Tempos => app.timing_form.render(f, body, "Tempos e arme automático", content_focus(app)),
            Tab::Sirene => app.siren_form.render(f, body, "Sirene e Telegram", content_focus(app)),
            Tab::Rede => {
                let now = app.admin.as_ref().map(|a| {
                    let c = &a.network.current;
                    format!("Rede — agora {} ({}), MAC {} — salvar reinicia a placa", c.ip, if a.network.ip_mode == "static" { "fixo" } else { "DHCP" }, c.mac)
                });
                app.net_form.render(f, body, now.as_deref().unwrap_or("Rede"), content_focus(app))
            }
            Tab::Eventos => draw_events(f, app, body),
            Tab::Sistema => draw_system(f, app, body),
            Tab::Usb => draw_usb(f, app, body),
        }
    }
    draw_help(f, &keys, help);
    draw_status(f, app, status);
    if let Some(m) = &app.modal {
        draw_modal(f, app, m);
    }
}

fn draw_header(f: &mut Frame, app: &App, area: Rect) {
    let mut spans = vec![Span::styled(" Homeguard Admin ", Style::new().black().on_cyan().bold()), Span::raw(" ")];
    if app.host.is_empty() {
        spans.push(Span::styled("sem placa (c: conectar)", Style::new().dark_gray()));
    } else if app.connected {
        spans.push(Span::styled("● ", Style::new().green()));
        let name = app.admin.as_ref().map(|a| a.network.device_name.clone()).unwrap_or_default();
        spans.push(Span::raw(if name.is_empty() { app.host.clone() } else { format!("{name} ({})", app.host) }));
    } else {
        spans.push(Span::styled("● ", Style::new().red()));
        spans.push(Span::raw(format!("{} — reconectando…", app.host)));
    }
    spans.push(Span::raw("   "));
    if app.admin.is_some() {
        spans.push(Span::styled("admin ✓", Style::new().green()));
    } else if app.connected {
        spans.push(Span::styled("L: admin", Style::new().dark_gray()));
    }
    f.render_widget(Line::from(spans), area);

    // Canto direito: placa no cabo USB.
    let usb = match app.usb_esp() {
        Some(p) if app.flashing => Line::from(vec![Span::styled("USB ● ", Style::new().yellow()), Span::raw(format!("gravando {} ", p.name))]),
        Some(p) => Line::from(vec![Span::styled("USB ● ", Style::new().green()), Span::raw(format!("ESP32 conectado ({}) ", p.name))]),
        None if app.flashing => Line::from(vec![Span::styled("USB ● ", Style::new().yellow()), Span::raw("gravando ")]),
        None => Line::styled("USB ○ sem ESP32 ", Style::new().dark_gray()),
    };
    f.render_widget(usb.alignment(Alignment::Right), area);
}

fn content_focus(app: &App) -> bool {
    app.modal.is_none() && !app.menu_focus
}

fn draw_menu(f: &mut Frame, app: &App, area: Rect) {
    let focused = app.menu_focus && app.modal.is_none();
    let items: Vec<ListItem> = TABS
        .iter()
        .enumerate()
        .map(|(i, (t, name))| {
            let locked = t.needs_admin() && app.admin.is_none();
            let style = if locked { Style::new().dark_gray() } else { Style::new() };
            ListItem::new(Line::from(vec![
                Span::styled(format!("{} ", i + 1), Style::new().cyan()),
                Span::styled(name.to_string(), style),
            ]))
        })
        .collect();
    let sel = TABS.iter().position(|t| t.0 == app.tab).unwrap_or(0);
    let mut st = ListState::default().with_selected(Some(sel));
    f.render_stateful_widget(
        List::new(items)
            .block(Block::bordered().title(" Menu ").border_style(if focused { Style::new().cyan() } else { Style::new().dark_gray() }))
            // Com o foco dentro da aba, a aba atual fica marcada, mas apagada.
            .highlight_style(if focused { Style::new().black().on_cyan().bold() } else { Style::new().cyan().bold() })
            .highlight_symbol("▶"),
        area,
        &mut st,
    );
}

fn draw_locked(f: &mut Frame, app: &App, area: Rect) {
    let text = if app.connected {
        "Esta parte é da administração.\n\nAperte L e digite a senha mestre."
    } else {
        "Sem conexão com a placa.\n\nAperte c para procurar ou digitar o endereço."
    };
    f.render_widget(
        Paragraph::new(text).alignment(Alignment::Center).block(Block::bordered()).wrap(Wrap { trim: true }),
        center(area, 60, 7),
    );
}

fn draw_panel(f: &mut Frame, app: &App, area: Rect) {
    let Some(s) = &app.state else {
        draw_locked(f, app, area);
        return;
    };
    let [top, zones] = Layout::vertical([Constraint::Length(6), Constraint::Min(4)]).areas(area);
    let exit = app.left(s.exit_remaining);
    let entry = app.left(s.entry_remaining);
    let (text, color, detail) = if s.is_triggered {
        ("ALARME DISPARADO", Color::Red, String::new())
    } else if entry > 0 {
        ("DESARME AGORA", Color::Yellow, format!("dispara em {entry} s"))
    } else if exit > 0 {
        ("SAINDO", Color::Yellow, format!("{}: arma em {exit} s", proto::mode_label(&s.mode)))
    } else if s.is_armed {
        ("ARMADO", Color::Red, proto::mode_label(&s.mode).to_string())
    } else {
        ("DESARMADO", Color::Green, String::new())
    };
    let siren = if s.is_siren_active {
        Span::styled("SIRENE TOCANDO", Style::new().red().bold().add_modifier(Modifier::SLOW_BLINK))
    } else {
        Span::styled("sirene desligada", Style::new().dark_gray())
    };
    let lines = vec![
        Line::from(Span::styled(text, Style::new().fg(color).bold())),
        Line::from(detail),
        Line::from(siren),
    ];
    f.render_widget(
        Paragraph::new(lines).alignment(Alignment::Center).block(Block::bordered().title(" Estado ")),
        top,
    );

    let rows = s.sectors.iter().map(|z| {
        let open = z.status == "Aberto";
        let mut obs = Vec::new();
        if z.bypass {
            obs.push("inibida");
        }
        if z.temp_bypass {
            obs.push("inibida até desarmar");
        }
        if z.is24h {
            obs.push("24 h");
        }
        if s.triggering_sector_id == z.id && s.is_triggered {
            obs.push("DISPAROU");
        }
        Row::new(vec![
            Cell::from(z.id.to_string()),
            Cell::from(z.name.clone()),
            Cell::from(Span::styled(z.status.clone(), if open { Style::new().red().bold() } else { Style::new().green() })),
            Cell::from(obs.join(", ")),
        ])
    });
    f.render_widget(
        Table::new(rows, [Constraint::Length(4), Constraint::Length(24), Constraint::Length(10), Constraint::Min(10)])
            .header(Row::new(vec!["Zona", "Nome", "Estado", ""]).style(Style::new().dark_gray()))
            .block(Block::bordered().title(" Zonas ")),
        zones,
    );
}

fn draw_zones(f: &mut Frame, app: &App, area: Rect) {
    let Some(a) = &app.admin else { return };
    let rows = a.zones.iter().map(|z| {
        Row::new(vec![
            z.id.to_string(),
            z.name.clone(),
            proto::zone_type_label(z.kind).to_string(),
            if z.home { "sim" } else { "—" }.to_string(),
            if z.bypass { "INIBIDA" } else { "—" }.to_string(),
        ])
    });
    let mut st = TableState::default().with_selected(Some(app.zone_sel));
    f.render_stateful_widget(
        Table::new(rows, [Constraint::Length(4), Constraint::Length(24), Constraint::Length(15), Constraint::Length(9), Constraint::Length(9)])
            .header(Row::new(vec!["Zona", "Nome", "Tipo", "Em casa", "Inibida"]).style(Style::new().dark_gray()))
            .row_highlight_style(Style::new().black().on_cyan())
            .block(Block::bordered().title(" Zonas — Enter edita ")),
        area,
        &mut st,
    );
}

fn draw_codes(f: &mut Frame, app: &App, area: Rect) {
    let Some(a) = &app.admin else { return };
    let rows = a.codes.iter().map(|c| {
        Row::new(vec![
            Cell::from(c.label.clone()),
            Cell::from(format!("{} dígitos", c.digits)),
            Cell::from(if c.duress { Span::styled("pânico", Style::new().yellow()) } else { Span::raw("") }),
        ])
    });
    let mut st = TableState::default().with_selected(if a.codes.is_empty() { None } else { Some(app.code_sel) });
    let title = if a.codes.is_empty() { " Senhas — nenhuma cadastrada (n: nova) " } else { " Senhas — o valor nunca aparece " };
    f.render_stateful_widget(
        Table::new(rows, [Constraint::Length(26), Constraint::Length(12), Constraint::Length(8)])
            .header(Row::new(vec!["Nome", "Tamanho", ""]).style(Style::new().dark_gray()))
            .row_highlight_style(Style::new().black().on_cyan())
            .block(Block::bordered().title(title)),
        area,
        &mut st,
    );
}

pub fn fmt_time(t: u64) -> String {
    if t == 0 {
        return "sem horário".into();
    }
    DateTime::from_timestamp(t as i64, 0)
        .map(|d| d.with_timezone(&Local).format("%d/%m %H:%M:%S").to_string())
        .unwrap_or_default()
}

fn draw_events(f: &mut Frame, app: &App, area: Rect) {
    let items: Vec<ListItem> = app
        .events
        .iter()
        .map(|e| {
            let style = if e.m.contains("DISPARO") || e.m.contains("COACAO") {
                Style::new().red().bold()
            } else if e.m.contains("errada") || e.m.contains("bloqueado") || e.m.contains("cancelado") || e.m.contains("aberta ha") {
                Style::new().yellow()
            } else if e.m.starts_with("Armado") {
                Style::new().cyan()
            } else if e.m.starts_with("Desarmado") {
                Style::new().green()
            } else {
                Style::new()
            };
            ListItem::new(Line::from(vec![
                Span::styled(format!("{:<16}", fmt_time(e.t)), Style::new().dark_gray()),
                Span::styled(e.m.clone(), style),
            ]))
        })
        .collect();
    let title = format!(" Eventos ({}) — os mais novos primeiro ", app.events.len());
    let mut st = ListState::default().with_selected(if app.events.is_empty() { None } else { Some(app.events_scroll) });
    f.render_stateful_widget(
        List::new(items).block(Block::bordered().title(title)).highlight_style(Style::new().add_modifier(Modifier::REVERSED)),
        area,
        &mut st,
    );
}

fn draw_system(f: &mut Frame, app: &App, area: Rect) {
    let Some(a) = &app.admin else { return };
    let s = &a.system;
    let [info, ota, gh] = Layout::vertical([Constraint::Min(8), Constraint::Length(3), Constraint::Length(3)]).areas(area);
    f.render_widget(
        Paragraph::new(app.release_line()).block(Block::bordered().title(" Firmware no GitHub (d baixa e já oferece o OTA) ")),
        gh,
    );
    let up = s.uptime_s;
    let uptime = if up >= 86400 { format!("{} d {} h", up / 86400, up % 86400 / 3600) } else { format!("{} h {} min", up / 3600, up % 3600 / 60) };
    let row = |k: &str, v: String| Line::from(vec![Span::styled(format!("{k:<18}"), Style::new().gray()), Span::raw(v)]);
    let lines = vec![
        row("Versão", format!("v{}", s.version)),
        row("Endereço", format!("{} ({})  {}.local", s.ip, if a.network.ip_mode == "static" { "IP fixo" } else { "DHCP" }, a.network.hostname)),
        row("Gateway / MAC", format!("{}  /  {}", a.network.current.gateway, a.network.current.mac)),
        row("Nome / ID MQTT", format!("{} / {}", a.network.device_name, a.network.mqtt_id)),
        row("Sinal do Wi-Fi", format!("{} dBm ({})", s.rssi, a.network.ssid)),
        row("MQTT", if s.mqtt { "conectado".into() } else { format!("desconectado ({})", a.network.mqtt_host) }),
        row("Ligada há", uptime),
        row("Memória livre", format!("{} KB", s.free_heap / 1024)),
        row("Relógio da placa", if s.clock > 0 { fmt_time(s.clock) } else { "ainda não acertou (sem internet?)".into() }),
    ];
    f.render_widget(Paragraph::new(lines).block(Block::bordered().title(" Sistema ")), info);
    match app.ota {
        Some((sent, total)) => {
            let pct = if total > 0 { sent as f64 / total as f64 } else { 0.0 };
            let label = if sent >= total { "gravando e conferindo na placa…".to_string() } else { format!("enviando {:.0}%", pct * 100.0) };
            f.render_widget(
                Gauge::default().ratio(pct.min(1.0)).label(label).gauge_style(Style::new().cyan()).block(Block::bordered().title(" Atualização por OTA ")),
                ota,
            );
        }
        None => f.render_widget(
            Paragraph::new(format!("Firmware: {}  —  {}", app.fw_path, app.fw_info))
                .block(Block::bordered().title(" Atualização por OTA (u) ")),
            ota,
        ),
    }
}

fn draw_usb(f: &mut Frame, app: &App, area: Rect) {
    let [ports, opts, prog, log] =
        Layout::vertical([Constraint::Length(6), Constraint::Length(6), Constraint::Length(3), Constraint::Min(3)]).areas(area);
    let items: Vec<ListItem> = if app.ports.is_empty() {
        vec![ListItem::new(Span::styled("nenhuma porta serial (conecte a placa pelo USB e aperte r)", Style::new().dark_gray()))]
    } else {
        app.ports
            .iter()
            .map(|p| {
                let mac = p.mac().map(|m| format!("  MAC {m}")).unwrap_or_default();
                let style = if p.is_esp() { Style::new().green() } else { Style::new() };
                ListItem::new(Line::from(vec![
                    Span::raw(format!("{:<22} ", p.name)),
                    Span::styled(format!("{}{mac}", p.desc), style),
                ]))
            })
            .collect()
    };
    let mut st = ListState::default().with_selected(if app.ports.is_empty() { None } else { Some(app.port_sel) });
    f.render_stateful_widget(
        List::new(items)
            .block(Block::bordered().title(match app.usb_esp() {
                Some(_) => " Porta — ESP32 conectado ✔ (atualiza sozinha ao plugar/tirar) ",
                None => " Porta — plugue o ESP32 no USB (atualiza sozinha) ",
            }))
            .highlight_style(Style::new().black().on_cyan())
            .highlight_symbol("▶ "),
        ports,
        &mut st,
    );
    let erase = if app.erase_all {
        Span::styled("[x] APAGAR TUDO antes (Wi-Fi, senhas, zonas e registro)", Style::new().red().bold())
    } else {
        Span::styled("[ ] apagar tudo — configuração e senhas ficam", Style::new().gray())
    };
    let lines = vec![
        Line::from(vec![Span::styled("Firmware (f): ", Style::new().gray()), Span::raw(app.fw_path.clone())]),
        Line::from(vec![Span::raw("              "), Span::styled(app.fw_info.clone(), Style::new().dark_gray())]),
        Line::from(vec![Span::styled("Apagar (e):   ", Style::new().gray()), erase]),
        Line::from(vec![Span::styled("GitHub (d):   ", Style::new().gray()), Span::raw(app.release_line())]),
    ];
    f.render_widget(
        Paragraph::new(lines).block(Block::bordered().title(" Gravar pelo cabo — bootloader e partições vão junto ")),
        opts,
    );
    let (ratio, label) = match app.flash_prog {
        Some((part, cur, total)) if total > 0 => (cur as f64 / total as f64, format!("{part}: {:.0}%", cur as f64 * 100.0 / total as f64)),
        _ if app.flashing => (0.0, "preparando…".into()),
        _ => (0.0, "g ou Enter: gravar".into()),
    };
    f.render_widget(Gauge::default().ratio(ratio.min(1.0)).label(label).gauge_style(Style::new().cyan()).block(Block::bordered()), prog);
    let start = app.flash_log.len().saturating_sub(log.height.saturating_sub(2) as usize);
    let lines: Vec<Line> = app.flash_log[start..].iter().map(|l| Line::raw(l.clone())).collect();
    f.render_widget(Paragraph::new(lines).block(Block::bordered().title(" Registro ")), log);
}

type Keys = Vec<(&'static str, &'static str)>;

/// Todos os atalhos válidos agora: os da janela aberta, ou os da aba + os gerais.
fn shortcuts(app: &App) -> Keys {
    let editing = match app.tab {
        Tab::Tempos => app.timing_form.editing,
        Tab::Sirene => app.siren_form.editing,
        Tab::Rede => app.net_form.editing,
        _ => false,
    };
    if let Some(m) = &app.modal {
        return match m {
            Modal::Connect { .. } => vec![("↑/↓", "escolher"), ("Enter", "conectar"), ("r", "procurar de novo"), ("u", "gravar pelo USB"), ("Esc", "fechar"), ("Ctrl+C", "sair")],
            Modal::Form { form, .. } if form.editing => vec![("digite", "o valor"), ("Backspace", "apagar"), ("Enter/Esc", "terminar")],
            Modal::Form { .. } => vec![("↑/↓", "campo"), ("Enter", "editar"), ("Espaço", "liga/desliga"), ("←/→", "trocar opção"), ("s", "salvar"), ("Esc", "cancelar")],
            Modal::Confirm { .. } => vec![("s/Enter", "sim"), ("n/Esc", "não")],
            Modal::Wifi { .. } => vec![("↑/↓", "escolher"), ("Enter", "usar esta rede"), ("Esc", "fechar")],
            Modal::Input { .. } => vec![("digite", "o valor"), ("Backspace", "apagar"), ("Enter", "confirmar"), ("Esc", "cancelar")],
        };
    }
    if editing {
        return vec![("digite", "o valor"), ("Backspace", "apagar"), ("Enter/Esc", "terminar")];
    }
    let mut k: Keys = if app.menu_focus {
        let mut m: Keys = vec![("↑/↓", "escolher aba")];
        m.push(if app.tab.needs_admin() && app.admin.is_none() { ("→/Enter", "entrar no admin") } else { ("→/Enter", "entrar na aba") });
        m
    } else {
        vec![("←/Esc", "voltar ao menu")]
    };
    k.extend(if app.tab.needs_admin() && app.admin.is_none() {
        vec![("L", "entrar no admin (senha mestre)")]
    } else {
        match app.tab {
            Tab::Painel => vec![("a", "armar ausente"), ("h", "armar em casa"), ("d", "desarmar"), ("s", "silenciar sirene")],
            Tab::Zonas => vec![("↑/↓", "escolher zona"), ("Enter", "editar nome, tipo, Em casa e inibir")],
            Tab::Senhas => vec![("↑/↓", "escolher"), ("n", "nova senha"), ("x/Del", "remover")],
            Tab::Tempos => vec![("↑/↓", "campo"), ("Enter", "editar/alternar"), ("←/→", "trocar opção"), ("1-7", "dias (no campo Dias)"), ("s", "salvar"), ("r", "desfazer")],
            Tab::Sirene => vec![("↑/↓", "campo"), ("Enter", "editar/alternar"), ("s", "salvar"), ("r", "desfazer"), ("t", "testar sirene"), ("k", "testar trava"), ("g", "mensagem de teste no Telegram")],
            Tab::Rede => vec![("↑/↓", "campo"), ("Enter", "editar"), ("w", "procurar redes Wi-Fi"), ("s", "salvar e reiniciar"), ("r", "desfazer")],
            Tab::Eventos => vec![("↑/↓", "rolar"), ("PgUp/PgDn", "rolar rápido"), ("r", "atualizar")],
            Tab::Sistema => vec![("d", "baixar o firmware do GitHub e mandar"), ("u", "atualizar firmware (OTA) de um arquivo"), ("b", "baixar backup"), ("o", "restaurar backup"), ("m", "trocar senha mestre"), ("a", "atualizar dados"), ("r", "reiniciar placa"), ("x", "sair do admin")],
            Tab::Usb => vec![("↑/↓", "porta"), ("d", "baixar o firmware do GitHub"), ("f", "escolher o .bin"), ("e", "apagar tudo sim/não"), ("g/Enter", "gravar"), ("r", "atualizar portas")],
        }
    }
    .into_iter()
    // No menu, ↑/↓ e Enter são do menu: as da aba ficam para quando entrar nela.
    .filter(|(key, _)| !app.menu_focus || !(key.contains('↑') || key.contains("Enter") || key.contains("PgUp"))));
    k.extend([("1-9", "ir para a aba"), ("c", "conectar em outra placa")]);
    if app.admin.is_none() && app.tab != Tab::Usb && !(app.tab.needs_admin()) {
        k.push(("L", "admin"));
    }
    k.push(("q", "sair"));
    k
}

fn key_spans(keys: &Keys) -> Vec<Span<'static>> {
    let mut spans = Vec::new();
    // Espaço inquebrável dentro de cada par: a quebra de linha só acontece entre atalhos.
    let nb = |s: &str| s.replace(' ', "\u{a0}");
    for (key, what) in keys {
        spans.push(Span::styled(nb(&format!(" {key} ")), Style::new().black().on_dark_gray()));
        spans.push(Span::raw(nb(&format!(" {what}"))));
        spans.push(Span::raw("   "));
    }
    spans
}

/// Altura da caixa de atalhos: o quanto o texto ocupa quebrado na largura.
fn help_height(keys: &Keys, width: u16) -> u16 {
    let inner = width.saturating_sub(2).max(10) as usize;
    let mut lines = 1;
    let mut col = 0;
    for s in key_spans(keys) {
        let w = s.content.chars().count();
        if col + w > inner {
            lines += 1;
            col = 0;
        }
        col += w;
    }
    lines as u16 + 2
}

fn draw_help(f: &mut Frame, keys: &Keys, area: Rect) {
    f.render_widget(
        Paragraph::new(Line::from(key_spans(keys)))
            .wrap(Wrap { trim: true })
            .block(Block::bordered().title(" Atalhos ").border_style(Style::new().dark_gray())),
        area,
    );
}

fn draw_status(f: &mut Frame, app: &App, area: Rect) {
    // Some sozinha depois de 15 s (erros ficam até a próxima mensagem).
    if let Some(s) = app.status.as_ref().filter(|s| !s.ok || s.at.elapsed().as_secs() < 15) {
        let style = if s.ok { Style::new().green() } else { Style::new().red().bold() };
        f.render_widget(Line::styled(format!(" {}", s.text), style), area);
    }
}

fn center(area: Rect, w: u16, h: u16) -> Rect {
    let [v] = Layout::vertical([Constraint::Length(h.min(area.height))]).flex(Flex::Center).areas(area);
    let [r] = Layout::horizontal([Constraint::Length(w.min(area.width))]).flex(Flex::Center).areas(v);
    r
}

fn draw_modal(f: &mut Frame, app: &App, m: &Modal) {
    match m {
        Modal::Connect { sel } => {
            let mut items: Vec<ListItem> = app
                .boards
                .iter()
                .map(|b| {
                    ListItem::new(Line::from(vec![
                        Span::styled(format!("{:<22}", b.name), Style::new().bold()),
                        Span::raw(format!("{:<16}", b.addr)),
                        Span::styled(b.detail.clone(), Style::new().dark_gray()),
                    ]))
                })
                .collect();
            items.push(ListItem::new(Span::styled("Digitar o endereço…", Style::new().italic())));
            items.push(ListItem::new(Span::styled("Varrer outra rede (ex.: 10.55.9.0/24)…", Style::new().italic())));
            let title = if app.searching {
                " Procurando Homeguard na rede… ".to_string()
            } else {
                format!(" {} placa(s) encontrada(s) — r procura de novo ", app.boards.len())
            };
            let area = center(f.area(), 76, (items.len() as u16 + 4).max(8));
            f.render_widget(Clear, area);
            let mut st = ListState::default().with_selected(Some(*sel));
            f.render_stateful_widget(
                List::new(items)
                    .block(Block::bordered().title(title).border_style(Style::new().cyan()))
                    .highlight_style(Style::new().black().on_cyan())
                    .highlight_symbol("▶ "),
                area,
                &mut st,
            );
        }
        Modal::Input { title, label, value, secret, .. } => {
            let area = center(f.area(), 64, 5);
            f.render_widget(Clear, area);
            let shown = if *secret { "•".repeat(value.chars().count()) } else { value.clone() };
            let text = Text::from(vec![
                Line::styled(label.clone(), Style::new().gray()),
                Line::from(vec![Span::styled(shown, Style::new().bold()), Span::styled("▏", Style::new().cyan())]),
            ]);
            f.render_widget(
                Paragraph::new(text).block(Block::bordered().title(format!(" {title} ")).border_style(Style::new().cyan())),
                area,
            );
        }
        Modal::Confirm { text, .. } => {
            let lines = text.lines().count() as u16 + 4;
            let area = center(f.area(), 70, lines + 2);
            f.render_widget(Clear, area);
            let mut t = Text::from(text.clone());
            t.push_line(Line::default());
            t.push_line(Line::styled("s: sim    n: não", Style::new().cyan()));
            f.render_widget(
                Paragraph::new(t).wrap(Wrap { trim: false }).block(Block::bordered().title(" Confirmar ").border_style(Style::new().yellow())),
                area,
            );
        }
        Modal::Form { title, form, .. } => {
            let area = center(f.area(), 70, form.fields.len() as u16 + 6);
            f.render_widget(Clear, area);
            form.render(f, area, &format!("{title} — s salva, Esc cancela"), true);
        }
        Modal::Wifi { nets, sel } => {
            let items: Vec<ListItem> = nets
                .iter()
                .map(|n| {
                    let bars = match n.rssi {
                        r if r >= -55 => "▂▄▆█",
                        r if r >= -65 => "▂▄▆·",
                        r if r >= -75 => "▂▄··",
                        _ => "▂···",
                    };
                    ListItem::new(format!("{}{:<32} {bars} {} dBm", if n.open { "  " } else { "🔒" }, n.ssid, n.rssi))
                })
                .collect();
            let area = center(f.area(), 60, (nets.len() as u16 + 2).min(20));
            f.render_widget(Clear, area);
            let mut st = ListState::default().with_selected(Some(*sel));
            f.render_stateful_widget(
                List::new(items)
                    .block(Block::bordered().title(" Redes encontradas pela placa ").border_style(Style::new().cyan()))
                    .highlight_style(Style::new().black().on_cyan()),
                area,
                &mut st,
            );
        }
    }
}
