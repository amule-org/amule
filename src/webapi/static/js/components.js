// Shared UI building blocks: small presentational preact components plus
// the two imperative helpers (toast, confirmDialog) reused across views.
// Class names match app.css.

import { html, render, useState } from "./dom.js";
import { formatPercent } from "./format.js";
import { t, terr } from "./i18n.js";
import { api } from "./api.js";
import { Icon } from "./icons.js";

// --- presentational components -----------------------------------------

// Horizontal progress bar (0..100).
export function ProgressBar({ percent }) {
  const p = Math.max(0, Math.min(100, Number(percent) || 0));
  return html`
    <div class="progress" style=${{ "--p": p }}>
      <span class="progress-label">${formatPercent(p)}</span>
      <div class="progress-fill">
        <span class="progress-label progress-label-fill">${formatPercent(p)}</span>
      </div>
    </div>`;
}

// Inline status/label pill.
export function Badge({ kind = "", title, children }) {
  return html`<span class=${"badge " + kind} title=${title}>${children}</span>`;
}

// Empty / loading / error placeholder for view bodies.
export function Placeholder({ kind, children }) {
  return html`<div class=${"placeholder placeholder-" + kind}>${children}</div>`;
}

// --- detail-panel building blocks --------------------------------------
// Shared by the Downloads and Shared Files detail panels (see split-detail.js).

// Build a stat row tuple: [label, value, tooltip] with label/tooltip resolved
// from i18n keys. Consumed by Section() below.
export const statRow = (labelKey, value, tipKey) => [t(labelKey), value, t(tipKey)];

// A group of label/value stat cells (reuses the kad stat-grid look). Each cell
// carries an explanatory tooltip. `rows` is a list of statRow tuples. The detail
// panels stack several of these (separated by the .detail-sections gap) to form
// their compact, title-less grid.
export function Section(rows) {
  if (!rows.length) return null;
  return html`
    <div class="kad-grid">
      ${rows.map(([label, value, tip]) => html`
        <div title=${tip || null}>
          <div class="kad-stat-label">${label}</div>
          <div class="kad-stat-value">${value}</div>
        </div>`)}
    </div>`;
}

// The identity group shared by both detail panels: the hash (with its two copy
// buttons stacked below it) plus extra fields (path, met_file / parts). Just a
// normal Section, so it lays out like every other group. `extra` is a list of
// statRow tuples.
export function IdentityLine({ file, copy, extra }) {
  const hash = statRow("downloads_detail_hash", html`
    <span class="mono">${(file.hash || "").toUpperCase()}</span>
    <div class="detail-actions">
      <button class="btn btn-sm" type="button" onClick=${() => copy(file.ed2k_link)}>
        <${Icon} name="copy" /> ${t("downloads_detail_copy_ed2k")}
      </button>
      <button class="btn btn-sm" type="button" onClick=${() => copy(magnetLink(file))}>
        <${Icon} name="copy" /> ${t("downloads_detail_copy_magnet")}
      </button>
    </div>`, "downloads_detail_tip_hash");
  return Section([hash, ...extra]);
}

// Flat notebook-style tab strip (aMule CMuleNotebook look). `tabs` is a list
// of { key, label, badge? }; `active` is the selected key; `onSelect(key)` is
// called on click.
export function Tabs({ tabs, active, onSelect }) {
  return html`
    <div class="tabs" role="tablist">
      ${tabs.map((tab) => html`
        <button type="button" role="tab"
                class=${"tab" + (tab.key === active ? " active" : "")}
                aria-selected=${tab.key === active}
                onClick=${() => onSelect(tab.key)}>
          ${tab.label}
          ${tab.badge != null ? html`<span class="tab-badge">${tab.badge}</span>` : null}
        </button>`)}
    </div>`;
}

// --- file comments / ratings -------------------------------------------
// Shared by both detail panels. Rating is a 0-5 integer (0 = not rated),
// with -1 in a per-source entry meaning "comment but no rating". The labels
// mirror the desktop GetRateString() (src/OtherFunctions.cpp).

const RATING_OPTIONS = [0, 1, 2, 3, 4, 5];

// i18n label for a rating value; -1 -> "comment only".
export function ratingLabel(r) {
  return r === -1 ? t("rating_none") : t("rating_" + r);
}

// Edit-your-own comment + rating form. `kind` is "downloads" or "shared";
// both PATCH endpoints accept {comment, rating} together (ADMIN, comment <= 50
// chars). Keyed on hash by the caller so switching files re-seeds the inputs.
// Wrapped in .admin-only so guests never see it (matches the app-wide gate).
// `disabled` greys the whole form out with an explanatory `disabledHint` — used
// by Downloads for a file that isn't shared yet (0 complete parts), which the
// daemon would reject with 409 not_shared.
export function CommentEditor({ hash, kind, comment, rating, onSaved, disabled = false, disabledHint }) {
  const [text, setText] = useState(comment || "");
  const [rate, setRate] = useState(Number(rating) || 0);
  const [busy, setBusy] = useState(false);

  const save = async () => {
    setBusy(true);
    try {
      await api.patch(kind + "/" + hash, { comment: text, rating: Number(rate) });
      toast(t("comments_saved"), "success");
      if (onSaved) onSaved();
    } catch (e) {
      toast(e.code === "not_shared" ? t("comments_not_shared") : terr(e), "error");
    } finally {
      setBusy(false);
    }
  };

  return html`
    <form class="comment-editor admin-only" onSubmit=${(e) => { e.preventDefault(); save(); }}>
      <label class="comment-editor-label">${t("comments_your_comment")}</label>
      <textarea class="input comment-editor-text" maxlength="50" rows="2"
                placeholder=${t("comments_placeholder")} disabled=${disabled}
                value=${text} onInput=${(e) => setText(e.target.value)}></textarea>
      <div class="comment-editor-row">
        <select class="input input-sm" value=${rate} disabled=${disabled}
                onChange=${(e) => setRate(e.target.value)}>
          ${RATING_OPTIONS.map((r) => html`<option value=${r}>${ratingLabel(r)}</option>`)}
        </select>
        <button class="btn btn-primary btn-sm" type="submit" disabled=${busy || disabled}>${t("comments_save")}</button>
      </div>
      ${disabled && disabledHint ? html`<p class="comment-editor-hint">${disabledHint}</p>` : null}
    </form>`;
}

