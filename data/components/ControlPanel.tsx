
import React from 'react';
import { ShieldCheckIcon } from './icons/ShieldCheckIcon';
import { ShieldOffIcon } from './icons/ShieldOffIcon';
import { BellRingingIcon } from './icons/BellRingingIcon';
import { BellOffIcon } from './icons/BellOffIcon';
import { ArmMode, MODE_LABEL } from '../types';

interface ControlPanelProps {
  isArmed: boolean;
  isTriggered: boolean;
  isSirenActive: boolean;
  mode: ArmMode;
  exitRemaining: number;       // s de contagem de saída (0 = nenhuma)
  entryRemaining: number;      // s de contagem de entrada (0 = nenhuma)
  onArm: (mode: ArmMode) => void;  // livre
  onDisarm: () => void;        // abre o teclado: exige senha cadastrada
  onSilenceSiren: () => void;  // livre: o disparo continua até desarmar
}

const ControlPanel: React.FC<ControlPanelProps> = ({
  isArmed, isTriggered, isSirenActive, mode, exitRemaining, entryRemaining, onArm, onDisarm, onSilenceSiren,
}) => {
  let statusText = 'SISTEMA DESARMADO';
  let statusColor = 'text-green-400';
  let detail = '';
  if (isTriggered) {
    statusText = 'ALARME DISPARADO';
    statusColor = 'text-red-500 animate-pulse';
  } else if (entryRemaining > 0) {
    statusText = 'DESARME AGORA';
    statusColor = 'text-yellow-400 animate-pulse';
    detail = `Dispara em ${entryRemaining} s`;
  } else if (exitRemaining > 0) {
    statusText = 'SAINDO';
    statusColor = 'text-yellow-400';
    detail = `${MODE_LABEL[mode]}: arma em ${exitRemaining} s`;
  } else if (isArmed) {
    statusText = 'SISTEMA ARMADO';
    statusColor = 'text-red-500';
    detail = MODE_LABEL[mode];
  }

  const sirenStatusText = isSirenActive ? 'SIRENE ATIVADA' : 'Sirene Desligada';
  const sirenStatusColor = isSirenActive ? 'text-red-500 animate-pulse' : 'text-gray-400';
  const SirenIcon = isSirenActive ? BellRingingIcon : BellOffIcon;

  const bigButton = `
    flex items-center justify-center gap-3 w-full px-6 py-4
    text-lg font-bold text-white rounded-lg shadow-md
    transition-all duration-300 transform hover:scale-[1.03]
    focus:outline-none focus-visible:ring-2 focus-visible:ring-offset-2
    focus-visible:ring-offset-gray-900`;

  return (
    <div className="flex flex-col items-center gap-4 p-6 my-6 bg-gray-800/60 rounded-xl border border-gray-700 shadow-lg w-full max-w-md mx-auto">
      <div className="grid grid-cols-2 divide-x divide-gray-600 w-full text-center">
        <div className='px-2'>
          <p className="text-sm font-medium text-gray-400">Status do Sistema</p>
          <p className={`text-xl font-bold tracking-widest ${statusColor} transition-colors duration-300`}>
            {statusText}
          </p>
          {detail && <p className="text-sm text-gray-300 mt-1">{detail}</p>}
        </div>
        <div className="px-2">
           <p className="text-sm font-medium text-gray-400">Status da Sirene</p>
           <div className={`flex items-center justify-center gap-2 text-xl font-bold ${sirenStatusColor} transition-colors duration-300`}>
            <SirenIcon className="w-6 h-6" />
            <p>{sirenStatusText}</p>
           </div>
        </div>
      </div>

      <div className="w-full flex flex-col gap-3 mt-4">
        {isArmed || isTriggered ? (
          <button onClick={onDisarm} className={`${bigButton} bg-red-600 hover:bg-red-700 focus-visible:ring-red-500`}>
            <ShieldOffIcon className="w-6 h-6" />
            Desarmar Sistema
          </button>
        ) : (
          <>
            <button onClick={() => onArm('away')}
                    className={`${bigButton} bg-green-600 hover:bg-green-700 focus-visible:ring-green-500`}>
              <ShieldCheckIcon className="w-6 h-6" />
              Armar: {MODE_LABEL.away}
            </button>
            <button onClick={() => onArm('home')} title='Só as zonas marcadas "Em casa" no admin'
                    className={`${bigButton} py-3 text-base bg-emerald-800 hover:bg-emerald-700 focus-visible:ring-emerald-500`}>
              <ShieldCheckIcon className="w-5 h-5" />
              Armar: {MODE_LABEL.home}
            </button>
          </>
        )}

        {isSirenActive && (
          <button
            onClick={onSilenceSiren}
            className={`${bigButton} py-3 text-base bg-yellow-600 hover:bg-yellow-700 focus-visible:ring-yellow-500`}
          >
            <BellOffIcon className="w-6 h-6" />
            Desligar Sirene
          </button>
        )}
      </div>
    </div>
  );
};

export default ControlPanel;
