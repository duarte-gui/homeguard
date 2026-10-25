import React, { useEffect, useState } from 'react';
import { AdminData, AdminZone, AlarmEvent, ArmMode, CommandResult, MODE_LABEL, SendCommand, WifiNetwork, ZoneType } from '../types';

type Notify = (msg: string, ok: boolean) => void;

interface AdminPanelProps {
  data: AdminData;
  send: SendCommand;
  notify: Notify;
  onLogout: () => void;
}

const input = 'w-full px-3 py-2 rounded-lg bg-gray-900/70 border border-gray-600 text-white ' +
              'placeholder-gray-500 focus:outline-none focus:border-sky-400';
const btn = 'px-4 py-2 rounded-lg font-semibold transition-colors disabled:opacity-50 ' +
            'focus:outline-none focus-visible:ring-2 focus-visible:ring-sky-400';
const btnPrimary = `${btn} bg-sky-600 hover:bg-sky-500`;
const btnGhost = `${btn} bg-gray-700 hover:bg-gray-600`;
const btnDanger = `${btn} bg-red-700 hover:bg-red-600`;

const Section: React.FC<{ title: string; hint?: string; children: React.ReactNode }> = ({ title, hint, children }) => (
  <section className="bg-gray-800/60 border border-gray-700 rounded-xl p-5">
    <h3 className="text-lg font-semibold">{title}</h3>
    {hint && <p className="text-sm text-gray-400 mt-1">{hint}</p>}
    <div className="mt-4">{children}</div>
  </section>
);

const Toggle: React.FC<{ checked: boolean; onChange: (v: boolean) => void; label: string }> = ({ checked, onChange, label }) => (
  <label className="inline-flex items-center gap-2 cursor-pointer select-none">
    <input type="checkbox" className="w-5 h-5 accent-sky-500" checked={checked} onChange={e => onChange(e.target.checked)} />
    <span>{label}</span>
  </label>
);

// Envia, avisa o resultado e devolve para quem quiser reagir (ex.: limpar o formulário).
function useRun(send: SendCommand, notify: Notify) {
  const [busy, setBusy] = useState(false);
  const run = async (command: string, payload: object, okMsg: string): Promise<CommandResult> => {
    setBusy(true);
    const r = await send(command, payload);
    setBusy(false);
    notify(r.ok ? okMsg : r.error || 'Não deu certo.', r.ok);
    return r;
  };
  return { busy, run };
}

// ---------------------------------------------------------------- zonas
const ZONE_TYPES: { type: ZoneType; label: string; hint: string }[] = [
  { type: ZoneType.Instant, label: 'Instantânea', hint: 'dispara na hora' },
  { type: ZoneType.Delayed, label: 'Entrada/saída', hint: 'dá o tempo de entrada para desarmar; ignorada durante a saída' },
  { type: ZoneType.Follower, label: 'Seguidora', hint: 'instantânea, mas espera se a contagem de entrada ou saída estiver correndo (ex.: presença no caminho)' },
  { type: ZoneType.H24, label: '24 horas', hint: 'dispara mesmo desarmado (botão de pânico, tamper)' },
];

const ZoneRow: React.FC<{ zone: AdminZone; send: SendCommand; notify: Notify }> = ({ zone, send, notify }) => {
  const initial = { name: zone.name, bypass: zone.bypass, type: zone.type, home: zone.home };
  const [z, setZ] = useState(initial);
  const { busy, run } = useRun(send, notify);
  useEffect(() => setZ({ name: zone.name, bypass: zone.bypass, type: zone.type, home: zone.home }),
            [zone.name, zone.bypass, zone.type, zone.home]);
  const dirty = z.name.trim() !== zone.name || z.bypass !== zone.bypass || z.type !== zone.type ||
                z.home !== zone.home;

  return (
    <div className="py-3 border-b border-gray-700 last:border-0">
      <div className="flex flex-col sm:flex-row sm:items-center gap-3">
        <span className="text-sm text-gray-400 w-16 shrink-0">Zona {zone.id}</span>
        <input className={input} value={z.name} maxLength={23} placeholder={zone.defaultName}
               onChange={e => setZ({ ...z, name: e.target.value })} aria-label={`Nome da zona ${zone.id}`} />
        <select className={`${input} sm:w-48`} value={z.type} aria-label={`Tipo da zona ${zone.id}`}
                title={ZONE_TYPES.find(t => t.type === z.type)?.hint}
                onChange={e => setZ({ ...z, type: Number(e.target.value) as ZoneType })}>
          {ZONE_TYPES.map(t => <option key={t.type} value={t.type}>{t.label}</option>)}
        </select>
      </div>
      <div className="flex flex-wrap items-center gap-x-5 gap-y-2 mt-2 sm:pl-[4.75rem]">
        <Toggle checked={z.home} onChange={v => setZ({ ...z, home: v })} label="Em casa" />
        <Toggle checked={z.bypass} onChange={v => setZ({ ...z, bypass: v })} label="Inibida" />
        <button className={`${btnPrimary} ml-auto`} disabled={!dirty || busy || !z.name.trim()}
                onClick={() => run('setZone', { id: zone.id, ...z, name: z.name.trim() }, `Zona ${zone.id} salva.`)}>
          Salvar
        </button>
      </div>
    </div>
  );
};

const ZonesSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify }> = ({ data, send, notify }) => (
  <Section title="Zonas"
           hint="Nome que aparece aqui, no Home Assistant e no Telegram. &quot;Em casa&quot;: a zona vigia também com gente em casa (no Ausente todas vigiam). Inibida: aparece, mas não dispara.">
    {data.zones.map(z => <ZoneRow key={z.id} zone={z} send={send} notify={notify} />)}
    <details className="mt-3 text-sm text-gray-400">
      <summary className="cursor-pointer">O que é cada tipo?</summary>
      <ul className="mt-2 space-y-1">
        {ZONE_TYPES.map(t => <li key={t.type}><b className="text-gray-300">{t.label}:</b> {t.hint}.</li>)}
      </ul>
    </details>
  </Section>
);

// ---------------------------------------------------------------- tempos
const numField = (v: string) => v.replace(/\D/g, '').slice(0, 4);

const TimingSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify }> = ({ data, send, notify }) => {
  const t = data.timing;
  const [f, setF] = useState({ entry: String(t.entryDelaySec), exit: String(t.exitDelaySec), open: String(t.openAlertMin) });
  const { busy, run } = useRun(send, notify);
  useEffect(() => setF({ entry: String(t.entryDelaySec), exit: String(t.exitDelaySec), open: String(t.openAlertMin) }),
            [t.entryDelaySec, t.exitDelaySec, t.openAlertMin]);

  return (
    <Section title="Tempos" hint="A contagem de saída só acontece se alguma zona for de entrada/saída.">
      <div className="grid gap-3 sm:grid-cols-3">
        <label><span className="text-sm text-gray-400">Entrada: tempo para desarmar (s)</span>
          <input className={input} inputMode="numeric" value={f.entry} onChange={e => setF({ ...f, entry: numField(e.target.value) })} /></label>
        <label><span className="text-sm text-gray-400">Saída: tempo para sair (s)</span>
          <input className={input} inputMode="numeric" value={f.exit} onChange={e => setF({ ...f, exit: numField(e.target.value) })} /></label>
        <label><span className="text-sm text-gray-400">Avisar zona aberta há (min, 0 = não)</span>
          <input className={input} inputMode="numeric" value={f.open} onChange={e => setF({ ...f, open: numField(e.target.value) })} /></label>
      </div>
      <button className={`${btnPrimary} mt-3`} disabled={busy}
              onClick={() => run('setTiming', { entryDelaySec: Number(f.entry || 0), exitDelaySec: Number(f.exit || 0),
                                                openAlertMin: Number(f.open || 0) }, 'Tempos salvos.')}>
        Salvar
      </button>
    </Section>
  );
};

// ---------------------------------------------------------------- arme automático
const DAYS = ['Dom', 'Seg', 'Ter', 'Qua', 'Qui', 'Sex', 'Sáb'];
const TIMEZONES = [
  { tz: '<-02>2', label: 'Fernando de Noronha (UTC-2)' },
  { tz: '<-03>3', label: 'Brasília (UTC-3)' },
  { tz: '<-04>4', label: 'Amazonas, MT, MS, RO, RR (UTC-4)' },
  { tz: '<-05>5', label: 'Acre (UTC-5)' },
];
const hhmm = (min: number) => `${String(Math.floor(min / 60)).padStart(2, '0')}:${String(min % 60).padStart(2, '0')}`;

const AutoArmSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify }> = ({ data, send, notify }) => {
  const a = data.autoArm;
  const from = () => ({ enabled: a.enabled, time: hhmm(a.minute), days: a.days, mode: a.mode, tz: a.tz });
  const [f, setF] = useState(from);
  const { busy, run } = useRun(send, notify);
  useEffect(() => setF(from()), [a.enabled, a.minute, a.days, a.mode, a.tz]);
  const toggleDay = (d: number) => setF({ ...f, days: f.days ^ (1 << d) });
  const [h, m] = f.time.split(':').map(Number);
  const clock = data.system.clock;

  return (
    <Section title="Arme automático"
             hint="Arma sozinho no horário. Se alguma zona estiver aberta, não arma e avisa no Telegram e no Home Assistant.">
      <Toggle checked={f.enabled} onChange={v => setF({ ...f, enabled: v })} label="Ligado" />
      <div className="grid gap-3 sm:grid-cols-3 mt-3">
        <label><span className="text-sm text-gray-400">Horário</span>
          <input className={input} type="time" value={f.time} onChange={e => setF({ ...f, time: e.target.value })} /></label>
        <label><span className="text-sm text-gray-400">Modo</span>
          <select className={input} value={f.mode} onChange={e => setF({ ...f, mode: e.target.value as ArmMode })}>
            {(Object.keys(MODE_LABEL) as ArmMode[]).map(k => <option key={k} value={k}>{MODE_LABEL[k]}</option>)}
          </select></label>
        <label><span className="text-sm text-gray-400">Fuso horário</span>
          <select className={input} value={f.tz} onChange={e => setF({ ...f, tz: e.target.value })}>
            {TIMEZONES.some(t => t.tz === f.tz) ? null : <option value={f.tz}>{f.tz}</option>}
            {TIMEZONES.map(t => <option key={t.tz} value={t.tz}>{t.label}</option>)}
          </select></label>
      </div>
      <div className="flex flex-wrap gap-2 mt-3" role="group" aria-label="Dias da semana">
        {DAYS.map((d, i) => (
          <button key={d} onClick={() => toggleDay(i)} aria-pressed={!!(f.days & (1 << i))}
                  className={`${btn} px-3 py-1 text-sm ${f.days & (1 << i) ? 'bg-sky-600 hover:bg-sky-500' : 'bg-gray-700 hover:bg-gray-600'}`}>
            {d}
          </button>
        ))}
      </div>
      <p className="text-xs text-gray-500 mt-2">
        {clock ? `Relógio da placa: ${new Date(clock * 1000).toLocaleString('pt-BR')}.`
               : 'O relógio da placa ainda não acertou (precisa de internet): o arme automático espera.'}
      </p>
      <button className={`${btnPrimary} mt-3`} disabled={busy || !f.time || Number.isNaN(h)}
              onClick={() => run('setAutoArm', { enabled: f.enabled, minute: h * 60 + m, days: f.days, mode: f.mode, tz: f.tz },
                                 'Arme automático salvo.')}>
        Salvar
      </button>
    </Section>
  );
};

// ---------------------------------------------------------------- senhas
const CodesSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify }> = ({ data, send, notify }) => {
  const [label, setLabel] = useState('');
  const [code, setCode] = useState('');
  const [duress, setDuress] = useState(false);
  const { busy, run } = useRun(send, notify);
  const valid = /^[0-9]{4,8}$/.test(code) && label.trim().length > 0;

  const add = async () => {
    const r = await run('addCode', { label: label.trim(), code, duress }, `Senha de ${label.trim()} cadastrada.`);
    if (r.ok) { setLabel(''); setCode(''); setDuress(false); }
  };

  return (
    <Section title="Senhas" hint="Valem para desarmar no teclado, nesta página e no Home Assistant. O valor nunca é mostrado.">
      <ul className="divide-y divide-gray-700">
        {data.codes.length === 0 && <li className="py-2 text-gray-400">Nenhuma senha cadastrada.</li>}
        {data.codes.map((c, i) => (
          <li key={`${c.label}-${i}`} className="flex items-center justify-between gap-3 py-2">
            <span>
              {c.label} <span className="text-gray-500 text-sm">· {c.digits} dígitos</span>
              {c.duress && <span className="ml-2 text-xs px-2 py-0.5 rounded bg-amber-700/60 text-amber-100">pânico</span>}
            </span>
            <button className={btnGhost} disabled={busy}
                    onClick={() => window.confirm(`Remover a senha de ${c.label}?`) &&
                                   run('removeCode', { index: i }, `Senha de ${c.label} removida.`)}>
              Remover
            </button>
          </li>
        ))}
      </ul>

      <div className="mt-4 grid gap-3 sm:grid-cols-2">
        <input className={input} placeholder="Nome (ex.: Ana)" value={label} maxLength={23}
               onChange={e => setLabel(e.target.value)} />
        <input className={input} placeholder="Senha (4 a 8 números)" value={code} inputMode="numeric"
               autoComplete="off" onChange={e => setCode(e.target.value.replace(/\D/g, '').slice(0, 8))} />
      </div>
      <div className="mt-3">
        <Toggle checked={duress} onChange={setDuress} label="Senha de pânico" />
        <p className="text-sm text-gray-400 mt-1">
          Desarma normalmente, sem sirene e sem nada diferente na tela, mas manda um alerta
          silencioso no Telegram e no Home Assistant. Para usar se alguém te obrigar a desarmar.
        </p>
      </div>
      <button className={`${btnPrimary} mt-3`} disabled={!valid || busy} onClick={add}>Cadastrar</button>
    </Section>
  );
};

