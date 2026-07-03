// Shared files view: list published files with session/total transfer,
// request and accept counters; change upload priority; reload shares.
// Live via the SSE "shared" channel.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Placeholder, toast } from "../components.js";
import { formatBytes, formatInt } from "../format.js";
import { Icon } from "../icons.js";
import { t } from "../i18n.js";

const PRIORITIES = ["auto", "very_low", "low", "normal", "high", "release"]
  .map((v) => [v, t("shared_prio_" + v)]);

export default function Shared({ isGuest }) {
  const shared = useStore("shared") || [];
  const [sortKey, setSortKey] = useState("name");
  const [sortDir, setSortDir] = useState(1);

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
    catch (e) { toast(e.message || t("shared_error"), "error"); }
  };
  const reload = async () => {
    try { await api.post("shared/reload"); toast(t("shared_toast_reloading"), "success"); setTimeout(() => data.refresh("shared"), 1500); }
    catch (e) { toast(e.message || t("shared_error"), "error"); }
  };

  const list = shared.slice().sort((a, b) => sortDir * cmp(sortVal(a, sortKey), sortVal(b, sortKey)));

  const sortArrow = (key) => key === sortKey
    ? html`<span class="sort-arrow"><${Icon} name=${sortDir > 0 ? "sort-asc" : "sort-desc"} /></span>` : null;
  const Th = (label, key, num) => html`
    <th class=${(key ? "sortable " : "") + (num ? "num" : "")} onClick=${key ? () => toggleSort(key) : null}>
      ${label}${sortArrow(key)}
    </th>`;

  const row = (s) => {
    const x = s.xfer || {}, rq = s.requests || {}, ac = s.accepts || {};
    return html`
      <tr>
        <td class="name" title=${s.name}>${s.name}</td>
        <td class="num">${formatBytes(s.size)}</td>
        <td class="num">${twin(x.session, x.total, formatBytes)}</td>
        <td class="num">${twin(rq.session, rq.total, formatInt)}</td>
        <td class="num">${twin(ac.session, ac.total, formatInt)}</td>
        <td class="num">${formatInt(s.complete_sources)}</td>
        ${isGuest
          ? html`<td>${prioLabel(s)}</td>`
          : html`<td>
              <select class="input input-sm admin-only" value=${prioValue(s)}
                      onChange=${(e) => setPriority(s.hash, e.target.value)}>
                ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${v === "auto" && s.priority_auto ? prioLabel(s) : l}</option>`)}
              </select></td>`}
      </tr>`;
  };

  return html`
    <div class="view-header">
      <h3 class="section-title">${t("shared_title")} (${list.length})</h3>
      <div class="spacer"></div>
      <div class="toolbar">
        <button class="btn admin-only" onClick=${reload}>${t("shared_refresh_shares")}</button>
      </div>
    </div>
    <div class="table-wrap">
      <table class="data">
        <thead>
          <tr>
            ${Th(t("shared_name"), "name")}${Th(t("shared_size"), "size", true)}
            ${Th(t("shared_transferred"), "xfer", true)}${Th(t("shared_requested"), "requests", true)}
            ${Th(t("shared_accepted"), "accepts", true)}${Th(t("shared_complete_src"), "sources", true)}
            ${Th(t("shared_priority"))}
          </tr>
        </thead>
        <tbody>
          ${list.length
            ? list.map(row)
            : html`<tr><td colspan="7"><${Placeholder} kind="info">${t("shared_empty")}<//></td></tr>`}
        </tbody>
      </table>
    </div>`;
}

function twin(session, total, fmt) { return fmt(session || 0) + " / " + fmt(total || 0); }
function prioValue(s) { return s.priority_auto ? "auto" : s.priority; }
function prioLabel(s) {
  const found = PRIORITIES.find(([v]) => v === s.priority);
  const base = found ? found[1] : s.priority;
  return s.priority_auto ? t("shared_prio_auto") + " (" + base + ")" : base;
}
function sortVal(s, k) {
  switch (k) {
    case "name": return (s.name || "").toLowerCase();
    case "size": return s.size || 0;
    case "xfer": return (s.xfer && s.xfer.total) || 0;
    case "requests": return (s.requests && s.requests.total) || 0;
    case "accepts": return (s.accepts && s.accepts.total) || 0;
    case "sources": return s.complete_sources || 0;
    default: return 0;
  }
}
function cmp(a, b) { return a < b ? -1 : a > b ? 1 : 0; }
