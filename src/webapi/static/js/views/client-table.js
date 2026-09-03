// Peer table shared by the Clients section and the per-file "Clients" tab in
// the Downloads / Shared Files detail panels. Columns, formatting helpers and
// the live `clients` store wiring live here once; each consumer decides which
// columns it shows by default and which rows it feeds in.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Badge, listPlaceholder, CountryCell, toast } from "../components.js";
import { searches } from "../searches.js";
import { chats } from "../chats.js";
import { VirtualTable, sortRows, textMatcher, useTablePrefs, ColumnPicker, ipNum } from "../table.js";
import { formatBytes, formatSpeed, formatInt, formatTimestamp } from "../format.js";
import { Icon } from "../icons.js";
import { t, terr } from "../i18n.js";

const ACTIVE = (s) => s && s !== "idle" && s !== "unknown";
export const isDown = (c) => (c.download_speed_bytes_per_second || 0) > 0 || ACTIVE(c.download_state);
export const isUp = (c) => (c.upload_speed_bytes_per_second || 0) > 0 || ACTIVE(c.upload_state);

// Column set, declared once. Every consumer offers all of them in the column
// picker and picks which ones start hidden (see ClientTable's defaultHidden),
// so a column is always one click away instead of missing from a tab.
export const softLabel = (c) => [c.software ? t("downloads_peer_soft_" + c.software) : "", c.software === "unknown" ? "" : c.software_version].filter(Boolean).join(" ") || "—";
// `remote_queue_position` is null when the peer's queue is full (the API turns
// amuled's 0xffff sentinel into null), and 0 when it reported no position at
// all. Both are "not a position", so they sort together as 0.
const rankLabel = (c) => c.remote_queue_position === null ? t("downloads_peer_queue_full") : c.remote_queue_position || "—";
const bytesOf = (c, k) => formatBytes(c[k]);
// The "file" column is shared by both directions: download_file_name is the
// peer-advertised name of what we're pulling FROM them; upload_file_name is
// the partfile they're pulling FROM us. An upload-only peer has no
// download_file_name, so falling back to upload_file_name is what actually
// makes the column non-blank for uploads (previously always "—" there).
export const fileNameOf = (c) => c.download_file_name || c.upload_file_name || "";

