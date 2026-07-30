// Peer table shared by the Clients section and the per-file "Clients" tab in
// the Downloads / Shared Files detail panels. Columns, formatting helpers and
// the live `clients` store wiring live here once; each consumer decides which
// columns it shows by default and which rows it feeds in.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Badge, Placeholder, CountryCell } from "../components.js";
import { VirtualTable, sortRows, textMatcher, useTablePrefs, ColumnPicker, ipNum } from "../table.js";
import { formatBytes, formatSpeed } from "../format.js";
import { Icon } from "../icons.js";
import { t } from "../i18n.js";

const ACTIVE = (s) => s && s !== "idle" && s !== "unknown";
export const isDown = (c) => (c.download_speed_bps || 0) > 0 || ACTIVE(c.download_state);
export const isUp = (c) => (c.upload_speed_bps || 0) > 0 || ACTIVE(c.upload_state);

// Column set, declared once. Every consumer offers all of them in the column
// picker and picks which ones start hidden (see ClientTable's defaultHidden),
// so a column is always one click away instead of missing from a tab.
const softLabel = (c) => [c.software ? t("downloads_peer_soft_" + c.software) : "", c.software === "unknown" ? "" : c.software_version].filter(Boolean).join(" ") || "—";
const rankLabel = (c) => !c.remote_queue_rank ? "—" : c.remote_queue_rank >= 0xFFFF ? t("downloads_peer_queue_full") : c.remote_queue_rank;
const bytesOf = (c, k) => formatBytes((c.xfer || {})[k]);
// The "file" column is shared by both directions: download_file_name is the
// peer-advertised name of what we're pulling FROM them; upload_file_name is
// the partfile they're pulling FROM us. An upload-only peer has no
// download_file_name, so falling back to upload_file_name is what actually
// makes the column non-blank for uploads (previously always "—" there).
export const fileNameOf = (c) => c.download_file_name || c.upload_file_name || "";

// Default order when no column sort is chosen: busiest peers first.
export const bySpeed = (a, b) =>
  ((b.download_speed_bps || 0) + (b.upload_speed_bps || 0)) -
  ((a.download_speed_bps || 0) + (a.upload_speed_bps || 0));

