// Peer table shared by the Clients section and the per-file "Clients" tab in
// the Downloads / Shared Files detail panels. Columns, formatting helpers and
// the live `clients` store wiring live here once; each consumer decides which
// columns it shows by default and which rows it feeds in.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useMemo, useStore } from "../dom.js";
import { Badge, listPlaceholder, CountryCell, toast } from "../components.js";
import { searches } from "../searches.js";
import { VirtualTable, sortRows, textMatcher, useTablePrefs, ColumnPicker, ipNum } from "../table.js";
import { formatBytes, formatSpeed } from "../format.js";
import { Icon } from "../icons.js";
import { t, terr } from "../i18n.js";

const ACTIVE = (s) => s && s !== "idle" && s !== "unknown";
export const isDown = (c) => (c.download_speed_bytes_per_second || 0) > 0 || ACTIVE(c.download_state);
export const isUp = (c) => (c.upload_speed_bytes_per_second || 0) > 0 || ACTIVE(c.upload_state);

// Column set, declared once. Every consumer offers all of them in the column
// picker and picks which ones start hidden (see ClientTable's defaultHidden),
// so a column is always one click away instead of missing from a tab.
const softLabel = (c) => [c.software ? t("downloads_peer_soft_" + c.software) : "", c.software === "unknown" ? "" : c.software_version].filter(Boolean).join(" ") || "—";
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
const originLabel = (c) => t("downloads_peer_origin_" + (c.source_origin || "unknown"));
// No inversion: the API renamed `view_shared_disabled` to stop consumers negating it.
const sharesListLabel = (c) => t(c.shared_files_browsable ? "common_yes" : "common_no");

// Peers for one file: those transferring it in either direction, plus its A4AF
// sources -- queued for this file while they serve another one, so neither hash
// points here and the plain join alone would leave them invisible. Membership
// comes off the download record (`source_ecids`, #1219), because A4AF is a
// relation between a client and a *file* and cannot live on the client row.
//
// Every derived field is computed here, once per row, and the cells read back
// what they sorted by. sortRows() calls sortVal per *comparison*, so testing set
// membership or resolving the other file's name inside a comparator would cost
// O(rows·log rows) scans instead of one pass.
function fileRows(clients, hash, downloads) {
  if (!Array.isArray(clients) || !Array.isArray(downloads)) return undefined;
  const dl = downloads.find((d) => d.hash === hash);
  // A file we only share has no download queue, so it has no A4AF sources. A
  // download record without `source_ecids` is a daemon older than #1219:
  // unknown, which must not be shown as "no".
  const ecids = dl ? dl.source_ecids : [];
  const sources = Array.isArray(ecids) ? new Set(ecids) : null;
  const names = new Map();
  for (const d of downloads) names.set(d.hash, d.name);

  const rows = [];
  for (const c of clients) {
    const a4af = sources ? sources.has(c.ecid) : undefined;
    if (!a4af && c.download_file_hash !== hash && c.upload_file_hash !== hash) continue;
    rows.push({ ...c, a4af,
                a4af_file_name: a4af ? names.get(c.download_file_hash) || "?" : "" });
  }
  return rows;
}

// A source queued for this file while it currently serves another one. Text
// rather than colour alone, and it names the file the peer is parked on.
function a4afCell(c) {
  if (c.a4af === undefined)
    return html`<span title=${t("downloads_peer_a4af_unknown_tip")}>?</span>`;
  if (!c.a4af) return "—";
  const other = c.a4af_file_name || "?";
  return html`<span class="peer-a4af" title=${t("downloads_peer_a4af_tip") + " — " + other}>
    ${t("downloads_peer_a4af")}: ${other}</span>`;
}

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
  { key: "origin", th: "downloads_peer_col_origin", width: "120px", sortable: true,
    sortVal: (c) => originLabel(c).toLowerCase(), cell: (c) => originLabel(c) },
  { key: "shares_list", th: "downloads_peer_col_shares_list", width: "110px", sortable: true,
    sortVal: (c) => (c.shared_files_browsable ? 1 : 0),
    cell: (c) => html`<span title=${t("downloads_peer_col_shares_list_tip")}>${sharesListLabel(c)}</span>` },
  { key: "file", th: "downloads_peer_col_file", cls: "name", sortable: true,
    sortVal: (c) => fileNameOf(c).toLowerCase(),
    cell: (c) => html`<span title=${fileNameOf(c)}>${fileNameOf(c) || "—"}</span>` },
  { key: "a4af", th: "downloads_peer_a4af", width: "180px", sortable: true,
    sortVal: (c) => (c.a4af ? (c.a4af_file_name || "?").toLowerCase() : ""), cell: (c) => a4afCell(c) },
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

  // "View files": browse this peer's share, the desktop's context-menu action.
  // A browse is an ordinary search on the API, so it opens as a tab in the
  // Search section, which is where this jumps to. Rides the shared column set,
  // so it also appears in the detail panels' Clients tab -- it targets a peer,
  // not a file, so that is correct.
  { key: "actions", th: "downloads_peer_col_actions", cls: "row-actions admin-only", width: "70px",
    cell: (c) => html`
      <button class="btn btn-icon btn-sm" type="button" title=${t("search_view_files")}
              onClick=${() => searches.browse(c.ecid, c.name).catch((e) => toast(terr(e), "error"))}>
        <${Icon} name="shared" />
      </button>` },
];

