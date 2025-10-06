
import React from 'react';

const Header: React.FC = () => {
  return (
    <header className="text-center p-4 md:p-6">
      <h1 className="text-4xl md:text-5xl font-bold text-gray-100 tracking-tight">
        ESP32 Homeguard
      </h1>
      <p className="text-gray-400 mt-2">
        Interface de Monitoramento e Controle do Sistema de Alarme
      </p>
    </header>
  );
};

export default Header;