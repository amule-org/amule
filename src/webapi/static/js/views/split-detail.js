// Reusable split layout: a table region on top and a detail panel below,
// separated by a draggable splitter. Extracted from the Downloads page so
// Shared Files can reuse the exact same slider + bottom-panel structure.
// The panel height is persisted per `storageKey`. On phones the CSS turns the
// bottom region into a full-screen drill-down sheet (see .split-bottom in
// app.css).

import { html, useState, useRef } from "../dom.js";
import { Icon } from "../icons.js";
import { t } from "../i18n.js";

export function SplitDetail({ storageKey, open, onClose, top, children }) {
  const [splitH, setSplitH] = useState(() => {
    const v = Number(localStorage.getItem(storageKey));
    return v > 0 ? v : 340;
  });
  const splitRef = useRef(null);
  const dragRef = useRef(null);

  // Drag the splitter to resize the detail panel (bottom-anchored: moving up
  // grows it). Clamp so neither region collapses; persist the final height.
  const onSplitDown = (e) => {
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
    dragRef.current = { startY: e.clientY, startH: splitH, id: e.pointerId, el: e.currentTarget };
  };
  const onSplitMove = (e) => {
    const g = dragRef.current;
    if (!g) return;
    const total = splitRef.current ? splitRef.current.clientHeight : window.innerHeight;
    const h = Math.max(160, Math.min(total - 160, g.startH + (g.startY - e.clientY)));
    g.lastH = h;
    setSplitH(h);
  };
  const onSplitUp = () => {
    const g = dragRef.current;
    if (!g) return;
    try { g.el.releasePointerCapture(g.id); } catch (_) {}
    if (g.lastH != null) localStorage.setItem(storageKey, String(g.lastH));
    dragRef.current = null;
  };

  return html`
    <div class="split" ref=${splitRef}>
      <div class="split-top">${top}</div>
      ${open ? html`
        <div class="splitter" onPointerDown=${onSplitDown} onPointerMove=${onSplitMove}
             onPointerUp=${onSplitUp} onPointerCancel=${onSplitUp}></div>
        <div class="split-bottom" style=${{ height: splitH + "px" }}>
          <button class="btn btn-icon btn-sm detail-close" title=${t("downloads_detail_close")}
                  onClick=${onClose}><${Icon} name="cancel" /></button>
          ${children}
        </div>` : null}
    </div>`;
}