// ---------------------------------------------------------------- testes e sirene
const OutputsSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify }> = ({ data, send, notify }) => {
  const [enabled, setEnabled] = useState(data.siren.enabled);
  const [timeout, setTimeoutS] = useState(String(data.siren.timeoutSec));
  const { busy, run } = useRun(send, notify);
  useEffect(() => { setEnabled(data.siren.enabled); setTimeoutS(String(data.siren.timeoutSec)); },
            [data.siren.enabled, data.siren.timeoutSec]);

  return (
    <Section title="Sirene e trava" hint="O teste liga a saída por 1 segundo. Só funciona com o sistema desarmado.">
      <div className="flex flex-wrap gap-3">
        <button className={btnGhost} disabled={busy} onClick={() => run('testOutput', { target: 'siren' }, 'Sirene acionada por 1 s.')}>
          Testar sirene
        </button>
        <button className={btnGhost} disabled={busy} onClick={() => run('testOutput', { target: 'lock' }, 'Trava acionada por 1 s.')}>
          Testar trava
        </button>
      </div>
      <div className="mt-5 flex flex-col sm:flex-row sm:items-end gap-3">
        <Toggle checked={enabled} onChange={setEnabled} label="Sirene física habilitada" />
        <label className="flex-1">
          <span className="text-sm text-gray-400">Desliga sozinha depois de (segundos, 0 = nunca)</span>
          <input className={input} inputMode="numeric" value={timeout}
                 onChange={e => setTimeoutS(e.target.value.replace(/\D/g, '').slice(0, 4))} />
        </label>
        <button className={btnPrimary} disabled={busy}
                onClick={() => run('setSiren', { enabled, timeoutSec: Number(timeout || 0) }, 'Sirene salva.')}>
          Salvar
        </button>
      </div>
    </Section>
  );
};

// ---------------------------------------------------------------- Telegram
const TelegramSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify }> = ({ data, send, notify }) => {
  const [chatId, setChatId] = useState(data.telegram.chatId);
  const [token, setToken] = useState('');
  const { busy, run } = useRun(send, notify);
  useEffect(() => setChatId(data.telegram.chatId), [data.telegram.chatId]);

  return (
    <Section title="Telegram" hint="Para onde vão os alertas de disparo e de pânico.">
      <div className="grid gap-3">
        <label>
          <span className="text-sm text-gray-400">ID do chat</span>
          <input className={input} value={chatId} onChange={e => setChatId(e.target.value.trim())} placeholder="999999999" />
        </label>
        <label>
          <span className="text-sm text-gray-400">Token do bot</span>
          <input className={input} value={token} onChange={e => setToken(e.target.value)} autoComplete="off"
                 placeholder={data.telegram.hasToken ? '•••••• salvo (deixe em branco para manter)' : '123456:ABC...'} />
        </label>
      </div>
      <div className="flex flex-wrap gap-3 mt-3">
        <button className={btnPrimary} disabled={busy}
                onClick={async () => { const r = await run('setTelegram', { chatId, token }, 'Telegram salvo.'); if (r.ok) setToken(''); }}>
          Salvar
        </button>
        <button className={btnGhost} disabled={busy || !data.telegram.hasToken}
                onClick={() => run('testTelegram', {}, 'Mensagem de teste enviada.')}>
          Enviar teste
        </button>
      </div>
    </Section>
  );
};

