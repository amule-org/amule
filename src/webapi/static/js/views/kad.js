// Kademlia panels, mirroring the aMule desktop layout:
//  - KadPanel: the Networks/Kad top tab — connect/disconnect + bootstrap
//    controls (no card wrapper, like the ED2K tab) plus the live Kad-nodes
//    graph (polled from /stats/graphs/kad while mounted).
//  - KadInfoPanel: the network status grid, shown as a bottom Networks tab
//    next to the logs. Live counters come from the SSE status_changed event
//    (status.kad.*); the extra detail fields (your IP, firewalled UDP, buddy)
//    are fetched from /kad on mount and whenever the Kad state changes — no
//    continuous polling.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { toast } from "../components.js";
import { Chart } from "../charts.js";
import { formatInt } from "../format.js";
import { t } from "../i18n.js";

const GRAPH_POLL_MS = 2000;
const GRAPH_WIDTH = 300; // samples per fetch (~chart pixel width; full window is ~1800)
const KAD_GRAPH = { name: "kad", title: t("networks_kad_nodes"), color: "#8a5cd6", fmt: formatInt };

// --- top tab: controls + graph -----------------------------------------
export function KadPanel() {
  const [graphData, setGraphData] = useState(null); // [xs, ys]
  const [node, setNode] = useState("");

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      try {
        const r = await api.get("stats/graphs/kad?width=" + GRAPH_WIDTH);
        const pts = r.points || [];
        if (alive) setGraphData([pts.map((p) => p.t_unix), pts.map((p) => p.value)]);
      } catch (_) { /* leave previous data */ }
    };
    tick();
    const timer = setInterval(tick, GRAPH_POLL_MS);
    return () => { alive = false; clearInterval(timer); };
  }, []);

  const netOp = async (op) => {
    try { await api.post("networks/" + op, { network: "kad" }); toast(op === "connect" ? t("networks_kad_toast_connecting") : t("networks_kad_toast_disconnecting"), "success"); }
    catch (e) { toast(e.message || t("networks_kad_error"), "error"); }
  };
  const bootstrap = async (e) => {
    e.preventDefault();
    const idx = node.lastIndexOf(":");
    const ipv = idx > 0 ? node.slice(0, idx).trim() : "";
    const portv = idx > 0 ? Number(node.slice(idx + 1)) : 0;
    if (!ipv || !portv) { toast(t("networks_kad_toast_enter_ip_port"), "warn"); return; }
    try { await api.post("kad/bootstrap", { ip: ipv, port: portv }); toast(t("networks_kad_toast_bootstrapping"), "success"); setNode(""); }
    catch (err) { toast(err.message || t("networks_kad_error"), "error"); }
  };

  return html`
    <form class="toolbar admin-only" onSubmit=${bootstrap}>
      <input class="input input-sm" placeholder=${t("networks_kad_ip_port_ph")} value=${node} onInput=${(e) => setNode(e.target.value)} />
      <button class="btn btn-sm" type="submit">${t("networks_kad_bootstrap_from_node")}</button>
      <div class="spacer"></div>
      <button class="btn btn-sm" type="button" onClick=${() => netOp("connect")}>${t("networks_kad_connect")}</button>
      <button class="btn btn-sm" type="button" onClick=${() => netOp("disconnect")}>${t("networks_kad_disconnect")}</button>
    </form>

    <${Chart} g=${KAD_GRAPH} data=${graphData} bare=${true} />`;
}

// --- bottom tab: network status grid ------------------------------------
export function KadInfoPanel() {
  const status = useStore("status");
  const kadState = status && status.kad && status.kad.state;
  const [detail, setDetail] = useState(null);
  const [error, setError] = useState("");

  const load = async () => {
    try { setDetail(await api.get("kad")); setError(""); }
    catch (e) { setError(e.message || t("networks_kad_error")); }
  };

  useEffect(() => { data.ensureStatus(); }, []);
  // Fetch the detail-only fields on mount and whenever the Kad state changes
  // (a connect/disconnect from the top tab arrives via the SSE status event).
  useEffect(() => { load(); }, [kadState]);

  const kad = (status && status.kad) || {};

  return html`
    <div class="card">
      ${error ? html`<p>${error}</p>` : statusGrid(kad, detail || {})}
    </div>`;
}

function statusGrid(kad, detail) {
  const connected = kad.state === "connected";
  const net = kad.network || {};
  const buddy = detail.buddy || {};
  const idx = detail.indexed || {};
  return html`
    <div class="kad-grid">
      ${stat(t("networks_kad_state"), html`<span class=${"status-chip " + (connected ? "ok" : "off")}>${kad.state ? t("networks_kad_conn_" + kad.state) : "—"}</span>`)}
      ${stat(t("networks_kad_firewalled_tcp"), yesno(kad.firewalled))}
      ${stat(t("networks_kad_firewalled_udp"), yesno(detail.firewalled_udp))}
      ${stat(t("networks_kad_in_lan_mode"), yesno(detail.in_lan_mode))}
      ${stat(t("networks_kad_your_ip"), detail.ip || "—")}
      ${stat(t("networks_kad_users"), formatInt(net.users))}
      ${stat(t("networks_kad_files"), formatInt(net.files))}
      ${stat(t("networks_kad_contacts_nodes"), formatInt(net.nodes))}
      ${stat(t("networks_kad_buddy"), buddy.status || "—")}
      ${stat(t("networks_kad_indexed_sources"), formatInt(idx.sources))}
      ${stat(t("networks_kad_indexed_keywords"), formatInt(idx.keywords))}
      ${stat(t("networks_kad_indexed_notes"), formatInt(idx.notes))}
      ${stat(t("networks_kad_indexed_load"), formatInt(idx.load))}
    </div>`;
}
function stat(label, value) {
  return html`
    <div class="kad-stat">
      <div class="kad-stat-label">${label}</div>
      <div class="kad-stat-value">${value}</div>
    </div>`;
}
function yesno(b) { return html`<span class=${"status-chip " + (b ? "warn" : "ok")}>${b ? t("networks_kad_yes") : t("networks_kad_no")}</span>`; }
