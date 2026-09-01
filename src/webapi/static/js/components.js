// Shared UI building blocks: small presentational preact components plus
// the two imperative helpers (toast, confirmDialog) reused across views.
// Class names match app.css.

import { html, render, useState, useEffect, useRef } from "./dom.js";
import { formatPercent } from "./format.js";
import { t, tn, terr, getLang } from "./i18n.js";
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

// Body of a list that has no rows. A `data` collection reads undefined until
// its snapshot lands and [] once it is known empty; the caller passes that as
// `loading` so a pending fetch doesn't claim "nothing here yet".
export function listPlaceholder(loading, emptyMsg) {
  return loading
    ? html`<${Placeholder} kind="loading">${t("app_loading")}<//>`
    : html`<${Placeholder} kind="info">${emptyMsg}<//>`;
}

// Row/select-all checkbox for a table's select column. The <label> fills the
// cell (see .cell-check), so the whole cell is the click target -- a bare box
// is a poor one.
export function checkCell(checked, onToggle, title) {
  return html`<label class="cell-check"><input type="checkbox" title=${title}
    checked=${checked} onChange=${(e) => onToggle(e.target.checked)} /></label>`;
}

// Relative, like BASE in api.js: it has to survive a reverse-proxy subpath.
const FLAG_BASE = window.location.pathname.replace(/\/?$/, "/") + "flags/";
const REGIONS = (() => {
  try { return new Intl.DisplayNames([getLang()], { type: "region" }); } catch (_) { return null; }
})();

// Flag from GET /flags/{code}.png (#694) next to the code, as the desktop lists
// draw it. Empty code (GeoIP off / unresolved IP) -> dash, no image; onError
// hides the image for a well-formed code the flag set has no artwork for.
export function CountryCell({ code }) {
  const cc = (code || "").toLowerCase();
  if (!cc) return "—";
  const CC = cc.toUpperCase();
  return html`<span class="country-cell" title=${(REGIONS && REGIONS.of(CC)) || CC}
    ><img class="flag" src=${FLAG_BASE + cc + ".png"} alt="" width="16" height="11"
          onError=${(e) => { e.target.style.visibility = "hidden"; }} />${CC}</span>`;
}

// --- download priority --------------------------------------------------
// Here rather than in downloads.js: the detail panel edits the same field, and
// downloads.js already imports it, so importing back would be a cycle.

export const PRIORITIES = ["auto", "low", "normal", "high"]
  .map((v) => [v, t("downloads_prio_" + v)]);

export function prioValue(d) { return d.priority_auto ? "auto" : d.priority; }

// Read-only rendering: "Auto (High)" when the daemon is choosing.
export function prioLabel(d) {
  const found = PRIORITIES.find(([v]) => v === d.priority);
  const base = found ? found[1] : d.priority;
  return d.priority_auto ? t("downloads_prio_auto") + " (" + base + ")" : base;
}

// --- detail-panel building blocks --------------------------------------
// Shared by the Downloads and Shared Files detail panels (see split-detail.js).

// Build a stat row tuple: [label, value, tooltip] with label/tooltip resolved
// from i18n keys. Consumed by Section() below.
export const statRow = (labelKey, value, tipKey) => [t(labelKey), value, t(tipKey)];

// A group of label/value stat cells (reuses the kad stat-grid look). Each cell
// carries an explanatory tooltip. `rows` is a list of statRow tuples. The detail
// panels stack several of these (separated by the .detail-sections gap).
// `titleKey` adds a heading; `actions` is a button row rendered before the grid.
export function Section(rows, titleKey, actions) {
  if (!rows.length) return null;
  const grid = html`
    <div class="kad-grid">
      ${rows.map(([label, value, tip]) => html`
        <div title=${tip || null}>
          <div class="kad-stat-label">${label}</div>
          <div class="kad-stat-value">${value}</div>
        </div>`)}
    </div>`;
  if (!titleKey && !actions) return grid;
  return html`
    <div class="detail-group">
      ${titleKey ? html`<h5 class="detail-group-title">${t(titleKey)}</h5>` : null}
      ${actions ? html`<div class="detail-actions">${actions}</div>` : null}
      ${grid}
    </div>`;
}

