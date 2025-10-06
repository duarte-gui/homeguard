

export enum SectorState {
  Aberto = 'Aberto',
  Fechado = 'Fechado',
}

export interface Sector {
  id: number;
  name: string;
  status: SectorState;
}