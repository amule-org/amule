// Log panels: the aMule log (live-appended via SSE log_appended) and the
// server-info log (polled). Both clearable (admin). Rendered as the bottom
// tabs of the Networks view.
//
// The <pre> boxes are written to imperatively (via refs) so we can append
// incoming lines while preserving the scroll-stick behaviour; preact never
// owns their contents.

import { api } from "../api.js";
import { data } from "../events.js";
import { store } from "../store.js";
import { html, useRef, useEffect } from "../dom.js";
import { toast, confirmDialog } from "../components.js";
import { t } from "../i18n.js";

const SRV_POLL_MS = 5000;
const AMULE_TAIL = 500; // initial history; live lines then arrive via log_appended

const atBottom = (box) => box.scrollHeight - box.scrollTop - box.clientHeight < 30;
const append = (box, text) => {
  const stick = atBottom(box);
  box.appendChild(document.createTextNode(text));
  if (stick) box.scrollTop = box.scrollHeight;
};

// --- aMule log (live via SSE) -------------------------------------------
export function AmuleLogPanel() {
  const boxRef = useRef(null);

  const load = async () => {
    try {
      const r = await api.get("logs/amule?tail=" + AMULE_TAIL);
      if (!boxRef.current) return;
      boxRef.current.textContent = (r.lines || []).join("");
      boxRef.current.scrollTop = boxRef.current.scrollHeight;
    } catch (e) { if (boxRef.current) boxRef.current.textContent = e.message || t("networks_log_error"); }
  };
  const clear = async () => {
    if (!(await confirmDialog(t("networks_log_confirm_clear_amule")))) return;
    try { await api.del("logs/amule"); if (boxRef.current) boxRef.current.textContent = ""; toast(t("networks_log_toast_cleared"), "success"); }
    catch (e) { toast(e.message || t("networks_log_error"), "error"); }
  };

  useEffect(() => {
    data.ensureStatus();
    let lastSeen = store.get("log:appended");
    const unsub = store.subscribe("log:appended", (v) => {
      if (v === lastSeen) return;
      lastSeen = v;
      if (!boxRef.current) return;
      for (const line of (v.lines || [])) append(boxRef.current, line);
    });
    load();
    return () => unsub();
  }, []);

  return html`
    <div class="logbox-wrap">
      <button class="btn admin-only logbox-clear" onClick=${clear}>${t("networks_log_clear")}</button>
      <pre class="logbox" ref=${boxRef}></pre>
    </div>`;
}

// --- server info (polled) -----------------------------------------------
export function ServerInfoPanel() {
  const boxRef = useRef(null);

  const load = async () => {
    try {
      const r = await api.get("logs/serverinfo");
      if (!boxRef.current) return;
      boxRef.current.textContent = r.text || "";
      boxRef.current.scrollTop = boxRef.current.scrollHeight;
    } catch (e) { if (boxRef.current) boxRef.current.textContent = e.message || t("networks_log_error"); }
  };
  const clear = async () => {
    if (!(await confirmDialog(t("networks_log_confirm_clear_serverinfo")))) return;
    try { await api.del("logs/serverinfo"); if (boxRef.current) boxRef.current.textContent = ""; toast(t("networks_log_toast_cleared"), "success"); }
    catch (e) { toast(e.message || t("networks_log_error"), "error"); }
  };

  useEffect(() => {
    load();
    const timer = setInterval(load, SRV_POLL_MS);
    return () => clearInterval(timer);
  }, []);

  return html`
    <div class="logbox-wrap">
      <button class="btn admin-only logbox-clear" onClick=${clear}>${t("networks_log_clear")}</button>
      <pre class="logbox logbox-sm" ref=${boxRef}></pre>
    </div>`;
}