// The directory only, so the file path has to be composed; separator taken from
// the path itself since amuled may be on Windows. Names the *file*: a
// partfile's bytes are still under `part_file_name`, so this is where the
// download will land rather than what is on disk right now.
//
// Reads `directory` with a `path` fallback because this helper serves both
// detail panels and only /downloads has been renamed so far. Drop the fallback
// when /shared moves.
function fullPath(file) {
  const dir = file.directory || file.path || "";
  return dir + (dir.includes("\\") ? "\\" : "/") + (file.name || "");
}

// Rejects "" (a path the daemon has not reported yet); accepts a POSIX path
// or a Windows drive letter.
const hasRealPath = (file) => /^([/\\]|[A-Za-z]:)/.test(file.directory || file.path || "");

// The identity group shared by both detail panels: the hash plus extra fields
// (directory, part_file_name / parts), with the copy buttons as the group's action row.
// `extra` is a list of statRow tuples; `titleKey` is forwarded to Section().
export function IdentityLine({ file, copy, extra, titleKey }) {
  const hash = statRow("identity_hash",
    html`<span class="mono">${(file.hash || "").toUpperCase()}</span>`,
    "identity_tip_hash");
  const actions = html`
    <button class="btn btn-sm" type="button" onClick=${() => copy(file.ed2k_link)}>
      <${Icon} name="copy" /> ${t("identity_copy_ed2k")}
    </button>
    <button class="btn btn-sm" type="button" onClick=${() => copy(magnetLink(file))}>
      <${Icon} name="copy" /> ${t("identity_copy_magnet")}
    </button>
    ${hasRealPath(file) ? html`
      <button class="btn btn-sm" type="button" onClick=${() => copy(fullPath(file))}>
        <${Icon} name="copy" /> ${t("identity_copy_path")}
      </button>` : null}`;
  return Section([hash, ...extra], titleKey, actions);
}

// Flat notebook-style tab strip (aMule CMuleNotebook look). `tabs` is a list
// of { key, label, badge?, cls?, title?, closeLabel? }; `active` is the
// selected key; `onSelect(key)` is called on click. `cls` goes on the strip
// itself. Pass `onClose(key)` for a close affordance per tab; callers that
// omit it render exactly as before.
//
// A closable tab becomes a two-button group rather than a button inside a
// button: nesting interactive elements is invalid markup and screen readers do
// not reliably expose the inner one.
export function Tabs({ tabs, active, onSelect, onClose, extra, cls = "" }) {
  const tabButton = (tab) => html`
    <button type="button" role="tab"
            class=${"tab" + (tab.key === active ? " active" : "") + (tab.cls ? " " + tab.cls : "")}
            aria-selected=${tab.key === active} title=${tab.title}
            onClick=${() => onSelect(tab.key)}>
      ${tab.label}
      ${tab.badge != null ? html`<span class="tab-badge">${tab.badge}</span>` : null}
    </button>`;
  return html`
    <div class=${"tabs" + (cls ? " " + cls : "")} role="tablist">
      ${tabs.map((tab) => (onClose ? html`
        <span class="tab-item" role="presentation">
          ${tabButton(tab)}
          <button type="button" class="tab-close" title=${tab.closeLabel}
                  aria-label=${tab.closeLabel} onClick=${() => onClose(tab.key)}>×</button>
        </span>` : tabButton(tab)))}
      ${extra ? html`<div class="tabs-extra">${extra}</div>` : null}
    </div>`;
}

// --- file comments / ratings -------------------------------------------
// Shared by both detail panels. Rating is a 0-5 integer (0 = not rated),
// with -1 in a per-source entry meaning "comment but no rating". The labels
// mirror the desktop GetRateString() (src/OtherFunctions.cpp).

const RATING_OPTIONS = [0, 1, 2, 3, 4, 5];