// Default order when no column sort is chosen: busiest peers first.
export const bySpeed = (a, b) =>
  ((b.download_speed_bytes_per_second || 0) + (b.upload_speed_bytes_per_second || 0)) -
  ((a.download_speed_bytes_per_second || 0) + (a.upload_speed_bytes_per_second || 0));

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
    sortVal: (c) => (c.name || "").toLowerCase(),
    cell: (c) => html`<span title=${c.name}>${c.name || "—"}</span>` },
  { key: "user_hash", th: "downloads_peer_col_user_hash", width: "150px", sortable: true,
    sortVal: (c) => c.user_hash || "",
    cell: (c) => html`<span title=${c.user_hash}>${c.user_hash || "—"}</span>` },
  { key: "ident", th: "downloads_peer_col_ident", width: "130px", sortable: true,
    sortVal: (c) => identLabel(c.ident_state).toLowerCase(), cell: (c) => identBadge(c.ident_state) },
  { key: "software", th: "downloads_peer_col_software", width: "140px", sortable: true,
    sortVal: (c) => softLabel(c).toLowerCase(), cell: (c) => softLabel(c) },
  // The peer's own self-reported OS string -- frequently empty.
  { key: "os", th: "downloads_peer_col_os", width: "110px", sortable: true,
    sortVal: (c) => (c.reported_os || "").toLowerCase(), cell: (c) => c.reported_os || "—" },
  // How this source was found (server/kad/passive/…), the desktop's Origin column.
  { key: "origin", th: "downloads_peer_col_origin", width: "120px", sortable: true,
    sortVal: (c) => c.source_origin || "",
    cell: (c) => c.source_origin ? t("downloads_peer_origin_" + c.source_origin) : "—" },
  { key: "file", th: "downloads_peer_col_file", cls: "name", sortable: true,
    sortVal: (c) => fileNameOf(c).toLowerCase(),
    // A4AF row: the peer-advertised name belongs to the other file it is parked
    // on, so the desktop blanks it (the A4AF badge is in the "avail" cell).
    cell: (c) => c.a4af ? "—" : html`<span title=${fileNameOf(c)}>${fileNameOf(c) || "—"}</span>` },
  // Parts the peer offers of this file. On an A4AF row, the whole cell becomes
  // the "A4AF: <other file>" badge, mirroring the desktop's Part Status cell.
  { key: "avail", th: "downloads_peer_col_avail", width: "130px", sortable: true,
    sortVal: (c) => c.parts_offered_count || 0,
    cell: (c) => c.a4af
      ? html`<span class="a4af-badge" title=${c.a4af_name || "?"}>${t("downloads_peer_a4af")}: ${c.a4af_name || "?"}</span>`
      : c.parts_offered_count == null ? "—"
      : formatInt(c.parts_offered_count) + (c.partsTotal ? " / " + formatInt(c.partsTotal) : "") },

  { key: "dl_state", th: "downloads_peer_col_dl_state", width: "120px", sortable: true,
    sortVal: (c) => c.download_state || "", cell: (c) => stateBadge(c.download_state) },
  { key: "dl_speed", th: "downloads_peer_col_dl_speed", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.download_speed_bytes_per_second || 0, cell: (c) => formatSpeed(c.download_speed_bytes_per_second) },
  { key: "downloaded", th: "downloads_peer_col_downloaded", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.downloaded_bytes_total || 0, cell: (c) => bytesOf(c, "downloaded_bytes_total") },
  { key: "dl_session", th: "downloads_peer_col_downloaded_session", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.downloaded_bytes_session || 0, cell: (c) => bytesOf(c, "downloaded_bytes_session") },
  { key: "remote_rank", th: "downloads_peer_col_remote_rank", num: true, width: "90px", sortable: true,
    sortVal: (c) => c.remote_queue_position || 0, cell: (c) => rankLabel(c) },

  { key: "ul_state", th: "downloads_peer_col_ul_state", width: "120px", sortable: true,
    sortVal: (c) => c.upload_state || "", cell: (c) => stateBadge(c.upload_state) },
  { key: "ul_speed", th: "downloads_peer_col_ul_speed", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.upload_speed_bytes_per_second || 0, cell: (c) => formatSpeed(c.upload_speed_bytes_per_second) },
  { key: "uploaded", th: "downloads_peer_col_uploaded", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.uploaded_bytes_total || 0, cell: (c) => bytesOf(c, "uploaded_bytes_total") },
  { key: "ul_session", th: "downloads_peer_col_uploaded_session", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.uploaded_bytes_session || 0, cell: (c) => bytesOf(c, "uploaded_bytes_session") },
  { key: "queue_pos", th: "downloads_peer_col_queue_pos", num: true, width: "90px", sortable: true,
    sortVal: (c) => c.upload_queue_position || 0, cell: (c) => c.upload_queue_position || "—" },
  { key: "score", th: "downloads_peer_col_score", num: true, width: "80px", sortable: true,
    sortVal: (c) => c.upload_queue_score || 0, cell: (c) => c.upload_queue_score || "—" },
  // Whether the peer lets us browse its shared list (desktop "Shares File List").
  { key: "shares", th: "downloads_peer_col_shares", width: "80px", sortable: true,
    sortVal: (c) => c.shared_files_browsable ? 1 : 0,
    cell: (c) => c.shared_files_browsable == null ? "—"
      : c.shared_files_browsable ? t("common_yes") : t("common_no") },

  // "View files": browse this peer's share, the desktop's context-menu action.
  // A browse is an ordinary search on the API, so it opens as a tab in the
  // Search section, which is where this jumps to. Rides the shared column set,
  // so it also appears in the detail panels' Clients tab -- it targets a peer,
  // not a file, so that is correct.
  //
  // "Send message" opens the Messages section with this peer selected (no
  // request; the core creates the conversation on the first message). Only for
  // a peer with an address, the conversation key.
  { key: "actions", th: "downloads_peer_col_actions", cls: "row-actions admin-only", width: "104px",
    cell: (c) => html`
      <button class="btn btn-icon btn-sm" type="button" title=${t("search_view_files")}
              onClick=${() => searches.browse(c.ecid, c.name).catch((e) => toast(terr(e), "error"))}>
        <${Icon} name="shared" />
      </button>
      ${c.ip && c.port ? html`
        <button class="btn btn-icon btn-sm" type="button" title=${t("messages_send_message")}
                onClick=${() => {
                  chats.open({ peer: c.ip + ":" + c.port, ip: c.ip, port: c.port,
                               name: c.name, clientEcid: c.ecid });
                  location.hash = "#/messages";
                }}>
          <${Icon} name="messages" />
        </button>` : null}` },
];

// Raw-detail columns no consumer leads with; each adds its own defaultHidden set
// on top of these.
export const HIDDEN_EVERYWHERE = ["address", "os", "user_hash", "ident"];

