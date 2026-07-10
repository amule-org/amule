// Inline SVG icon set. Monochrome line icons (stroke="currentColor") so they
// inherit the surrounding text colour and the active theme. No external
// assets — everything is inlined, which keeps the strict CSP happy.
//
//   import { Icon } from "../icons.js";
//   html`<${Icon} name="downloads" />`
//   html`<${Icon} name="connect" size=${16} title="Connect" />`

import { html } from "./dom.js";

// Each entry returns fresh vnodes (a function, not a shared vnode) so the same
// icon can be rendered in many places at once without preact reusing nodes.
const ICONS = {
  // --- navigation -------------------------------------------------------
  networks: () => html`<circle cx="12" cy="4.5" r="2.5"/><circle cx="5" cy="18" r="2.5"/><circle cx="19" cy="18" r="2.5"/><path d="M10.5 6.4 6.3 15.7M13.5 6.4l4.2 9.3M7.5 18h9"/>`,
  downloads: () => html`<path d="M12 3v10"/><path d="M8 9l4 4 4-4"/><path d="M4 17v2a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-2"/>`,
  search: () => html`<circle cx="11" cy="11" r="7"/><line x1="21" y1="21" x2="16.5" y2="16.5"/>`,
  shared: () => html`<circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><line x1="8.6" y1="10.7" x2="15.4" y2="6.3"/><line x1="8.6" y1="13.3" x2="15.4" y2="17.7"/>`,
  servers: () => html`<rect x="3" y="4" width="18" height="7" rx="1.5"/><rect x="3" y="13" width="18" height="7" rx="1.5"/><line x1="7" y1="7.5" x2="7.01" y2="7.5"/><line x1="7" y1="16.5" x2="7.01" y2="16.5"/>`,
  kad: () => html`<circle cx="12" cy="12" r="9"/><path d="M3 12h18"/><path d="M12 3a14 14 0 0 1 0 18"/><path d="M12 3a14 14 0 0 0 0 18"/>`,
  stats: () => html`<path d="M5 20V12"/><path d="M12 20V5"/><path d="M19 20V9"/><line x1="3" y1="20" x2="21" y2="20"/>`,
  logs: () => html`<line x1="5" y1="7" x2="19" y2="7"/><line x1="5" y1="12" x2="19" y2="12"/><line x1="5" y1="17" x2="14" y2="17"/>`,
  categories: () => html`<path d="M20.6 13.4 12 22l-9-9V3h10l7.6 7.6a2 2 0 0 1 0 2.8z"/><circle cx="7.5" cy="7.5" r="1.2" fill="currentColor"/>`,
  preferences: () => html`<circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9 7 7M17 17l2.1 2.1M19.1 4.9 17 7M7 17l-2.1 2.1"/>`,
  about: () => html`<circle cx="12" cy="12" r="9"/><line x1="12" y1="11" x2="12" y2="16"/><line x1="12" y1="8" x2="12.01" y2="8"/>`,

  // --- actions / status -------------------------------------------------
  menu: () => html`<line x1="4" y1="6" x2="20" y2="6"/><line x1="4" y1="12" x2="20" y2="12"/><line x1="4" y1="18" x2="20" y2="18"/>`,
  language: () => html`<path d="M4 5.5A1.5 1.5 0 0 1 5.5 4h13A1.5 1.5 0 0 1 20 5.5v9a1.5 1.5 0 0 1-1.5 1.5H12l-4 4v-4H5.5A1.5 1.5 0 0 1 4 14.5z"/><path d="M9.5 13.5 12 7l2.5 6.5M10.4 11.5h3.2"/>`,
  connect: () => html`<path d="M9 2v6M15 2v6M7 8h10v2a5 5 0 0 1-10 0z"/><path d="M12 15v7"/>`,
  cancel: () => html`<line x1="6" y1="6" x2="18" y2="18"/><line x1="18" y1="6" x2="6" y2="18"/>`,
  remove: () => html`<line x1="6" y1="6" x2="18" y2="18"/><line x1="18" y1="6" x2="6" y2="18"/>`,
  edit: () => html`<path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>`,
  pause: () => html`<rect x="6" y="5" width="4" height="14" rx="1"/><rect x="14" y="5" width="4" height="14" rx="1"/>`,
  play: () => html`<path d="M7 5l12 7-12 7z"/>`,
  stop: () => html`<rect x="6" y="6" width="12" height="12" rx="1"/>`,
  up: () => html`<path d="M12 19V5"/><path d="M5 12l7-7 7 7"/>`,
  down: () => html`<path d="M12 5v14"/><path d="M5 12l7 7 7-7"/>`,
  live: () => html`<circle cx="12" cy="12" r="5" fill="currentColor" stroke="none"/>`,
  polling: () => html`<path d="M21 12a9 9 0 1 1-3-6.7"/><path d="M21 4v5h-5"/>`,
  "sort-asc": () => html`<path d="M6 14l6-6 6 6"/>`,
  "sort-desc": () => html`<path d="M6 10l6 6 6-6"/>`,
  verified: () => html`<path d="M12 3l7 3v5c0 4.5-3 8-7 10-4-2-7-5.5-7-10V6z"/><path d="M9 12l2 2 4-4"/>`,
  warning: () => html`<path d="M12 3l9 16H3z"/><line x1="12" y1="10" x2="12" y2="14"/><line x1="12" y1="16.5" x2="12.01" y2="16.5"/>`,
  lock: () => html`<rect x="5" y="11" width="14" height="9" rx="1.5"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>`,
  star: () => html`<path d="M12 3l2.7 5.5 6 .9-4.3 4.2 1 6-5.4-2.8-5.4 2.8 1-6L3.3 9.4l6-.9z"/>`,
  logout: () => html`<path d="M15 4h4a1 1 0 0 1 1 1v14a1 1 0 0 1-1 1h-4"/><path d="M10 17l5-5-5-5"/><line x1="15" y1="12" x2="3" y2="12"/>`,

  // --- theme ------------------------------------------------------------
  "theme-system": () => html`<rect x="3" y="4" width="18" height="12" rx="1.5"/><path d="M8 20h8M12 16v4"/>`,
  "theme-light": () => html`<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5 19 19M19 5l-1.5 1.5M6.5 17.5 5 19"/>`,
  "theme-dark": () => html`<path d="M21 12.8A8 8 0 1 1 11.2 3a6 6 0 0 0 9.8 9.8z"/>`,
};

export function Icon({ name, size = 18, title, class: cls }) {
  const draw = ICONS[name];
  if (!draw) return null;
  return html`
    <svg class=${"icon" + (cls ? " " + cls : "")} width=${size} height=${size}
         viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
         stroke-linecap="round" stroke-linejoin="round"
         role=${title ? "img" : null} aria-hidden=${title ? null : "true"}>
      ${title ? html`<title>${title}</title>` : null}
      ${draw()}
    </svg>`;
}
