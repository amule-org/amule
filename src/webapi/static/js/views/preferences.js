// Preferences view, aligned with the desktop amuleGUI PrefsUnifiedDlg: same
// page order and grouping, horizontal category tabs on top. Reads /preferences
// into one shared form state (edits survive tab switches), PATCHes the editable
// subset back with a single Apply. Admin-only edit; guest sees a read-only form.
//
// A tab is a UI grouping; the API groups preferences into categories. Most tabs
// map to one category (tab.cat), but some don't: Proxy draws the proxy_* fields
// out of the "connection" category into its own tab (like the desktop Proxy
// page), and Advanced pulls files.mmap_enabled in next to the core tweaks. So
// every field's real API category is `f.cat || tab.cat`, and that is what keys
// the form state and the PATCH body. A category may itself be a dotted path
// into a nested payload object ("remote_controls.webserver"); the form state
// stays flat and only the GET read / PATCH build walk the path.

import { api } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder, toast, Tabs } from "../components.js";
import { t, terr } from "../i18n.js";

// Field types: text (default), int, bool, select, password, textarea, trigger.
// Flags: readonly (shown disabled, never sent), hidden (capability flag loaded
// only to gate others), gatedBy (disabled + skipped when values[cap] === false),
// password (write-only, only sent when non-empty), trigger (write-only action,
// only sent when checked), scale (int shown/edited in value/scale units, e.g.
// ms stored but minutes shown), cat (override the tab's API category).
const PROXY_TYPES = [
  { value: "socks5", labelKey: "prefs_opt_proxy_socks5" },
  { value: "socks4", labelKey: "prefs_opt_proxy_socks4" },
  { value: "http", labelKey: "prefs_opt_proxy_http" },
  { value: "socks4a", labelKey: "prefs_opt_proxy_socks4a" },
];

// security.shared_files_visibility is a 3-state enum string, not a bool.
const SEE_SHARES = [
  { value: "everybody", labelKey: "prefs_opt_see_shares_everybody" },
  { value: "friends", labelKey: "prefs_opt_see_shares_friends" },
  { value: "nobody", labelKey: "prefs_opt_see_shares_nobody" },
];
const GEOIP_SOURCES = [
  { value: "dbip", labelKey: "prefs_opt_source_dbip" },
  { value: "maxmind", labelKey: "prefs_opt_source_maxmind" },
  { value: "custom", labelKey: "prefs_opt_source_custom" },
];