// 1:1 with ClientIdentStateName() in src/webapi/Refresher.cpp.
export const IDENT_STATES = ["identified", "not_available", "id_needed", "id_failed", "bad_guy", "unknown"];
export const identLabel = (s) => t("downloads_peer_ident_" + (s || "unknown"));
export const IDENT_FILTERS = ["all", ...IDENT_STATES].map((v) => [v, t("downloads_peer_ident_" + v)]);

// Column set for the Known-clients tab (GET /known_clients, the credit store).
// Reuses the identity cells; swaps live transfer columns for first/last seen,
// session count and the cross-session totals.
export const KNOWN_COLS = [
  { key: "country", th: "downloads_peer_col_country", width: "70px", sortable: true,
    sortVal: (c) => c.country_code || "", cell: (c) => html`<${CountryCell} code=${c.country_code} />` },
  { key: "name", th: "downloads_peer_col_name", width: "180px", sortable: true,
    sortVal: (c) => (c.name || "").toLowerCase(),
    cell: (c) => html`<span title=${c.name}>${c.name || "—"}</span>` },
  { key: "user_hash", th: "downloads_peer_col_user_hash", width: "150px", sortable: true,
    sortVal: (c) => c.user_hash || "",
    cell: (c) => html`<span title=${c.user_hash}>${c.user_hash || "—"}</span>` },
  { key: "software", th: "downloads_peer_col_software", width: "150px", sortable: true,
    sortVal: (c) => softLabel(c).toLowerCase(), cell: (c) => softLabel(c) },
  { key: "origin", th: "downloads_peer_col_origin", width: "120px", sortable: true,
    sortVal: (c) => c.source_origin || "",
    cell: (c) => c.source_origin ? t("downloads_peer_origin_" + c.source_origin) : "—" },
  { key: "address", th: "downloads_peer_col_address", num: true, width: "170px", sortable: true,
    sortVal: (c) => ipNum(c.ip), cell: (c) => c.ip ? c.ip + ":" + c.port : "—" },
  { key: "first_seen", th: "downloads_peer_col_first_seen", width: "160px", sortable: true,
    sortVal: (c) => c.first_seen_at || 0, cell: (c) => formatTimestamp(c.first_seen_at) },
  // "Online now" while the peer is connected (matched by user hash against the
  // live /clients store), the last-seen timestamp otherwise.
  { key: "last_seen", th: "downloads_peer_col_last_seen", width: "160px", sortable: true,
    sortVal: (c) => c.online ? Infinity : (c.last_seen_at || 0),
    cell: (c) => c.online
      ? html`<${Badge} kind="downloading">${t("clients_online_now")}<//>`
      : formatTimestamp(c.last_seen_at) },
  { key: "sessions", th: "downloads_peer_col_sessions", num: true, width: "90px", sortable: true,
    sortVal: (c) => c.session_count || 0,
    cell: (c) => c.session_count == null ? "—" : formatInt(c.session_count) },
  // Grouped by direction: DL speed + total, then UL speed + total. Speeds are
  // borrowed from the online peer (see ClientsPanel); "—" offline, matching the
  // desktop's History list (live-only speed columns).
  { key: "dl_speed", th: "downloads_peer_col_dl_speed", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.download_speed_bytes_per_second || 0, cell: (c) => formatSpeed(c.download_speed_bytes_per_second) },
  { key: "downloaded", th: "downloads_peer_col_downloaded", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.downloaded_bytes_total || 0, cell: (c) => bytesOf(c, "downloaded_bytes_total") },
  { key: "ul_speed", th: "downloads_peer_col_ul_speed", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.upload_speed_bytes_per_second || 0, cell: (c) => formatSpeed(c.upload_speed_bytes_per_second) },
  { key: "uploaded", th: "downloads_peer_col_uploaded", num: true, width: "100px", sortable: true,
    sortVal: (c) => c.uploaded_bytes_total || 0, cell: (c) => bytesOf(c, "uploaded_bytes_total") },
];

export const KNOWN_HIDDEN = ["user_hash"];

// Known clients (GET /known_clients): the daemon's credit store. A plain fetch,
// not an SSE resource -- "online" and live activity are derived by the caller
// cross-referencing user_hash against the live clients store. `enabled` gates
// the fetch so it only runs once its tab is opened (and re-runs on re-open).
export function useKnownClients(enabled = true) {
  const [rows, setRows] = useState(undefined);
  useEffect(() => {
    if (!enabled) return;
    let alive = true;
    api.list("known_clients").then((r) => { if (alive) setRows(r.known_clients || []); })
       .catch(() => { if (alive) setRows([]); });
    return () => { alive = false; };
  }, [enabled]);
  return rows;
}

