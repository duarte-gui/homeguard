import React from 'react';

interface HeaderProps {
  view: 'painel' | 'admin';
  onAdmin: () => void;
  ip: string;
  hostname: string;
}

const Header: React.FC<HeaderProps> = ({ view, onAdmin, ip, hostname }) => {
  return (
    <header className="text-center p-4 md:p-6 relative">
      <button
        onClick={onAdmin}
        className="absolute right-0 top-0 md:top-2 px-3 py-2 rounded-lg text-sm font-semibold
                   bg-gray-800/70 border border-gray-700 hover:bg-gray-700 transition-colors
                   focus:outline-none focus-visible:ring-2 focus-visible:ring-sky-400"
      >
        {view === 'admin' ? '← Painel' : '⚙ Admin'}
      </button>
      <h1 className="text-4xl md:text-5xl font-bold text-gray-100 tracking-tight">
        ESP32 Homeguard
      </h1>
      <p className="text-gray-400 mt-2">
        {view === 'admin' ? 'Administração' : 'Interface de Monitoramento e Controle do Sistema de Alarme'}
      </p>
      {ip && (
        <p className="text-xs text-gray-500 mt-1 font-mono">
          {ip}{hostname && ` · ${hostname}.local`}
        </p>
      )}
    </header>
  );
};

export default Header;