// Tabs follow the desktop pages[] order (skipping pages the API does not
// expose: Interface, Statistics, Events, Debugging).
const TABS = [
  { id: "general", labelKey: "prefs_general", cat: "general", groups: [
    { legendKey: "prefs_group_general", fields: [
      { key: "nickname", type: "text" },
      { key: "check_new_version", type: "bool" },
      { key: "local_host_name", type: "text", readonly: true },
      { key: "user_hash", type: "text", readonly: true },
    ] },
  ] },
  { id: "connection", labelKey: "prefs_connection", cat: "connection", groups: [
    { legendKey: "prefs_group_bandwidth", fields: [
      { key: "max_download_kbps", type: "int", min: 0, max: 1000000 },
      { key: "max_upload_kbps", type: "int", min: 0, max: 1000000 },
      { key: "upload_slot_kbps", type: "int", min: 1, max: 100000 },
    ] },
    { legendKey: "prefs_group_ports", fields: [
      { key: "tcp_port", type: "int", min: 0, max: 65535 },
      // Not an API field: the desktop shows the server-request UDP port, which
      // the core derives as TCP+3. Computed, read-only, never sent.
      { key: "udp_server_port", type: "int", readonly: true,
        derived: (v, cat) => (parseInt(v[cat + ".tcp_port"], 10) || 0) + 3 },
      { key: "extended_udp_port_enabled", type: "bool" },
      { key: "udp_port", type: "int", min: 0, max: 65535, sub: true, gatedBy: "extended_udp_port_enabled" },
      { key: "upnp_enabled", type: "bool", gatedBy: "upnp_available" },
      { key: "upnp_tcp_port", type: "int", min: 0, max: 65535, sub: true, gatedBy: ["upnp_available", "upnp_enabled"] },
      { key: "upnp_available", type: "bool", hidden: true },
    ] },
    { legendKey: "prefs_group_binding", fields: [
      { key: "bind_address", type: "text" },
      { key: "bind_interface", type: "text" },
    ] },
    { legendKey: "prefs_group_conn_limits", fields: [
      { key: "max_sources_per_file", type: "int", min: 40, max: 5000 },
      { key: "max_connections", type: "int", min: 5, max: 7500 },
    ] },
    { legendKey: "prefs_group_networks", fields: [
      { key: "network_kad", type: "bool" },
      { key: "network_ed2k", type: "bool", readonly: true, labelKey: "prefs_ed2k_readonly" },
      { key: "autoconnect", type: "bool" },
      { key: "reconnect", type: "bool" },
    ] },
  ] },
  { id: "directories", labelKey: "prefs_directories", cat: "directories", groups: [
    { legendKey: "prefs_group_incoming", fields: [{ key: "incoming", type: "text" }] },
    { legendKey: "prefs_group_temp", fields: [{ key: "temp", type: "text" }] },
    { legendKey: "prefs_group_shared", fields: [
      { key: "shared", type: "textarea" },
      { key: "share_hidden", type: "bool" },
      { key: "auto_rescan", type: "bool" },
      { key: "follow_symlinks", type: "bool" },
      { key: "exclude_patterns", type: "text" },
      { key: "exclude_patterns_use_regex", type: "bool" },
    ] },
  ] },
  { id: "servers", labelKey: "prefs_servers", cat: "servers", groups: [
    { legendKey: "prefs_group_server_list", fields: [
      { key: "remove_dead", type: "bool" },
      { key: "dead_server_retries", type: "int", min: 1, max: 10, sub: true, gatedBy: "remove_dead" },
      { key: "auto_update", type: "bool" },
      { key: "update_list_from_server", type: "bool" },
      { key: "update_list_from_client", type: "bool" },
      { key: "update_url", type: "text" },
    ] },
    { legendKey: "prefs_group_server_conn", fields: [
      { key: "use_priority_system", type: "bool" },
      { key: "smart_id_check", type: "bool" },
      { key: "safe_connect", type: "bool" },
      { key: "autoconnect_static_servers_only", type: "bool" },
      { key: "manual_servers_high_priority", type: "bool" },
    ] },
    { legendKey: "prefs_group_kademlia", fields: [
      { key: "update_url", type: "text", cat: "kademlia" },
    ] },
  ] },
  { id: "files", labelKey: "prefs_files", cat: "files", groups: [
    { legendKey: "prefs_group_downloads", fields: [
      { key: "add_new_downloads_paused", type: "bool" },
      { key: "new_downloads_auto_priority", type: "bool" },
      { key: "prioritize_first_last_chunks", type: "bool" },
      { key: "start_next_paused", type: "bool" },
      { key: "start_next_same_category", type: "bool", sub: true, gatedBy: "start_next_paused" },
      { key: "start_next_alphabetical", type: "bool", sub: true, gatedBy: "start_next_paused" },
      { key: "endgame_enabled", type: "bool" },
      { key: "preallocate_full_file_size", type: "bool" },
      { key: "create_sparse_files", type: "bool" },
      { key: "stop_on_low_disk_space", type: "bool" },
      { key: "min_free_space_mb", type: "int", min: 1, max: 1000000, sub: true, gatedBy: "stop_on_low_disk_space" },
      { key: "save_source_seeds_for_rare_files", type: "bool" },
    ] },
    { legendKey: "prefs_group_uploads", fields: [
      { key: "new_shared_files_auto_priority", type: "bool" },
    ] },
    { legendKey: "prefs_group_ich", fields: [
      { key: "ich_enabled", type: "bool" },
      { key: "aich_trust_every_hash", type: "bool" },
    ] },
    { legendKey: "prefs_group_media", fields: [
      { key: "media_metadata_enabled", type: "bool" },
      { key: "ffprobe_path", type: "text", sub: true, gatedBy: "media_metadata_enabled" },
    ] },
  ] },
  { id: "security", labelKey: "prefs_security", cat: "security", groups: [
    { legendKey: "prefs_group_privacy", fields: [
      { key: "use_secident", type: "bool" },
      { key: "shared_files_visibility", type: "select", int: true, options: SEE_SHARES },
    ] },
    { legendKey: "prefs_group_obfuscation", fields: [
      { key: "obfuscation_enabled", type: "bool" },
      { key: "obfuscation_requested", type: "bool", sub: true, gatedBy: "obfuscation_enabled" },
      { key: "obfuscation_required", type: "bool", sub: true, gatedBy: "obfuscation_enabled" },
    ] },
    { legendKey: "prefs_group_ipfilter", fields: [
      { key: "ipfilter_clients", type: "bool" },
      { key: "ipfilter_servers", type: "bool" },
      { key: "ipfilter_update_url", type: "text" },
      { key: "ipfilter_auto_update", type: "bool" },
      { key: "ipfilter_block_below_access_level", type: "int", min: 0, max: 255 },
      { key: "ipfilter_include_lan_ips", type: "bool" },
      { key: "reject_spoofed_source_ips", type: "bool" },
      { key: "use_system_ipfilter", type: "bool" },
    ] },
  ] },
  { id: "ip2country", labelKey: "prefs_ip2country", cat: "ip2country",
    hideWhen: (v) => v["ip2country.supported"] === false, groups: [
    { legendKey: "prefs_group_geoip_db", fields: [
      { key: "enabled", type: "bool", gatedBy: "supported" },
      { key: "supported", type: "bool", hidden: true },
      { key: "source", type: "select", options: GEOIP_SOURCES, sub: true, gatedBy: ["supported", "enabled"] },
      { key: "custom_url", type: "text", sub: 2, gatedBy: ["supported", "enabled"], gatedByEq: { key: "source", value: "custom" } },
      { key: "maxmind_license", type: "text", sub: 2, gatedBy: ["supported", "enabled"], gatedByEq: { key: "source", value: "maxmind" } },
      { key: "auto_update", type: "bool", sub: true, gatedBy: ["supported", "enabled"] },
    ] },
    { legendKey: "prefs_group_geoip_status", fields: [
      { key: "update_now", type: "trigger", gatedBy: ["supported", "enabled"] },
      { key: "download_in_progress", type: "bool", readonly: true },
      { key: "loaded_source", type: "text", readonly: true },
      { key: "db_path", type: "text", readonly: true },
      { key: "db_loaded", type: "bool", readonly: true },
      { key: "last_update_result", type: "text", readonly: true },
    ] },
  ] },
  { id: "proxy", labelKey: "prefs_proxy", cat: "connection", groups: [
    { legendKey: "prefs_group_proxy", fields: [
      { key: "proxy_enabled", type: "bool" },
      { key: "proxy_type", type: "select", int: true, options: PROXY_TYPES, sub: true, gatedBy: "proxy_enabled" },
      { key: "proxy_host", type: "text", sub: true, gatedBy: "proxy_enabled" },
      { key: "proxy_port", type: "int", min: 0, max: 65535, sub: true, gatedBy: "proxy_enabled" },
      { key: "proxy_auth", type: "bool", sub: true, gatedBy: "proxy_enabled" },
      { key: "proxy_user", type: "text", sub: 2, gatedBy: ["proxy_enabled", "proxy_auth"] },
      { key: "proxy_password", type: "password", sub: 2, gatedBy: ["proxy_enabled", "proxy_auth"] },
    ] },
  ] },
  { id: "message_filter", labelKey: "prefs_message_filter", cat: "message_filter", groups: [
    { legendKey: "prefs_group_messages", fields: [
      { key: "enabled", type: "bool" },
      { key: "filter_all_messages", type: "bool", sub: true, gatedBy: "enabled" },
      { key: "accept_from_friends_only", type: "bool", sub: true, gatedBy: "enabled" },
      { key: "accept_from_known_clients_only", type: "bool", sub: true, gatedBy: "enabled" },
      { key: "by_keyword", type: "bool", sub: true, gatedBy: "enabled" },
      { key: "keywords", type: "text", sub: 2, gatedBy: ["enabled", "by_keyword"] },
      { key: "show_in_log", type: "bool" },
    ] },
    { legendKey: "prefs_group_comments", fields: [
      { key: "filter_comments", type: "bool" },
      { key: "comment_keywords", type: "text", sub: true, gatedBy: "filter_comments" },
    ] },
  ] },
  // The two subsystems are nested categories in the payload
  // (remote_controls.webserver / .amuleapi), so each group names its own cat.
  { id: "remote_controls", labelKey: "prefs_remote_controls", cat: "remote_controls", groups: [
    { legendKey: "prefs_group_amuleapi", fields: [
      { key: "enabled", type: "bool", cat: "remote_controls.amuleapi" },
      { key: "port", type: "int", min: 0, max: 65535, sub: true, cat: "remote_controls.amuleapi", gatedBy: "enabled" },
      { key: "bind_address", type: "text", sub: true, cat: "remote_controls.amuleapi", gatedBy: "enabled" },
      { key: "password", type: "password", sub: true, cat: "remote_controls.amuleapi", gatedBy: "enabled" },
    ] },
    { legendKey: "prefs_group_webserver", fields: [
      { key: "enabled", type: "bool", cat: "remote_controls.webserver" },
      { key: "template", type: "text", sub: true, cat: "remote_controls.webserver", gatedBy: "enabled" },
      { key: "password", type: "password", sub: true, cat: "remote_controls.webserver", gatedBy: "enabled" },
      { key: "guest_enabled", type: "bool", sub: true, cat: "remote_controls.webserver", gatedBy: "enabled" },
      { key: "guest_password", type: "password", sub: 2, cat: "remote_controls.webserver", gatedBy: ["enabled", "guest_enabled"] },
      { key: "port", type: "int", min: 0, max: 65535, sub: true, cat: "remote_controls.webserver", gatedBy: "enabled" },
      { key: "refresh_seconds", type: "int", min: 0, sub: true, cat: "remote_controls.webserver", gatedBy: "enabled" },
      { key: "use_gzip", type: "bool", sub: true, cat: "remote_controls.webserver", gatedBy: "enabled" },
    ] },
  ] },
  { id: "online_signature", labelKey: "prefs_online_signature", cat: "online_signature", groups: [
    { legendKey: "prefs_group_onlinesig", fields: [
      { key: "enabled", type: "bool" },
      { key: "update_frequency_seconds", type: "int", min: 0, max: 600, sub: true, gatedBy: "enabled" },
      { key: "directory", type: "text", sub: true, gatedBy: "enabled" },
    ] },
  ] },
  { id: "core_tweaks", labelKey: "prefs_core_tweaks", cat: "core_tweaks", noteKey: "prefs_core_tweaks_warning", groups: [
    { legendKey: "prefs_group_tweaks", fields: [
      { key: "max_new_connections_per_5s", type: "int", min: 0 },
      { key: "kad_max_source_searches", type: "int", min: 0 },
      { key: "kad_reask_ms", type: "int", min: 0, scale: 60000 },
      { key: "source_reask_ms", type: "int", min: 0, scale: 60000 },
      { key: "file_buffer_bytes", type: "int", min: 0 },
      { key: "mmap_enabled", type: "bool", cat: "files", gatedBy: "mmap_supported" },
      { key: "mmap_supported", type: "bool", cat: "files", hidden: true },
      { key: "max_upload_queue_clients", type: "int", min: 0 },
      // Stored in ms, shown in minutes like the desktop slider (0-30).
      { key: "server_keepalive_timeout_ms", type: "int", min: 0, max: 30, scale: 60000 },
      { key: "verbose_logging", type: "bool" },
    ] },
  ] },
];

