//! Formulário genérico: uma lista de campos navegada com ↑/↓. Enter edita texto
//! (ou alterna sim/não, ou passa para a próxima opção); Esc/Enter termina a edição.

use crossterm::event::{KeyCode, KeyEvent};
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style, Stylize};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph, Wrap};
use ratatui::Frame;

#[derive(Clone)]
pub enum Kind {
    Text(usize),
    Secret(usize),
    Digits(usize),
    Number { min: u32, max: u32 },
    Bool,
    Choice(Vec<(String, String)>), // (valor, rótulo)
    Days,                          // máscara de 7 bits, dom..sáb
    Time,                          // HH:MM
}

#[derive(Clone)]
pub struct Field {
    pub key: &'static str,
    pub label: String,
    pub kind: Kind,
    pub value: String,
    pub hint: String,
}

impl Field {
    pub fn new(key: &'static str, label: &str, kind: Kind, value: impl Into<String>) -> Self {
        Field { key, label: label.into(), kind, value: value.into(), hint: String::new() }
    }
    pub fn hint(mut self, h: &str) -> Self {
        self.hint = h.into();
        self
    }
}

pub const DAYS: [&str; 7] = ["Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"];

#[derive(Clone, Default)]
pub struct Form {
    pub fields: Vec<Field>,
    pub sel: usize,
    pub editing: bool,
    pub dirty: bool,
    /// Acabou de entrar na edição: o primeiro caractere de número/horário
    /// substitui o valor em vez de ir para o fim dele.
    fresh: bool,
}

pub enum FormKey {
    Handled,
    NotHandled,
}

impl Form {
    pub fn new(fields: Vec<Field>) -> Self {
        Form { fields, sel: 0, editing: false, dirty: false, fresh: false }
    }

    pub fn get(&self, key: &str) -> &str {
        self.fields.iter().find(|f| f.key == key).map(|f| f.value.as_str()).unwrap_or("")
    }
    pub fn bool(&self, key: &str) -> bool {
        self.get(key) == "true"
    }
    pub fn num(&self, key: &str) -> u32 {
        self.get(key).parse().unwrap_or(0)
    }
    /// Minutos do dia a partir de "HH:MM" (None se inválido).
    pub fn minutes(&self, key: &str) -> Option<u32> {
        let (h, m) = self.get(key).split_once(':')?;
        let (h, m): (u32, u32) = (h.parse().ok()?, m.parse().ok()?);
        (h < 24 && m < 60).then_some(h * 60 + m)
    }

