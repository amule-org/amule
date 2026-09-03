// Statistics view, mirroring the desktop statsDlg 2×2 grid: Download |
// Upload speed on top, Connections | Statistics Tree below. Each speed
// chart shows current + running average (computed client-side, like
// amulegui's CStatGraphRem); the connections chart draws the three lines
// the daemon itself reports. Graphs and tree are polled. (The Kad nodes
// graph lives in the Networks/Kad tab.)

import { api } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder } from "../components.js";
import { Chart } from "../charts.js";
import { formatBytes, formatSpeed, formatInt, formatDuration, bytesAxis } from "../format.js";
import { t, terr } from "../i18n.js";

const GRAPH_POLL_MS = 2000;
const TREE_EVERY = 3; // refresh tree every N graph ticks
const GRAPH_WIDTH = 300; // samples per fetch (~chart pixel width; full window is ~1800)
const SMA_WINDOW = 50; // ponytail: SMA over ~5 min of samples; amulegui makes this a pref

const speedAxis = (max) => bytesAxis(max, true);
// `series` is parallel to the arrays each graph loads below: entry 0 is the
// endpoint's own `value`, the rest are whatever that graph adds beside it.
const GRAPHS = [
  { name: "download_speed", title: t("stats_download_speed"), fmt: formatSpeed, axis: speedAxis,
    series: [{ color: "#3aaf5d", label: t("common_legend_current") }, { color: "#1fb5ad", label: t("common_legend_running_avg") }] },
  { name: "upload_speed", title: t("stats_upload_speed"), fmt: formatSpeed, axis: speedAxis,
    series: [{ color: "#3b86e0", label: t("common_legend_current") }, { color: "#8a5cd6", label: t("common_legend_running_avg") }] },
  { name: "connections", title: t("stats_connections"), fmt: formatInt,
    series: [{ color: "#d68a0c", label: t("stats_legend_active_connections") },
             { color: "#c94f7c", label: t("stats_legend_active_downloads") },
             { color: "#1fb5ad", label: t("stats_legend_active_uploads") }] },
];

// Simple moving average over the fetched window.
function sma(ys, w) {
  const out = new Array(ys.length);
  let sum = 0;
  for (let i = 0; i < ys.length; i++) {
    sum += ys[i];
    if (i >= w) sum -= ys[i - w];
    out[i] = sum / Math.min(i + 1, w);
  }
  return out;
}

