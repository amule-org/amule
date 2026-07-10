// Statistics view, mirroring the desktop statsDlg 2×2 grid: Download |
// Upload speed on top, Connections | Statistics Tree below. Each speed
// chart shows current + running average (computed client-side, like
// amulegui's CStatGraphRem) with a colour-chip legend. Graphs and tree are
// polled. (The Kad nodes graph lives in the Networks/Kad tab.)

import { api } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder } from "../components.js";
import { Chart } from "../charts.js";
import { formatBytes, formatSpeed, formatInt, formatDuration } from "../format.js";
import { t, terr } from "../i18n.js";

const GRAPH_POLL_MS = 2000;
const TREE_EVERY = 3; // refresh tree every N graph ticks
const GRAPH_WIDTH = 300; // samples per fetch (~chart pixel width; full window is ~1800)
const SMA_WINDOW = 50; // ponytail: SMA over ~5 min of samples; amulegui makes this a pref

const GRAPHS = [
  { name: "download", title: t("stats_download_speed"), color: "#3aaf5d", avgColor: "#1fb5ad", fmt: formatSpeed },
  { name: "upload", title: t("stats_upload_speed"), color: "#3b86e0", avgColor: "#8a5cd6", fmt: formatSpeed },
  { name: "connections", title: t("stats_connections"), color: "#d68a0c", avgColor: "#c94f7c", fmt: formatInt },
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
  const [graphData, setGraphData] = useState({}); // name -> [xs, ys, avgYs]
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
        if (alive) setGraphData((d) => ({ ...d, [g.name]: [pts.map((p) => p.t_unix), ys, sma(ys, SMA_WINDOW)] }));
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
  // Dynamic per-client version/OS rows: the label head is data. Render `raw`
  // verbatim when present (version/OS string, never translated); when raw is
  // absent it's a known placeholder we translate by kind. Keep the ": %s"
  // tail so nodeText still fills the count/percent. ponytail: heads have no ":".
  if (node.key === "client_version" || node.key === "client_os") {
    const tail = node.label.slice(node.label.indexOf(":"));
    if (node.raw) return node.raw + tail;
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
    case "string": s = v.enum ? tEnum(v.enum) : tLabel(String(v.value)); break;
    default: s = formatInt(v.value); break; // integer/istring/ishort
  }
  if (v.extra) {
    // A double sub-value is a percentage (GUI hardcodes "%.2f%%").
    const e = v.extra.type === "double" ? Number(v.extra.value).toFixed(2) + "%" : fmtValue(v.extra);
    s += " (" + e + ")";
  }
  return s;
}

// UL:DL ratio from the raw numbers (node.ratio), formatted locale-aware
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
  // Ratio node: build from node.ratio when present; else fall back to the
  // composite string value (legacy daemons emit no ratio object).
  if (node.ratio && node.ratio.session != null) {
    return tNodeLabel(node).replace("%s", formatRatio(node.ratio));
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
