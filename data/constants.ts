

import { Sector } from './types';
import { SectorState } from './types';

export const INITIAL_SECTORS: Sector[] = [
  { id: 1, name: 'Setor 1 - Portão Garagem', status: SectorState.Fechado },
  { id: 2, name: 'Setor 2 - Presença Garagem', status: SectorState.Fechado },
  { id: 3, name: 'Setor 3 - Portão Principal', status: SectorState.Fechado },
  { id: 4, name: 'Setor 4 - Porta Gui', status: SectorState.Fechado },
];