// Networks view: mirrors the aMule desktop "Networks" page (NetDialog) — a top
// notebook with the ED2K server list and Kad, and a bottom notebook with the
// aMule log, server info and the two per-network info grids.
//
// One file on purpose: every panel below is used only by this page, so keeping
// them together means the lazy route import (see app.js RouteView) is a single
// request instead of a five-file waterfall.

import { api } from "../api.js";
import { data } from "../events.js";
import { store } from "../store.js";
import { html, useState, useEffect, useRef, useStore } from "../dom.js";
import { Tabs, Placeholder, toast, confirmDialog } from "../components.js";
import { VirtualTable, sortRows, useTablePrefs, ColumnPicker, ipNum } from "../table.js";
import { Chart } from "../charts.js";
import { formatInt } from "../format.js";
import { Icon } from "../icons.js";
import { t, terr } from "../i18n.js";
import { useSplitHeight } from "./split-detail.js";

const SRV_POLL_MS = 5000;
const AMULE_TAIL = 500; // initial history; live lines then arrive via log_appended
const GRAPH_POLL_MS = 2000;
const GRAPH_WIDTH = 300; // samples per fetch (~chart pixel width; full window is ~1800)
const KAD_GRAPH = { name: "kad", title: t("networks_kad_nodes"), color: "#8a5cd6", fmt: formatInt };
// The three values ServerPriorityCode() accepts, in rank order (also the sort order).
const SERVER_PRIORITIES = ["low", "normal", "high"].map((v) => [v, t("networks_server_prio_" + v)]);

export default function Networks({ isGuest }) {
  const [top, setTop] = useState("ed2k");
  const [bottom, setBottom] = useState("amulelog");
  // Draggable divider like the desktop NetDialog's wxSplitterWindow. On phones
  // the CSS hides it and both panes flow (see .net-split in app.css).
  const { height, containerRef, splitterProps } = useSplitHeight("net:split");

  const topTabs = [
    { key: "ed2k", label: t("networks_tab_ed2k") },
    { key: "kad", label: t("networks_tab_kad") },
  ];
  const bottomTabs = [
    { key: "amulelog", label: t("networks_tab_amule_log") },
    { key: "serverinfo", label: t("networks_tab_server_info") },
    { key: "ed2kinfo", label: t("networks_tab_ed2k_info") },
    { key: "kadinfo", label: t("networks_tab_kad_info") },
  ];

  return html`
    <div class="net-split fill-view" ref=${containerRef}>
      <section class="net-pane pane-fill">
        <${Tabs} tabs=${topTabs} active=${top} onSelect=${setTop} />
        <div class="net-pane-body">
          ${top === "ed2k"
            ? html`<${ServersPanel} isGuest=${isGuest} />`
            : html`<${KadPanel} />`}
        </div>
      </section>

      <div class="splitter" ...${splitterProps}></div>

      <section class="net-pane" style=${{ height: height + "px" }}>
        <${Tabs} tabs=${bottomTabs} active=${bottom} onSelect=${setBottom} />
        <div class="net-pane-body">
          ${bottom === "amulelog"
            ? html`<${AmuleLogPanel} />`
            : bottom === "serverinfo"
            ? html`<${ServerInfoPanel} />`
            : bottom === "ed2kinfo"
            ? html`<${Ed2kInfoPanel} />`
            : html`<${KadInfoPanel} />`}
        </div>
      </section>
    </div>`;
}