    pub fn key(&mut self, k: KeyEvent) -> FormKey {
        if self.fields.is_empty() {
            return FormKey::NotHandled;
        }
        let f = &mut self.fields[self.sel];
        let numeric = matches!(f.kind, Kind::Digits(_) | Kind::Number { .. } | Kind::Time);
        // Digitar um número num campo numérico/horário já entra na edição. Sem
        // isto, o número trocava de aba (1-9 são as abas). Letras não: s, t, r...
        // são atalhos da aba; texto continua precisando do Enter.
        if !self.editing && numeric {
            if let KeyCode::Char(c) = k.code {
                if c.is_ascii_digit() {
                    self.editing = true;
                    self.fresh = true;
                }
            }
        }
        if self.editing {
            let replace = std::mem::take(&mut self.fresh) && matches!(f.kind, Kind::Digits(_) | Kind::Number { .. } | Kind::Time);
            match k.code {
                KeyCode::Enter | KeyCode::Esc | KeyCode::Tab => self.editing = false,
                KeyCode::Backspace => {
                    // Horário: o ':' sai junto com o dígito antes dele ("12:" -> "1").
                    if f.value.pop() == Some(':') && matches!(f.kind, Kind::Time) {
                        f.value.pop();
                    }
                    self.dirty = true;
                }
                KeyCode::Char(c) => {
                    if replace && c.is_ascii_digit() {
                        f.value.clear();
                    }
                    let ok = match &f.kind {
                        Kind::Text(max) | Kind::Secret(max) => f.value.chars().count() < *max,
                        Kind::Digits(max) => c.is_ascii_digit() && f.value.len() < *max,
                        Kind::Number { .. } => c.is_ascii_digit() && f.value.len() < 5,
                        // Horário: só dígitos; o ':' entra sozinho depois da hora.
                        Kind::Time => c.is_ascii_digit() && f.value.len() < 5,
                        _ => false,
                    };
                    if ok {
                        f.value.push(c);
                        if matches!(f.kind, Kind::Time) && f.value.len() == 2 {
                            f.value.push(':');
                        }
                        self.dirty = true;
                    }
                }
                _ => {}
            }
            if !self.editing {
                // Número fora da faixa volta para o limite mais próximo.
                if let Kind::Number { min, max } = f.kind {
                    let v: u32 = f.value.parse().unwrap_or(min);
                    f.value = v.clamp(min, max).to_string();
                }
                // "7" vira 07:00, "7:3" vira 07:30; hora ou minuto fora da faixa, o limite.
                if let Kind::Time = f.kind {
                    let (h, m) = f.value.split_once(':').unwrap_or((f.value.as_str(), "0"));
                    let h: u32 = h.parse().unwrap_or(0);
                    let m: u32 = if m.is_empty() { 0 } else { m.parse().unwrap_or(0) };
                    let m = if m < 10 && f.value.ends_with(&format!(":{m}")) && f.value.len() == 4 { m * 10 } else { m };
                    f.value = format!("{:02}:{:02}", h.min(23), m.min(59));
                }
            }
            return FormKey::Handled;
        }
        match k.code {
            KeyCode::Up => self.sel = self.sel.saturating_sub(1),
            KeyCode::Down | KeyCode::Tab => self.sel = (self.sel + 1).min(self.fields.len() - 1),
            KeyCode::Enter | KeyCode::Char(' ') | KeyCode::Right | KeyCode::Left => {
                let back = k.code == KeyCode::Left;
                match &f.kind {
                    Kind::Bool => {
                        f.value = if f.value == "true" { "false" } else { "true" }.into();
                        self.dirty = true;
                    }
                    Kind::Choice(opts) => {
                        let i = opts.iter().position(|o| o.0 == f.value).unwrap_or(0);
                        let n = opts.len();
                        let j = if back { (i + n - 1) % n } else { (i + 1) % n };
                        f.value = opts[j].0.clone();
                        self.dirty = true;
                    }
                    Kind::Days => return FormKey::NotHandled,
                    _ if k.code == KeyCode::Enter => {
                        self.editing = true;
                        self.fresh = true;
                    }
                    _ => return FormKey::NotHandled,
                }
            }
            // Dias: 1..7 liga/desliga dom..sáb.
            KeyCode::Char(c @ '1'..='7') if matches!(f.kind, Kind::Days) => {
                let mask: u8 = f.value.parse().unwrap_or(0);
                let bit = c as u8 - b'1';
                f.value = (mask ^ (1 << bit)).to_string();
                self.dirty = true;
            }
            _ => return FormKey::NotHandled,
        }
        FormKey::Handled
    }