// ---------------------------------------------------------------- rede
const NetworkSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify }> = ({ data, send, notify }) => {
  const n = data.network;
  const [f, setF] = useState({ ssid: n.ssid, hostname: n.hostname, deviceName: n.deviceName, mqttId: n.mqttId, pass: '', mqttHost: n.mqttHost, mqttPort: String(n.mqttPort), mqttUser: n.mqttUser, mqttPass: '',
                               ipMode: n.ipMode, ip: n.ip, gateway: n.gateway, subnet: n.subnet, dns: n.dns });
  // Ao trocar para IP fixo, começa pelo que a placa está usando agora.
  const setMode = (ipMode: 'dhcp' | 'static') =>
    setF(ipMode === 'static' && !f.ip
      ? { ...f, ipMode, ip: n.current.ip, gateway: n.current.gateway, subnet: n.current.subnet, dns: n.current.dns }
      : { ...f, ipMode });
  const ipOk = (s: string) => /^(\d{1,3}\.){3}\d{1,3}$/.test(s.trim()) && s.trim().split('.').every(p => Number(p) <= 255);
  const staticOk = f.ipMode === 'dhcp' || (ipOk(f.ip) && ipOk(f.gateway) && ipOk(f.subnet) && (!f.dns.trim() || ipOk(f.dns)));
  const { busy, run } = useRun(send, notify);
  const set = (k: keyof typeof f) => (e: React.ChangeEvent<HTMLInputElement>) => setF({ ...f, [k]: e.target.value });

  const save = () => {
    const aviso = f.ipMode === 'static'
      ? `\n\nCom IP fixo, a página passa a ser http://${f.ip.trim()}. Se o IP estiver errado, a placa some da rede: aí é o teclado (#, senha mestre, 3) que abre o hotspot para voltar ao DHCP.`
      : '';
    if (!window.confirm('A placa vai reiniciar para usar a rede nova. Se não conseguir conectar, ela abre o portal de configuração sozinha. Continuar?' + aviso)) return;
    run('setNetwork', { ...f, mqttPort: Number(f.mqttPort || 1883) }, 'Salvo. A placa está reiniciando…');
  };

  const [nets, setNets] = useState<WifiNetwork[] | null>(null);
  const [scanning, setScanning] = useState(false);
  const scan = async () => {
    setScanning(true);
    const r = await send('scanWifi');
    setScanning(false);
    if (r.ok) setNets(r.networks || []);
    else notify(r.error || 'A varredura falhou.', false);
  };
  // Mesma escala do portal: 4 barras a partir de -55 dBm.
  const bars = (rssi: number) => (rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1);

  return (
    <Section title="Rede" hint="Trocar o Wi-Fi ou o MQTT sem reset de fábrica. Senhas em branco ficam como estão.">
      <div className="mb-3">
        <button className={btnGhost} disabled={scanning} onClick={scan}>
          {scanning ? 'Procurando redes…' : '📶 Procurar redes'}
        </button>
        {nets && (
          nets.length === 0 ? (
            <p className="text-sm text-gray-400 mt-2">Nenhuma rede encontrada.</p>
          ) : (
            <ul className="mt-2 max-h-60 overflow-y-auto rounded-lg border border-gray-700 divide-y divide-gray-700">
              {nets.map(net => (
                <li key={net.ssid}>
                  <button
                    className={`w-full flex items-center justify-between gap-3 px-3 py-2 text-left hover:bg-gray-700/60
                                focus:outline-none focus-visible:bg-gray-700 ${net.ssid === f.ssid ? 'bg-sky-900/40' : ''}`}
                    onClick={() => setF({ ...f, ssid: net.ssid })}>
                    <span className="truncate">{net.open ? '' : '🔒 '}{net.ssid}</span>
                    <span className="shrink-0 text-xs text-gray-400 font-mono" title={`${net.rssi} dBm`}>
                      {'▂▄▆█'.slice(0, bars(net.rssi)).padEnd(4, '·')} {net.rssi} dBm
                    </span>
                  </button>
                </li>
              ))}
            </ul>
          )
        )}
      </div>
      <div className="grid gap-3 sm:grid-cols-2">
        <label className="sm:col-span-2"><span className="text-sm text-gray-400">Nome na rede</span>
          <div className="flex items-center gap-2">
            <input className={input} value={f.hostname} placeholder={n.defaultHostname} autoComplete="off" spellCheck={false}
                   onChange={e => setF({ ...f, hostname: e.target.value.toLowerCase().replace(/[^a-z0-9-]/g, '') })} />
            <span className="text-gray-400 shrink-0">.local</span>
          </div>
          <span className="text-xs text-gray-500">
            Endereço da página (http://{f.hostname || n.defaultHostname}.local). Em branco volta ao padrão, {n.defaultHostname}.
            Use nomes diferentes se houver mais de uma placa na casa.
          </span></label>
        <label><span className="text-sm text-gray-400">Nome da central</span>
          <input className={input} value={f.deviceName} placeholder={n.defaultDeviceName} maxLength={32}
                 onChange={set('deviceName')} />
          <span className="text-xs text-gray-500">Nome do aparelho no Home Assistant e no topo dos avisos do Telegram.</span></label>
        <label><span className="text-sm text-gray-400">ID no MQTT</span>
          <input className={input} value={f.mqttId} placeholder={n.defaultMqttId} autoComplete="off" spellCheck={false}
                 onChange={e => setF({ ...f, mqttId: e.target.value.toLowerCase().replace(/[^a-z0-9-]/g, '') })} />
          <span className="text-xs text-gray-500">
            Prefixo dos tópicos ({f.mqttId || n.defaultMqttId}/alarm/state…). Trocar cria um aparelho novo no Home Assistant e
            apaga o antigo: automações que usam as entidades antigas precisam ser refeitas.
          </span></label>
        <label><span className="text-sm text-gray-400">Wi-Fi (só 2,4 GHz)</span>
          <input className={input} value={f.ssid} onChange={set('ssid')} /></label>
        <label><span className="text-sm text-gray-400">Senha do Wi-Fi</span>
          <input className={input} type="password" value={f.pass} onChange={set('pass')} placeholder="•••••• (mantém)" autoComplete="new-password" /></label>
        <label><span className="text-sm text-gray-400">Broker MQTT</span>
          <input className={input} value={f.mqttHost} onChange={set('mqttHost')} /></label>
        <label><span className="text-sm text-gray-400">Porta</span>
          <input className={input} inputMode="numeric" value={f.mqttPort} onChange={set('mqttPort')} /></label>
        <label><span className="text-sm text-gray-400">Usuário MQTT</span>
          <input className={input} value={f.mqttUser} onChange={set('mqttUser')} autoComplete="off" /></label>
        <label><span className="text-sm text-gray-400">Senha MQTT</span>
          <input className={input} type="password" value={f.mqttPass} onChange={set('mqttPass')}
                 placeholder={n.hasMqttPass ? '•••••• (mantém)' : ''} autoComplete="new-password" /></label>
      </div>

      <div className="mt-5 pt-4 border-t border-gray-700">
        <h4 className="font-semibold">Endereço na rede</h4>
        <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-0.5 text-sm mt-2">
          <dt className="text-gray-400">Agora</dt>
          <dd><b>{n.current.ip}</b> {n.ipMode === 'static' ? '(fixo)' : '(DHCP)'}</dd>
          <dt className="text-gray-400">Gateway · máscara</dt><dd>{n.current.gateway} · {n.current.subnet}</dd>
          <dt className="text-gray-400">DNS</dt><dd>{n.current.dns}</dd>
          <dt className="text-gray-400">MAC</dt><dd className="font-mono">{n.current.mac}</dd>
        </dl>
        <div className="flex gap-5 mt-3" role="radiogroup" aria-label="Modo do endereço">
          <label className="inline-flex items-center gap-2 cursor-pointer">
            <input type="radio" className="accent-sky-500" checked={f.ipMode === 'dhcp'} onChange={() => setMode('dhcp')} />
            Automático (DHCP)
          </label>
          <label className="inline-flex items-center gap-2 cursor-pointer">
            <input type="radio" className="accent-sky-500" checked={f.ipMode === 'static'} onChange={() => setMode('static')} />
            IP fixo
          </label>
        </div>
        {f.ipMode === 'static' && (
          <div className="grid gap-3 sm:grid-cols-2 mt-3">
            <label><span className="text-sm text-gray-400">IP</span>
              <input className={input} inputMode="decimal" value={f.ip} onChange={set('ip')} placeholder="192.168.1.50" /></label>
            <label><span className="text-sm text-gray-400">Gateway (roteador)</span>
              <input className={input} inputMode="decimal" value={f.gateway} onChange={set('gateway')} placeholder="192.168.1.1" /></label>
            <label><span className="text-sm text-gray-400">Máscara</span>
              <input className={input} inputMode="decimal" value={f.subnet} onChange={set('subnet')} placeholder="255.255.255.0" /></label>
            <label><span className="text-sm text-gray-400">DNS (em branco = o gateway)</span>
              <input className={input} inputMode="decimal" value={f.dns} onChange={set('dns')} placeholder={f.gateway} /></label>
            <p className="text-xs text-gray-500 sm:col-span-2">
              Escolha um IP fora da faixa que o roteador distribui (DHCP), ou reserve este IP para o MAC acima no roteador.
            </p>
          </div>
        )}
      </div>
      <button className={`${btnPrimary} mt-4`} disabled={busy || !f.ssid.trim() || !staticOk} onClick={save}>Salvar e reiniciar</button>
    </Section>
  );
};

// ---------------------------------------------------------------- senha mestre
const MasterSection: React.FC<{ send: SendCommand; notify: Notify }> = ({ send, notify }) => {
  const [cur, setCur] = useState('');
  const [nova, setNova] = useState('');
  const [conf, setConf] = useState('');
  const { busy, run } = useRun(send, notify);
  const digits = (s: string) => s.replace(/\D/g, '').slice(0, 8);
  const valid = cur.length >= 4 && /^[0-9]{4,8}$/.test(nova) && nova === conf;

  return (
    <Section title="Senha mestre" hint="Entra nesta página e programa o teclado. Também desarma.">
      <div className="grid gap-3 sm:grid-cols-3">
        <input className={input} type="password" inputMode="numeric" placeholder="Atual" value={cur} onChange={e => setCur(digits(e.target.value))} />
        <input className={input} type="password" inputMode="numeric" placeholder="Nova (4 a 8 números)" value={nova} onChange={e => setNova(digits(e.target.value))} />
        <input className={input} type="password" inputMode="numeric" placeholder="Repita a nova" value={conf} onChange={e => setConf(digits(e.target.value))} />
      </div>
      {conf.length > 0 && nova !== conf && <p className="text-sm text-red-400 mt-2">As duas não batem.</p>}
      <button className={`${btnPrimary} mt-3`} disabled={!valid || busy}
              onClick={async () => { const r = await run('setMaster', { current: cur, newCode: nova }, 'Senha mestre alterada.');
                                     if (r.ok) { setCur(''); setNova(''); setConf(''); } }}>
        Trocar
      </button>
    </Section>
  );
};

// ---------------------------------------------------------------- sistema
const fmtUptime = (s: number) => {
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : `${m} min`;
};

const SystemSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify; onLogout: () => void }> = ({ data, send, notify, onLogout }) => {
  const s = data.system;
  const { busy, run } = useRun(send, notify);
  const rows: [string, string][] = [
    ['Versão', s.version], ['Endereço', s.ip], ['Sinal do Wi-Fi', `${s.rssi} dBm`],
    ['MQTT', s.mqtt ? 'conectado' : 'desconectado'], ['Ligada há', fmtUptime(s.uptimeS)],
    ['Memória livre', `${Math.round(s.freeHeap / 1024)} KB`],
  ];
  return (
    <Section title="Sistema">
      <dl className="grid grid-cols-2 gap-x-4 gap-y-1 text-sm">
        {rows.map(([k, v]) => (<React.Fragment key={k}><dt className="text-gray-400">{k}</dt><dd>{v}</dd></React.Fragment>))}
      </dl>
      <div className="flex flex-wrap gap-3 mt-4">
        <button className={btnGhost} disabled={busy} onClick={() => send('getAdmin')}>Atualizar</button>
        <button className={btnDanger} disabled={busy}
                onClick={() => window.confirm('Reiniciar a placa agora?') && run('reboot', {}, 'Reiniciando…')}>
          Reiniciar placa
        </button>
        <button className={btnGhost} onClick={onLogout}>Sair do admin</button>
      </div>
    </Section>
  );
};