// --- connect toggle, shared by both top tabs -----------------------------
// aMule-style coloured plug (red/amber/green) for one network. Colour and label
// follow the real state from the SSE status_changed event; the click does the
// opposite. ed2k idles as "disconnected", Kad as "disabled" — both read red.
// Kad reports "connecting" while running-but-not-routing, which still has to be
// stoppable, so "connecting" counts as up (matches CKadDlg).
function NetworkConnectButton({ network }) {
  const status = useStore("status");
  const state = (status && status[network] && status[network].state) || "disconnected";
  const cls = state === "connected" ? "connected"
            : state === "connecting" ? "connecting" : "disconnected";
  const up = cls !== "disconnected";
  const label = t("networks_tab_" + network) + ": " + t("app_" + cls);

  const toggle = async () => {
    try {
      await api.post("networks/" + (up ? "disconnect" : "connect"), { network });
      toast(t(up ? "app_toast_disconnecting" : "app_toast_connecting"), "success");
    } catch (e) { toast(terr(e) || t("app_error"), "error"); }
  };

  return html`
    <button type="button" class=${"btn btn-sm conn-btn admin-only " + cls}
            title=${label} onClick=${toggle}>
      <${Icon} name="connect" /> ${label}
    </button>`;
}

// --- ED2K tab: server list, live via the SSE "servers" channel ------------
function ServersPanel({ isGuest }) {
  const servers = useStore("servers") || [];
  const status = useStore("status");
  const ed2k = status && status.ed2k;
  const { sortKey, sortDir, hidden, widths, toggleSort, toggleCol, setWidth, resetPrefs } =
    useTablePrefs("servers", { sortKey: "users", sortDir: -1,
                               hidden: ["address", "version", "ping"] });
  const [addr, setAddr] = useState("");
  const [name, setName] = useState("");
  const [connectingEcid, setConnectingEcid] = useState(null);

  useEffect(() => {
    data.register({ key: "servers", eventPrefix: "server", id: "ecid",
      list: () => api.get("servers").then((r) => r.servers || []) });
    data.ensure("servers");
  }, []);

  // Drop the optimistic "connecting" row once ed2k leaves that state, however
  // it left it (connected here, connected elsewhere, or failed).
  useEffect(() => {
    if (ed2k && ed2k.state !== "connecting") setConnectingEcid(null);
  }, [ed2k && ed2k.state]);

  const connect = async (ecid) => {
    setConnectingEcid(ecid);
    try { await api.post("servers/" + ecid + "/connect"); toast(t("networks_server_toast_connecting"), "success"); }
    catch (e) { setConnectingEcid(null); toast(terr(e) || t("networks_server_error"), "error"); }
  };
  const remove = async (s) => {
    if (!(await confirmDialog(t("networks_server_confirm_remove", { name: s.name })))) return;
    try { await api.del("servers/" + s.ecid); data.refresh("servers"); }
    catch (e) { toast(terr(e) || t("networks_server_error"), "error"); }
  };
  const addServer = async (e) => {
    e.preventDefault();
    const address = addr.trim();
    if (!address) { toast(t("networks_server_toast_enter_host_port"), "warn"); return; }
    const body = { address };
    if (name.trim()) body.name = name.trim();
    try { await api.post("servers", body); setAddr(""); setName(""); toast(t("networks_server_toast_added"), "success"); data.refresh("servers"); }
    catch (err) { toast(terr(err) || t("networks_server_error"), "error"); }
  };
  const patchServer = async (ecid, body) => {
    try { await api.patch("servers/" + ecid, body); data.refresh("servers"); }
    catch (e) { toast(terr(e) || t("networks_server_error"), "error"); }
  };

  // The two sides format the address differently — the list ships "ip:port",
  // status.ed2k.server_ip comes from EC_IPv4_t::StringIP() as "[ip:port]" — so
  // compare the dotted quad pulled out of each, plus the port.
  const ipv4 = (v) => { const m = String(v || "").match(/\d+\.\d+\.\d+\.\d+/); return m ? m[0] : ""; };
  const isConnected = (s) =>
    ed2k && ed2k.state === "connected"
    && ipv4(ed2k.server_ip) !== "" && ipv4(ed2k.server_ip) === ipv4(s.address)
    && ed2k.server_port === s.port;

  const columns = [
    // Server host country (#440): same cell and header as the peer table's.
    { key: "country", label: t("networks_server_country"), width: "52px", sortable: true,
      sortVal: (s) => s.country_code || "", cell: (s) => (s.country_code || "").toUpperCase() || "—" },
    { key: "address", label: t("networks_server_address"), num: true, width: "180px", sortable: true,
      sortVal: (s) => ipNum(s.address),
      cell: (s) => s.address && s.address.includes(":") ? s.address : (s.address + ":" + s.port) },
    { key: "name", label: t("networks_server_name"), cls: "name", sortable: true,
      sortVal: (s) => (s.name || "").toLowerCase(),
      // flex cell so a long name ellipsizes
      cell: (s) => html`<div class="name-cell" title=${s.name}><span class="name-text">${s.name}</span></div>` },
    // No width, like `name`: descriptions are long, so the two split the leftover.
    { key: "description", label: t("networks_server_description"), sortable: true,
      sortVal: (s) => (s.description || "").toLowerCase(), cell: (s) => s.description || "" },
    { key: "users", label: t("networks_server_users"), num: true, width: "130px", sortable: true,
      sortVal: (s) => s.users || 0,
      cell: (s) => formatInt(s.users) + (s.max_users ? " / " + formatInt(s.max_users) : "") },
    { key: "files", label: t("networks_server_files"), num: true, width: "110px", sortable: true,
      sortVal: (s) => s.files || 0, cell: (s) => formatInt(s.files) },
    { key: "version", label: t("networks_server_version"), width: "90px", sortable: true,
      sortVal: (s) => s.version || "", cell: (s) => s.version || "" },
    { key: "ping", label: t("networks_server_ping"), num: true, width: "90px", sortable: true,
      sortVal: (s) => s.ping_ms || 0, cell: (s) => s.ping_ms ? s.ping_ms + " ms" : "—" },
    // Static and priority are both PATCH /servers/{ecid} fields, so both cells are
    // selects for an admin and plain labels for a guest (as in downloads/shared).
    { key: "static", label: t("networks_server_static"), width: "90px", sortable: true,
      sortVal: (s) => (s.static ? 1 : 0),
      cell: (s) => isGuest
        ? (s.static ? t("networks_server_static_yes") : t("networks_server_static_no"))
        : html`
            <select class="input input-sm admin-only" value=${s.static ? "yes" : "no"}
                    onChange=${(e) => patchServer(s.ecid, { static: e.target.value === "yes" })}>
              <option value="yes">${t("networks_server_static_yes")}</option>
              <option value="no">${t("networks_server_static_no")}</option>
            </select>` },
    { key: "priority", label: t("networks_server_priority"), width: "110px", sortable: true,
      sortVal: (s) => SERVER_PRIORITIES.findIndex(([v]) => v === s.priority),
      cell: (s) => {
        const found = SERVER_PRIORITIES.find(([v]) => v === s.priority);
        return isGuest
          ? (found ? found[1] : s.priority || "")
          : html`
              <select class="input input-sm admin-only" value=${found ? s.priority : "normal"}
                      onChange=${(e) => patchServer(s.ecid, { priority: e.target.value })}>
                ${SERVER_PRIORITIES.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
              </select>`;
      } },
    ...(isGuest ? [] : [{ key: "actions", label: t("networks_server_actions"), cls: "row-actions admin-only", width: "90px",
      cell: (s) => html`
        <button class="btn btn-icon btn-sm" title=${t("networks_server_connect")} onClick=${() => connect(s.ecid)}>
          <${Icon} name="connect" />
        </button>
        <button class="btn btn-icon btn-sm btn-danger" title=${t("networks_server_remove")} onClick=${() => remove(s)}>
          <${Icon} name="remove" />
        </button>` }]),
  ];

  const list = sortRows(servers, columns, sortKey, sortDir);
  const shown = columns.filter((c) => !c.key || !hidden.has(c.key));
  const rowClass = (s) => isConnected(s) ? "connected" : connectingEcid === s.ecid ? "connecting" : "";

  return html`
    <div class="server-toolbars admin-only">
      <form class="toolbar admin-only" onSubmit=${addServer}>
        <input class="input input-sm" placeholder=${t("networks_server_host_port_ph")} value=${addr} onInput=${(e) => setAddr(e.target.value)} />
        <input class="input input-sm" placeholder=${t("networks_server_name_ph")} value=${name} onInput=${(e) => setName(e.target.value)} />
        <button class="btn btn-sm" type="submit">${t("networks_server_add")}</button>
        <div class="spacer"></div>
        <${NetworkConnectButton} network="ed2k" />
        <${ColumnPicker} columns=${columns} hidden=${hidden} onToggle=${toggleCol} onReset=${resetPrefs} />
      </form>
    </div>
    <${VirtualTable} columns=${shown} rows=${list} rowKey=${(s) => s.ecid} rowClass=${rowClass}
                     sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                     widths=${widths} onResize=${setWidth}
                     empty=${html`<${Placeholder} kind="info">${t("networks_server_empty")}<//>`} />`;
}

