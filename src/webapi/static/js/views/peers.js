// Peers panel: every connected peer (both directions) in one table with a
// Todos / Descargas / Subidas selector. Data comes from the live `clients`
// store (api.get("clients") + SSE client_added/updated/removed) which already
// carries all peers regardless of transfer direction, so filtering is purely
// client-side. Renders every field GET /api/v0/clients exposes.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Badge, Placeholder, Tabs } from "../components.js";
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
const softLabel = (c) => [c.software ? t("downloads_peer_soft_" + c.software) : "", c.software_version].filter(Boolean).join(" ") || "—";
const rankLabel = (c) => !c.remote_queue_rank ? "—" : c.remote_queue_rank >= 0xFFFF ? t("downloads_peer_queue_full") : c.remote_queue_rank;
const bytesOf = (c, k) => formatBytes((c.xfer || {})[k]);

const COLS = [
  { cls: "peer-flags", show: ALL, cell: (c) => peerFlags(c) },
  { th: "downloads_peer_col_name", show: ALL, cell: (c) => html`<span title=${c.client_name}>${c.client_name || "—"}</span>` },
  { th: "downloads_peer_col_software", show: ALL, cell: (c) => softLabel(c) },
  { th: "downloads_peer_col_file", cls: "name", show: ALL, cell: (c) => html`<span title=${c.download_file_name}>${c.download_file_name || "—"}</span>` },

  { th: "downloads_peer_col_dl_state", show: DL, cell: (c) => stateBadge(c.download_state) },
  { th: "downloads_peer_col_dl_speed", num: true, show: DL, cell: (c) => formatSpeed(c.download_speed_bps) },
  { th: "downloads_peer_col_downloaded", num: true, show: DL, cell: (c) => bytesOf(c, "down_total") },
  { th: "downloads_peer_col_downloaded_session", num: true, show: ["downloads"], cell: (c) => bytesOf(c, "down_session") },
  { th: "downloads_peer_col_remote_rank", num: true, show: ["downloads"], cell: (c) => rankLabel(c) },

  { th: "downloads_peer_col_ul_state", show: UL, cell: (c) => stateBadge(c.upload_state) },
  { th: "downloads_peer_col_ul_speed", num: true, show: UL, cell: (c) => formatSpeed(c.upload_speed_bps) },
  { th: "downloads_peer_col_uploaded", num: true, show: UL, cell: (c) => bytesOf(c, "up_total") },
  { th: "downloads_peer_col_uploaded_session", num: true, show: ["uploads"], cell: (c) => bytesOf(c, "up_session") },
  { th: "downloads_peer_col_queue_pos", num: true, show: ["uploads"], cell: (c) => c.queue_waiting_position || "—" },
  { th: "downloads_peer_col_score", num: true, show: ["uploads"], cell: (c) => c.score || "—" },
];
const colClass = (col) => [col.num ? "num" : "", col.cls || ""].filter(Boolean).join(" ");

const IDENT_FILTERS = ["all", "identified", "not_identified"].map((v) => [v, t("downloads_peer_ident_" + v)]);

export function PeersPanel() {
  const clients = useStore("clients") || [];
  const [filter, setFilter] = useState("all");
  const [ident, setIdent] = useState("identified");

  useEffect(() => {
    data.register({ key: "clients", eventPrefix: "client", id: "client_ecid",
      list: () => api.get("clients").then((r) => r.clients || []) });
    data.ensure("clients");
  }, []);

  const nDown = clients.filter(isDown).length;
  const nUp = clients.filter(isUp).length;

  let list = clients.slice();
  if (filter === "downloads") list = list.filter(isDown);
  else if (filter === "uploads") list = list.filter(isUp);
  if (ident === "identified") list = list.filter((c) => c.ident_state === "identified");
  else if (ident === "not_identified") list = list.filter((c) => c.ident_state !== "identified");
  list.sort((a, b) =>
    ((b.download_speed_bps || 0) + (b.upload_speed_bps || 0)) -
    ((a.download_speed_bps || 0) + (a.upload_speed_bps || 0)));

  const tabs = [
    { key: "all", label: t("downloads_peer_all"), badge: clients.length },
    { key: "downloads", label: t("downloads_peer_download"), badge: nDown },
    { key: "uploads", label: t("downloads_peer_upload"), badge: nUp },
  ];

  const cols = COLS.filter((col) => col.show.includes(filter));

  return html`
    <h3 class="section-title">${t("downloads_peer_title")}</h3>
    <section class="net-pane">
      <${Tabs} tabs=${tabs} active=${filter} onSelect=${setFilter} />
      <div class="net-pane-body">
        <div class="toolbar pane-toolbar">
          <label>${t("downloads_peer_identity")}:</label>
          <select class="input input-sm" value=${ident} onChange=${(e) => setIdent(e.target.value)}>
            ${IDENT_FILTERS.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
        </div>
        <div class="table-wrap">
          <table class="data">
            <thead>
              <tr>${cols.map((col) => html`<th class=${colClass(col)}>${col.th ? t(col.th) : ""}</th>`)}</tr>
            </thead>
            <tbody>
              ${list.length
                ? list.map((c) => html`
                    <tr key=${c.client_ecid}>
                      ${cols.map((col) => html`<td class=${colClass(col)}>${col.cell(c)}</td>`)}
                    </tr>`)
                : html`<tr><td colspan=${cols.length}><${Placeholder} kind="info">${t("downloads_peer_empty")}<//></td></tr>`}
            </tbody>
          </table>
        </div>
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
