

import React from 'react';

export const ShieldCheckIcon: React.FC<{ className?: string }> = ({ className }) => (
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
    <path d="M12 21.5l-3.3 -3.3a5 5 0 0 1 -1.4 -3.5v-4.2a5 5 0 0 1 1.4 -3.5l3.3 -3.3l3.3 3.3a5 5 0 0 1 1.4 3.5v4.2a5 5 0 0 1 -1.4 3.5l-3.3 3.3" />
    <path d="M15 9l-4.5 4.5" />
    <path d="M10.5 12l-1.5 1.5" />
  </svg>
);