// ---------------------------------------------------------------- eventos
const EventsSection: React.FC<{ send: SendCommand; notify: Notify }> = ({ send, notify }) => {
  const [events, setEvents] = useState<AlarmEvent[] | null>(null);
  const [busy, setBusy] = useState(false);
  const [all, setAll] = useState(false);
  const load = async () => {
    setBusy(true);
    const r = await send('getEvents');
    setBusy(false);
    if (r.ok) setEvents(r.events || []); else notify(r.error || 'Não deu para ler o registro.', false);
  };
  useEffect(() => { load(); }, []);
  const shown = events ? (all ? events : events.slice(0, 30)) : [];
  // Destaque para o que importa numa olhada rápida.
  const tone = (m: string) =>
    /DISPARO|COACAO/.test(m) ? 'text-red-400 font-semibold'
    : /errada|bloqueado|cancelado|aberta ha/i.test(m) ? 'text-amber-300'
    : /^Armado/.test(m) ? 'text-sky-300'
    : /^Desarmado/.test(m) ? 'text-emerald-300' : 'text-gray-300';

  return (
    <Section title="Eventos" hint="Os últimos 200, gravados na placa: continuam lá depois de reiniciar ou faltar energia.">
      <button className={btnGhost} disabled={busy} onClick={load}>{busy ? 'Carregando…' : 'Atualizar'}</button>
      {events && events.length === 0 && <p className="text-gray-400 mt-3">Nenhum evento ainda.</p>}
      {shown.length > 0 && (
        <ol className="mt-3 text-sm divide-y divide-gray-700/70">
          {shown.map((e, i) => (
            <li key={i} className="flex gap-3 py-1.5">
              <span className="text-gray-500 shrink-0 w-36 tabular-nums">
                {e.t ? new Date(e.t * 1000).toLocaleString('pt-BR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit' })
                     : 'sem horário'}
              </span>
              <span className={tone(e.m)}>{e.m}</span>
            </li>
          ))}
        </ol>
      )}
      {events && events.length > 30 && (
        <button className={`${btnGhost} mt-3`} onClick={() => setAll(!all)}>
          {all ? 'Mostrar só os 30 mais recentes' : `Mostrar todos (${events.length})`}
        </button>
      )}
    </Section>
  );
};

