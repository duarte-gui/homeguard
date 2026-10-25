import React from 'react';
import { useState, useEffect, useRef, useCallback } from 'react';
import { AdminData, ArmMode, CommandResult, MODE_LABEL, Sector } from './types';
import { INITIAL_SECTORS } from './constants';
import Header from './components/Header';
import ControlPanel from './components/ControlPanel';
import SectorStatusCard from './components/SectorStatusCard';
import PinPad from './components/PinPad';
import AdminPanel from './components/AdminPanel';

// A URL do WebSocket agora aponta para o endpoint "/ws" no mesmo host,
// já que o ESPAsyncWebServer serve tudo na porta 80.
const WEBSOCKET_URL = `ws://${window.location.hostname}/ws`;
const COMMAND_TIMEOUT_MS = 8000;

interface Toast { id: number; msg: string; ok: boolean }

function App() {
  const [isArmed, setIsArmed] = useState<boolean>(false);
  const [sectors, setSectors] = useState<Sector[]>(INITIAL_SECTORS);
  const [isSirenActive, setIsSirenActive] = useState<boolean>(false);
  const [isTriggered, setIsTriggered] = useState<boolean>(false);
  const [triggeringSectorId, setTriggeringSectorId] = useState<number | null>(null);
  const [isConnected, setIsConnected] = useState<boolean>(false);
  const [mode, setMode] = useState<ArmMode>('away');
  const [boardIp, setBoardIp] = useState('');
  const [boardHost, setBoardHost] = useState('');
  // Fim das contagens no relógio do navegador: a placa manda os segundos
  // restantes só quando o estado muda, e a página conta sozinha.
  const [exitEndsAt, setExitEndsAt] = useState(0);
  const [entryEndsAt, setEntryEndsAt] = useState(0);
  const [now, setNow] = useState(Date.now());

  const [admin, setAdmin] = useState<AdminData | null>(null);
  const [view, setView] = useState<'painel' | 'admin'>('painel');
  const [pin, setPin] = useState<null | 'disarm' | 'admin'>(null);
  const [toasts, setToasts] = useState<Toast[]>([]);

  const websocket = useRef<WebSocket | null>(null);
  // Um comando pendente por nome; a placa responde {"type":"result","command":...}.
  const pending = useRef(new Map<string, (r: CommandResult) => void>());

  const failAllPending = (error: string) => {
    pending.current.forEach(resolve => resolve({ ok: false, error }));
    pending.current.clear();
  };

  useEffect(() => {
    console.log(`Tentando conectar ao WebSocket em: ${WEBSOCKET_URL}`);

    const connectWebSocket = () => {
      websocket.current = new WebSocket(WEBSOCKET_URL);

      websocket.current.onopen = () => {
        console.log('WebSocket Conectado');
        setIsConnected(true);
      };

      websocket.current.onclose = () => {
        console.log('WebSocket Desconectado. Tentando reconectar em 3 segundos...');
        setIsConnected(false);
        setAdmin(null);                 // a sessão de admin é presa à conexão
        failAllPending('A conexão com a placa caiu.');
        setTimeout(connectWebSocket, 3000);
      };

      websocket.current.onerror = (error) => {
        console.error('Erro no WebSocket:', error);
        websocket.current?.close();
      };

      websocket.current.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);

          if (data.type === 'result') {
            const resolve = pending.current.get(data.command);
            if (resolve) resolve({ ...data, ok: !!data.ok });
            if (!data.ok && typeof data.error === 'string' && data.error.startsWith('Sessão de administrador')) {
              setAdmin(null);
            }
            return;
          }
          if (data.type === 'admin') {
            setAdmin(data as AdminData);
            return;
          }

          // Estado do alarme ({"type":"state"} ou o formato antigo, sem tipo)
          if (typeof data.isArmed === 'boolean') setIsArmed(data.isArmed);
          if (typeof data.isSirenActive === 'boolean') setIsSirenActive(data.isSirenActive);
          if (typeof data.isTriggered === 'boolean') setIsTriggered(data.isTriggered);
          if (typeof data.triggeringSectorId === 'number') {
            setTriggeringSectorId(data.triggeringSectorId === 0 ? null : data.triggeringSectorId);
          }
          if (Array.isArray(data.sectors)) setSectors(data.sectors);
          if (typeof data.mode === 'string') setMode(data.mode as ArmMode);
          if (typeof data.ip === 'string') setBoardIp(data.ip);
          if (typeof data.hostname === 'string') setBoardHost(data.hostname);
          if (typeof data.exitRemaining === 'number') setExitEndsAt(data.exitRemaining ? Date.now() + data.exitRemaining * 1000 : 0);
          if (typeof data.entryRemaining === 'number') setEntryEndsAt(data.entryRemaining ? Date.now() + data.entryRemaining * 1000 : 0);
        } catch (e) {
          console.error('Erro ao processar mensagem do WebSocket:', e);
        }
      };
    };

    connectWebSocket();

    // Função de limpeza para fechar a conexão ao desmontar o componente
    return () => {
      if (websocket.current) {
        websocket.current.onclose = null; // Evita a tentativa de reconexão
        websocket.current.close();
      }
    };
  }, []);

  const send = useCallback((command: string, payload: object = {}): Promise<CommandResult> => {
    return new Promise(resolve => {
      const ws = websocket.current;
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        resolve({ ok: false, error: 'Sem conexão com a placa.' });
        return;
      }
      pending.current.get(command)?.({ ok: false, error: 'Comando repetido.' });
      let timer: ReturnType<typeof setTimeout>;
      const done = (r: CommandResult) => {
        clearTimeout(timer);
        if (pending.current.get(command) === done) pending.current.delete(command);
        resolve(r);
      };
      timer = setTimeout(() => done({ ok: false, error: 'A placa não respondeu.' }), COMMAND_TIMEOUT_MS);
      pending.current.set(command, done);
      ws.send(JSON.stringify({ command, ...payload }));
    });
  }, []);

  const notify = useCallback((msg: string, ok: boolean) => {
    const id = Date.now() + Math.random();
    setToasts(t => [...t, { id, msg, ok }]);
    setTimeout(() => setToasts(t => t.filter(x => x.id !== id)), ok ? 3500 : 6000);
  }, []);

  // Contagem regressiva na tela (só roda enquanto há contagem).
  useEffect(() => {
    if (!exitEndsAt && !entryEndsAt) return;
    const id = setInterval(() => setNow(Date.now()), 250);
    return () => clearInterval(id);
  }, [exitEndsAt, entryEndsAt]);
  const left = (endsAt: number) => (endsAt ? Math.max(0, Math.ceil((endsAt - now) / 1000)) : 0);

  const handleArm = useCallback(async (m: ArmMode) => {
    let r = await send('arm', { mode: m });
    if (!r.ok && r.openZones?.length) {
      // Zona instantânea aberta: arma mesmo assim só se a pessoa confirmar.
      const lista = r.openZones.join(', ');
      if (!window.confirm(`${lista} ${r.openZones.length > 1 ? 'estão abertas' : 'está aberta'}.\n\n` +
                          `Armar (${MODE_LABEL[m]}) mesmo assim? ${r.openZones.length > 1 ? 'Elas ficam inibidas' : 'Ela fica inibida'} até desarmar.`)) return;
      r = await send('arm', { mode: m, force: true });
    }
    if (!r.ok) notify(r.error || 'Não foi possível armar.', false);
  }, [send, notify]);

  const handleSilenceSiren = useCallback(() => {
    send('silenceSiren');
  }, [send]);

  const openAdmin = () => {
    if (view === 'admin') { setView('painel'); return; }
    if (admin) setView('admin'); else setPin('admin');
  };

  const logout = async () => {
    await send('adminLogout');
    setAdmin(null);
    setView('painel');
  };

  return (
    <div className="min-h-screen bg-gradient-to-br from-gray-900 to-slate-800 text-white font-sans">
      <main className="relative container mx-auto px-4 py-8">
        <Header view={view} onAdmin={openAdmin} ip={boardIp} hostname={boardHost} />

        {!isConnected && (
            <div className="text-center p-4 my-4 bg-yellow-800/80 border border-yellow-600 rounded-lg animate-pulse">
                <p className="font-bold text-lg">⚠️ Conexão Perdida</p>
                <p className="text-sm">Tentando reconectar ao ESP32...</p>
            </div>
        )}

        {view === 'admin' ? (
          admin ? (
            <AdminPanel data={admin} send={send} notify={notify} onLogout={logout} />
          ) : (
            <div className="max-w-md mx-auto text-center bg-gray-800/60 border border-gray-700 rounded-xl p-6">
              <p className="text-gray-300">A sessão de administrador terminou.</p>
              <button className="mt-4 px-4 py-2 rounded-lg font-semibold bg-sky-600 hover:bg-sky-500"
                      onClick={() => setPin('admin')}>
                Entrar de novo
              </button>
            </div>
          )
        ) : (
          <>
            <ControlPanel
              isArmed={isArmed}
              isTriggered={isTriggered}
              isSirenActive={isSirenActive}
              mode={mode}
              exitRemaining={left(exitEndsAt)}
              entryRemaining={left(entryEndsAt)}
              onArm={handleArm}
              onDisarm={() => setPin('disarm')}
              onSilenceSiren={handleSilenceSiren}
            />

            <div className="mt-10">
              <h2 className="text-2xl font-semibold text-center text-gray-300 mb-6">
                Status dos Setores
              </h2>
              <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4">
                {sectors.map(sector => (
                  <SectorStatusCard
                    key={sector.id}
                    sector={sector}
                    isTriggering={sector.id === triggeringSectorId}
                  />
                ))}
              </div>
            </div>
          </>
        )}
      </main>

      {pin === 'disarm' && (
        <PinPad title="Desarmar" subtitle="Digite uma senha cadastrada"
                onSubmit={code => send('disarm', { code })} onClose={() => setPin(null)} />
      )}
      {pin === 'admin' && (
        <PinPad title="Administração" subtitle="Digite a senha mestre"
                onSubmit={async code => {
                  const r = await send('adminLogin', { code });
                  if (r.ok) setView('admin');
                  return r;
                }}
                onClose={() => setPin(null)} />
      )}

      <div className="fixed bottom-4 inset-x-0 flex flex-col items-center gap-2 px-4 z-40 pointer-events-none" aria-live="polite">
        {toasts.map(t => (
          <div key={t.id}
               className={`max-w-md w-full px-4 py-3 rounded-lg shadow-lg text-sm ${t.ok ? 'bg-emerald-700' : 'bg-red-700'}`}>
            {t.msg}
          </div>
        ))}
      </div>

      <footer className="text-center p-4 text-gray-500 text-sm relative">
        <p>Desenvolvido para o sistema ESP32 Homeguard.</p>
      </footer>
    </div>
  );
}

export default App;
