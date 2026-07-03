// Search view: start an ed2k/Kad search, then track it SSE-first: the
// `search` channel delivers per-result upserts (search_result_added, same
// shape as a /search/results[] entry, keyed by hash) and live progress
// (search_progress {state, percent, results, kind}). GET /search/results is
// only used to seed on mount (a search may already be running), as a single
// reconcile once the terminal "finished" frame arrives, and as a polling
// fallback while the stream isn't live. Download selected results into a
// category.

import { api } from "../api.js";
import { data } from "../events.js";
import { store } from "../store.js";
import { html, useState, useEffect, useRef } from "../dom.js";
import { Badge, Placeholder, Tabs, toast } from "../components.js";
import { formatBytes } from "../format.js";
import { Icon } from "../icons.js";
import { t, tn } from "../i18n.js";

const SIZE_UNITS = { B: 1, KB: 1024, MB: 1048576, GB: 1073741824 };
// [API value, label key] — the value goes to the backend verbatim.
const FILE_TYPES = [
  ["", "search_ftype_any"], ["Audio", "search_ftype_audio"], ["Video", "search_ftype_video"],
  ["Image", "search_ftype_image"], ["Document", "search_ftype_document"], ["Program", "search_ftype_program"],
  ["Archive", "search_ftype_archive"], ["CD/DVD", "search_ftype_cddvd"],
];
const POLL_MS = 1500;
// While a search streams results over SSE, coalesce the re-render so the table
// refreshes at most once per second no matter how fast frames arrive — a burst
// of hundreds of per-result upserts would otherwise lock the main thread.
const SYNC_MS = 1000;

