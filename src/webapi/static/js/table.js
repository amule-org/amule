// Virtualized data table shared across list views (shared/downloads/search/
// servers/peers). Renders only the rows in (or near) the viewport so a list of
// thousands of files never puts thousands of <tr> in the DOM. Column-config
// driven — each view declares its columns; sorting state stays in the view.
//
//   const columns = [{ key: "name", label: t("..."), sortable: true,
//                      sortVal: (r) => r.name, cell: (r) => r.name }, ...];
//   html`<${VirtualTable} columns=${columns} rows=${rows} rowKey=${r => r.hash}
//                         sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
//                         empty=${html`...`} />`

import { html, useRef, useState, useEffect } from "./dom.js";
import { Icon } from "./icons.js";
import { loadPref, savePref } from "./store.js";
import { t } from "./i18n.js";

// Fixed row height: the scroll math assumes every row is exactly this tall, and
// each <tr> is pinned to it inline. Must fit the tallest cell content (input-sm
// selects, progress bars) without clipping.
// ponytail: 38px clears the tallest cell content (4px cell padding + input-sm
// select ≈ 35px) so the browser never grows a row past it — a row taller than
// ROW_HEIGHT would desync the scroll math. Retune if a view adds taller cells.
export const ROW_HEIGHT = 38;
const OVERSCAN = 8; // rows rendered above/below the viewport to hide scroll seams
// Minimum width for a flexible (no explicit width) column, matching the old
// non-virtual `.name { min-width: 220px }`. Feeds the table's computed min-width
// so narrow screens scroll horizontally instead of collapsing the name column.
const FLEX_MIN = 220;
// Floor for a user-resized column -- narrower than this and a header's label
// or sort arrow starts clipping/overlapping unreadably.
const MIN_COL_WIDTH = 48;

export function cmp(a, b) { return a < b ? -1 : a > b ? 1 : 0; }

// Sort a copy of `rows` by the column matching sortKey (using its sortVal
// accessor). Returns rows unchanged if no sortable column matches.
export function sortRows(rows, columns, sortKey, sortDir) {
  const col = columns.find((c) => c.key === sortKey && c.sortVal);
  if (!col) return rows;
  return rows.slice().sort((a, b) => sortDir * cmp(col.sortVal(a), col.sortVal(b)));
}

// Sort key for an IPv4 column: 10.x has to sort after 9.x, which a string
// compare gets backwards. Takes "1.2.3.4" or "1.2.3.4:port"; unparseable -> 0.
export function ipNum(ip) {
  return String(ip || "").split(".").reduce((acc, octet) => acc * 256 + (parseInt(octet, 10) || 0), 0);
}

// Build a predicate for a free-text filter box: case-insensitive and
// order-independent — every whitespace-separated token must appear in the
// string. Empty query → matches everything.
export function textMatcher(query) {
  const tokens = query.toLowerCase().split(/\s+/).filter(Boolean);
  return (s) => { const n = (s || "").toLowerCase(); return tokens.every((tok) => n.includes(tok)); };
}

const colClass = (c) => [c.num ? "num" : "", c.cls || ""].filter(Boolean).join(" ");

// Per-table UI prefs (sort direction + hidden columns + resized widths)
// persisted as one object under "amule.table.<storageKey>". `defaults` =
// { sortKey, sortDir, hidden:[] }. Stored prefs are spread over the defaults
// so a column added as default-hidden later stays hidden only for users who
// never opened the picker (their stored object has no entry for it → the
// default wins), while everyone else keeps their explicit choice. Returns
// the current sort plus toggles the view wires to VirtualTable.onSort and
// the ColumnPicker.
export function useTablePrefs(storageKey, defaults) {
  const [prefs, setPrefs] = useState(() => ({
    sortKey: defaults.sortKey,
    sortDir: defaults.sortDir,
    hidden: defaults.hidden || [],
    widths: {},
    ...loadPref("table." + storageKey, {}),
  }));
  useEffect(() => { savePref("table." + storageKey, prefs); }, [prefs]);

  const toggleSort = (key) =>
    setPrefs((p) => (p.sortKey === key
      ? { ...p, sortDir: -p.sortDir }
      : { ...p, sortKey: key, sortDir: 1 }));
  const toggleCol = (key) =>
    setPrefs((p) => {
      const h = new Set(p.hidden);
      h.has(key) ? h.delete(key) : h.add(key);
      return { ...p, hidden: [...h] };
    });
  const setWidth = (key, px) =>
    setPrefs((p) => ({ ...p, widths: { ...p.widths, [key]: px } }));
  // Back to the view's declared defaults -- clears sort, hidden columns AND
  // resized widths, wiping the stored object entirely rather than merging
  // defaults back in (so a stale key from a since-removed column doesn't
  // linger in localStorage).
  const resetPrefs = () =>
    setPrefs({ sortKey: defaults.sortKey, sortDir: defaults.sortDir,
               hidden: defaults.hidden || [], widths: {} });

  return { sortKey: prefs.sortKey, sortDir: prefs.sortDir,
           hidden: new Set(prefs.hidden), widths: prefs.widths || {},
           toggleSort, toggleCol, setWidth, resetPrefs };
}

