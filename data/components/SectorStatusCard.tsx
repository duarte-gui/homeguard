

import React from 'react';
import { Sector } from '../types';
import { SectorState } from '../types';
import { SensorClosedIcon } from './icons/SensorClosedIcon';
import { SensorOpenIcon } from './icons/SensorOpenIcon';
import { BellRingingIcon } from './icons/BellRingingIcon';

interface SectorStatusCardProps {
  sector: Sector;
  isTriggering: boolean;
}

const SectorStatusCard: React.FC<SectorStatusCardProps> = ({ sector, isTriggering }) => {
  const isClosed = sector.status === SectorState.Fechado;

  const statusColor = isClosed ? 'text-green-400' : 'text-red-500';
  const Icon = isClosed ? SensorClosedIcon : SensorOpenIcon;
  
  const cardHighlight = isTriggering
    ? 'border-red-500 ring-2 ring-red-500/50 shadow-lg shadow-red-500/20'
    : 'border-gray-700';
  const bgColor = isClosed ? 'bg-gray-800/50' : 'bg-red-900/40';

  return (
    <div
      className={`
        w-full
        p-4 rounded-lg border backdrop-blur-sm transition-all duration-300
        flex flex-col justify-between
        ${bgColor} ${cardHighlight}
      `}
    >
      <div>
        <div className="flex items-center justify-between mb-2">
          <div>
            <p className="text-xs uppercase tracking-wider text-gray-500">Zona {sector.id}</p>
            <h3 className="font-semibold text-gray-200">{sector.name}</h3>
          </div>
          <div className="flex items-center gap-2">
            {isTriggering && <BellRingingIcon className="w-5 h-5 text-red-500 animate-pulse" />}
            <Icon className={`w-8 h-8 ${statusColor}`} />
          </div>
        </div>
        <p className={`text-2xl font-bold ${statusColor}`}>{sector.status}</p>
        <div className="flex flex-wrap gap-2 mt-2">
          {sector.bypass && (
            <span className="text-xs px-2 py-0.5 rounded bg-gray-600/80 text-gray-200">Inibida: não dispara</span>
          )}
          {sector.tempBypass && (
            <span className="text-xs px-2 py-0.5 rounded bg-amber-700/70 text-amber-100">Inibida até desarmar</span>
          )}
          {sector.is24h && (
            <span className="text-xs px-2 py-0.5 rounded bg-purple-800/70 text-purple-100">24 horas</span>
          )}
        </div>
      </div>
    </div>
  );
};

export default SectorStatusCard;