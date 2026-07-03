// Statistics view, mirroring the desktop statsDlg 2×2 grid: Download |
// Upload speed on top, Connections | Statistics Tree below. Each speed
// chart shows current + running average (computed client-side, like
// amulegui's CStatGraphRem) with a colour-chip legend. Graphs and tree are
// polled. (The Kad nodes graph lives in the Networks/Kad tab.)

import { api } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder } from "../components.js";
import { Chart } from "../charts.js";
import { formatSpeed, formatInt } from "../format.js";
import { t } from "../i18n.js";

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
  // Expanded tree nodes by index path ("0", "0.2", …). Index paths survive
  // the label changes each refresh brings (labels embed live values).
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
          setExpanded((prev) => prev.size ? prev : new Set(nodes.map((_, i) => String(i))));
        }
      }
      catch (e) { if (alive) setTreeErr(e.message || t("stats_error")); }
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
            : tree.length ? tree.map((n, i) => html`<${TreeNode} node=${n} path=${String(i)} expanded=${expanded} onToggle=${onToggle} />`)
            : html`<${Placeholder} kind="info">${t("stats_empty")}<//>`}
        </div>
      </div>
    </div>`;
}

function TreeNode({ node, path, expanded, onToggle }) {
  const children = node.children || [];
  if (!children.length) return html`<div class="tree-leaf">${node.label}</div>`;
  const open = expanded.has(path);
  return html`
    <details open=${open} onToggle=${(e) => { if (e.target.open !== open) onToggle(path, e.target.open); }}>
      <summary>${node.label}</summary>
      ${children.map((c, i) => html`<${TreeNode} node=${c} path=${path + "." + i} expanded=${expanded} onToggle=${onToggle} />`)}
    </details>`;
}