// --- rename file --------------------------------------------------------
// Rename form shared by both detail panels. `kind` is "downloads" or
// "shared"; both PATCH endpoints accept {name} (ADMIN, mapped to
// EC_OP_RENAME_FILE). The daemon rejects empty names and names with path
// separators, so we mirror that client-side before sending. Keyed on hash by
// the caller so switching files re-seeds the input. Wrapped in .admin-only.
export function RenameForm({ hash, kind, name, onSaved, disabled = false }) {
  const [val, setVal] = useState(name || "");
  const [busy, setBusy] = useState(false);
  const clean = val.trim();
  const invalid = !clean || clean.includes("/") || clean.includes("\\");

  const save = async () => {
    setBusy(true);
    try {
      await api.patch(kind + "/" + hash, { name: clean });
      toast(t("rename_saved"), "success");
      if (onSaved) onSaved();
    } catch (e) {
      toast(terr(e), "error");
    } finally {
      setBusy(false);
    }
  };

  return html`
    <form class="rename-form admin-only" onSubmit=${(e) => { e.preventDefault(); save(); }}>
      <label class="rename-label">${t("filename_rename_label")}</label>
      <div class="rename-row">
        <input class="input input-sm" type="text" disabled=${disabled}
               placeholder=${t("filename_rename_placeholder")}
               value=${val} onInput=${(e) => setVal(e.target.value)} />
        <button class="btn btn-primary btn-sm" type="submit"
                disabled=${busy || disabled || invalid}>${t("comments_save")}</button>
      </div>
    </form>`;
}

// --- toast --------------------------------------------------------------
// Lightweight imperative notifications. A single host div is lazily created
// and reused; toasts auto-dismiss (errors linger a bit longer).

let toastHost = null;
export function toast(message, kind) {
  if (!toastHost) {
    toastHost = document.createElement("div");
    toastHost.className = "toast-host";
    document.body.appendChild(toastHost);
  }
  const node = document.createElement("div");
  node.className = "toast " + (kind || "info");
  node.textContent = message;
  toastHost.appendChild(node);
  setTimeout(() => {
    node.classList.add("toast-out");
    setTimeout(() => node.remove(), 300);
  }, kind === "error" ? 5000 : 3000);
}

// --- confirm dialog -----------------------------------------------------
// Promise-based modal confirm. Renders a preact tree into a throwaway host
// appended to <body>, resolves true/false, then unmounts and cleans up.

export function confirmDialog(message, { okLabel = t("common_yes"), cancelLabel = t("common_cancel") } = {}) {
  return new Promise((resolve) => {
    const host = document.createElement("div");
    document.body.appendChild(host);
    const close = (val) => { render(null, host); host.remove(); resolve(val); };
    render(html`
      <div class="modal-overlay" onClick=${(e) => { if (e.target === e.currentTarget) close(false); }}>
        <div class="modal">
          <p class="modal-msg">${message}</p>
          <div class="modal-actions">
            <button class="btn" onClick=${() => close(false)}>${cancelLabel}</button>
            <button class="btn btn-primary" onClick=${() => close(true)}>${okLabel}</button>
          </div>
        </div>
      </div>`, host);
  });
}

// --- ed2k link + clipboard helpers -------------------------------------
// Shared by the detail panels' Copy ED2K / Copy magnet buttons.

// Build the ed2k-compatible magnet URI exactly like the desktop GUI's
// CamuleAppCommon::CreateMagnetLink (src/amuleAppCommon.cpp): field order
// dn, xt:urn:ed2k, xt:urn:ed2khash, xl; hash lower-cased; the name only has
// spaces -> %20 and '/' stripped (CPath::Cleanup(false)), not full URL-encode.
export function magnetLink(d) {
  let dn = "";
  for (const ch of d.name || "") {
    if (ch === "/") continue;
    if (ch === " ") dn += "%20";
    else if (ch.codePointAt(0) >= 32) dn += ch;
  }
  const h = (d.hash || "").toLowerCase();
  return "magnet:?dn=" + dn +
    "&xt=urn:ed2k:" + h + "&xt=urn:ed2khash:" + h +
    "&xl=" + (d.size || 0);
}

// Copy to clipboard with a plain fallback for non-secure contexts (the web UI
// may be reached over http on a LAN IP, where navigator.clipboard is absent).
export async function copyText(text) {
  if (navigator.clipboard && window.isSecureContext) {
    await navigator.clipboard.writeText(text);
    return;
  }
  const ta = document.createElement("textarea");
  ta.value = text;
  ta.style.position = "fixed";
  ta.style.opacity = "0";
  document.body.appendChild(ta);
  ta.select();
  try { document.execCommand("copy"); } finally { ta.remove(); }
}