const catOf = (tab, f) => f.cat || tab.cat;
const asArr = (x) => (x == null ? [] : Array.isArray(x) ? x : [x]);
// A category may be a dotted path into a nested payload object (e.g.
// "remote_controls.webserver"); walk/create it on read and write.
const catGet = (obj, cat) => cat.split(".").reduce((o, k) => (o == null ? o : o[k]), obj);
const catPut = (obj, cat, key, val) => {
  const o = cat.split(".").reduce((acc, k) => (acc[k] || (acc[k] = {})), obj);
  o[key] = val;
};
// i18n keys stay flat: remote_controls.webserver + "port" ->
// prefs_field_remote_controls_webserver_port.
const labelKeyFor = (cat, key) => "prefs_field_" + cat.replace(/\./g, "_") + "_" + key;
// Drop objects that ended up with no fields, innermost first, so an untouched
// nested category never reaches the PATCH body as an empty {}.
const pruneEmpty = (obj) => {
  for (const k of Object.keys(obj)) {
    if (obj[k] && typeof obj[k] === "object" && !Array.isArray(obj[k])) {
      pruneEmpty(obj[k]);
      if (!Object.keys(obj[k]).length) delete obj[k];
    }
  }
  return obj;
};
// Clamp to the field's [min, max] before sending; floor at 0 when no min is set.
const clamp = (n, lo, hi) => Math.min(hi == null ? Infinity : hi, Math.max(lo == null ? 0 : lo, n));

