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

export function cmp(a, b) { return a < b ? -1 : a > b ? 1 : 0; }

// Sort a copy of `rows` by the column matching sortKey (using its sortVal
// accessor). Returns rows unchanged if no sortable column matches.
export function sortRows(rows, columns, sortKey, sortDir) {
  const col = columns.find((c) => c.key === sortKey && c.sortVal);
  if (!col) return rows;
  return rows.slice().sort((a, b) => sortDir * cmp(col.sortVal(a), col.sortVal(b)));
}

// Build a predicate for a free-text filter box: case-insensitive and
// order-independent — every whitespace-separated token must appear in the
// string. Empty query → matches everything.
export function textMatcher(query) {
  const tokens = query.toLowerCase().split(/\s+/).filter(Boolean);
  return (s) => { const n = (s || "").toLowerCase(); return tokens.every((tok) => n.includes(tok)); };
}

const colClass = (c) => [c.num ? "num" : "", c.cls || ""].filter(Boolean).join(" ");

// Per-table UI prefs (sort direction + hidden columns) persisted as one object
// under "amule.table.<storageKey>". `defaults` = { sortKey, sortDir, hidden:[] }.
// Stored prefs are spread over the defaults so a column added as default-hidden
// later stays hidden only for users who never opened the picker (their stored
// object has no entry for it → the default wins), while everyone else keeps
// their explicit choice. Returns the current sort plus toggles the view wires
// to VirtualTable.onSort and the ColumnPicker.
export function useTablePrefs(storageKey, defaults) {
  const [prefs, setPrefs] = useState(() => ({
    sortKey: defaults.sortKey,
    sortDir: defaults.sortDir,
    hidden: defaults.hidden || [],
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

  return { sortKey: prefs.sortKey, sortDir: prefs.sortDir,
           hidden: new Set(prefs.hidden), toggleSort, toggleCol };
}

// Dropdown of checkboxes to show/hide a table's toggleable columns. Native
// <details> so the browser owns open/close (no outside-click handler). Columns
// without a `key`, or flagged `always`, are fixed and never listed.
export function ColumnPicker({ columns, hidden, onToggle }) {
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
      </div>
    </details>`;
}

export function VirtualTable({
  columns, rows, rowKey, rowClass, sortKey, sortDir, onSort, empty, onRowClick,
  rowHeight = ROW_HEIGHT, maxHeight = "70vh",
}) {
  const ref = useRef(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [viewH, setViewH] = useState(0);

  // Track the scroll container's height (viewport for the window calc). A
  // ResizeObserver catches layout changes beyond window resize (tab switches,
  // toolbar wrap, etc.).
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setViewH(el.clientHeight));
    ro.observe(el);
    setViewH(el.clientHeight);
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

  const th = (c) => html`
    <th class=${(c.sortable ? "sortable " : "") + colClass(c)}
        onClick=${c.sortable && onSort ? () => onSort(c.key) : null}>
      ${c.label}${c.sortable ? arrow(c.key) : null}
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
  // to fit a phone. Floor its width at the sum of the declared column widths
  // (plus FLEX_MIN for each width-less column) so narrow viewports scroll
  // horizontally — matching the app's other tables — instead of crushing the
  // flexible column to nothing. On wide screens width:100% (from CSS) wins and
  // the flexible column absorbs the extra space.
  const minWidth = columns.reduce((sum, c) => sum + (c.width ? parseInt(c.width, 10) : FLEX_MIN), 0);

  return html`
    <div class="table-wrap virtual" ref=${ref} style=${{ maxHeight }}
         onScroll=${(e) => setScrollTop(e.currentTarget.scrollTop)}>
      <table class="data virtual" style=${{ minWidth: minWidth + "px" }}>
        ${columns.some((c) => c.width)
          ? html`<colgroup>${columns.map((c) => html`<col style=${c.width ? { width: c.width } : null} />`)}</colgroup>`
          : null}
        <thead><tr>${columns.map(th)}</tr></thead>
        <tbody>
          ${total === 0
            ? html`<tr><td colspan=${ncols}>${empty}</td></tr>`
            : html`${spacer(padTop)}${rows.slice(start, end).map(rowTr)}${spacer(padBot)}`}
        </tbody>
      </table>
    </div>`;
}
