

import React from 'react';

export const SensorOpenIcon: React.FC<{ className?: string }> = ({ className }) => (
  <svg
    xmlns="http://www.w3.org/2000/svg"
    className={className}
    width="24"
    height="24"
    viewBox="0 0 24 24"
    strokeWidth="2"
    stroke="currentColor"
    fill="none"
    strokeLinecap="round"
    strokeLinejoin="round"
  >
    <path stroke="none" d="M0 0h24v24H0z" fill="none" />
    <path d="M21 12.6v-3.6a1 1 0 0 0 -1 -1h-16a1 1 0 0 0 -1 1v10a1 1 0 0 0 1 1h8" />
    <path d="M3 13h18" />
    <path d="M6 8v-2a1 1 0 0 1 1 -1h2a1 1 0 0 1 1 1v2" />
    <path d="M19 16l-2 3h4l-2 3" />
  </svg>
);