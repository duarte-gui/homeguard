
import React from 'react';
import { ShieldCheckIcon } from './icons/ShieldCheckIcon';
import { ShieldOffIcon } from './icons/ShieldOffIcon';
import { BellRingingIcon } from './icons/BellRingingIcon';
import { BellOffIcon } from './icons/BellOffIcon';

interface ControlPanelProps {
  isArmed: boolean;
  onToggleArm: () => void;
  isSirenActive: boolean;
  onSilenceSiren: () => void;
}

const ControlPanel: React.FC<ControlPanelProps> = ({ isArmed, onToggleArm, isSirenActive, onSilenceSiren }) => {
  const statusText = isArmed ? 'SISTEMA ARMADO' : 'SISTEMA DESARMADO';
  const statusColor = isArmed ? 'text-red-500' : 'text-green-400';
  const buttonText = isArmed ? 'Desarmar Sistema' : 'Armar Sistema';
  const buttonColor = isArmed
    ? 'bg-red-600 hover:bg-red-700 focus-visible:ring-red-500'
    : 'bg-green-600 hover:bg-green-700 focus-visible:ring-green-500';
  const Icon = isArmed ? ShieldCheckIcon : ShieldOffIcon;

  const sirenStatusText = isSirenActive ? 'SIRENE ATIVADA' : 'Sirene Desligada';
  const sirenStatusColor = isSirenActive ? 'text-red-500 animate-pulse' : 'text-gray-400';
  const SirenIcon = isSirenActive ? BellRingingIcon : BellOffIcon;

  return (
    <div className="flex flex-col items-center gap-4 p-6 my-6 bg-gray-800/60 rounded-xl border border-gray-700 shadow-lg w-full max-w-md mx-auto">
      <div className="grid grid-cols-2 divide-x divide-gray-600 w-full text-center">
        <div className='px-2'>
          <p className="text-sm font-medium text-gray-400">Status do Sistema</p>
          <p className={`text-xl font-bold tracking-widest ${statusColor} transition-colors duration-300`}>
            {statusText}
          </p>
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
        <button
          onClick={onToggleArm}
          className={`
            flex items-center justify-center gap-3 w-full px-8 py-4
            text-lg font-bold text-white rounded-lg shadow-md
            transition-all duration-300 transform hover:scale-105
            focus:outline-none focus-visible:ring-2 focus-visible:ring-offset-2
            focus-visible:ring-offset-gray-900
            ${buttonColor}
          `}
        >
          <Icon className="w-6 h-6" />
          {buttonText}
        </button>

        {isSirenActive && (
          <button
            onClick={onSilenceSiren}
            className={`
              flex items-center justify-center gap-3 w-full px-8 py-3
              text-base font-bold text-white rounded-lg shadow-md
              bg-yellow-600 hover:bg-yellow-700 focus-visible:ring-yellow-500
              transition-all duration-300 transform hover:scale-105
              focus:outline-none focus-visible:ring-2 focus-visible:ring-offset-2
              focus-visible:ring-offset-gray-900
            `}
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