// --- Kad tab: connect toggle, bootstrap, live nodes graph -----------------
function KadPanel() {
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

  const bootstrap = async (e) => {
    e.preventDefault();
    const idx = node.lastIndexOf(":");
    const ipv = idx > 0 ? node.slice(0, idx).trim() : "";
    const portv = idx > 0 ? Number(node.slice(idx + 1)) : 0;
    if (!ipv || !portv) { toast(t("networks_kad_toast_enter_ip_port"), "warn"); return; }
    try { await api.post("kad/bootstrap", { ip: ipv, port: portv }); toast(t("networks_kad_toast_bootstrapping"), "success"); setNode(""); }
    catch (err) { toast(terr(err) || t("networks_kad_error"), "error"); }
  };

  return html`
    <form class="toolbar admin-only" onSubmit=${bootstrap}>
      <input class="input input-sm" placeholder=${t("networks_kad_ip_port_ph")} value=${node} onInput=${(e) => setNode(e.target.value)} />
      <button class="btn btn-sm" type="submit">${t("networks_kad_bootstrap_from_node")}</button>
      <div class="spacer"></div>
      <${NetworkConnectButton} network="kad" />
    </form>

    <${Chart} g=${KAD_GRAPH} data=${graphData} bare=${true} />`;
}

// --- bottom tabs: logs ---------------------------------------------------
// The <pre> boxes are written imperatively via refs so incoming lines can be
// appended while keeping the scroll stuck to the bottom; preact never owns them.

