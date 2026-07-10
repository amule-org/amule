// Categories panel: CRUD for download categories, shown inline on the
// Downloads page (it replaces the category tabs when "Manage categories" is
// toggled on). No SSE channel, so the list is fetched on mount and after each
// mutation. Index 0 (the default "all") cannot be edited or deleted. Adding
// and editing open a modal form; deleting uses a confirm dialog. Admin-only
// edit; guests see just the list.

import { api } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder, toast, confirmDialog } from "../components.js";
import { Icon } from "../icons.js";
import { t, terr } from "../i18n.js";

const PRIORITIES = ["auto", "low", "normal", "high"]
  .map((v) => [v, t("downloads_prio_" + v)]);

export function CategoriesPanel({ isGuest }) {
  const [categories, setCategories] = useState([]);
  const [loadErr, setLoadErr] = useState("");
  const [formOpen, setFormOpen] = useState(false);
  const [editing, setEditing] = useState(null); // null = create mode, else index
  const [name, setName] = useState("");
  const [path, setPath] = useState("");
  const [comment, setComment] = useState("");
  const [color, setColor] = useState("#1664c0");
  const [prio, setPrio] = useState("auto");

  const load = async () => {
    try { setCategories((await api.get("categories")).categories || []); setLoadErr(""); }
    catch (e) { setLoadErr(terr(e) || t("downloads_cat_error")); }
  };
  useEffect(() => { load(); }, []);

  const openCreate = () => {
    setEditing(null); setName(""); setPath(""); setComment(""); setColor("#1664c0"); setPrio("auto");
    setFormOpen(true);
  };
  const openEdit = (c) => {
    setEditing(c.index);
    setName(c.name || ""); setPath(c.path || ""); setComment(c.comment || "");
    setColor(intToHex(c.color)); setPrio(c.priority || "auto");
    setFormOpen(true);
  };

  const save = async (e) => {
    e.preventDefault();
    const n = name.trim(), p = path.trim();
    if (!n || !p) { toast(t("downloads_cat_toast_name_path_required"), "warn"); return; }
    const body = { name: n, path: p, color: hexToInt(color), priority: prio };
    if (comment.trim()) body.comment = comment.trim();
    try {
      if (editing !== null) await api.patch("categories/" + editing, body);
      else await api.post("categories", body);
      toast(t("downloads_cat_toast_saved"), "success"); setFormOpen(false); load();
    } catch (err) { toast(terr(err) || t("downloads_cat_error"), "error"); }
  };
  const remove = async (c) => {
    if (!(await confirmDialog(t("downloads_cat_confirm_delete", { name: c.name })))) return;
    try { await api.del("categories/" + c.index); toast(t("downloads_cat_toast_deleted"), "success"); load(); }
    catch (e) { toast(terr(e) || t("downloads_cat_error"), "error"); }
  };

  const row = (c) => html`
    <tr>
      <td class="name">${c.index === 0 ? html`<strong>${t("downloads_cat_none")}</strong>` : c.name}</td>
      <td>${c.comment || ""}</td>
      <td>${c.path || ""}</td>
      <td>${prioLabel(c.priority)}</td>
      <td><span class="color-swatch" style=${{ background: intToHex(c.color) }}></span> ${intToHex(c.color)}</td>
      ${isGuest ? null : html`
        <td class="row-actions admin-only">
          ${c.index === 0 ? null : html`
            <button class="btn btn-icon btn-sm" title=${t("downloads_cat_edit")} onClick=${() => openEdit(c)}>
              <${Icon} name="edit" />
            </button>
            <button class="btn btn-icon btn-sm btn-danger" title=${t("downloads_cat_delete")} onClick=${() => remove(c)}>
              <${Icon} name="cancel" />
            </button>`}
        </td>`}
    </tr>`;

  const colspan = isGuest ? 5 : 6;

  return html`
    <div class="card">
      <div class="cat-panel-header">
        <h3>${t("downloads_cat_title")}</h3>
        <div class="spacer"></div>
        ${isGuest ? null : html`
          <button class="btn btn-sm admin-only" type="button" onClick=${openCreate}>
            ${t("downloads_cat_add")}
          </button>`}
      </div>
      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr>
              <th>${t("downloads_cat_name")}</th><th>${t("downloads_cat_comment")}</th><th>${t("downloads_cat_incoming_dir")}</th>
              <th>${t("downloads_cat_priority")}</th><th>${t("downloads_cat_color")}</th>
              ${isGuest ? null : html`<th class="admin-only">${t("downloads_cat_actions")}</th>`}
            </tr>
          </thead>
          <tbody>
            ${loadErr ? html`<tr><td colspan=${colspan}>${loadErr}</td></tr>`
              : categories.length
                ? categories.map(row)
                : html`<tr><td colspan=${colspan}><${Placeholder} kind="info">${t("downloads_cat_empty")}<//></td></tr>`}
          </tbody>
        </table>
      </div>
    </div>

    ${formOpen && !isGuest ? html`
      <div class="modal-overlay" onClick=${(e) => { if (e.target === e.currentTarget) setFormOpen(false); }}>
        <div class="modal">
          <form onSubmit=${save}>
            <div class="modal-header">
              <h3>${editing !== null ? t("downloads_cat_edit_title") : t("downloads_cat_add")}</h3>
            </div>
            <div class="form-grid form-grid-2">
              ${field(t("downloads_cat_name"), html`<input class="input" placeholder=${t("downloads_cat_name_ph")} required value=${name} onInput=${(e) => setName(e.target.value)} />`)}
              ${field(t("downloads_cat_comment"), html`<input class="input" placeholder=${t("downloads_cat_comment_ph")} value=${comment} onInput=${(e) => setComment(e.target.value)} />`)}
              ${field(t("downloads_cat_path"), html`<input class="input" placeholder=${t("downloads_cat_incoming_path_ph")} required value=${path} onInput=${(e) => setPath(e.target.value)} />`)}
              ${field(t("downloads_cat_priority"), html`<select class="input" value=${prio} onChange=${(e) => setPrio(e.target.value)}>${PRIORITIES.map(([v, l]) => html`<option value=${v}>${l}</option>`)}</select>`)}
              ${field(t("downloads_cat_color"), html`<input class="input" type="color" value=${color} onInput=${(e) => setColor(e.target.value)} />`)}
            </div>
            <div class="modal-actions">
              <button class="btn" type="button" onClick=${() => setFormOpen(false)}>${t("downloads_cat_cancel")}</button>
              <button class="btn btn-primary" type="submit">${editing !== null ? t("downloads_cat_save") : t("downloads_cat_create")}</button>
            </div>
          </form>
        </div>
      </div>` : null}`;
}

function field(label, control) {
  return html`<div class="field"><label>${label}</label>${control}</div>`;
}
function intToHex(n) { return "#" + ((Number(n) || 0) & 0xffffff).toString(16).padStart(6, "0"); }
function hexToInt(hex) { return parseInt((hex || "#000000").slice(1), 16) || 0; }
function prioLabel(p) {
  const f = PRIORITIES.find(([v]) => v === p);
  return f ? f[1] : (p || "");
}
