// Shared directories editor, shown in Preferences -> Directories -> Shared
// folders. Replaces the old `directories.shared_paths` textarea, which wrote
// the derived union back through PATCH /preferences: that lost the recursive
// flag, validated nothing, persisted nothing and got reverted by the next
// reconcile. This panel talks to /share_directories, the endpoint that
// validates each path server-side, sets both intent lists and saves.
//
// Immediate per-row: add and recursive-toggle POST one entry (idempotent), and
// delete removes one; the core applies each under its own lock, so there is no
// client-side read-modify-write race. Each write is followed by a re-GET
// because the bulk envelope reports ok/rejected per path but not the recursive
// flag. Admin-only edit; guests see the list read-only.

import { api, bulkFailures } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder, toast } from "../components.js";
import { Icon } from "../icons.js";
import { t, terr } from "../i18n.js";

export function SharedDirectories({ isGuest }) {
  const [dirs, setDirs] = useState(undefined); // undefined until the first GET
  const [err, setErr] = useState("");
  const [path, setPath] = useState("");
  const [recursive, setRecursive] = useState(false);
  const [busy, setBusy] = useState(false);

  const load = async () => {
    try { setDirs((await api.get("share_directories")).directories || []); setErr(""); }
    catch (e) { setErr(terr(e) || t("prefs_shdir_error")); setDirs([]); }
  };
  useEffect(() => { load(); }, []);

  // The POST bulk envelope carries one {id: path, ok, error} per applied root.
  // A partial reject answers 207, which api.js resolves rather than throws, so
  // the rejection is read from results[], not caught.
  const rejectionFor = (res, p) => {
    const hit = ((res && res.results) || []).find((r) => r.id === p && r.ok === false);
    return hit ? hit.error : null;
  };

  // Every single-path op re-applies the whole intent list, so the core can drop
  // a *sibling* root the caller never touched (e.g. one that became unreadable).
  // Surface those so the drop isn't silent; `exclude` skips the row the caller
  // already reported on its own.
  const warnSiblings = (res, exclude) => {
    const others = bulkFailures(res).filter((r) => r.id !== exclude);
    if (others.length) {
      toast(t("common_bulk_partial", { failed: others.length,
        total: (res.results || []).length, message: terr(others[0].error) }), "warn");
    }
  };

  const upsert = async (p, rec, isAdd) => {
    setBusy(true);
    try {
      const res = await api.post("share_directories", { path: p, recursive: rec });
      const bad = rejectionFor(res, p);
      if (bad) { toast(terr(bad) || t("prefs_shdir_error"), "error"); return; }
      warnSiblings(res, p);
      if (isAdd) { setPath(""); setRecursive(false); }
      toast(t(isAdd ? "prefs_shdir_toast_added" : "prefs_shdir_toast_updated"), "success");
    } catch (e) { toast(terr(e) || t("prefs_shdir_error"), "error"); }
    finally { setBusy(false); await load(); }
  };

  const remove = async (p) => {
    setBusy(true);
    try {
      warnSiblings(await api.del("share_directories?path=" + encodeURIComponent(p)));
      toast(t("prefs_shdir_toast_removed"), "success");
    } catch (e) { toast(terr(e) || t("prefs_shdir_error"), "error"); }
    finally { setBusy(false); await load(); }
  };

  const submitAdd = () => { const p = path.trim(); if (!busy && p) upsert(p, recursive, true); };
  // Not a <form>: this renders inside the preferences <form>, so Enter would
  // otherwise fire the bulk Apply (same reason AmuleApiCredentials avoids one).
  const onKeyDown = (e) => { if (e.key === "Enter") { e.preventDefault(); submitAdd(); } };

  if (dirs === undefined) return html`<${Placeholder} kind="loading">${t("prefs_shdir_loading")}<//>`;

  return html`
    <div class="shared-dirs">
      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr>
              <th>${t("prefs_shdir_col_path")}</th>
              <th>${t("prefs_shdir_col_recursive")}</th>
              ${isGuest ? null : html`<th class="admin-only">${t("prefs_shdir_col_actions")}</th>`}
            </tr>
          </thead>
          <tbody>
            ${err ? null : dirs.map((d) => html`
              <tr>
                <td class="name">${d.path}</td>
                <td>
                  <input type="checkbox" checked=${!!d.recursive} disabled=${isGuest || busy}
                         title=${t("prefs_shdir_col_recursive")}
                         onChange=${() => upsert(d.path, !d.recursive, false)} />
                </td>
                ${isGuest ? null : html`
                  <td class="row-actions admin-only">
                    <button class="btn btn-icon btn-sm btn-danger" type="button" disabled=${busy}
                            title=${t("prefs_shdir_remove")} onClick=${() => remove(d.path)}>
                      <${Icon} name="trash" />
                    </button>
                  </td>`}
              </tr>`)}
          </tbody>
        </table>
        ${err ? html`<div class="table-empty"><${Placeholder} kind="error">${err}<//></div>`
          : dirs.length ? null
            : html`<div class="table-empty"><${Placeholder} kind="info">${t("prefs_shdir_empty")}<//></div>`}
      </div>
      ${isGuest ? null : html`
        <div class="shared-dirs-add toolbar admin-only" onKeyDown=${onKeyDown}>
          <input class="input" type="text" placeholder=${t("prefs_shdir_path_ph")}
                 value=${path} disabled=${busy} onInput=${(e) => setPath(e.target.value)} />
          <label class="field-inline">
            <input type="checkbox" checked=${recursive} disabled=${busy}
                   onChange=${(e) => setRecursive(e.target.checked)} />
            ${t("prefs_shdir_recursive")}
          </label>
          <button class="btn btn-primary btn-sm" type="button" disabled=${busy || !path.trim()}
                  onClick=${submitAdd}>${t("prefs_shdir_add")}</button>
        </div>`}
    </div>`;
}