// The comments/ratings table, shared by the download detail panel (source
// comments + retrieved Kad notes) and the search view's per-result dialog
// (Kad notes only). Both render the same rows, so they render them from the
// same place -- a second hand-written copy is how the two drift apart.
export function CommentsList({ comments }) {
  const list = comments || [];
  if (!list.length) return html`<${Placeholder} kind="info">${t("comments_none")}<//>`;
  return html`
    <table class="comments-list">
      <thead><tr>
        <th>${t("comments_col_username")}</th>
        <th>${t("comments_col_rating")}</th>
        <th>${t("comments_col_filename")}</th>
        <th>${t("comments_col_comment")}</th>
      </tr></thead>
      <tbody>
        ${list.map((c, i) => html`
          <tr key=${i}>
            <td>${c.username}</td>
            <td><span class=${"rating-badge rating-" + c.rating}>${ratingLabel(c.rating)}</span></td>
            <td class="comments-fname" title=${c.filename}>${c.filename}</td>
            <td>${c.comment}</td>
          </tr>`)}
      </tbody>
    </table>`;
}

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
      await api.patch(kind + "/" + hash, { my_comment: text, my_rating: Number(rate) });
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
    "&xl=" + (d.size_bytes || 0);
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

// --- pieces bar (downloads chunk map + shared availability bar) --------

// The pieces bar mirrors the aMule GUI download bar, theme-tuned via CSS vars:
// green (--ok) = have it, blue (--piece-avail-lo -> --piece-avail, faded by
// source count) = available, red (--bad) = missing (nobody has it).
// Sources at which an available part reaches full-intensity blue (aMule's
// gradient saturates around 10 sources: blue = 210 - 22*(sources-1)).
const AVAIL_FULL = 10;

