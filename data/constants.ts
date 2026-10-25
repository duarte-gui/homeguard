

import { Sector } from './types';
import { SectorState } from './types';

// Só até chegar o primeiro estado da placa, que traz os nomes configurados.
export const INITIAL_SECTORS: Sector[] = [
  { id: 1, name: 'Portão Garagem', status: SectorState.Fechado },
  { id: 2, name: 'Presença Garagem', status: SectorState.Fechado },
  { id: 3, name: 'Portão Principal', status: SectorState.Fechado },
  { id: 4, name: 'Porta Gui', status: SectorState.Fechado },
];
