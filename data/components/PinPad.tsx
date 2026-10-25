import React, { useCallback, useEffect, useState } from 'react';
import { CommandResult } from '../types';

interface PinPadProps {
  title: string;
  subtitle?: string;
  onSubmit: (code: string) => Promise<CommandResult>;
  onClose: () => void;
}

// Teclado numérico em tela cheia: usado para desarmar e para entrar no admin.
// Também aceita o teclado físico (números, Backspace, Enter e Esc).
const PinPad: React.FC<PinPadProps> = ({ title, subtitle, onSubmit, onClose }) => {
  const [code, setCode] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const press = useCallback((d: string) => {
    setError(null);
    setCode(c => (c.length < 8 ? c + d : c));
  }, []);

  const back = useCallback(() => setCode(c => c.slice(0, -1)), []);

  const submit = useCallback(async () => {
    if (code.length < 4 || busy) return;
    setBusy(true);
    const r = await onSubmit(code);
    setBusy(false);
    if (r.ok) {
      onClose();
    } else {
      setError(r.error || 'Não deu certo.');
      setCode('');
    }
  }, [code, busy, onSubmit, onClose]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (/^[0-9]$/.test(e.key)) press(e.key);
      else if (e.key === 'Backspace') back();
      else if (e.key === 'Enter') submit();
      else if (e.key === 'Escape') onClose();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [press, back, submit, onClose]);

  const key = 'h-16 rounded-xl text-2xl font-semibold bg-gray-700/80 hover:bg-gray-600 active:bg-gray-500 ' +
              'transition-colors focus:outline-none focus-visible:ring-2 focus-visible:ring-sky-400';

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/70 backdrop-blur-sm p-4"
         role="dialog" aria-modal="true" aria-label={title}>
      <div className="w-full max-w-xs bg-gray-800 border border-gray-700 rounded-2xl shadow-2xl p-6">
        <h2 className="text-xl font-bold text-center">{title}</h2>
        {subtitle && <p className="text-sm text-gray-400 text-center mt-1">{subtitle}</p>}

        <div className="flex justify-center gap-3 my-6 h-4" aria-live="polite" aria-label={`${code.length} dígitos`}>
          {Array.from({ length: Math.max(4, code.length) }).map((_, i) => (
            <span key={i} className={`w-3.5 h-3.5 rounded-full ${i < code.length ? 'bg-sky-400' : 'bg-gray-600'}`} />
          ))}
        </div>

        <p className="text-center text-sm text-red-400 min-h-5 mb-3" role="alert">{error}</p>

        <div className="grid grid-cols-3 gap-3">
          {['1', '2', '3', '4', '5', '6', '7', '8', '9'].map(d => (
            <button key={d} className={key} onClick={() => press(d)} disabled={busy}>{d}</button>
          ))}
          <button className={key + ' text-lg'} onClick={back} disabled={busy} aria-label="Apagar">⌫</button>
          <button className={key} onClick={() => press('0')} disabled={busy}>0</button>
          <button
            className={`${key} text-lg ${code.length >= 4 ? 'bg-sky-600 hover:bg-sky-500' : 'opacity-50'}`}
            onClick={submit} disabled={busy || code.length < 4} aria-label="Confirmar">
            {busy ? '…' : 'OK'}
          </button>
        </div>

        <button onClick={onClose}
                className="w-full mt-4 py-2 text-gray-400 hover:text-white transition-colors">
          Cancelar
        </button>
      </div>
    </div>
  );
};

export default PinPad;