// Parse a CSS colour ("#rgb", "#rrggbb", or "rgb(...)") to [r,g,b].
function toRGB(s) {
  s = (s || "").trim();
  if (s[0] === "#") {
    let h = s.slice(1);
    if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    const n = parseInt(h, 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }
  const m = s.match(/[\d.]+/g);
  return m ? [+m[0], +m[1], +m[2]] : [0, 0, 0];
}
// Linear blend a->b by f in [0,1], as a canvas fillStyle string.
function mix(a, b, f) {
  return "rgb(" + Math.round(a[0] + (b[0] - a[0]) * f) + "," +
    Math.round(a[1] + (b[1] - a[1]) * f) + "," +
    Math.round(a[2] + (b[2] - a[2]) * f) + ")";
}

// Blue shade for a part that at least one peer has: fades from
// --piece-avail-lo to --piece-avail as the source count rises, matching
// the desktop gradient (blue = 210 - 22*(sources-1), saturating near 10).
function availShade(sources, lo, hi) {
  const frac = Math.min(1, Math.max(0, ((sources || 1) - 1) / (AVAIL_FULL - 1)));
  return mix(lo, hi, frac);
}

// Chunk map: one proportional slice per ~9.28 MB part.
// `mode="download"` colours by local state (the Downloads panel);
// `mode="availability"` colours purely by source count (the Shared
// panel's "Obtained Parts" bar, mirroring the desktop column);
// `mode="hashing"` splits the bar green/amber at the hashed/pending boundary
// while a re-hash runs over the file (Verify Local Data, or an AICH hashset
// rebuild), mirroring the desktop's two-span bar. That pass reports only a
// *count* of parts done, never a per-part map, so it is driven by
// `total` (= parts_total_count) + `hashed` instead of `parts`.
// Canvas rather than N <div>s so files with hundreds/thousands of parts redraw
// cheaply on every live tick. Colours are read from the theme each draw.
export function PiecesBar({ parts, mode = "download", total = 0, hashed = 0 }) {
  const ref = useRef(null);
  const drawRef = useRef(null);
  const partsRef = useRef(parts);
  partsRef.current = parts;
  const modeRef = useRef(mode);
  modeRef.current = mode;
  const totalRef = useRef(total);
  totalRef.current = total;
  const hashedRef = useRef(hashed);
  hashedRef.current = hashed;

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const draw = () => {
      const cs = getComputedStyle(document.documentElement);
      const complete = cs.getPropertyValue("--ok").trim();
      const missing = cs.getPropertyValue("--bad").trim();
      // not-yet-hashed remainder of a hashing pass (desktop: amber)
      const pending = cs.getPropertyValue("--warn").trim();
      // available parts fade from few-sources -> many-sources blue
      const availLo = toRGB(cs.getPropertyValue("--piece-avail-lo"));
      const availHi = toRGB(cs.getPropertyValue("--piece-avail"));
      const dpr = window.devicePixelRatio || 1;
      const w = Math.max(1, Math.round(canvas.clientWidth * dpr));
      const h = Math.max(1, Math.round(canvas.clientHeight * dpr));
      if (canvas.width !== w) canvas.width = w;
      if (canvas.height !== h) canvas.height = h;
      const ctx = canvas.getContext("2d");
      ctx.clearRect(0, 0, w, h);
      const list = partsRef.current || [];
      const hashing = modeRef.current === "hashing";
      const count = hashing ? totalRef.current : list.length;
      const pw = w / (count || 1);
      // When pieces are wide enough, leave a 1px gap so each piece is
      // individually countable; when many/thin, overdraw to avoid seams and
      // let them blend into a continuous availability bar (the cleared track
      // background shows through the gaps).
      const gap = pw >= 6 * dpr ? Math.max(1, Math.round(dpr)) : 0;
      // Availability mode has no local-completeness axis at all: a shared
      // file is 100% local by definition, so every part is either held by
      // some peer (blue, faded by how many) or by nobody (red).
      const avail = modeRef.current === "availability";
      for (let i = 0; i < count; i++) {
        const p = list[i];
        let fill;
        // Overshoot needs no clamp: `i < hashed` just saturates to all-green,
        // which is what the desktop's byte-offset clamp achieves.
        if (hashing) fill = i < hashedRef.current ? complete : pending;
        else if (avail) {
          fill = p.sources > 0 ? availShade(p.sources, availLo, availHi) : missing;
        } else if (p.state === "complete") fill = complete;
        else if (p.state === "pending") {
          fill = availShade(p.sources, availLo, availHi);
        } else fill = missing;
        ctx.fillStyle = fill;
        const x0 = Math.round(i * pw), x1 = Math.round((i + 1) * pw);
        const width = gap ? Math.max(1, x1 - x0 - gap) : (x1 - x0) + 1;
        ctx.fillRect(x0, 0, width, h);
      }
      writeTitle();
    };
    // Per-part readout via the native `title` tooltip: no positioning code,
    // no CSS, no new strings -- composed from the legend's own labels. The
    // canvas itself is aria-hidden (the legend beside it is the text
    // alternative), so this is a pointer-only progressive enhancement.
    //
    // The hovered piece is remembered so a redraw can refresh its label: the
    // pointer can sit still while what is under it changes -- a part completing
    // in download mode, or the boundary sweeping past it in hashing mode --
    // which would otherwise leave the reading stale until the mouse moved.
    let hover = -1;
    const writeTitle = () => {
      const list = partsRef.current || [];
      const avail = modeRef.current === "availability";
      const hashing = modeRef.current === "hashing";
      // Same piece count the draw uses, so the hovered index matches the fill.
      const count = hashing ? totalRef.current : list.length;
      if (hover < 0 || hover >= count) { canvas.title = ""; return; }
      const p = list[hover];
      // Show the source count only where the colour actually encodes it,
      // i.e. wherever the fill came from availShade(). A `pending` part
      // with no sources still paints blue but has nothing to count.
      const graded = !hashing && (avail || p.state === "pending") && p.sources > 0;
      const label = hashing
        ? t(hover < hashedRef.current ? "pieces_hashed" : "pieces_pending")
        : avail
        ? t(p.sources > 0 ? "pieces_available" : "pieces_no_sources")
        : t(p.state === "complete" ? "pieces_complete"
          : p.state === "pending" ? "pieces_available" : "pieces_missing");
      // Only write on a real change: mousemove fires far more often than the
      // hovered piece changes, and each write is a DOM attribute mutation.
      const next = "#" + (hover + 1) + " \u00b7 " + label + (graded ? " \u00b7 " + p.sources : "");
      if (canvas.title !== next) canvas.title = next;
    };
    const onMove = (e) => {
      const list = partsRef.current || [];
      const count = modeRef.current === "hashing" ? totalRef.current : list.length;
      // A zero clientWidth would make offsetX/width NaN and index past the end.
      const w = canvas.clientWidth;
      hover = (!count || !w) ? -1
        : Math.min(count - 1, Math.max(0, Math.floor((e.offsetX / w) * count)));
      writeTitle();
    };
    canvas.addEventListener("mousemove", onMove);
    drawRef.current = draw;
    draw();
    // Colours are read from CSS vars on every draw, so a theme switch only
    // needs a redraw (mirrors charts.js).
    const ro = new ResizeObserver(draw);
    ro.observe(canvas);
    const mo = new MutationObserver(draw);
    mo.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
    const mq = window.matchMedia("(prefers-color-scheme: dark)");
    mq.addEventListener("change", draw);
    return () => {
      ro.disconnect(); mo.disconnect();
      mq.removeEventListener("change", draw);
      canvas.removeEventListener("mousemove", onMove);
    };
  }, []);

  useEffect(() => { drawRef.current && drawRef.current(); }, [parts, mode, total, hashed]);

  return html`<div class="pieces-bar"><canvas ref=${ref} aria-hidden="true"></canvas></div>`;
}

