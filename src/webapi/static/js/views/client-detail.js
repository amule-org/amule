// Client detail panel: the full per-peer info shown below the Clients table
// when a row is clicked, mirroring the desktop's "Client Details" dialog. Data
// comes from GET clients/{ecid} (the list fields plus the detail-only ones:
// ed2k_user_id, high_id, server, kad_port, friend, credit_ratio). It re-fetches
// on each live tick of the clients store so speeds/states stay current while the
// panel is open (GET is ETag-cached, so unchanged frames short-circuit to 304).
//
// A Known-tab row that is offline has no live ecid, so only the stored
// credit-store fields (`known`) are shown -- the desktop opens no detail there.

import { api } from "../api.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Section, statRow, Placeholder, CountryCell, Tabs } from "../components.js";
import { formatBytes, formatSpeed, formatInt, formatPercent, formatTimestamp, twin } from "../format.js";
import { softLabel, identLabel, stateBadge } from "./client-table.js";
import { t } from "../i18n.js";

const hashCell = (h) => html`<span class="mono">${(h || "").toUpperCase() || "—"}</span>`;
const origin = (c) => c.source_origin ? t("downloads_peer_origin_" + c.source_origin) : "—";
const yesNo = (v) => v ? t("common_yes") : t("common_no");
// "undefined" is the core's not-reported token; render it (and an empty) as a dash.
const obfuscation = (c) => c.obfuscation_state && c.obfuscation_state !== "undefined"
  ? t("client_detail_obf_" + c.obfuscation_state) : "—";

function panel(name, sections) {
  // A single "General" tab: a client has one view, but the tab strip keeps the
  // panel visually consistent with the Downloads / Shared Files detail panels.
  return html`
    <div class="detail-panel">
      <div class="detail-head">
        <div class="detail-titlebar">
          <h4 class="detail-name" title=${name}>${name || "—"}</h4>
        </div>
      </div>
      <${Tabs} tabs=${[{ key: "general", label: t("client_detail_tab_general") }]}
               active="general" onSelect=${() => {}} />
      <div class="detail-body">
        <div class="detail-sections">${sections}</div>
      </div>
    </div>`;
}

