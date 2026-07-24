// Hand-rolled canvas chart, reused by the Statistics view and the
// Networks/Kad tab. Draws a time series (filled line, y-grid with
// formatted labels, time x-axis, hover readout) — all the charts here are
// this simple, so a hand-rolled canvas draw is enough; no charting library
// is bundled.
// data is [xs, ys] or [xs, ys, avgYs]; the optional third series is drawn
// as a thinner line (g.avgColor) with a GUI-style legend below the canvas.

import { html, useEffect, useRef } from "./dom.js";
import { t, getLang } from "./i18n.js";

// Time-axis clock, localized to the UI language, 24h (no AM/PM). The axis uses
// hour:minute; the hover readout adds seconds. Formatters are built once —
// constructing Intl.DateTimeFormat per label is expensive.
const clockHM = new Intl.DateTimeFormat(getLang(), { hour: "2-digit", minute: "2-digit", hour12: false });
const clockHMS = new Intl.DateTimeFormat(getLang(), { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false });
const clock = (unixSeconds, withSeconds) =>
  (withSeconds ? clockHMS : clockHM).format(new Date((Number(unixSeconds) || 0) * 1000));

const HEIGHT = 180;
const AXIS_FONT = "12px system-ui, sans-serif"; // matches the .chart-legend size
const GUTTER_LEFT = 44;   // hover-mapping fallback until the first draw sizes it
const GUTTER_BOTTOM = 22; // x-axis labels
const PAD_TOP = 8;
const PAD_RIGHT = 8;

export function Chart({ g, data, bare }) {
  const canvas = useRef(null);
  const state = useRef({ data: null, hover: -1 });

  const redraw = () => {
    if (canvas.current && state.current.data) {
      // draw() returns the plot's horizontal bounds (the left gutter is sized
      // dynamically to the labels), so hover mapping stays accurate.
      const box = draw(canvas.current, g, state.current.data, state.current.hover);
      if (box) { state.current.x0 = box.x0; state.current.x1 = box.x1; }
    }
  };

  useEffect(() => {
    // Colours are read from CSS variables on every draw, so a theme switch
    // only needs a redraw.
    const mo = new MutationObserver(redraw);
    mo.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
    const mq = window.matchMedia("(prefers-color-scheme: dark)");
    mq.addEventListener("change", redraw);
    // Redraw on any host-size change: covers window resizes and the Networks
    // slider, which resizes the pane without firing a window resize event.
    const ro = new ResizeObserver(redraw);
    if (canvas.current && canvas.current.parentNode) ro.observe(canvas.current.parentNode);
    return () => {
      mo.disconnect();
      mq.removeEventListener("change", redraw);
      ro.disconnect();
    };
  }, []);

  useEffect(() => {
    if (!data) return;
    state.current.data = data;
    redraw();
  }, [data]);

  const onMove = (e) => {
    const d = state.current.data;
    if (!d || !d[0].length) return;
    const rect = canvas.current.getBoundingClientRect();
    const x0 = state.current.x0 != null ? state.current.x0 : GUTTER_LEFT;
    const x1 = state.current.x1 != null ? state.current.x1 : rect.width - PAD_RIGHT;
    const frac = (e.clientX - rect.left - x0) / Math.max(1, x1 - x0);
    const idx = Math.max(0, Math.min(d[0].length - 1, Math.round(frac * (d[0].length - 1))));
    if (idx !== state.current.hover) { state.current.hover = idx; redraw(); }
  };
  const onLeave = () => { state.current.hover = -1; redraw(); };

  const avg = data && data[2];
  const last = (a) => (a && a.length ? a[a.length - 1] : 0);
  return html`
    <div class=${bare ? "chart-card chart-bare" : "card chart-card"}>
      <h3>${g.title}</h3>
      <div class="chart-host">
        <canvas ref=${canvas} style="width:100%;height:100%;display:block"
          onMouseMove=${onMove} onMouseLeave=${onLeave}></canvas>
      </div>
      ${avg ? html`
        <div class="chart-legend">
          <span class="legend-item"><span class="legend-chip" style="background:${g.color}"></span>${t("common_legend_current")}: ${g.fmt(last(data[1]))}</span>
          <span class="legend-item"><span class="legend-chip" style="background:${g.avgColor}"></span>${t("common_legend_running_avg")}: ${g.fmt(last(avg))}</span>
        </div>` : null}
    </div>`;
}

// Round `raw` up to a "nice" tick step: 1/2/5 × 10^n.
function niceStep(raw) {
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const n = raw / mag;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * mag;
}