// Each column carries key + sortVal so the header is clickable-to-sort (the
// flags column has no key → stays non-sortable).
export const COLS = [
  { cls: "peer-flags", width: "60px", cell: (c) => peerFlags(c) },
  // Identity block, each field next to the one it qualifies: where the peer is
  // (country, address), who it claims (name, user_hash), what it runs (software, os).
  // Abbreviated header: a spelled-out "Country" would still be wider than the cell.
  { key: "country", th: "downloads_peer_col_country", width: "70px", sortable: true,
    sortVal: (c) => c.country_code || "", cell: (c) => html`<${CountryCell} code=${c.country_code} />` },
  // Empty ip: a peer we never connected to directly (LowID).
  { key: "address", th: "downloads_peer_col_address", num: true, width: "180px", sortable: true,
    sortVal: (c) => ipNum(c.ip), cell: (c) => c.ip ? c.ip + ":" + c.port : "—" },
  { key: "name", th: "downloads_peer_col_name", width: "170px", sortable: true,
    sortVal: (c) => (c.client_name || "").toLowerCase(),
    cell: (c) => html`<span title=${c.client_name}>${c.client_name || "—"}</span>` },
  { key: "user_hash", th: "downloads_peer_col_user_hash", width: "150px", sortable: true,
    sortVal: (c) => c.user_hash || "",
    cell: (c) => html`<span title=${c.user_hash}>${c.user_hash || "—"}</span>` },
  { key: "ident", th: "downloads_peer_col_ident", width: "130px", sortable: true,
    sortVal: (c) => identLabel(c.ident_state).toLowerCase(), cell: (c) => identBadge(c.ident_state) },
  { key: "software", th: "downloads_peer_col_software", width: "140px", sortable: true,
    sortVal: (c) => softLabel(c).toLowerCase(), cell: (c) => softLabel(c) },
  // The peer's own self-reported OS string -- frequently empty.
  { key: "os", th: "downloads_peer_col_os", width: "110px", sortable: true,
    sortVal: (c) => (c.os_info || "").toLowerCase(), cell: (c) => c.os_info || "—" },
  { key: "file", th: "downloads_peer_col_file", cls: "name", sortable: true,
    sortVal: (c) => fileNameOf(c).toLowerCase(),
    cell: (c) => html`<span title=${fileNameOf(c)}>${fileNameOf(c) || "—"}</span>` },

  { key: "dl_state", th: "downloads_peer_col_dl_state", width: "120px", sortable: true,
    sortVal: (c) => c.download_state || "", cell: (c) => stateBadge(c.download_state) },
  { key: "dl_speed", th: "downloads_peer_col_dl_speed", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.download_speed_bps || 0, cell: (c) => formatSpeed(c.download_speed_bps) },
  { key: "downloaded", th: "downloads_peer_col_downloaded", num: true, width: "100px", sortable: true,
    sortVal: (c) => (c.xfer && c.xfer.down_total) || 0, cell: (c) => bytesOf(c, "down_total") },
  { key: "dl_session", th: "downloads_peer_col_downloaded_session", num: true, width: "100px", sortable: true,
    sortVal: (c) => (c.xfer && c.xfer.down_session) || 0, cell: (c) => bytesOf(c, "down_session") },
  { key: "remote_rank", th: "downloads_peer_col_remote_rank", num: true, width: "90px", sortable: true,
    sortVal: (c) => c.remote_queue_rank || 0, cell: (c) => rankLabel(c) },

  { key: "ul_state", th: "downloads_peer_col_ul_state", width: "120px", sortable: true,
    sortVal: (c) => c.upload_state || "", cell: (c) => stateBadge(c.upload_state) },
  { key: "ul_speed", th: "downloads_peer_col_ul_speed", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.upload_speed_bps || 0, cell: (c) => formatSpeed(c.upload_speed_bps) },
  { key: "uploaded", th: "downloads_peer_col_uploaded", num: true, width: "100px", sortable: true,
    sortVal: (c) => (c.xfer && c.xfer.up_total) || 0, cell: (c) => bytesOf(c, "up_total") },
  { key: "ul_session", th: "downloads_peer_col_uploaded_session", num: true, width: "100px", sortable: true,
    sortVal: (c) => (c.xfer && c.xfer.up_session) || 0, cell: (c) => bytesOf(c, "up_session") },
  { key: "queue_pos", th: "downloads_peer_col_queue_pos", num: true, width: "90px", sortable: true,
    sortVal: (c) => c.queue_waiting_position || 0, cell: (c) => c.queue_waiting_position || "—" },
  { key: "score", th: "downloads_peer_col_score", num: true, width: "80px", sortable: true,
    sortVal: (c) => c.score || 0, cell: (c) => c.score || "—" },
];

// Raw-detail columns no consumer leads with; each adds its own defaultHidden set
// on top of these.
export const HIDDEN_EVERYWHERE = ["address", "os", "user_hash", "ident"];

// 1:1 with ClientIdentStateName() in src/webapi/Refresher.cpp.
export const IDENT_STATES = ["identified", "not_available", "id_needed", "id_failed", "bad_guy", "unknown"];
export const identLabel = (s) => t("downloads_peer_ident_" + (s || "unknown"));
export const IDENT_FILTERS = ["all", ...IDENT_STATES].map((v) => [v, t("downloads_peer_ident_" + v)]);

// Live `clients` collection (GET /clients seed + SSE client_added/updated/
// removed). register/ensure are idempotent, so every consumer can just call
// this; the resource starts on the first mount and stays live from then on.
export function useClients() {
  useEffect(() => {
    data.register({ key: "clients", eventPrefix: "client", id: "client_ecid",
      list: () => api.get("clients").then((r) => r.clients || []) });
    data.ensure("clients");
  }, []);
  return useStore("clients") || [];
}

// Compact status icons (replacing the ident/obfuscation/friend columns). Each
// icon carries an explanatory tooltip; only meaningful states show an icon.
export function peerFlags(c) {
  const flags = [];
  const identTip = () => t("downloads_peer_ident") + ": " + identLabel(c.ident_state);
  // bad_guy / id_failed both mean "this peer's identity is wrong"; the other
  // states are just an absence of SecIdent and earn no icon.
  if (c.ident_state === "identified") flags.push(["verified", identTip()]);
  else if (c.ident_state === "bad_guy" || c.ident_state === "id_failed") flags.push(["warning", identTip()]);
  if (c.obfuscation_status === "enabled")
    flags.push(["lock", t("downloads_peer_obfuscation") + ": " + t("downloads_peer_enabled")]);
  if (c.friend_slot)
    flags.push(["star", t("downloads_peer_friend")]);
  return flags.map(([name, tip]) => html`<${Icon} name=${name} size=${18} title=${tip} />`);
}