    pub fn render(&self, frame: &mut Frame, area: Rect, title: &str, focused: bool) {
        let mut lines: Vec<Line> = Vec::new();
        let width = self.fields.iter().map(|f| f.label.chars().count()).max().unwrap_or(10) + 2;
        for (i, f) in self.fields.iter().enumerate() {
            let sel = i == self.sel && focused;
            let shown = match &f.kind {
                Kind::Secret(_) if f.value.is_empty() => Span::styled(f.hint.clone(), Style::new().dark_gray()),
                Kind::Secret(_) => Span::raw("•".repeat(f.value.chars().count())),
                Kind::Bool => {
                    if f.value == "true" {
                        Span::styled("[x] sim", Style::new().green())
                    } else {
                        Span::styled("[ ] não", Style::new().dark_gray())
                    }
                }
                Kind::Choice(opts) => Span::raw(format!(
                    "◂ {} ▸",
                    opts.iter().find(|o| o.0 == f.value).map(|o| o.1.as_str()).unwrap_or(&f.value)
                )),
                Kind::Days => {
                    let mask: u8 = f.value.parse().unwrap_or(0);
                    let s: Vec<String> = DAYS
                        .iter()
                        .enumerate()
                        .map(|(i, d)| if mask & (1 << i) != 0 { format!("[{d}]") } else { format!(" {} ", d.to_lowercase()) })
                        .collect();
                    Span::raw(s.join(" "))
                }
                _ if f.value.is_empty() && !f.hint.is_empty() => Span::styled(f.hint.clone(), Style::new().dark_gray()),
                _ => Span::raw(f.value.clone()),
            };
            let mut spans = vec![
                Span::styled(if sel { "▶ " } else { "  " }, Style::new().cyan()),
                Span::styled(format!("{:<width$}", f.label), Style::new().gray()),
            ];
            if sel && self.editing {
                spans.push(Span::styled(
                    match &f.kind {
                        Kind::Secret(_) => "•".repeat(f.value.chars().count()),
                        _ => f.value.clone(),
                    },
                    Style::new().black().on_cyan(),
                ));
                spans.push(Span::styled("▏", Style::new().cyan().add_modifier(Modifier::SLOW_BLINK)));
            } else if sel {
                spans.push(shown.add_modifier(Modifier::BOLD));
            } else {
                spans.push(shown);
            }
            lines.push(Line::from(spans));
        }
        // Ajuda do campo selecionado.
        if let Some(f) = self.fields.get(self.sel) {
            let help = match f.kind {
                Kind::Bool => "Enter/Espaço: liga/desliga",
                Kind::Choice(_) => "←/→ ou Enter: troca a opção",
                Kind::Days => "1 a 7: liga/desliga dom…sáb",
                _ => "Enter: editar",
            };
            lines.push(Line::default());
            lines.push(Line::from(Span::styled(help, Style::new().dark_gray())));
        }
        let border = if focused { Color::Cyan } else { Color::DarkGray };
        let mut title_line = Line::from(format!(" {title} "));
        if self.dirty {
            title_line.push_span(Span::styled("● não salvo ", Style::new().yellow()));
        }
        frame.render_widget(
            Paragraph::new(lines)
                .wrap(Wrap { trim: false })
                .block(Block::default().borders(Borders::ALL).border_style(Style::new().fg(border)).title(title_line)),
            area,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossterm::event::{KeyCode, KeyEvent};

    fn typed(start: &str, keys: &[KeyCode]) -> String {
        let mut f = Form::new(vec![Field::new("t", "Horário", Kind::Time, start)]);
        for k in keys {
            f.key(KeyEvent::from(*k));
        }
        f.get("t").to_string()
    }

    #[test]
    fn horario() {
        use KeyCode::*;
        // Digitar direto (sem Enter) substitui e põe o ':' sozinho.
        assert_eq!(typed("23:00", &[Char('2'), Char('2'), Char('3'), Char('0'), Enter]), "22:30");
        // Enter, depois digitar: também substitui.
        assert_eq!(typed("23:00", &[Enter, Char('0'), Char('7'), Char('1'), Char('5'), Enter]), "07:15");
        // Só a hora: completa os minutos.
        assert_eq!(typed("23:00", &[Char('7'), Enter]), "07:00");
        assert_eq!(typed("23:00", &[Char('0'), Char('7'), Char('3'), Enter]), "07:30");
        // Fora da faixa vira o limite.
        assert_eq!(typed("23:00", &[Char('9'), Char('9'), Char('9'), Char('9'), Enter]), "23:59");
        // Backspace apaga o ':' junto.
        assert_eq!(typed("23:00", &[Enter, Char('1'), Char('2'), Backspace, Char('9'), Char('4'), Char('5'), Enter]), "19:45");
    }
}
