
export enum SectorState {
  Aberto = 'Aberto',
  Fechado = 'Fechado',
}

export interface Sector {
  id: number;
  name: string;
  status: SectorState;
  bypass?: boolean;           // inibida: aparece, mas não dispara
  tempBypass?: boolean;       // inibida só até desarmar (armou com ela aberta)
  is24h?: boolean;            // dispara mesmo desarmado
}

export type ArmMode = 'away' | 'home';

export const MODE_LABEL: Record<ArmMode, string> = {
  away: 'Ausente',
  home: 'Em casa',
};

export interface AlarmEvent {
  t: number;                  // epoch (s); 0 = antes do relógio acertar
  m: string;
}

// Resposta da placa a um comando ({"type":"result",...}).
export interface WifiNetwork {
  ssid: string;
  rssi: number;
  open: boolean;
}

export interface CommandResult {
  ok: boolean;
  error?: string;
  networks?: WifiNetwork[];   // resposta do scanWifi
  openZones?: string[];       // arm recusado: zonas instantâneas abertas
  events?: AlarmEvent[];      // getEvents
  backup?: object;            // getBackup
}

export type SendCommand = (command: string, payload?: object) => Promise<CommandResult>;

export interface AdminCode {
  label: string;
  digits: number;
  duress?: boolean;
}

export interface AdminZone {
  id: number;
  name: string;
  defaultName: string;
  bypass: boolean;
  type: ZoneType;
  home: boolean;              // ativa no modo "Em casa"
}

// Mesma numeração do firmware (config.h).
export enum ZoneType { Instant = 0, Delayed = 1, Follower = 2, H24 = 3 }

// Tudo o que a página de admin mostra ({"type":"admin",...}). Senhas e tokens
// nunca vêm: só se já existem.
export interface AdminData {
  token: string;              // vale para o envio do firmware e do backup
  codes: AdminCode[];
  zones: AdminZone[];
  timing: { entryDelaySec: number; exitDelaySec: number; openAlertMin: number };
  autoArm: { enabled: boolean; minute: number; days: number; mode: ArmMode; tz: string };
  telegram: { chatId: string; hasToken: boolean };
  network: { ssid: string; hostname: string; defaultHostname: string;
             mqttId: string; defaultMqttId: string; deviceName: string; defaultDeviceName: string; mqttHost: string; mqttPort: number; mqttUser: string; hasMqttPass: boolean;
             ipMode: 'dhcp' | 'static'; ip: string; gateway: string; subnet: string; dns: string;
             current: { ip: string; gateway: string; subnet: string; dns: string; mac: string } };
  siren: { enabled: boolean; timeoutSec: number };
  system: { version: string; ip: string; rssi: number; uptimeS: number; freeHeap: number; mqtt: boolean; clock: number };
}