const IDENT_KIND = { identified: "downloading", id_failed: "stopped", bad_guy: "stopped", id_needed: "waiting" };
export function identBadge(s) {
  return html`<${Badge} kind=${IDENT_KIND[s] || "paused"}>${identLabel(s)}<//>`;
}

export function stateBadge(s) {
  if (!s || s === "idle") return html`<${Badge}>${t("downloads_peer_state_" + (s || "idle"))}<//>`;
  const kind = s === "downloading" || s === "uploading" ? "downloading"
    : s === "banned" || s === "error" ? "paused" : "waiting";
  return html`<${Badge} kind=${kind}>${t("downloads_peer_state_" + s)}<//>`;
}

// Toolbar + peer table, shared by every consumer. Always offers the full
// column set in the picker; `defaultHidden` / `defaultSort` are only the
// starting point, and `prefsKey` is where the user's choice (sort, hidden
// columns, widths) is persisted. Every caller sorts descending by default
// (biggest transfer first), so only the column key is a prop. `toolbar` is
// whatever filter controls the caller wants left of the picker. Returns the
// two siblings so the caller keeps owning the layout box around them.
export function ClientTable({ rows, prefsKey, defaultHidden, defaultSort, toolbar, toolbarCls = "toolbar" }) {
  const { sortKey, sortDir, hidden, widths, toggleSort, toggleCol, setWidth, resetPrefs } =
    useTablePrefs(prefsKey, { sortKey: defaultSort, sortDir: -1, hidden: defaultHidden });

  const columns = COLS.map((col) => ({ ...col, label: col.th ? t(col.th) : "" }));
  const shown = columns.filter((c) => !c.key || !hidden.has(c.key));
  // Sort by the chosen column when set; otherwise keep the default "busiest
  // peers first" order (combined dl+ul speed, descending).
  const list = columns.some((c) => c.key === sortKey && c.sortVal)
    ? sortRows(rows, columns, sortKey, sortDir)
    : rows.slice().sort(bySpeed);

  return html`
    <div class=${toolbarCls}>
      ${toolbar}
      <div class="spacer"></div>
      <${ColumnPicker} columns=${columns} hidden=${hidden} onToggle=${toggleCol} onReset=${resetPrefs} />
    </div>
    <${VirtualTable} columns=${shown} rows=${list} rowKey=${(c) => c.client_ecid}
                     sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                     widths=${widths} onResize=${setWidth}
                     maxHeight="none"
                     empty=${html`<${Placeholder} kind="info">${t("downloads_peer_empty")}<//>`} />`;
}

// Per-file peer table for the detail panels. Rows are the live clients whose
// download_file_hash (they serve us this file) or upload_file_hash (they pull
// it from us) matches; both hashes come from the same m_files map that produced
// the file's own hash, so a plain string compare is enough. Peers that are not
// currently connected for this file (A4AF, offline sources) have neither hash
// and therefore never show up here.
export function FileClients({ hash, prefsKey, defaultHidden, defaultSort }) {
  const clients = useClients();
  const [ident, setIdent] = useState("all");
  const [q, setQ] = useState("");

  let rows = clients.filter((c) => c.download_file_hash === hash || c.upload_file_hash === hash);
  if (ident !== "all") rows = rows.filter((c) => c.ident_state === ident);
  if (q) { const match = textMatcher(q); rows = rows.filter((c) => match((c.client_name || "") + " " + fileNameOf(c))); }

  return html`
    <div class="detail-clients">
      <${ClientTable} rows=${rows} prefsKey=${prefsKey} defaultHidden=${defaultHidden}
                      defaultSort=${defaultSort}
                      toolbar=${ClientFilters({ ident, setIdent, q, setQ })} />
    </div>`;
}

// The identity <select> + free-text box, identical in both consumers.
export function ClientFilters({ ident, setIdent, q, setQ }) {
  return html`
    <select class="input input-sm" value=${ident} onChange=${(e) => setIdent(e.target.value)}>
      ${IDENT_FILTERS.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
    </select>
    <input class="input input-sm" type="text" placeholder=${t("downloads_peer_filter")} value=${q} onInput=${(e) => setQ(e.target.value)} />`;
}
