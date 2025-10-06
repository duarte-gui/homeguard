

import React from 'react';

export const SensorClosedIcon: React.FC<{ className?: string }> = ({ className }) => (
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
    <path d="M20 12v-6a1 1 0 0 0 -1 -1h-16a1 1 0 0 0 -1 1v12a1 1 0 0 0 1 1h9" />
    <path d="M3 13h18" />
    <path d="M6 8v-2a1 1 0 0 1 1 -1h2a1 1 0 0 1 1 1v2" />
    <path d="M15 19l2 2l4 -4" />
  </svg>
);