export default function Search({ isGuest }) {
  const [query, setQuery] = useState("");
  const [type, setType] = useState("global");
  const [fileType, setFileType] = useState("");
  const [ext, setExt] = useState("");
  const [minAvail, setMinAvail] = useState("");
  const [minSize, setMinSize] = useState("");
  const [minUnit, setMinUnit] = useState("MB");
  const [maxSize, setMaxSize] = useState("");
  const [maxUnit, setMaxUnit] = useState("MB");

  const [results, setResults] = useState([]);
  const [filter, setFilter] = useState("");
  const [selection, setSelection] = useState(() => new Set());
  const [sortKey, setSortKey] = useState("sources");
  const [sortDir, setSortDir] = useState(-1);
  const [progress, setProgress] = useState("");
  const [searching, setSearching] = useState(false);
  const [categories, setCategories] = useState([]);
  const [cat, setCat] = useState(0);
  const [rowCat, setRowCat] = useState({});
  const catFor = (h) => (rowCat[h] != null ? rowCat[h] : Number(cat)) || 0;

  const pollRef = useRef(null);
  const stopPolling = () => { if (pollRef.current) { clearInterval(pollRef.current); pollRef.current = null; } };

  // Canonical result set keyed by hash. Both the SSE channel (upsert) and the
  // poll (full authoritative replace) write here, then we push the values
  // into render state.
  const resultsMap = useRef(new Map());
  // Trailing throttle: the first pending change schedules a flush ~SYNC_MS out;
  // everything arriving in that window folds into it. Progress text piggybacks
  // on the same flush so it never renders on its own hot path.
  const syncTimer = useRef(0);
  const pendingProgress = useRef(null);
  const flush = () => {
    syncTimer.current = 0;
    setResults(Array.from(resultsMap.current.values()));
    if (pendingProgress.current !== null) { setProgress(pendingProgress.current); pendingProgress.current = null; }
  };
  const scheduleSync = () => { if (!syncTimer.current) syncTimer.current = setTimeout(flush, SYNC_MS); };

  // `progress.state` is canonical: "running" | "finished" | "idle". Only show
  // the live "Searching… N%" text; once finished (or idle) hide it — the tab
  // count already reflects the outcome.
  const progressText = (pr) =>
    pr.state === "running" ? t("search_searching_fmt", { percent: pr.percent || 0 }) : "";

  const fetchResults = async () => {
    try {
      const r = await api.get("search/results");
      const res = r.results || [];
      resultsMap.current = new Map(res.map((x) => [x.hash, x]));
      scheduleSync();
      const pr = r.progress || {};
      setProgress(progressText(pr));
      setSearching(pr.state === "running");
      if (pr.state === "running") { if (!pollRef.current) startPolling(); }
      else stopPolling();
    } catch (e) { setProgress(e.message || t("search_error")); }
  };

  // Fallback only: while SSE is live the search channel drives everything,
  // so the tick stays idle and no periodic GETs go out.
  const startPolling = () => {
    stopPolling();
    pollRef.current = setInterval(() => { if (!data.isLive()) fetchResults(); }, POLL_MS);
  };

  useEffect(() => {
    api.get("categories").then((r) => setCategories(r.categories || [])).catch(() => {});

    // Live updates from the SSE search channel (opened app-wide by the
    // shell). search_result_added payloads are /search/results[] entries
    // verbatim, so upsert them by hash as-is.
    let lastResult = store.get("search:result");
    const offResult = store.subscribe("search:result", (p) => {
      if (!p || p === lastResult) return;
      lastResult = p;
      resultsMap.current.set(p.hash, p);
      scheduleSync();
    });
    // search_progress drives the live percent; its `results` count is the
    // backend's map size and may run ahead of the upserts we've seen. The
    // terminal "finished" frame is the completion signal — reconcile once
    // against REST to pick up anything the stream dropped.
    let lastProgress = store.get("search:progress");
    const offProgress = store.subscribe("search:progress", (p) => {
      if (!p || p === lastProgress) return;
      lastProgress = p;
      pendingProgress.current = progressText(p);
      scheduleSync();
      if (p.state === "finished") {
        stopPolling();
        setSearching(false);
        fetchResults();
      }
    });

    fetchResults(); // a search may already be running
    return () => { stopPolling(); clearTimeout(syncTimer.current); offResult(); offProgress(); };
  }, []);
  const sizeBytes = (v, unit) => { const n = Number(v); return (!n || n < 0) ? 0 : Math.round(n * SIZE_UNITS[unit]); };

  const startSearch = async (e) => {
    e.preventDefault();
    const q = query.trim();
    if (!q) { toast(t("search_toast_enter_search_terms"), "warn"); return; }
    const body = { query: q, type };
    if (fileType) body.file_type = fileType;
    if (ext.trim()) body.extension = ext.trim();
    if (Number(minAvail) > 0) body.min_avail = Number(minAvail);
    const mn = sizeBytes(minSize, minUnit), mx = sizeBytes(maxSize, maxUnit);
    if (mn) body.min_size = mn;
    if (mx) body.max_size = mx;
    try {
      await api.post("search", body);
      resultsMap.current.clear();
      setResults([]); setSelection(new Set()); setSearching(true);
      setProgress(progressText({ state: "running", kind: type }));
      startPolling();
      toast(t("search_toast_search_started"), "success");
    } catch (err) { toast(err.message || t("search_error"), "error"); }
  };

  const stop = async () => {
    stopPolling(); setSearching(false);
    try { await api.post("search/stop"); } catch (_) {}
    fetchResults();
  };

  const downloadSelected = async () => {
    const hashes = Array.from(selection);
    if (!hashes.length) { toast(t("search_toast_no_results_selected"), "warn"); return; }
    const category = Number(cat) || 0;
    try {
      await Promise.all(hashes.map((h) => api.post("search/results/" + h + "/download", { category })));
      toast(tn("search_toast_added_downloads", hashes.length), "success");
      setSelection(new Set());
    } catch (e) { toast(e.message || t("search_error"), "error"); }
  };

  const downloadOne = async (h) => {
    try {
      await api.post("search/results/" + h + "/download", { category: catFor(h) });
      toast(tn("search_toast_added_downloads", 1), "success");
    } catch (e) { toast(e.message || t("search_error"), "error"); }
  };

  const toggleRow = (hash, checked) => {
    const next = new Set(selection);
    if (checked) next.add(hash); else next.delete(hash);
    setSelection(next);
  };
  const toggleAll = (checked) =>
    setSelection(checked ? new Set(list.map((r) => r.hash)) : new Set());
  const toggleSort = (key) => {
    if (sortKey === key) setSortDir(-sortDir);
    else { setSortKey(key); setSortDir(1); }
  };

  // Front-end filter: case-insensitive, word-order-independent — every
  // whitespace-separated token must appear somewhere in the name.
  const tokens = filter.toLowerCase().split(/\s+/).filter(Boolean);
  const matchName = (name) => { const n = (name || "").toLowerCase(); return tokens.every((tok) => n.includes(tok)); };
  const sorted = results.slice().sort((a, b) => sortDir * cmp(sortVal(a, sortKey), sortVal(b, sortKey)));
  const list = tokens.length ? sorted.filter((r) => matchName(r.name)) : sorted;
  const allSelected = list.length > 0 && list.every((r) => selection.has(r.hash));

  const sortArrow = (key) => key === sortKey
    ? html`<span class="sort-arrow"><${Icon} name=${sortDir > 0 ? "sort-asc" : "sort-desc"} /></span>` : null;
  const Th = (label, key, num) => html`
    <th class=${(key ? "sortable " : "") + (num ? "num" : "")} onClick=${key ? () => toggleSort(key) : null}>
      ${label}${sortArrow(key)}
    </th>`;

  const catOptions = () => html`
    <option value=${0}>${t("search_category_none")}</option>
    ${categories.filter((c) => c.index !== 0).map((c) => html`<option value=${c.index}>${c.name || ("#" + c.index)}</option>`)}`;

  const unitSelect = (value, onChange) => html`
    <select class="input input-sm" value=${value} onChange=${onChange}>
      ${Object.keys(SIZE_UNITS).map((u) => html`<option value=${u}>${u}</option>`)}
    </select>`;

  const resultRow = (r) => {
    const selected = selection.has(r.hash);
    const src = r.sources || {};
    return html`
      <tr class=${selected ? "row-selected" : ""}>
        <td><input type="checkbox" checked=${selected} onChange=${(e) => toggleRow(r.hash, e.target.checked)} /></td>
        <td class="name" title=${r.name}>
          ${r.name}${r.already_have ? html`<${Badge} title=${t("search_already_have_title")}>${t("search_badge_have")}<//>` : null}
        </td>
        <td class="num">${formatBytes(r.size)}</td>
        <td class="num" title=${t("search_title_complete_total")}>${(src.complete || 0) + " / " + (src.total || 0)}</td>
        <td class="num">${r.rating || 0}</td>
        <td class="row-actions admin-only">
          <select class="input input-sm" value=${catFor(r.hash)}
                  onChange=${(e) => setRowCat({ ...rowCat, [r.hash]: Number(e.target.value) })}>
            ${catOptions()}
          </select>
          <button class="btn btn-icon btn-sm" type="button" title=${t("search_download")} onClick=${() => downloadOne(r.hash)}>
            <${Icon} name="downloads" />
          </button>
        </td>
      </tr>`;
  };

  return html`
    <form class="card search-form" onSubmit=${startSearch}>
      <div class="search-grid">
        ${field(t("search_query"), html`<input class="input" type="text" placeholder=${t("search_terms_ph")} required value=${query} onInput=${(e) => setQuery(e.target.value)} />`, "field-wide")}
        ${field(t("search_type"), html`<select class="input" value=${type} onChange=${(e) => setType(e.target.value)}>
          <option value="global">${t("search_type_global")}</option><option value="local">${t("search_type_local")}</option><option value="kad">${t("search_type_kad")}</option>
        </select>`)}
        ${field(t("search_file_type"), html`<select class="input" value=${fileType} onChange=${(e) => setFileType(e.target.value)}>
          ${FILE_TYPES.map(([v, k]) => html`<option value=${v}>${t(k)}</option>`)}
        </select>`)}
        ${field(t("search_extension"), html`<input class="input" type="text" placeholder=${t("search_ext_ph")} value=${ext} onInput=${(e) => setExt(e.target.value)} />`)}
        ${field(t("search_min_availability"), html`<input class="input" type="number" min="0" placeholder="0" value=${minAvail} onInput=${(e) => setMinAvail(e.target.value)} />`)}
        ${field(t("search_min_size"), html`<div class="field-inline">
          <input class="input" type="number" min="0" value=${minSize} onInput=${(e) => setMinSize(e.target.value)} />
          ${unitSelect(minUnit, (e) => setMinUnit(e.target.value))}
        </div>`)}
        ${field(t("search_max_size"), html`<div class="field-inline">
          <input class="input" type="number" min="0" value=${maxSize} onInput=${(e) => setMaxSize(e.target.value)} />
          ${unitSelect(maxUnit, (e) => setMaxUnit(e.target.value))}
        </div>`)}
      </div>
      <div class="toolbar">
        <button class="btn admin-only" type="submit" disabled=${searching}>${t("search_search")}</button>
        <button class="btn admin-only" type="button" onClick=${stop} disabled=${!searching}>${t("search_stop")}</button>
        <button class="btn" type="button" onClick=${fetchResults}>${t("search_update_results")}</button>
      </div>
      ${isGuest ? html`<p class="hint">${t("search_guest_readonly")}</p>` : null}
    </form>

    <section class="net-pane">
      <${Tabs} tabs=${[{ key: "results", label: t("search_results") + " (" + results.length + ")" }]}
               active="results" onSelect=${() => {}} />
      <div class="net-pane-body">
        <div class="toolbar pane-toolbar">
          <span class="admin-only">${t("search_selected")}:</span>
          <select class="input input-sm admin-only" value=${cat} onChange=${(e) => setCat(e.target.value)}>
            ${catOptions()}
          </select>
          <button class="btn btn-sm admin-only" onClick=${downloadSelected}>${t("search_download")}</button>
          <span class="search-progress">${progress}</span>
          <div class="spacer"></div>
          <span>${t("search_filter")}:</span>
          <input class="input input-sm" type="text" value=${filter} onInput=${(e) => setFilter(e.target.value)} />
        </div>

        <div class="table-wrap">
          <table class="data">
            <thead>
              <tr>
                <th><input type="checkbox" title=${t("search_select_all")} checked=${allSelected}
                           onChange=${(e) => toggleAll(e.target.checked)} /></th>${Th(t("search_name"), "name")}${Th(t("search_size"), "size", true)}
                ${Th(t("search_sources"), "sources", true)}${Th(t("search_rating"), "rating", true)}${Th(t("search_actions"))}
              </tr>
            </thead>
            <tbody>
              ${list.length
                ? list.map(resultRow)
                : html`<tr><td colspan="6"><${Placeholder} kind="info">${t("search_empty")}<//></td></tr>`}
            </tbody>
          </table>
        </div>
      </div>
    </section>`;
}

function field(label, control, cls = "") {
  return html`<div class=${"field " + cls}><label>${label}</label>${control}</div>`;
}
function sortVal(r, k) {
  if (k === "name") return (r.name || "").toLowerCase();
  if (k === "size") return r.size || 0;
  if (k === "sources") return (r.sources && r.sources.total) || 0;
  if (k === "rating") return r.rating || 0;
  return 0;
}
function cmp(a, b) { return a < b ? -1 : a > b ? 1 : 0; }