const atBottom = (box) => box.scrollHeight - box.scrollTop - box.clientHeight < 30;
const append = (box, text) => {
  const stick = atBottom(box);
  box.appendChild(document.createTextNode(text));
  if (stick) box.scrollTop = box.scrollHeight;
};

function logBox(clear, boxRef, extraCls) {
  return html`
    <div class="logbox-wrap">
      <button class="btn admin-only logbox-clear" onClick=${clear}>${t("networks_log_clear")}</button>
      <pre class=${"logbox" + (extraCls ? " " + extraCls : "")} ref=${boxRef}></pre>
    </div>`;
}

// Live-appended from the SSE log_appended event.
function AmuleLogPanel() {
  const boxRef = useRef(null);

  const load = async () => {
    try {
      const r = await api.get("logs/amule?tail=" + AMULE_TAIL);
      if (!boxRef.current) return;
      boxRef.current.textContent = (r.lines || []).join("");
      boxRef.current.scrollTop = boxRef.current.scrollHeight;
    } catch (e) { if (boxRef.current) boxRef.current.textContent = terr(e) || t("networks_log_error"); }
  };
  const clear = async () => {
    if (!(await confirmDialog(t("networks_log_confirm_clear_amule")))) return;
    try { await api.del("logs/amule"); if (boxRef.current) boxRef.current.textContent = ""; toast(t("networks_log_toast_cleared"), "success"); }
    catch (e) { toast(terr(e) || t("networks_log_error"), "error"); }
  };

  useEffect(() => {
    let lastSeen = store.get("log:appended");
    const unsub = store.subscribe("log:appended", (v) => {
      if (v === lastSeen) return;
      lastSeen = v;
      if (!boxRef.current) return;
      for (const line of (v.lines || [])) append(boxRef.current, line);
    });
    load();
    return () => unsub();
  }, []);

  return logBox(clear, boxRef);
}