export default function Preferences({ isGuest }) {
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState("");
  const [values, setValues] = useState({}); // "cat.key" -> value (display units)
  const [active, setActive] = useState("general");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    api.get("preferences").then((p) => {
      const v = {};
      for (const tab of TABS)
        for (const grp of tab.groups)
          for (const f of grp.fields) {
            const cat = catOf(tab, f);
            let val = (catGet(p, cat) || {})[f.key];
            if (f.type === "textarea" && Array.isArray(val)) val = val.join("\n");
            else if (f.scale && typeof val === "number") val = Math.round(val / f.scale);
            v[cat + "." + f.key] = val;
          }
      setValues(v);
      setLoaded(true);
    }).catch((e) => setError(terr(e) || t("prefs_error")));
  }, []);

  const setVal = (id, val) => setValues((vs) => ({ ...vs, [id]: val }));

  // gatedBy: disable when any listed flag is explicitly false (capability flags
  // or an "enable" parent); a missing flag (older daemon) leaves it editable.
  // gatedByNot: disable when any listed flag is true (an inverted parent shown
  // as an "Enable ..." checkbox).
  // gatedByEq: enable only when a sibling equals a value (e.g. show the MaxMind
  // license only when source === "maxmind"); disabled otherwise.
  const isGated = (cat, f) =>
    asArr(f.gatedBy).some((k) => values[cat + "." + k] === false) ||
    asArr(f.gatedByNot).some((k) => values[cat + "." + k] === true) ||
    (f.gatedByEq && values[cat + "." + f.gatedByEq.key] !== f.gatedByEq.value);

  const buildField = (cat, f) => {
    if (f.hidden) return null;
    const id = cat + "." + f.key;
    const val = f.derived ? f.derived(values, cat) : values[id];
    const label = t(f.labelKey || labelKeyFor(cat, f.key));
    const disabled = isGuest || f.readonly || isGated(cat, f);
    const subCls = f.sub === 2 ? " field-sub2" : f.sub ? " field-sub" : "";

    if (f.type === "bool" || f.type === "trigger") {
      // invert: the API stores the opposite sense but we show an "Enable ..."
      // checkbox. State keeps the API value; only the checkbox's checked state
      // and its toggle are flipped.
      const checked = f.invert ? !val : !!val;
      return html`
        <div class=${"field field-inline" + subCls}>
          <input type="checkbox" id=${id} checked=${checked} disabled=${disabled}
                 onChange=${(e) => setVal(id, f.invert ? !e.target.checked : e.target.checked)} />
          <label for=${id}>${label}</label>
        </div>`;
    }
    if (f.type === "select") {
      return html`
        <div class=${"field" + subCls}>
          <label for=${id}>${label}</label>
          <select id=${id} disabled=${disabled}
                  value=${val == null ? "" : String(val)}
                  onChange=${(e) => setVal(id, e.target.value)}>
            ${f.options.map((o) => html`<option value=${String(o.value)}>${t(o.labelKey)}</option>`)}
          </select>
        </div>`;
    }
    if (f.type === "textarea") {
      return html`
        <div class=${"field field-wide" + subCls}>
          <label for=${id}>${label}</label>
          <textarea id=${id} rows="4" disabled=${disabled}
                    value=${val == null ? "" : val} onInput=${(e) => setVal(id, e.target.value)}></textarea>
        </div>`;
    }
    return html`
      <div class=${"field" + subCls}>
        <label for=${id}>${label}</label>
        <input class="input" id=${id} disabled=${disabled}
               type=${f.type === "int" ? "number" : f.type === "password" ? "password" : "text"}
               autocomplete=${f.type === "password" ? "new-password" : null}
               min=${f.min} max=${f.max}
               value=${val === undefined || val === null ? "" : val} onInput=${(e) => setVal(id, e.target.value)} />
      </div>`;
  };

  const collect = () => {
    const body = {};
    for (const tab of TABS) {
      for (const grp of tab.groups) {
        for (const f of grp.fields) {
          const cat = catOf(tab, f);
          if (f.hidden || f.readonly || isGated(cat, f)) continue;
          const val = values[cat + "." + f.key];
          let out;
          if (f.type === "password") {
            if (typeof val !== "string" || val === "") continue;
            out = val;
          } else if (f.type === "trigger") {
            if (!val) continue;
            out = true;
          } else if (f.type === "textarea") {
            out = String(val || "").split("\n").map((s) => s.trim()).filter(Boolean);
          } else if (f.type === "select") {
            out = val == null ? "" : val;
          } else if (f.type === "bool") {
            out = !!val;
          } else if (f.type === "int") {
            const n = clamp(f.scale ? (parseFloat(val) || 0) : (parseInt(val, 10) || 0), f.min, f.max);
            out = Math.round(n * (f.scale || 1));
          } else {
            out = val == null ? "" : val;
          }
          catPut(body, cat, f.key, out);
        }
      }
    }
    return pruneEmpty(body);
  };

  const save = async (e) => {
    e.preventDefault();
    setBusy(true);
    try { await api.patch("preferences", collect()); toast(t("prefs_toast_saved"), "success"); }
    catch (err) { toast(terr(err) || t("prefs_error"), "error"); }
    finally { setBusy(false); }
  };

  if (error) return html`<p>${error}</p>`;
  if (!loaded) return html`<${Placeholder} kind="loading">${t("prefs_loading")}<//>`;

  const shownTabs = TABS.filter((s) => !s.hideWhen || !s.hideWhen(values));
  const tab = shownTabs.find((s) => s.id === active) || shownTabs[0];
  const tabList = shownTabs.map((s) => ({ key: s.id, label: t(s.labelKey) }));
  return html`
    <form onSubmit=${save} class="net-pane">
      <${Tabs} tabs=${tabList} active=${active} onSelect=${setActive} />
      <div class="net-pane-body prefs-panel">
        ${tab.noteKey ? html`<p class="hint prefs-warning">${t(tab.noteKey)}</p>` : null}
        <div class="prefs-groups">
          ${tab.groups.map((grp) => html`
            <fieldset>
              <legend>${t(grp.legendKey)}</legend>
              <div class="form-grid">${grp.fields.map((f) => buildField(catOf(tab, f), f))}</div>
            </fieldset>`)}
        </div>
        ${isGuest
          ? html`<p class="hint">${t("prefs_guest_readonly")}</p>`
          : html`<div class="toolbar prefs-actions"><button class="btn btn-primary admin-only" type="submit" disabled=${busy}>${t("prefs_apply")}</button></div>`}
      </div>
    </form>`;
}
