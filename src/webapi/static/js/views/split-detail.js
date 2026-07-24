// Reusable split layout: a table region on top and a detail panel below,
// separated by a draggable splitter. Extracted from the Downloads page so
// Shared Files can reuse the exact same slider + bottom-panel structure.
// The panel height is persisted per `storageKey`. On phones the CSS turns the
// bottom region into a full-screen drill-down sheet (see .split-bottom in
// app.css).

import { html, useState, useRef } from "../dom.js";
import { loadPref, savePref } from "../store.js";
import { Icon } from "../icons.js";
import { t } from "../i18n.js";

// Bottom-anchored resizable-height hook, shared by the Downloads/Shared detail
// panel and the Networks bottom pane. Drag the splitter (moving up grows the
// bottom region); the height is clamped so neither region collapses and
// persisted per `storageKey`. Returns the current height plus the props to
// spread onto a `.splitter` element and the ref for the enclosing container.
export function useSplitHeight(storageKey, def = 340) {
  const [height, setHeight] = useState(() => {
    const v = loadPref(storageKey, def);
    return v > 0 ? v : def;
  });
  const containerRef = useRef(null);
  const dragRef = useRef(null);

  const onPointerDown = (e) => {
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
    dragRef.current = { startY: e.clientY, startH: height, id: e.pointerId, el: e.currentTarget };
  };
  const onPointerMove = (e) => {
    const g = dragRef.current;
    if (!g) return;
    const total = containerRef.current ? containerRef.current.clientHeight : window.innerHeight;
    const h = Math.max(160, Math.min(total - 160, g.startH + (g.startY - e.clientY)));
    g.lastH = h;
    setHeight(h);
  };
  const onPointerUp = () => {
    const g = dragRef.current;
    if (!g) return;
    try { g.el.releasePointerCapture(g.id); } catch (_) {}
    if (g.lastH != null) savePref(storageKey, g.lastH);
    dragRef.current = null;
  };

  const splitterProps = { onPointerDown, onPointerMove, onPointerUp, onPointerCancel: onPointerUp };
  return { height, containerRef, splitterProps };
}

export function SplitDetail({ storageKey, open, onClose, top, children }) {
  const { height, containerRef, splitterProps } = useSplitHeight(storageKey);

  return html`
    <div class="split" ref=${containerRef}>
      <div class="split-top">${top}</div>
      ${open ? html`
        <div class="splitter" ...${splitterProps}></div>
        <div class="split-bottom" style=${{ height: height + "px" }}>
          <button class="btn btn-icon btn-sm detail-close" title=${t("downloads_detail_close")}
                  onClick=${onClose}><${Icon} name="cancel" /></button>
          ${children}
        </div>` : null}
    </div>`;
}
