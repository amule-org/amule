// ED2K network info panel, shown as a bottom Networks tab next to the logs.
// Mirrors amuleGUI's ED2K info panel (ServerWnd UpdateED2KInfo): the eD2k
// connection status and the connection type (HighID/LowID). Driven entirely
// by the live SSE status_changed event (status.ed2k.*); amuleGUI also shows
// your own IP:Port and ED2K ID, but the amuleapi backend does not expose those.

import { data } from "../events.js";
import { html, useEffect, useStore } from "../dom.js";
import { t } from "../i18n.js";

export function Ed2kInfoPanel() {
  const status = useStore("status");
  useEffect(() => { data.ensureStatus(); }, []);

  const ed2k = (status && status.ed2k) || {};
  const connected = ed2k.state === "connected";

  return html`
    <div class="card">
      <div class="kad-grid">
        ${stat(t("networks_ed2k_status"), html`
          <span class=${"status-chip " + (connected ? "ok" : "off")}>
            ${connected ? t("networks_ed2k_connected") : t("networks_ed2k_not_connected")}
          </span>`)}
        ${stat(t("networks_ed2k_connection_type"),
          connected ? (ed2k.low_id ? t("networks_ed2k_low_id") : t("networks_ed2k_high_id")) : "—")}
      </div>
    </div>`;
}

function stat(label, value) {
  return html`
    <div class="kad-stat">
      <div class="kad-stat-label">${label}</div>
      <div class="kad-stat-value">${value}</div>
    </div>`;
}
