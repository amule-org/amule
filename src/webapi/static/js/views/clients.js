// Clients view: every connected peer (both directions) in one table with a
// Todos / Descargas / Subidas selector. Data comes from the live `clients`
// store (api.get("clients") + SSE client_added/updated/removed) which already
// carries all peers regardless of transfer direction, so filtering is purely
// client-side. Renders every field GET /api/v0/clients exposes.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Badge, Placeholder, Tabs } from "../components.js";
import { VirtualTable, sortRows, textMatcher } from "../table.js";
import { formatBytes, formatSpeed } from "../format.js";
import { Icon } from "../icons.js";
import { t } from "../i18n.js";

const ACTIVE = (s) => s && s !== "idle" && s !== "unknown";
const isDown = (c) => (c.download_speed_bps || 0) > 0 || ACTIVE(c.download_state);
const isUp = (c) => (c.upload_speed_bps || 0) > 0 || ACTIVE(c.upload_state);

// Column set, declared once with the tabs each column shows in. The "all" tab
// keeps the shared columns plus a condensed state/speed/total per direction;
// each single-direction tab adds that direction's extra detail columns.
const ALL = ["all", "downloads", "uploads"];
const DL = ["all", "downloads"];
const UL = ["all", "uploads"];
const softLabel = (c) => [c.software ? t("downloads_peer_soft_" + c.software) : "", c.software === "unknown" ? "" : c.software_version].filter(Boolean).join(" ") || "—";
const rankLabel = (c) => !c.remote_queue_rank ? "—" : c.remote_queue_rank >= 0xFFFF ? t("downloads_peer_queue_full") : c.remote_queue_rank;
const bytesOf = (c, k) => formatBytes((c.xfer || {})[k]);

// Each column carries key + sortVal so the header is clickable-to-sort (the
// flags column has no key → stays non-sortable).
const COLS = [
  { cls: "peer-flags", width: "60px", show: ALL, cell: (c) => peerFlags(c) },
  { key: "name", th: "downloads_peer_col_name", width: "170px", show: ALL, sortable: true,
    sortVal: (c) => (c.client_name || "").toLowerCase(),
    cell: (c) => html`<span title=${c.client_name}>${c.client_name || "—"}</span>` },
  { key: "software", th: "downloads_peer_col_software", width: "140px", show: ALL, sortable: true,
    sortVal: (c) => softLabel(c).toLowerCase(), cell: (c) => softLabel(c) },
  { key: "file", th: "downloads_peer_col_file", cls: "name", show: ALL, sortable: true,
    sortVal: (c) => (c.download_file_name || "").toLowerCase(),
    cell: (c) => html`<span title=${c.download_file_name}>${c.download_file_name || "—"}</span>` },

  { key: "dl_state", th: "downloads_peer_col_dl_state", width: "120px", show: DL, sortable: true,
    sortVal: (c) => c.download_state || "", cell: (c) => stateBadge(c.download_state) },
  { key: "dl_speed", th: "downloads_peer_col_dl_speed", num: true, width: "100px", show: DL, sortable: true,
    sortVal: (c) => c.download_speed_bps || 0, cell: (c) => formatSpeed(c.download_speed_bps) },
  { key: "downloaded", th: "downloads_peer_col_downloaded", num: true, width: "100px", show: DL, sortable: true,
    sortVal: (c) => (c.xfer && c.xfer.down_total) || 0, cell: (c) => bytesOf(c, "down_total") },
  { key: "dl_session", th: "downloads_peer_col_downloaded_session", num: true, width: "110px", show: ["downloads"], sortable: true,
    sortVal: (c) => (c.xfer && c.xfer.down_session) || 0, cell: (c) => bytesOf(c, "down_session") },
  { key: "remote_rank", th: "downloads_peer_col_remote_rank", num: true, width: "90px", show: ["downloads"], sortable: true,
    sortVal: (c) => c.remote_queue_rank || 0, cell: (c) => rankLabel(c) },

  { key: "ul_state", th: "downloads_peer_col_ul_state", width: "120px", show: UL, sortable: true,
    sortVal: (c) => c.upload_state || "", cell: (c) => stateBadge(c.upload_state) },
  { key: "ul_speed", th: "downloads_peer_col_ul_speed", num: true, width: "100px", show: UL, sortable: true,
    sortVal: (c) => c.upload_speed_bps || 0, cell: (c) => formatSpeed(c.upload_speed_bps) },
  { key: "uploaded", th: "downloads_peer_col_uploaded", num: true, width: "100px", show: UL, sortable: true,
    sortVal: (c) => (c.xfer && c.xfer.up_total) || 0, cell: (c) => bytesOf(c, "up_total") },
  { key: "ul_session", th: "downloads_peer_col_uploaded_session", num: true, width: "110px", show: ["uploads"], sortable: true,
    sortVal: (c) => (c.xfer && c.xfer.up_session) || 0, cell: (c) => bytesOf(c, "up_session") },
  { key: "queue_pos", th: "downloads_peer_col_queue_pos", num: true, width: "90px", show: ["uploads"], sortable: true,
    sortVal: (c) => c.queue_waiting_position || 0, cell: (c) => c.queue_waiting_position || "—" },
  { key: "score", th: "downloads_peer_col_score", num: true, width: "80px", show: ["uploads"], sortable: true,
    sortVal: (c) => c.score || 0, cell: (c) => c.score || "—" },
];