// One section layout for both a live client (GET /clients/{ecid}) and an
// offline Known row, so the panel looks the same either way. A row shows only
// when the client carries that field (the live object has them all; a Known row
// has a subset), and a section with no rows is hidden — so an offline client
// simply drops Files/Flags and the live-only transfer rows rather than showing a
// different shape.
function clientSections(c) {
  const has = (k) => c[k] !== undefined;
  const idType = c.high_id != null ? t(c.high_id ? "client_detail_id_high" : "client_detail_id_low") : "";
  // Downloaded/Uploaded: session+total (live) or just the credit-store total (Known).
  const total = (sess, tot, label, totalLabel) =>
    c[sess] !== undefined ? statRow(label, twin(c, sess, tot, formatBytes))
    : c[tot] !== undefined ? statRow(totalLabel, formatBytes(c[tot])) : null;

  const identity = [
    statRow("client_detail_name", c.name || "—"),
    statRow("client_detail_hash", hashCell(c.user_hash)),
    has("ed2k_user_id") ? statRow("client_detail_id",
      formatInt(c.ed2k_user_id) + (idType ? " (" + idType + ")" : "")) : null,
    has("ident_state") ? statRow("client_detail_ident", identLabel(c.ident_state)) : null,
  ];
  const network = [
    has("ip") ? statRow("client_detail_ip", c.ip ? c.ip + ":" + c.port : "—") : null,
    has("kad_port") ? statRow("client_detail_kad", t(c.kad_port ? "client_detail_kad_on" : "client_detail_kad_off")) : null,
    has("country_code") ? statRow("client_detail_country", html`<${CountryCell} code=${c.country_code} />`) : null,
    has("server_ip") ? statRow("client_detail_server", c.server_ip ? c.server_ip + ":" + c.server_port : "—") : null,
    has("server_name") ? statRow("client_detail_server_name", c.server_name || "—") : null,
  ];
  const software = [
    has("software") ? statRow("client_detail_software", softLabel(c)) : null,
    has("reported_os") ? statRow("client_detail_os", c.reported_os || "—") : null,
    has("client_mod_name") ? statRow("client_detail_mod", c.client_mod_name || "—") : null,
    has("source_origin") ? statRow("client_detail_origin", origin(c)) : null,
    has("obfuscation_state") ? statRow("client_detail_obfuscation", obfuscation(c)) : null,
  ];
  const transfer = [
    has("download_state") ? statRow("client_detail_dl_state", stateBadge(c.download_state)) : null,
    has("upload_state") ? statRow("client_detail_ul_state", stateBadge(c.upload_state)) : null,
    has("download_speed_bytes_per_second") ? statRow("client_detail_dl_speed", formatSpeed(c.download_speed_bytes_per_second)) : null,
    has("upload_speed_bytes_per_second") ? statRow("client_detail_ul_speed", formatSpeed(c.upload_speed_bytes_per_second)) : null,
    total("downloaded_bytes_session", "downloaded_bytes_total", "client_detail_downloaded", "client_detail_dl_total"),
    total("uploaded_bytes_session", "uploaded_bytes_total", "client_detail_uploaded", "client_detail_ul_total"),
    has("credit_ratio") ? statRow("client_detail_ratio", c.credit_ratio != null ? Number(c.credit_ratio).toFixed(2) : "—") : null,
    has("part_progress_percent") ? statRow("client_detail_progress", c.part_progress_percent != null ? formatPercent(c.part_progress_percent) : "—") : null,
    has("upload_queue_position") ? statRow("client_detail_queue", c.upload_queue_position || "—") : null,
    has("upload_queue_score") ? statRow("client_detail_score", c.upload_queue_score || "—") : null,
    has("first_seen_at") ? statRow("client_detail_first_seen", formatTimestamp(c.first_seen_at)) : null,
    has("last_seen_at") ? statRow("client_detail_last_seen", formatTimestamp(c.last_seen_at)) : null,
    has("session_count") ? statRow("client_detail_sessions", c.session_count == null ? "—" : formatInt(c.session_count)) : null,
  ];
  const files = [
    has("download_file_name") ? statRow("client_detail_dl_file", c.download_file_name || "—") : null,
    has("upload_file_name") ? statRow("client_detail_ul_file", c.upload_file_name || "—") : null,
  ];
  const flags = [
    has("friend") ? statRow("client_detail_friend", yesNo(c.friend)) : null,
    has("friend_slot") ? statRow("client_detail_friend_slot", yesNo(c.friend_slot)) : null,
    has("shared_files_browsable") ? statRow("client_detail_shares", yesNo(c.shared_files_browsable)) : null,
  ];
  return html`
    ${Section(identity.filter(Boolean), "client_detail_group_identity")}
    ${Section(network.filter(Boolean), "client_detail_group_network")}
    ${Section(software.filter(Boolean), "client_detail_group_software")}
    ${Section(transfer.filter(Boolean), "client_detail_group_transfer")}
    ${Section(files.filter(Boolean), "client_detail_group_files")}
    ${Section(flags.filter(Boolean), "client_detail_group_flags")}`;
}

export function ClientDetail({ ecid, known }) {
  const clients = useStore("clients") || []; // live tick source (SSE)
  const [detail, setDetail] = useState(null);
  const [gone, setGone] = useState(false);

  useEffect(() => {
    if (ecid == null) { setDetail(null); setGone(false); return; }
    let alive = true;
    api.get("clients/" + ecid)
      .then((d) => { if (alive) { setDetail(d); setGone(false); } })
      .catch(() => { if (alive) setGone(true); });
    return () => { alive = false; };
  }, [ecid, clients]);

  if (ecid == null) return known ? panel(known.name, clientSections(known)) : null;
  if (gone) return html`<div class="detail-panel"><${Placeholder} kind="info">${t("client_detail_gone")}<//></div>`;
  if (!detail) return html`<div class="detail-panel"><${Placeholder} kind="loading">${t("client_detail_loading")}<//></div>`;
  return panel(detail.name, clientSections(detail));
}
