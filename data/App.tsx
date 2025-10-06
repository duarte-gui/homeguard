

import React from 'react';
import { useState, useEffect, useRef, useCallback } from 'react';
import { Sector, SectorState } from './types';
import { INITIAL_SECTORS } from './constants';
import Header from './components/Header';
import ControlPanel from './components/ControlPanel';
import SectorStatusCard from './components/SectorStatusCard';

// A URL do WebSocket agora aponta para o endpoint "/ws" no mesmo host,
// já que o ESPAsyncWebServer serve tudo na porta 80.
const WEBSOCKET_URL = `ws://${window.location.hostname}/ws`;

function App() {
  const [isArmed, setIsArmed] = useState<boolean>(false);
  const [sectors, setSectors] = useState<Sector[]>(INITIAL_SECTORS);
  const [isSirenActive, setIsSirenActive] = useState<boolean>(false);
  const [triggeringSectorId, setTriggeringSectorId] = useState<number | null>(null);
  const [isConnected, setIsConnected] = useState<boolean>(false);

  const websocket = useRef<WebSocket | null>(null);

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
        setTimeout(connectWebSocket, 3000);
      };

      websocket.current.onerror = (error) => {
        console.error('Erro no WebSocket:', error);
        websocket.current?.close();
      };

      websocket.current.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          console.log('Dados recebidos do ESP32:', data);

          // Atualiza o estado da UI com os dados vindos do ESP32
          if (typeof data.isArmed === 'boolean') {
            setIsArmed(data.isArmed);
          }
          if (typeof data.isSirenActive === 'boolean') {
            setIsSirenActive(data.isSirenActive);
          }
          if (typeof data.triggeringSectorId === 'number') {
            setTriggeringSectorId(data.triggeringSectorId === 0 ? null : data.triggeringSectorId);
          }
          if (Array.isArray(data.sectors)) {
            setSectors(data.sectors);
          }
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

  const sendCommand = useCallback((command: string, payload: object = {}) => {
    if (websocket.current && websocket.current.readyState === WebSocket.OPEN) {
      const message = JSON.stringify({ command, ...payload });
      websocket.current.send(message);
      console.log(`Comando enviado para o ESP32: ${message}`);
    } else {
      console.error('WebSocket não está conectado. Não foi possível enviar o comando.');
    }
  }, []);

  const handleToggleArm = useCallback(() => {
    sendCommand('toggleArm');
  }, [sendCommand]);

  const handleSilenceSiren = useCallback(() => {
    sendCommand('silenceSiren');
  }, [sendCommand]);

  return (
    <div className="min-h-screen bg-gradient-to-br from-gray-900 to-slate-800 text-white font-sans">
      <main className="relative container mx-auto px-4 py-8">
        <Header />
        
        {!isConnected && (
            <div className="text-center p-4 my-4 bg-yellow-800/80 border border-yellow-600 rounded-lg animate-pulse">
                <p className="font-bold text-lg">⚠️ Conexão Perdida</p>
                <p className="text-sm">Tentando reconectar ao ESP32...</p>
            </div>
        )}

        <ControlPanel
          isArmed={isArmed}
          onToggleArm={handleToggleArm}
          isSirenActive={isSirenActive}
          onSilenceSiren={handleSilenceSiren}
        />
        
        <div className="mt-10">
          <h2 className="text-2xl font-semibold text-center text-gray-300 mb-6">
            Status dos Setores
          </h2>
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-5 gap-4">
            {sectors.map(sector => (
              <SectorStatusCard
                key={sector.id}
                sector={sector}
                isTriggering={sector.id === triggeringSectorId}
              />
            ))}
          </div>
        </div>
      </main>
      <footer className="text-center p-4 text-gray-500 text-sm relative">
        <p>Desenvolvido para o sistema ESP32 Homeguard.</p>
      </footer>
    </div>
  );
}

export default App;