export default function Stats() {
  const [graphData, setGraphData] = useState({}); // name -> [xs, ...series]
  const [tree, setTree] = useState(null);          // null=loading, []=empty
  const [treeErr, setTreeErr] = useState("");
  // Expanded tree nodes by key path ("transfer", "transfer.uploads", …).
  // Node keys are stable machine ids (falling back to the index for keyless
  // nodes), so state survives both the per-refresh label changes and
  // structural shifts in dynamic subtrees.
  const [expanded, setExpanded] = useState(new Set());

  useEffect(() => {
    let alive = true;
    let tick = 0;

    const loadGraph = async (g) => {
      try {
        const r = await api.get("stats/graphs/" + g.name + "?width=" + GRAPH_WIDTH);
        const pts = r.points || [];
        const ys = pts.map((p) => p.value);
        // The connections graph gets its two extra lines from the daemon, so
        // no client-side stand-in is needed there. They are omitted whole (not
        // zeroed) by an amuled that does not report them — then it draws as the
        // single line it used to be.
        const rest = g.name !== "connections" ? [sma(ys, SMA_WINDOW)]
          : pts.length && pts[0].active_download_count !== undefined
            ? [pts.map((p) => p.active_download_count), pts.map((p) => p.active_upload_count)]
            : [];
        if (alive) setGraphData((d) => ({ ...d, [g.name]: [pts.map((p) => p.at), ys, ...rest] }));
      } catch (_) { /* leave previous data */ }
    };
    const loadTree = async () => {
      try {
        const r = await api.get("stats/tree");
        if (alive) {
          const nodes = r.nodes || [];
          setTree(nodes);
          setTreeErr("");
          // first load: open the top-level branches, like the GUI
          setExpanded((prev) => prev.size ? prev : new Set(nodes.map((n, i) => n.key || String(i))));
        }
      }
      catch (e) { if (alive) setTreeErr(terr(e) || t("stats_error")); }
    };
    const refresh = () => {
      GRAPHS.forEach(loadGraph);
      if (tick % TREE_EVERY === 0) loadTree();
      tick++;
    };

    refresh();
    const timer = setInterval(refresh, GRAPH_POLL_MS);
    return () => { alive = false; clearInterval(timer); };
  }, []);

  const onToggle = (path, open) => setExpanded((prev) => {
    const next = new Set(prev);
    if (open) next.add(path); else next.delete(path);
    return next;
  });

  return html`
    <div class="charts-grid stats-grid">
      ${GRAPHS.map((g) => html`<${Chart} key=${g.name} g=${g} data=${graphData[g.name]} />`)}
      <div class="card chart-card stats-tree-card">
        <h3>${t("stats_statistics_tree")}</h3>
        <div class="stats-tree">
          ${treeErr ? html`<p>${treeErr}</p>`
            : tree === null ? html`<${Placeholder} kind="loading">${t("stats_loading")}<//>`
            : tree.length ? tree.map((n, i) => html`<${TreeNode} node=${n} path=${n.key || String(i)} expanded=${expanded} onToggle=${onToggle} />`)
            : html`<${Placeholder} kind="info">${t("stats_empty")}<//>`}
        </div>
      </div>
    </div>`;
}

// The API sends untranslated English label templates ("Uptime: %s") plus raw
// typed values; we translate the template and format the values client-side so
// the display honours the UI language and locale. See docs/api/REFERENCE.md.

// t() echoes the key back when there's no entry; treat that as "missing" and
// use the fallback. The one place that knows this convention.
function tOr(key, fallback) {
  const s = t(key);
  return s === key ? fallback : s;
}

// Translate a label template by its exact English text; fall back to the raw
// English for dynamic labels (client names, versions, OS) that have no key.
function tLabel(label) { return tOr("stats_tree_" + label, label); }

// Locale-independent sentinel token ("never"/"not_available") -> localized.
function tEnum(token) { return tOr("stats_tree_enum_" + token, token); }

// Translate a node's label template. Prefer the stable machine key
// (stats_tree_<key>) so translations survive label rewording and don't depend
// on matching English text; fall back to the label for legacy daemons.
function tNodeLabel(node) {
  // Dynamic per-client version/OS rows: the label head is data. Render
  // `label_value` verbatim when present (version/OS string, never translated);
  // when it's absent it's a known placeholder we translate by kind. Keep the
  // ": %s" tail so nodeText still fills the count/percent.
  // ponytail: heads have no ":".
  if (node.key === "client_version" || node.key === "client_os") {
    const tail = node.label.slice(node.label.indexOf(":"));
    if (node.label_value) return node.label_value + tail;
    return tOr("stats_tree_" + node.key + "_unknown", node.label) + tail;
  }
  return tOr("stats_tree_" + node.key, tLabel(node.label));
}

// One typed value -> display string. Mirrors ECSpecialTags::FormatValue.
function fmtValue(v, spec) {
  if (!v) return "";
  let s;
  switch (v.type) {
    case "bytes": s = formatBytes(v.value); break;
    case "speed": s = formatBytes(v.value) + "/s"; break;
    case "time": s = formatDuration(v.value); break;
    case "double": s = /f/.test(spec || "") ? Number(v.value).toFixed(2) : String(v.value); break;
    case "string": s = v.token ? tEnum(v.token) : tLabel(String(v.value)); break;
    default: s = formatInt(v.value); break; // integer
  }
  if (v.extra) {
    // A double sub-value is a percentage (GUI hardcodes "%.2f%%").
    const e = v.extra.type === "double" ? Number(v.extra.value).toFixed(2) + "%" : fmtValue(v.extra);
    s += " (" + e + ")";
  }
  return s;
}

// UL:DL ratio from the raw ratio_session / ratio_total numbers, formatted locale-aware
// instead of pasting the daemon's pre-formatted composite string. Mirrors the
// desktop GUI's "1 : <session> (1 : <total>)".
function formatRatio(r) {
  const n = (x) => Number(x).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
  let s = "1 : " + n(r.session);
  if (r.total != null) s += " (1 : " + n(r.total) + ")";
  return s;
}

// Fill the label template's printf placeholders from values, in order.
function nodeText(node) {
  // Ratio node: build from ratio_session / ratio_total when present; else fall
  // back to the composite string value (legacy daemons emit no ratio numbers).
  if (node.ratio_session != null) {
    return tNodeLabel(node).replace("%s",
      formatRatio({ session: node.ratio_session, total: node.ratio_total }));
  }
  const values = node.values || [];
  let i = 0;
  return tNodeLabel(node).replace(/%(%|[-.0-9]*(?:ll|l|h)?[a-zA-Z])/g,
    (m, spec) => spec === "%" ? "%" : fmtValue(values[i++], spec));
}

function TreeNode({ node, path, expanded, onToggle }) {
  const children = node.children || [];
  const text = nodeText(node);
  if (!children.length) return html`<div class="tree-leaf">${text}</div>`;
  const open = expanded.has(path);
  return html`
    <details open=${open} onToggle=${(e) => { if (e.target.open !== open) onToggle(path, e.target.open); }}>
      <summary>${text}</summary>
      ${children.map((c, i) => html`<${TreeNode} node=${c} path=${path + "." + (c.key || i)} expanded=${expanded} onToggle=${onToggle} />`)}
    </details>`;
}