export function PiecesLegend({ parts, mode = "download", total = 0, hashed = 0 }) {
  const chip = (v) => html`<span class="legend-chip" style=${{ background: "var(" + v + ")" }}></span>`;
  // Hashing mode: hashed / pending counts and the percentage. Clamped because
  // amuled reports part + 1, which can overshoot `total` on the short tail
  // part -- pending must never go negative.
  if (mode === "hashing") {
    const n = Math.min(Number(hashed) || 0, total);
    return html`
      <div class="chart-legend pieces-legend" title=${t("pieces_hint_hashing")}>
        <span class="legend-item">${chip("--ok")} ${t("pieces_hashed")} <b>${n}</b></span>
        <span class="legend-item">${chip("--warn")} ${t("pieces_pending")} <b>${total - n}</b></span>
        <span class="pieces-total">
          ${formatPercent(total ? (n / total) * 100 : 0)} · ${tn("pieces_count", total)}
        </span>
      </div>`;
  }
  // Availability mode: two buckets only (some source / no source), plus the
  // gradient scale that explains what the blue shade encodes.
  if (mode === "availability") {
    let avail = 0;
    for (const p of parts) if (p.sources > 0) avail++;
    return html`
      <div class="chart-legend pieces-legend">
        <span class="legend-item" title=${t("pieces_hint_peers")}>
          ${chip("--piece-avail")} ${t("pieces_available")} <b>${avail}</b>
          ${avail > 0 ? html`
            <span class="pieces-scale">
              <small>(${t("pieces_fewer")}</small>
              <span class="pieces-scale-bar"></span>
              <small>${t("pieces_more")})</small>
            </span>` : null}
        </span>
        <span class="legend-item" title=${t("pieces_no_sources_hint")}>
          ${chip("--bad")} ${t("pieces_no_sources")} <b>${parts.length - avail}</b>
        </span>
        <span class="pieces-total">${tn("pieces_count", parts.length)}</span>
      </div>`;
  }
  const counts = { complete: 0, pending: 0, unavailable: 0 };
  for (const p of parts) counts[p.state] = (counts[p.state] || 0) + 1;
  // One line: three flat state chips (green/red/blue), then the availability
  // gradient scale grouped right after "Available" (it explains that blue's
  // shade encodes source count) — keeps the row compact and the chips uniform.
  return html`
    <div class="chart-legend pieces-legend">
      <span class="legend-item">${chip("--ok")} ${t("pieces_complete")} <b>${counts.complete}</b></span>
      <span class="legend-item">${chip("--bad")} ${t("pieces_missing")} <b>${counts.unavailable}</b></span>
      <span class="legend-item" title=${t("pieces_hint_sources")}>
        ${chip("--piece-avail")} ${t("pieces_available")} <b>${counts.pending}</b>
        ${counts.pending > 0 ? html`
          <span class="pieces-scale">
            <small>(${t("pieces_fewer")}</small>
            <span class="pieces-scale-bar"></span>
            <small>${t("pieces_more")})</small>
          </span>` : null}
      </span>
      <span class="pieces-total">${tn("pieces_count", parts.length)}</span>
    </div>`;
}
