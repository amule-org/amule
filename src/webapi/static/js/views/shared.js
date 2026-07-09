// Shared files view: list published files with session/total transfer,
// request and accept counters; change upload priority; reload shares.
// Live via the SSE "shared" channel.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Placeholder, toast } from "../components.js";
import { VirtualTable, sortRows, textMatcher } from "../table.js";
import { formatBytes, formatInt } from "../format.js";
import { t, terr } from "../i18n.js";

const PRIORITIES = ["auto", "very_low", "low", "normal", "high", "release"]
  .map((v) => [v, t("shared_prio_" + v)]);

export default function Shared({ isGuest }) {
  const shared = useStore("shared") || [];
  const [sortKey, setSortKey] = useState("name");
  const [sortDir, setSortDir] = useState(1);
  const [filterText, setFilterText] = useState("");

  useEffect(() => {
    data.register({ key: "shared", eventPrefix: "shared", id: "hash",
      list: () => api.get("shared").then((r) => r.shared || []) });
    data.ensure("shared");
  }, []);

  const toggleSort = (key) => {
    if (sortKey === key) setSortDir(-sortDir);
    else { setSortKey(key); setSortDir(1); }
  };

  const setPriority = async (hash, p) => {
    try { await api.patch("shared/" + hash, { priority: p }); data.refresh("shared"); }
    catch (e) { toast(terr(e) || t("shared_error"), "error"); }
  };
  const reload = async () => {
    try { await api.post("shared/reload"); toast(t("shared_toast_reloading"), "success"); setTimeout(() => data.refresh("shared"), 1500); }
    catch (e) { toast(terr(e) || t("shared_error"), "error"); }
  };

  const columns = [
    { key: "name", label: t("shared_name"), cls: "name", sortable: true,
      sortVal: (s) => (s.name || "").toLowerCase(),
      cell: (s) => html`<span title=${s.name}>${s.name}</span>` },
    { key: "size", label: t("shared_size"), num: true, width: "110px", sortable: true,
      sortVal: (s) => s.size || 0, cell: (s) => formatBytes(s.size) },
    { key: "xfer", label: t("shared_transferred"), num: true, width: "170px", sortable: true,
      sortVal: (s) => (s.xfer && s.xfer.total) || 0,
      cell: (s) => twin(s.xfer, "session", "total", formatBytes) },
    { key: "requests", label: t("shared_requested"), num: true, width: "120px", sortable: true,
      sortVal: (s) => (s.requests && s.requests.total) || 0,
      cell: (s) => twin(s.requests, "session", "total", formatInt) },
    { key: "accepts", label: t("shared_accepted"), num: true, width: "120px", sortable: true,
      sortVal: (s) => (s.accepts && s.accepts.total) || 0,
      cell: (s) => twin(s.accepts, "session", "total", formatInt) },
    { key: "sources", label: t("shared_complete_src"), num: true, width: "90px", sortable: true,
      sortVal: (s) => s.complete_sources || 0, cell: (s) => formatInt(s.complete_sources) },
    { label: t("shared_priority"), width: "160px", cell: (s) => isGuest
        ? prioLabel(s)
        : html`
            <select class="input input-sm admin-only" value=${prioValue(s)}
                    onChange=${(e) => setPriority(s.hash, e.target.value)}>
              ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${v === "auto" && s.priority_auto ? prioLabel(s) : l}</option>`)}
            </select>` },
  ];

  const match = textMatcher(filterText);
  const filtered = filterText ? shared.filter((s) => match(s.name)) : shared;
  const list = sortRows(filtered, columns, sortKey, sortDir);

  return html`
    <div class="view-header">
      <h3 class="section-title">${t("shared_title")} (${list.length})</h3>
      <div class="spacer"></div>
      <div class="toolbar">
        <span>${t("shared_filter")}:</span>
        <input class="input input-sm" type="text" value=${filterText} onInput=${(e) => setFilterText(e.target.value)} />
        <button class="btn admin-only" onClick=${reload}>${t("shared_refresh_shares")}</button>
      </div>
    </div>
    <${VirtualTable} columns=${columns} rows=${list} rowKey=${(s) => s.hash}
                     sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                     empty=${html`<${Placeholder} kind="info">${t("shared_empty")}<//>`} />`;
}

function twin(o, a, b, fmt) { return fmt((o && o[a]) || 0) + " / " + fmt((o && o[b]) || 0); }
function prioValue(s) { return s.priority_auto ? "auto" : s.priority; }
function prioLabel(s) {
  const found = PRIORITIES.find(([v]) => v === s.priority);
  const base = found ? found[1] : s.priority;
  return s.priority_auto ? t("shared_prio_auto") + " (" + base + ")" : base;
}
