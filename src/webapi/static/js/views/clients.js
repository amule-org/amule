// Clients view: the global peer surface, mirroring amulegui's Clients tab.
// "Active" peers (both directions) come from the live `clients` store, split by
// a Todos / Descargas / Subidas selector; "Known" peers come from GET
// /known_clients (the credit store) and are matched to the live store by user
// hash to show who is online now. Clicking a row opens the full client detail in
// the bottom panel (SplitDetail). Columns/formatting live in client-table.js.

import { html, useState } from "../dom.js";
import { Tabs } from "../components.js";
import { ClientFilters, ClientTable, HIDDEN_EVERYWHERE, KNOWN_COLS, KNOWN_HIDDEN,
         fileNameOf, isDown, isUp, useClients, useKnownClients } from "./client-table.js";
import { ClientDetail } from "./client-detail.js";
import { SplitDetail } from "./split-detail.js";
import { textMatcher } from "../table.js";
import { t } from "../i18n.js";

// Origin stays visible (a desktop Active column); the per-file "shares"/"avail"
// columns start hidden here -- they answer "for which file?", which the global
// list has no single answer for.
const ACTIVE_HIDDEN = [...HIDDEN_EVERYWHERE, "shares", "avail"];
const TAB_HIDDEN = {
  active: [...ACTIVE_HIDDEN, "dl_session", "remote_rank", "ul_session", "queue_pos", "score"],
  downloading: [...ACTIVE_HIDDEN, "ul_state", "ul_speed", "uploaded", "ul_session", "queue_pos", "score"],
  uploading: [...ACTIVE_HIDDEN, "dl_state", "dl_speed", "downloaded", "dl_session", "remote_rank"],
  known: KNOWN_HIDDEN,
};
const TAB_SORT = { active: "downloaded", downloading: "downloaded", uploading: "uploaded", known: "downloaded" };

const rowKey = (c) => c.ecid != null ? c.ecid : c.user_hash;

export default function ClientsPanel() {
  const raw = useClients(); // undefined until the first snapshot lands
  const clients = raw || [];
  const [filter, setFilter] = useState("active"); // active / downloading / uploading / known
  const [ident, setIdent] = useState("all");
  const [q, setQ] = useState("");
  const [sel, setSel] = useState(null); // { key, ecid?, known? } row shown in detail

  const isKnown = filter === "known";
  const knownRaw = useKnownClients(isKnown);
  const liveByHash = new Map(clients.map((c) => [c.user_hash, c]));

  const nDown = clients.filter(isDown).length;
  const nUp = clients.filter(isUp).length;

  let list;
  if (isKnown) {
    // Fold live peers into the credit-store rows by user hash: online state and
    // the live client (for the detail) without polling /known_clients.
    list = (knownRaw || []).map((k) => {
      const live = liveByHash.get(k.user_hash);
      // Borrow the live transfer speeds for a peer that is online now (the
      // credit store has none); offline -> undefined -> "—" in the cell.
      return { ...k, _live: live, online: !!live,
               download_speed_bytes_per_second: live && live.download_speed_bytes_per_second,
               upload_speed_bytes_per_second: live && live.upload_speed_bytes_per_second };
    });
  } else {
    list = clients.slice();
    if (filter === "downloading") list = list.filter(isDown);
    else if (filter === "uploading") list = list.filter(isUp);
    if (ident !== "all") list = list.filter((c) => c.ident_state === ident);
  }
  if (q) { const match = textMatcher(q); list = list.filter((c) => match((c.name || "") + " " + fileNameOf(c))); }

  const tabs = [
    { key: "active", label: t("clients_tab_active"), badge: clients.length },
    { key: "downloading", label: t("clients_tab_downloading"), badge: nDown },
    { key: "uploading", label: t("clients_tab_uploading"), badge: nUp },
    { key: "known", label: t("clients_tab_known"), badge: knownRaw ? knownRaw.length : null },
  ];

  const onRowClick = (c, e) => {
    // A click on the row-actions buttons (View files / Send message) must not
    // also toggle the detail panel.
    if (e && e.target && e.target.closest(".row-actions")) return;
    const key = rowKey(c);
    // A known row that is online resolves to its live ecid for the full detail;
    // offline, only the stored credit-store fields are available.
    const next = isKnown
      ? (c._live ? { key, ecid: c._live.ecid } : { key, known: c })
      : { key, ecid: c.ecid };
    setSel((s) => (s && s.key === key) ? null : next);
  };

  // Direction/Known selector; drop the detail so a stale highlight/row doesn't
  // carry across tabs (an ecid still resolves, but the highlight would be lost).
  const onTab = (k) => { setFilter(k); setSel(null); };

  // Known has no ident/direction, so its toolbar is just the text filter.
  const toolbar = isKnown
    ? html`<input class="input input-sm" type="text" placeholder=${t("downloads_peer_filter")}
                  value=${q} onInput=${(e) => setQ(e.target.value)} />`
    : ClientFilters({ ident, setIdent, q, setQ });

  return html`
    <div class="fill-view clients-fill">
      <${SplitDetail} storageKey="clients_detail_height" open=${!!sel} onClose=${() => setSel(null)}
        top=${html`
        <section class="net-pane pane-fill">
          <${Tabs} tabs=${tabs} active=${filter} onSelect=${onTab} />
          <div class="net-pane-body">
            <${ClientTable} key=${filter} rows=${list} cols=${isKnown ? KNOWN_COLS : undefined}
                            prefsKey=${"clients_" + filter}
                            defaultHidden=${TAB_HIDDEN[filter]} defaultSort=${TAB_SORT[filter]}
                            loading=${isKnown ? knownRaw === undefined : raw === undefined}
                            onRowClick=${onRowClick} selectedKey=${sel ? sel.key : null}
                            toolbarCls="toolbar pane-toolbar" toolbar=${toolbar} />
          </div>
        </section>`}>
        <${ClientDetail} ecid=${sel ? sel.ecid : null} known=${sel ? sel.known : null} />
      <//>
    </div>`;
}