// Live `clients` collection (GET /clients seed + SSE client_added/updated/
// removed). register/ensure are idempotent, so every consumer can just call
// this; the resource starts on the first mount and stays live from then on.
// Returns the raw store value: undefined until the first snapshot lands, []
// once there are known to be no peers (see ClientTable's `loading`).
export function useClients() {
  useEffect(() => {
    data.register({ key: "clients", eventPrefix: "client", id: "ecid",
      list: () => api.list("clients").then((r) => r.clients || []) });
    data.ensure("clients");
  }, []);
  return useStore("clients");
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
  if (c.obfuscation_state === "enabled")
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
// `loading` (useClients() still undefined) makes empty `rows` mean "not seeded
// yet" rather than "no peers".
export function ClientTable({ rows, cols = COLS, prefsKey, defaultHidden, defaultSort, toolbar,
                              toolbarCls = "toolbar", loading = false, onRowClick, rowClass, selectedKey }) {
  const { sortKey, sortDir, hidden, widths, toggleSort, toggleCol, setWidth, resetPrefs } =
    useTablePrefs(prefsKey, { sortKey: defaultSort, sortDir: -1, hidden: defaultHidden });

  const columns = cols.map((col) => ({ ...col, label: col.th ? t(col.th) : "" }));
  const shown = columns.filter((c) => !c.key || !hidden.has(c.key));
  // Sort by the chosen column when set; otherwise keep the default "busiest
  // peers first" order (combined dl+ul speed, descending).
  const list = columns.some((c) => c.key === sortKey && c.sortVal)
    ? sortRows(rows, columns, sortKey, sortDir)
    : rows.slice().sort(bySpeed);

  // Highlight the row shown in the detail panel (keyed by ecid, or user_hash on
  // the Known tab), on top of whatever class the caller already asks for.
  const key = (c) => c.ecid != null ? c.ecid : c.user_hash;
  const rowClassFn = selectedKey == null ? rowClass
    : (c) => ((rowClass ? rowClass(c) + " " : "") + (key(c) === selectedKey ? "row-active" : "")).trim();

  return html`
    <div class=${toolbarCls}>
      ${toolbar}
      <div class="spacer"></div>
      <${ColumnPicker} columns=${columns} hidden=${hidden} onToggle=${toggleCol} onReset=${resetPrefs} />
    </div>
    <${VirtualTable} columns=${shown} rows=${list} rowKey=${key}
                     sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                     onRowClick=${onRowClick} rowClass=${rowClassFn}
                     widths=${widths} onResize=${setWidth}
                     maxHeight="none"
                     empty=${listPlaceholder(loading, t("downloads_peer_empty"))} />`;
}

// Per-file peer table for the detail panels. Rows come from the live `clients`
// store: every peer transferring this file (download_file_hash / upload_file_hash
// match) PLUS the A4AF sources parked on another file, whose ecids ride the
// download object's `source_ecids` (join-SSE, no polling). `partsTotal` and
// `files` (the downloads list) only matter for the download side: the first
// fills the "n / total" parts cell, the second resolves an A4AF row's other
// file name.
export function FileClients({ hash, prefsKey, defaultHidden, defaultSort, a4afEcids = [], partsTotal, files = [] }) {
  const clients = useClients();
  const [ident, setIdent] = useState("all");
  const [q, setQ] = useState("");

  const a4afSet = new Set(a4afEcids);
  const nameByHash = new Map((files || []).map((f) => [f.hash, f.name]));
  const inThisFile = (c) => c.download_file_hash === hash || c.upload_file_hash === hash;

  let rows = [];
  for (const c of clients || []) {
    const a4af = a4afSet.has(c.ecid);
    if (!a4af && !inThisFile(c)) continue;
    rows.push({ ...c, a4af, partsTotal,
                a4af_name: a4af ? (nameByHash.get(c.download_file_hash) || c.download_file_name || "?") : undefined });
  }
  if (ident !== "all") rows = rows.filter((c) => c.ident_state === ident);
  if (q) { const match = textMatcher(q); rows = rows.filter((c) => match((c.name || "") + " " + fileNameOf(c))); }

  return html`
    <div class="detail-clients">
      <${ClientTable} rows=${rows} prefsKey=${prefsKey} defaultHidden=${defaultHidden}
                      defaultSort=${defaultSort} loading=${clients === undefined}
                      rowClass=${(c) => c.a4af ? "a4af" : ""}
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