// Raw-detail columns no consumer leads with; each adds its own defaultHidden set
// on top of these.
export const HIDDEN_EVERYWHERE = ["address", "os", "user_hash", "ident", "origin", "shares_list"];

// A `defaultHidden` entry only reaches a user who has never touched the column
// picker: useTablePrefs restores a stored `hidden` array wholesale, and a column
// that did not exist when it was stored is absent from it, so it arrives
// visible. PREFS_VERSION + ADDED_COLS is the one-shot migration for that: on the
// first load after the bump, every key listed here that the consumer's own
// defaultHidden also hides is folded into the stored array. Bump the version and
// list the key whenever a column is added to a defaultHidden set.
export const PREFS_VERSION = 1;
export const ADDED_COLS = ["origin", "shares_list", "a4af"];

// 1:1 with ClientIdentStateName() in src/webapi/Refresher.cpp.
export const IDENT_STATES = ["identified", "not_available", "id_needed", "id_failed", "bad_guy", "unknown"];
export const identLabel = (s) => t("downloads_peer_ident_" + (s || "unknown"));
export const IDENT_FILTERS = ["all", ...IDENT_STATES].map((v) => [v, t("downloads_peer_ident_" + v)]);

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
// `loading` (rows still undefined) makes empty `rows` mean "not seeded yet"
// rather than "no peers".
export function ClientTable({ rows, prefsKey, defaultHidden, defaultSort, toolbar, toolbarCls = "toolbar",
                              loading = false }) {
  const { sortKey, sortDir, hidden, widths, toggleSort, toggleCol, setWidth, resetPrefs } =
    useTablePrefs(prefsKey, { sortKey: defaultSort, sortDir: -1, hidden: defaultHidden,
                              version: PREFS_VERSION, added: ADDED_COLS });

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
    <${VirtualTable} columns=${shown} rows=${list} rowKey=${(c) => c.ecid}
                     sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                     widths=${widths} onResize=${setWidth}
                     maxHeight="none"
                     empty=${listPlaceholder(loading, t("downloads_peer_empty"))} />`;
}

// Per-file peer table for the detail panels (issue #984). Rows are derived from
// the live `clients` store, so an open tab issues no request of its own and a
// closed one costs nothing at all.
export function FileClients({ hash, prefsKey, defaultHidden, defaultSort }) {
  const clients = useClients();
  // Both the A4AF membership and the name of the file an A4AF peer is parked on
  // hang off the download record, so this panel needs `downloads` live even for
  // a *shared* file. Must be the SAME loader views/downloads.js registers, since
  // register is first-wins: api.list appends limit=<all>, a plain GET would cap
  // it at 100.
  useEffect(() => {
    data.register({ key: "downloads", eventPrefix: "download", id: "hash",
      list: () => api.list("downloads?status=all").then((r) => r.downloads || []) });
    data.ensure("downloads");
  }, []);
  const downloads = useStore("downloads");
  const [ident, setIdent] = useState("all");
  const [q, setQ] = useState("");

  // undefined until both stores have seeded, so an empty table reads as "no
  // peers" and never as "not known yet". Keyed on `hash`, which is what keeps
  // one file's peers from ever being drawn under another file's name.
  const rows = useMemo(() => fileRows(clients, hash, downloads), [clients, hash, downloads]);

  let list = rows || [];
  if (ident !== "all") list = list.filter((c) => c.ident_state === ident);
  if (q) { const match = textMatcher(q); list = list.filter((c) => match((c.name || "") + " " + fileNameOf(c))); }

  return html`
    <div class="detail-clients">
      <${ClientTable} rows=${list} prefsKey=${prefsKey} defaultHidden=${defaultHidden}
                      defaultSort=${defaultSort} loading=${rows === undefined}
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