// ---------------------------------------------------------------- backup
const BackupSection: React.FC<{ data: AdminData; send: SendCommand; notify: Notify }> = ({ data, send, notify }) => {
  const [busy, setBusy] = useState(false);

  const download = async () => {
    setBusy(true);
    const r = await send('getBackup');
    setBusy(false);
    if (!r.ok || !r.backup) { notify(r.error || 'Não deu para gerar o backup.', false); return; }
    const blob = new Blob([JSON.stringify(r.backup, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    const dia = new Date().toISOString().slice(0, 10);
    a.download = `homeguard-backup-${data.network.mqttId}-${dia}.json`;
    a.click();
    URL.revokeObjectURL(a.href);
  };

  const restore = async (file: File) => {
    if (!window.confirm(`Restaurar "${file.name}"? Zonas, senhas, tempos e Telegram desta placa serão substituídos.`)) return;
    setBusy(true);
    try {
      const res = await fetch(`/api/restore?t=${data.token}`, { method: 'POST', body: await file.text(),
                                                               headers: { 'Content-Type': 'application/json' } });
      const r = await res.json();
      notify(r.ok ? 'Backup restaurado.' : r.error || 'A placa recusou o arquivo.', !!r.ok);
    } catch {
      notify('Não deu para enviar o arquivo.', false);
    }
    setBusy(false);
  };

  return (
    <Section title="Backup"
             hint="Zonas, senhas (com os valores), senha mestre, tempos, agenda e Telegram. Wi-Fi, MQTT e o nome da placa ficam de fora. Guarde o arquivo em lugar seguro.">
      <div className="flex flex-wrap gap-3">
        <button className={btnPrimary} disabled={busy} onClick={download}>Baixar backup</button>
        <label className={`${btnGhost} cursor-pointer ${busy ? 'opacity-50 pointer-events-none' : ''}`}>
          Restaurar de um arquivo…
          <input type="file" accept="application/json,.json" className="hidden"
                 onChange={e => { const f = e.target.files?.[0]; e.target.value = ''; if (f) restore(f); }} />
        </label>
      </div>
    </Section>
  );
};

// ---------------------------------------------------------------- atualização
const UpdateSection: React.FC<{ data: AdminData; notify: Notify }> = ({ data, notify }) => {
  const [progress, setProgress] = useState<number | null>(null);

  const upload = (file: File) => {
    if (!file.name.endsWith('.bin')) { notify('Escolha o arquivo .bin do firmware.', false); return; }
    if (!window.confirm(`Atualizar para "${file.name}"? A placa reinicia no fim (uns 10 s sem alarme). ` +
                        'Se a versão nova não subir, ela volta sozinha para a atual.')) return;
    const form = new FormData();
    form.append('firmware', file);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', `/api/ota?t=${data.token}`);
    xhr.upload.onprogress = e => e.lengthComputable && setProgress(Math.round((e.loaded / e.total) * 100));
    xhr.onload = () => {
      setProgress(null);
      let r: { ok?: boolean; error?: string } = {};
      try { r = JSON.parse(xhr.responseText); } catch { /* resposta vazia */ }
      notify(r.ok ? 'Firmware enviado. A placa está reiniciando…' : r.error || 'A placa recusou o arquivo.', !!r.ok);
    };
    xhr.onerror = () => { setProgress(null); notify('O envio caiu no meio. A versão atual continua.', false); };
    setProgress(0);
    xhr.send(form);
  };

  return (
    <Section title="Atualizar firmware" hint={`Versão atual: ${data.system.version}. O arquivo é o homeguard.ino.bin (firmware e página juntos).`}>
      {progress === null ? (
        <label className={`${btnPrimary} inline-block cursor-pointer`}>
          Escolher arquivo .bin…
          <input type="file" accept=".bin" className="hidden"
                 onChange={e => { const f = e.target.files?.[0]; e.target.value = ''; if (f) upload(f); }} />
        </label>
      ) : (
        <div>
          <div className="h-3 rounded bg-gray-700 overflow-hidden" role="progressbar" aria-valuenow={progress} aria-valuemin={0} aria-valuemax={100}>
            <div className="h-full bg-sky-500 transition-all" style={{ width: `${progress}%` }} />
          </div>
          <p className="text-sm text-gray-400 mt-1">{progress < 100 ? `Enviando… ${progress}%` : 'Gravando e conferindo…'}</p>
        </div>
      )}
    </Section>
  );
};

const AdminPanel: React.FC<AdminPanelProps> = ({ data, send, notify, onLogout }) => (
  <div className="max-w-3xl mx-auto grid gap-5">
    <ZonesSection data={data} send={send} notify={notify} />
    <TimingSection data={data} send={send} notify={notify} />
    <AutoArmSection data={data} send={send} notify={notify} />
    <CodesSection data={data} send={send} notify={notify} />
    <OutputsSection data={data} send={send} notify={notify} />
    <TelegramSection data={data} send={send} notify={notify} />
    <EventsSection send={send} notify={notify} />
    <NetworkSection data={data} send={send} notify={notify} />
    <MasterSection send={send} notify={notify} />
    <BackupSection data={data} send={send} notify={notify} />
    <UpdateSection data={data} notify={notify} />
    <SystemSection data={data} send={send} notify={notify} onLogout={onLogout} />
  </div>
);

export default AdminPanel;
