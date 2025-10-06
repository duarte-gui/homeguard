

import React from 'react';

export const ShieldOffIcon: React.FC<{ className?: string }> = ({ className }) => (
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
    <path d="M12 21.5l-3.3 -3.3a5 5 0 0 1 -1.4 -3.5v-4.2c0 -1.336 .526 -2.6 1.464 -3.536l2.836 -2.864" />
    <path d="M16.7 4.7l3.3 3.3a5 5 0 0 1 1.4 3.5v4.2a5 5 0 0 1 -1.4 3.5l-3.3 3.3" />
    <path d="M3 3l18 18" />
  </svg>
);