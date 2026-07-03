// Shared UI building blocks: small presentational preact components plus
// the two imperative helpers (toast, confirmDialog) reused across views.
// Class names match app.css.

import { html, render } from "./dom.js";
import { formatPercent } from "./format.js";
import { t } from "./i18n.js";

// --- presentational components -----------------------------------------

// Horizontal progress bar (0..100).
export function ProgressBar({ percent }) {
  const p = Math.max(0, Math.min(100, Number(percent) || 0));
  return html`
    <div class="progress" style=${{ "--p": p }}>
      <span class="progress-label">${formatPercent(p)}</span>
      <div class="progress-fill">
        <span class="progress-label progress-label-fill">${formatPercent(p)}</span>
      </div>
    </div>`;
}

// Inline status/label pill.
export function Badge({ kind = "", title, children }) {
  return html`<span class=${"badge " + kind} title=${title}>${children}</span>`;
}

// Empty / loading / error placeholder for view bodies.
export function Placeholder({ kind, children }) {
  return html`<div class=${"placeholder placeholder-" + kind}>${children}</div>`;
}

// Flat notebook-style tab strip (aMule CMuleNotebook look). `tabs` is a list
// of { key, label, badge? }; `active` is the selected key; `onSelect(key)` is
// called on click.
export function Tabs({ tabs, active, onSelect }) {
  return html`
    <div class="tabs" role="tablist">
      ${tabs.map((tab) => html`
        <button type="button" role="tab"
                class=${"tab" + (tab.key === active ? " active" : "")}
                aria-selected=${tab.key === active}
                onClick=${() => onSelect(tab.key)}>
          ${tab.label}
          ${tab.badge != null ? html`<span class="tab-badge">${tab.badge}</span>` : null}
        </button>`)}
    </div>`;
}

// --- toast --------------------------------------------------------------
// Lightweight imperative notifications. A single host div is lazily created
// and reused; toasts auto-dismiss (errors linger a bit longer).

let toastHost = null;
export function toast(message, kind) {
  if (!toastHost) {
    toastHost = document.createElement("div");
    toastHost.className = "toast-host";
    document.body.appendChild(toastHost);
  }
  const node = document.createElement("div");
  node.className = "toast " + (kind || "info");
  node.textContent = message;
  toastHost.appendChild(node);
  setTimeout(() => {
    node.classList.add("toast-out");
    setTimeout(() => node.remove(), 300);
  }, kind === "error" ? 5000 : 3000);
}

// --- confirm dialog -----------------------------------------------------
// Promise-based modal confirm. Renders a preact tree into a throwaway host
// appended to <body>, resolves true/false, then unmounts and cleans up.

export function confirmDialog(message, { okLabel = t("common_yes"), cancelLabel = t("common_cancel") } = {}) {
  return new Promise((resolve) => {
    const host = document.createElement("div");
    document.body.appendChild(host);
    const close = (val) => { render(null, host); host.remove(); resolve(val); };
    render(html`
      <div class="modal-overlay" onClick=${(e) => { if (e.target === e.currentTarget) close(false); }}>
        <div class="modal">
          <p class="modal-msg">${message}</p>
          <div class="modal-actions">
            <button class="btn" onClick=${() => close(false)}>${cancelLabel}</button>
            <button class="btn btn-primary" onClick=${() => close(true)}>${okLabel}</button>
          </div>
        </div>
      </div>`, host);
  });
}