const IDENT_FILTERS = ["all", "identified", "not_identified"].map((v) => [v, t("downloads_peer_ident_" + v)]);

export default function ClientsPanel() {
  const clients = useStore("clients") || [];
  const [filter, setFilter] = useState("all"); // direction tab: all / downloads / uploads
  const [ident, setIdent] = useState("identified");
  const [q, setQ] = useState("");
  const [sortKey, setSortKey] = useState(null); // null → default sort (busiest first)
  const [sortDir, setSortDir] = useState(1);

  useEffect(() => {
    data.register({ key: "clients", eventPrefix: "client", id: "client_ecid",
      list: () => api.get("clients").then((r) => r.clients || []) });
    data.ensure("clients");
  }, []);

  const toggleSort = (key) => {
    if (sortKey === key) setSortDir(-sortDir);
    else { setSortKey(key); setSortDir(1); }
  };

  const nDown = clients.filter(isDown).length;
  const nUp = clients.filter(isUp).length;

  let list = clients.slice();
  if (filter === "downloads") list = list.filter(isDown);
  else if (filter === "uploads") list = list.filter(isUp);
  if (ident === "identified") list = list.filter((c) => c.ident_state === "identified");
  else if (ident === "not_identified") list = list.filter((c) => c.ident_state !== "identified");
  if (q) { const match = textMatcher(q); list = list.filter((c) => match((c.client_name || "") + " " + (c.download_file_name || ""))); }

  const tabs = [
    { key: "all", label: t("downloads_peer_all"), badge: clients.length },
    { key: "downloads", label: t("downloads_peer_download"), badge: nDown },
    { key: "uploads", label: t("downloads_peer_upload"), badge: nUp },
  ];

  const columns = COLS.filter((col) => col.show.includes(filter))
    .map((col) => ({ ...col, label: col.th ? t(col.th) : "" }));

  // Sort by the chosen column when set (and visible in this tab); otherwise keep
  // the default "busiest peers first" order (combined dl+ul speed, descending).
  if (columns.some((c) => c.key === sortKey && c.sortVal)) {
    list = sortRows(list, columns, sortKey, sortDir);
  } else {
    list.sort((a, b) =>
      ((b.download_speed_bps || 0) + (b.upload_speed_bps || 0)) -
      ((a.download_speed_bps || 0) + (a.upload_speed_bps || 0)));
  }

  return html`
    <div class="view-header">
      <h3 class="section-title">${t("app_nav_clients")}</h3>
    </div>
    <section class="net-pane">
      <${Tabs} tabs=${tabs} active=${filter} onSelect=${setFilter} />
      <div class="net-pane-body">
        <div class="toolbar pane-toolbar">
          <label>${t("downloads_peer_identity")}:</label>
          <select class="input input-sm" value=${ident} onChange=${(e) => setIdent(e.target.value)}>
            ${IDENT_FILTERS.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
          <span>${t("downloads_peer_filter")}:</span>
          <input class="input input-sm" type="text" value=${q} onInput=${(e) => setQ(e.target.value)} />
        </div>
        <${VirtualTable} columns=${columns} rows=${list} rowKey=${(c) => c.client_ecid}
                         sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                         empty=${html`<${Placeholder} kind="info">${t("downloads_peer_empty")}<//>`} />
      </div>
    </section>`;
}

// Compact status icons (replacing the ident/obfuscation/friend columns). Each
// icon carries an explanatory tooltip; only meaningful states show an icon.
function peerFlags(c) {
  const flags = [];
  if (c.ident_state === "identified")
    flags.push(["verified", t("downloads_peer_ident") + ": " + t("downloads_peer_identified")]);
  else if (c.ident_state === "bad_guy")
    flags.push(["warning", t("downloads_peer_ident") + ": " + t("downloads_peer_bad_guy")]);
  if (c.obfuscation_status === "enabled")
    flags.push(["lock", t("downloads_peer_obfuscation") + ": " + t("downloads_peer_enabled")]);
  if (c.friend_slot)
    flags.push(["star", t("downloads_peer_friend")]);
  return flags.map(([name, tip]) => html`<${Icon} name=${name} size=${18} title=${tip} />`);
}

function stateBadge(s) {
  if (!s || s === "idle") return html`<${Badge}>${t("downloads_peer_state_" + (s || "idle"))}<//>`;
  const kind = s === "downloading" || s === "uploading" ? "downloading"
    : s === "banned" || s === "error" ? "paused" : "waiting";
  return html`<${Badge} kind=${kind}>${t("downloads_peer_state_" + s)}<//>`;
}