// Dropdown of checkboxes to show/hide a table's toggleable columns, plus a
// reset action. Native <details> so the browser owns open/close (no
// outside-click handler). Columns without a `key`, or flagged `always`, are
// fixed and never listed. `onReset` is optional -- omit it to keep the menu
// checkbox-only (existing callers keep working unchanged).
export function ColumnPicker({ columns, hidden, onToggle, onReset }) {
  const toggleable = columns.filter((c) => c.key && !c.always);
  return html`
    <details class="col-picker">
      <summary class="btn btn-sm btn-icon" title=${t("table_columns")}>
        <${Icon} name="menu" />
      </summary>
      <div class="col-picker-menu">
        ${toggleable.map((c) => html`
          <label class="col-picker-item">
            <input type="checkbox" checked=${!hidden.has(c.key)}
                   onChange=${() => onToggle(c.key)} />
            <span>${c.label}</span>
          </label>`)}
        ${onReset ? html`
          <div class="col-picker-sep"></div>
          <button type="button" class="col-picker-item col-picker-reset" onClick=${onReset}>
            <${Icon} name="reset" />
            <span>${t("table_reset_columns")}</span>
          </button>` : null}
      </div>
    </details>`;
}

export function VirtualTable({
  columns, rows, rowKey, rowClass, sortKey, sortDir, onSort, empty, onRowClick,
  rowHeight = ROW_HEIGHT, maxHeight = "70vh", widths = {}, onResize,
}) {
  const ref = useRef(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewH, setViewH] = useState(0);
  const [viewW, setViewW] = useState(0);
  // Live width of the column currently being dragged, e.g. {key:"name", px:230}.
  // This -- not a direct DOM mutation -- is the resize drag's source of truth:
  // any prop change mid-drag (rows refreshed by the SSE stream tick every
  // second or so) triggers a Preact re-render, and colgroup's <col> nodes get
  // reconciled from whatever colWidth() computes. A version that instead
  // pushed the live width straight into col.style.width bypassed Preact, so
  // that very reconciliation stomped it back to the pre-drag width mid-drag --
  // the longer the drag, the likelier a tick landed inside it, so the border
  // (and the cursor tracking it) visibly snapped backwards while the pointer
  // kept moving. Routing the value through state instead means every render,
  // including one forced by an unrelated prop change, still reflects the drag.
  const [dragging, setDragging] = useState(null);

  // A user-resized width (from `widths`, persisted by the caller) overrides
  // the column's declared default; null for the one flexible column that
  // has neither (colWidth, below, is what actually fills that in). The
  // in-progress drag (if any) wins over both.
  const effWidth = (c) => {
    if (dragging && c.key === dragging.key) return dragging.px + "px";
    const px = c.key && widths[c.key];
    return px ? px + "px" : c.width || null;
  };

  // The flexible column's width, computed here instead of left to the
  // browser's table-layout:fixed auto-distribution. Every *other* column
  // renders with an explicit pixel width (its own default or a user
  // resize); split whatever's left of the measured container width across
  // however many columns declared neither (in practice always at most
  // one -- typically "name"), floored at FLEX_MIN. Doing this ourselves
  // means colWidth() below can hand *every* <col> an explicit width, so
  // the browser has nothing left to redistribute when one column resizes
  // -- no neighbour drifts during the drag, and none needs to snap to a
  // recomputed width on release either, both of which a version relying
  // on the browser's own auto-column redistribution actually hit.
  const flexKeys = columns.filter((c) => !effWidth(c));
  const fixedTotal = columns.reduce((sum, c) => sum + (effWidth(c) ? parseInt(effWidth(c), 10) : 0), 0);
  const flexWidth = flexKeys.length
    ? Math.max(FLEX_MIN, Math.floor((viewW - fixedTotal) / flexKeys.length))
    : 0;
  const colWidth = (c) => effWidth(c) || flexWidth + "px";

  // Drag-to-resize a column header edge. Pointer Events (not mouse-only)
  // so this works on touch devices too, matching the handle's own
  // touch-action: none in app.css. setPointerCapture routes subsequent
  // move/up events to the handle itself regardless of where the pointer
  // wanders, which sidesteps two problems a window-listener version had:
  // stray listeners surviving a mid-drag unmount, and needing manual
  // cleanup on every exit path. The live width goes through `dragging`
  // state (see its comment above) rather than a direct DOM mutation, so
  // it survives a re-render forced by unrelated props mid-drag. Persisted
  // prefs are only touched via onResize on release -- and only if the
  // width actually changed, so a click with no drag doesn't turn the
  // flexible "name" column into a fixed-width one by mistake.
  const startResize = (e, key) => {
    if (!onResize) return;
    e.preventDefault();
    e.stopPropagation();
    const handle = e.currentTarget;
    const startX = e.clientX;
    const startWidth = parseInt(colWidth(columns.find((c) => c.key === key)), 10);
    let finalWidth = startWidth;

    handle.setPointerCapture(e.pointerId);
    setDragging({ key, px: startWidth });

    const onMove = (ev) => {
      finalWidth = Math.max(MIN_COL_WIDTH, startWidth + (ev.clientX - startX));
      setDragging({ key, px: finalWidth });
    };
    const onUp = () => {
      handle.removeEventListener("pointermove", onMove);
      handle.removeEventListener("pointerup", onUp);
      setDragging(null);
      if (finalWidth !== startWidth) onResize(key, Math.round(finalWidth));
    };
    handle.addEventListener("pointermove", onMove);
    handle.addEventListener("pointerup", onUp);
  };

  // Track the scroll container's size (viewport for the row-window calc,
  // and now also the flex-column split above). A ResizeObserver catches
  // layout changes beyond window resize (tab switches, toolbar wrap, etc.).
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const ro = new ResizeObserver(() => {
      setViewH(el.clientHeight);
      setViewW(el.clientWidth);
    });
    ro.observe(el);
    setViewH(el.clientHeight);
    setViewW(el.clientWidth);
    return () => ro.disconnect();
  }, []);

  const total = rows.length;
  const start = Math.max(0, Math.min(total, Math.floor(scrollTop / rowHeight) - OVERSCAN));
  const end = Math.min(total, Math.ceil((scrollTop + viewH) / rowHeight) + OVERSCAN);
  const padTop = start * rowHeight;
  const padBot = (total - end) * rowHeight;
  const ncols = columns.length;

  const arrow = (key) => key === sortKey
    ? html`<span class="sort-arrow"><${Icon} name=${sortDir > 0 ? "sort-asc" : "sort-desc"} /></span>` : null;

  // Resizable = has a key to persist under. That includes the flexible
  // no-declared-width column (typically "name") -- it starts the drag from
  // its current *rendered* width (via getBoundingClientRect in startResize,
  // not effWidth) rather than a declared one, and a user resize turns it
  // into an ordinary fixed-width column from then on, same as any other.
  // Full label on hover, since app.css ellipsizes a header too narrow for its own
  // text. String labels only -- the select-all column's label is a checkbox VNode.
  const th = (c) => html`
    <th class=${(c.sortable ? "sortable " : "") + colClass(c)}
        title=${typeof c.label === "string" ? c.label : null}
        onClick=${c.sortable && onSort ? () => onSort(c.key) : null}>
      ${c.label}${c.sortable ? arrow(c.key) : null}
      ${onResize && c.key ? html`
        <span class="col-resize-handle"
              onPointerDown=${(e) => startResize(e, c.key)}
              onClick=${(e) => e.stopPropagation()} />` : null}
    </th>`;

  // Striping is keyed off the absolute row index, not :nth-child — a spacer <tr>
  // shifts parity, so app.css disables nth-child striping for .virtual tables.
  const rowTr = (r, i) => html`
    <tr key=${rowKey(r)}
        class=${((start + i) % 2 ? "stripe " : "") + (rowClass ? rowClass(r) : "")}
        onClick=${onRowClick ? (e) => onRowClick(r, e) : null}
        style=${{ height: rowHeight + "px" }}>
      ${columns.map((c) => html`<td class=${colClass(c)}>${c.cell(r)}</td>`)}
    </tr>`;

  const spacer = (h) => h > 0
    ? html`<tr class="spacer" style=${{ height: h + "px" }}><td colspan=${ncols}></td></tr>` : null;

  // table-layout:fixed needs explicit widths, so the table can't shrink columns
  // to fit a phone. Floor its width at the sum of what colWidth() actually
  // hands every column (own default/resize, or its FLEX_MIN-floored share
  // of colWidth's flex split) so narrow viewports scroll horizontally —
  // matching the app's other tables — instead of crushing the flexible
  // column to nothing. Same source as the <colgroup> below on purpose: two
  // independent width calculations drifting apart is exactly the class of
  // bug this file just spent a few iterations chasing out of the resize
  // path.
  const minWidth = columns.reduce((sum, c) => sum + parseInt(colWidth(c), 10), 0);

  return html`
    <div class="table-wrap virtual" ref=${ref} style=${{ maxHeight }}
         onScroll=${(e) => setScrollTop(e.currentTarget.scrollTop)}>
      <table class="data virtual" style=${{ minWidth: minWidth + "px" }}>
        <colgroup>${columns.map((c) => html`<col style=${{ width: colWidth(c) }} />`)}</colgroup>
        <thead><tr>${columns.map(th)}</tr></thead>
        <tbody>
          ${total === 0
            ? null
            : html`${spacer(padTop)}${rows.slice(start, end).map(rowTr)}${spacer(padBot)}`}
        </tbody>
      </table>
      ${total === 0 ? html`<div class="table-empty">${empty}</div>` : null}
    </div>`;
}
