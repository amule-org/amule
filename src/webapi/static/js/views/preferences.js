// Preferences view, mirroring the desktop PrefsUnifiedDlg: category list on
// the left, active page on the right, GUI-style group boxes (fieldsets)
// inside. Reads /preferences into one shared form state (edits survive
// category switches), PATCHes the editable subset back with a single Apply.
// Admin-only edit; guest sees a read-only form.

import { api } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder, toast, Tabs } from "../components.js";
import { t } from "../i18n.js";

// Categories -> group boxes -> fields (keys the PATCH endpoint accepts;
// readonly fields are shown but never sent). Labels come from the
// "pref_<key>" i18n entries unless labelKey overrides. min/max mirror the
// GUI spinner ranges.
const SECTIONS = [
  { id: "general", labelKey: "prefs_general", groups: [
    { fields: [
      { key: "nickname", type: "text" },
      { key: "host_name", type: "text" },
      { key: "check_new_version", type: "bool" },
      { key: "user_hash", type: "text", readonly: true, labelKey: "prefs_user_hash_readonly" },
    ] },
  ] },
  { id: "connection", labelKey: "prefs_connection", groups: [
    { legendKey: "prefs_group_bandwidth", fields: [
      { key: "max_download_kbps", type: "int", min: 0 },
      { key: "max_upload_kbps", type: "int", min: 0 },
      { key: "slot_allocation", type: "int", min: 1, max: 100000 },
    ] },
    { legendKey: "prefs_group_capacities", fields: [
      { key: "max_download_cap_kbps", type: "int", min: 0 },
      { key: "max_upload_cap_kbps", type: "int", min: 0 },
    ] },
    { legendKey: "prefs_group_ports", fields: [
      { key: "tcp_port", type: "int", min: 1, max: 65535 },
      { key: "udp_port", type: "int", min: 1, max: 65535 },
      { key: "udp_disabled", type: "bool" },
    ] },
    { legendKey: "prefs_group_conn_limits", fields: [
      { key: "max_sources_per_file", type: "int", min: 40, max: 5000 },
      { key: "max_connections", type: "int", min: 5, max: 7500 },
    ] },
    { legendKey: "prefs_group_networks", fields: [
      { key: "network_kad", type: "bool" },
      { key: "network_ed2k", type: "bool", readonly: true, labelKey: "prefs_ed2k_readonly" },
    ] },
    { fields: [
      { key: "autoconnect", type: "bool" },
      { key: "reconnect", type: "bool" },
    ] },
  ] },
];

export default function Preferences({ isGuest }) {
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState("");
  const [values, setValues] = useState({}); // "section.key" -> value
  const [active, setActive] = useState("general");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    api.get("preferences").then((p) => {
      const v = {};
      for (const s of SECTIONS)
        for (const grp of s.groups)
          for (const f of grp.fields) v[s.id + "." + f.key] = (p[s.id] || {})[f.key];
      setValues(v);
      setLoaded(true);
    }).catch((e) => setError(e.message || t("prefs_error")));
  }, []);

  const setVal = (id, val) => setValues((vs) => ({ ...vs, [id]: val }));

  const buildField = (section, f) => {
    const id = section + "." + f.key;
    const val = values[id];
    const label = t(f.labelKey || "prefs_field_" + f.key);
    const disabled = isGuest || f.readonly;
    if (f.type === "bool") {
      return html`
        <div class="field field-inline">
          <input type="checkbox" id=${id} checked=${!!val} disabled=${disabled}
                 onChange=${(e) => setVal(id, e.target.checked)} />
          <label for=${id}>${label}</label>
        </div>`;
    }
    return html`
      <div class="field">
        <label for=${id}>${label}</label>
        <input class="input" id=${id} disabled=${disabled} type=${f.type === "int" ? "number" : "text"}
               min=${f.min} max=${f.max}
               value=${val === undefined || val === null ? "" : val} onInput=${(e) => setVal(id, e.target.value)} />
      </div>`;
  };

  const collect = (section) => {
    const out = {};
    for (const grp of section.groups) {
      for (const f of grp.fields) {
        if (f.readonly) continue;
        const val = values[section.id + "." + f.key];
        out[f.key] = f.type === "bool" ? !!val
          : f.type === "int" ? Math.max(0, parseInt(val, 10) || 0)
          : (val == null ? "" : val);
      }
    }
    return out;
  };

  const save = async (e) => {
    e.preventDefault();
    const body = {};
    for (const s of SECTIONS) body[s.id] = collect(s);
    setBusy(true);
    try { await api.patch("preferences", body); toast(t("prefs_toast_saved"), "success"); }
    catch (err) { toast(err.message || t("prefs_error"), "error"); }
    finally { setBusy(false); }
  };

  if (error) return html`<p>${error}</p>`;
  if (!loaded) return html`<${Placeholder} kind="loading">${t("prefs_loading")}<//>`;

  const section = SECTIONS.find((s) => s.id === active);
  const gridClass = section.id === "general" ? "form-grid" : "form-grid form-grid-2";
  const tabList = SECTIONS.map((s) => ({ key: s.id, label: t(s.labelKey) }));
  return html`
    <form onSubmit=${save} class="net-pane">
      <${Tabs} tabs=${tabList} active=${active} onSelect=${setActive} />
      <div class="net-pane-body prefs-panel">
        ${section.groups.map((grp) => {
          const fields = html`<div class=${gridClass}>${grp.fields.map((f) => buildField(section.id, f))}</div>`;
          return grp.legendKey
            ? html`<fieldset><legend>${t(grp.legendKey)}</legend>${fields}</fieldset>`
            : fields;
        })}
        ${isGuest
          ? html`<p class="hint">${t("prefs_guest_readonly")}</p>`
          : html`<div class="toolbar prefs-actions"><button class="btn btn-primary admin-only" type="submit" disabled=${busy}>${t("prefs_apply")}</button></div>`}
      </div>
    </form>`;
}