// No SSE channel for this one — polled.
function ServerInfoPanel() {
  const boxRef = useRef(null);

  const load = async () => {
    try {
      const r = await api.get("logs/serverinfo");
      if (!boxRef.current) return;
      boxRef.current.textContent = r.text || "";
      boxRef.current.scrollTop = boxRef.current.scrollHeight;
    } catch (e) { if (boxRef.current) boxRef.current.textContent = terr(e) || t("networks_log_error"); }
  };
  const clear = async () => {
    if (!(await confirmDialog(t("networks_log_confirm_clear_serverinfo")))) return;
    try { await api.del("logs/serverinfo"); if (boxRef.current) boxRef.current.textContent = ""; toast(t("networks_log_toast_cleared"), "success"); }
    catch (e) { toast(terr(e) || t("networks_log_error"), "error"); }
  };

  useEffect(() => {
    load();
    const timer = setInterval(load, SRV_POLL_MS);
    return () => clearInterval(timer);
  }, []);

  return logBox(clear, boxRef, "logbox-sm");
}

// --- bottom tabs: per-network info grids ---------------------------------

function stat(label, value) {
  return html`
    <div class="kad-stat">
      <div class="kad-stat-label">${label}</div>
      <div class="kad-stat-value">${value}</div>
    </div>`;
}
function yesno(b) { return html`<span class=${"status-chip " + (b ? "warn" : "ok")}>${b ? t("networks_kad_yes") : t("networks_kad_no")}</span>`; }

// Mirrors amuleGUI's ServerWnd UpdateED2KInfo, minus your own IP:Port and ED2K
// ID — the amuleapi backend does not expose those.
function Ed2kInfoPanel() {
  const status = useStore("status");
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

// Live counters ride the SSE status event; the detail-only fields (your IP,
// firewalled UDP, buddy) are fetched from /kad on mount and whenever the Kad
// state changes — a connect/disconnect from the top tab arrives that way.
function KadInfoPanel() {
  const status = useStore("status");
  const kadState = status && status.kad && status.kad.state;
  const [detail, setDetail] = useState(null);
  const [error, setError] = useState("");

  useEffect(() => {
    (async () => {
      try { setDetail(await api.get("kad")); setError(""); }
      catch (e) { setError(terr(e) || t("networks_kad_error")); }
    })();
  }, [kadState]);

  const kad = (status && status.kad) || {};
  const net = kad.network || {};
  const d = detail || {};
  const buddy = d.buddy || {};
  const idx = d.indexed || {};

  return html`
    <div class="card">
      ${error ? html`<p>${error}</p>` : html`
        <div class="kad-grid">
          ${stat(t("networks_kad_state"), html`<span class=${"status-chip " + (kad.state === "connected" ? "ok" : "off")}>${kad.state ? t("networks_kad_conn_" + kad.state) : "—"}</span>`)}
          ${stat(t("networks_kad_firewalled_tcp"), yesno(kad.firewalled))}
          ${stat(t("networks_kad_firewalled_udp"), yesno(d.firewalled_udp))}
          ${stat(t("networks_kad_in_lan_mode"), yesno(d.in_lan_mode))}
          ${stat(t("networks_kad_your_ip"), d.ip || "—")}
          ${stat(t("networks_kad_users"), formatInt(net.users))}
          ${stat(t("networks_kad_files"), formatInt(net.files))}
          ${stat(t("networks_kad_contacts_nodes"), formatInt(net.nodes))}
          ${stat(t("networks_kad_buddy"), buddy.status || "—")}
          ${stat(t("networks_kad_indexed_sources"), formatInt(idx.sources))}
          ${stat(t("networks_kad_indexed_keywords"), formatInt(idx.keywords))}
          ${stat(t("networks_kad_indexed_notes"), formatInt(idx.notes))}
          ${stat(t("networks_kad_indexed_load"), formatInt(idx.load))}
        </div>`}
    </div>`;
}