function draw(cv, g, [xs, ys, avg], hover) {
  const cssW = cv.parentNode.clientWidth || 400;
  // Height follows the host: fixed 180px for the bare (Kad) chart, but on the
  // Stats grid the canvas is height:100% so the host fills its card cell.
  const h = cv.parentNode.clientHeight || HEIGHT;
  const dpr = window.devicePixelRatio || 1;
  cv.width = cssW * dpr;
  cv.height = h * dpr;
  const ctx = cv.getContext("2d");
  ctx.scale(dpr, dpr);

  const styles = getComputedStyle(document.documentElement);
  const fg = styles.getPropertyValue("--text").trim() || "#000";
  const grid = styles.getPropertyValue("--border").trim() || "#0001";
  ctx.clearRect(0, 0, cssW, h);
  ctx.font = AXIS_FONT;

  const n = xs.length;
  if (!n) return;

  // y scale: 0 .. max*1.05, ticks on a nice step (shared by both series)
  const yMax = Math.max(1, Math.max(...ys, ...(avg || [0])) * 1.05);
  const step = Math.max(1, niceStep(yMax / 4)); // values are integer counts/B·s⁻¹

  // Axis unit: charts with `g.axis` (speeds) pick ONE unit for the whole axis so
  // ticks are bare numbers with the unit shown once; others keep g.fmt per tick.
  const axis = g.axis ? g.axis(yMax) : null;
  const tickLabel = (v) => {
    if (!axis) return g.fmt(v);
    if (v === 0) return "0";
    const s = v / axis.div;
    const raw = s >= 100 ? s.toFixed(0) : s >= 10 ? s.toFixed(1) : s.toFixed(2);
    return raw.indexOf(".") < 0 ? raw : raw.replace(/\.?0+$/, "");
  };
  const ticks = [];
  for (let v = 0; v <= yMax; v += step) ticks.push([v, tickLabel(v)]);

  // Left gutter sized to the widest label (+ 6px gap + 6px margin) instead of a
  // fixed width, so short numeric axes don't waste horizontal space.
  let labelW = 0;
  for (const [, label] of ticks) labelW = Math.max(labelW, ctx.measureText(label).width);
  const x0 = Math.ceil(labelW) + 12;
  const x1 = cssW - PAD_RIGHT;
  // reserve a line at the top for the unit caption when present
  const y0 = axis ? PAD_TOP + 15 : PAD_TOP;
  const y1 = h - GUTTER_BOTTOM;
  const sx = (i) => x0 + (n < 2 ? 0 : (i / (n - 1)) * (x1 - x0));
  const sy = (v) => y1 - (v / yMax) * (y1 - y0);

  // grid + y labels
  ctx.strokeStyle = grid;
  ctx.fillStyle = fg;
  ctx.lineWidth = 1;
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (const [v, label] of ticks) {
    const y = Math.round(sy(v)) + 0.5;
    ctx.beginPath(); ctx.moveTo(x0, y); ctx.lineTo(x1, y); ctx.stroke();
    ctx.fillText(label, x0 - 6, y);
  }
  // unit shown once, above the axis numbers (dim, right-aligned to match them)
  if (axis) {
    ctx.fillStyle = styles.getPropertyValue("--text-dim").trim() || fg;
    ctx.textBaseline = "top";
    ctx.fillText(axis.unit, x0 - 6, 1);
    ctx.fillStyle = fg;
  }

  // x labels: ~4 evenly spaced timestamps
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  const nLabels = Math.min(4, n);
  for (let k = 0; k < nLabels; k++) {
    const i = Math.round((k / Math.max(1, nLabels - 1)) * (n - 1));
    const label = clock(xs[i]);
    // keep edge labels inside the plot
    const x = Math.max(x0 + 20, Math.min(x1 - 20, sx(i)));
    ctx.fillText(label, x, y1 + 5);
  }

  // series: fill then stroke
  ctx.beginPath();
  ctx.moveTo(sx(0), sy(ys[0]));
  for (let i = 1; i < n; i++) ctx.lineTo(sx(i), sy(ys[i]));
  ctx.strokeStyle = g.color;
  ctx.lineWidth = 2;
  ctx.lineJoin = "round";
  ctx.stroke();
  ctx.lineTo(sx(n - 1), y1); ctx.lineTo(sx(0), y1); ctx.closePath();
  ctx.fillStyle = g.color + "22";
  ctx.fill();

  // running average: thinner line, no fill
  if (avg) {
    ctx.beginPath();
    ctx.moveTo(sx(0), sy(avg[0]));
    for (let i = 1; i < n; i++) ctx.lineTo(sx(i), sy(avg[i]));
    ctx.strokeStyle = g.avgColor;
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }

  // hover: vertical line + point + readout
  if (hover >= 0 && hover < n) {
    const hx = sx(hover), hy = sy(ys[hover]);
    ctx.strokeStyle = grid;
    ctx.beginPath(); ctx.moveTo(hx + 0.5, y0); ctx.lineTo(hx + 0.5, y1); ctx.stroke();
    ctx.fillStyle = g.color;
    ctx.beginPath(); ctx.arc(hx, hy, 3, 0, 2 * Math.PI); ctx.fill();
    ctx.fillStyle = fg;
    ctx.textBaseline = "top";
    ctx.textAlign = hx > (x0 + x1) / 2 ? "right" : "left";
    const text = g.fmt(ys[hover]) + " · " + clock(xs[hover], true);
    ctx.fillText(text, hx > (x0 + x1) / 2 ? hx - 8 : hx + 8, y0);
  }

  return { x0, x1 }; // plot bounds, for accurate hover mapping (see redraw)
}
