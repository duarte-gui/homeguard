

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
          <h3 className="font-semibold text-gray-200">{sector.name}</h3>
          <div className="flex items-center gap-2">
            {isTriggering && <BellRingingIcon className="w-5 h-5 text-red-500 animate-pulse" />}
            <Icon className={`w-8 h-8 ${statusColor}`} />
          </div>
        </div>
        <p className={`text-2xl font-bold ${statusColor}`}>{sector.status}</p>
      </div>
    </div>
  );
};

export default SectorStatusCard;