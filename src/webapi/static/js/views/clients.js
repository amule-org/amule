// Clients view: every connected peer (both directions) in one table with a
// Todos / Descargas / Subidas selector. Data comes from the live `clients`
// store (api.get("clients") + SSE client_added/updated/removed) which already
// carries all peers regardless of transfer direction, so filtering is purely
// client-side. Columns and formatting live in client-table.js, shared with the
// per-file Clients tab of the detail panels.

import { html, useState } from "../dom.js";
import { Tabs } from "../components.js";
import { ClientFilters, ClientTable, HIDDEN_EVERYWHERE, fileNameOf, isDown, isUp, useClients } from "./client-table.js";
import { textMatcher } from "../table.js";
import { t } from "../i18n.js";

// Every tab lists the full column set in the picker; these are the ones each
// starts with hidden, so its default view stays focused on that direction.
const TAB_HIDDEN = {
  all: [...HIDDEN_EVERYWHERE, "dl_session", "remote_rank", "ul_session", "queue_pos", "score"],
  downloads: [...HIDDEN_EVERYWHERE, "ul_state", "ul_speed", "uploaded", "ul_session", "queue_pos", "score"],
  uploads: [...HIDDEN_EVERYWHERE, "dl_state", "dl_speed", "downloaded", "dl_session", "remote_rank"],
};
// Default sort per tab: most transferred first, in the tab's own direction.
const TAB_SORT = { all: "downloaded", downloads: "downloaded", uploads: "uploaded" };

export default function ClientsPanel() {
  const clients = useClients();
  const [filter, setFilter] = useState("all"); // direction tab: all / downloads / uploads
  const [ident, setIdent] = useState("identified");
  const [q, setQ] = useState("");

  const nDown = clients.filter(isDown).length;
  const nUp = clients.filter(isUp).length;

  let list = clients.slice();
  if (filter === "downloads") list = list.filter(isDown);
  else if (filter === "uploads") list = list.filter(isUp);
  if (ident === "identified") list = list.filter((c) => c.ident_state === "identified");
  else if (ident === "not_identified") list = list.filter((c) => c.ident_state !== "identified");
  if (q) { const match = textMatcher(q); list = list.filter((c) => match((c.client_name || "") + " " + fileNameOf(c))); }

  const tabs = [
    { key: "all", label: t("downloads_peer_all"), badge: clients.length },
    { key: "downloads", label: t("downloads_peer_download"), badge: nDown },
    { key: "uploads", label: t("downloads_peer_upload"), badge: nUp },
  ];

  // key=${filter} remounts the table on a tab switch: useTablePrefs reads its
  // storage key once, so each tab needs its own instance to keep its own
  // hidden columns / sort / widths.
  return html`
    <div class="fill-view">
      <section class="net-pane pane-fill">
        <${Tabs} tabs=${tabs} active=${filter} onSelect=${setFilter} />
        <div class="net-pane-body">
          <${ClientTable} key=${filter} rows=${list} prefsKey=${"clients_" + filter}
                          defaultHidden=${TAB_HIDDEN[filter]} defaultSort=${TAB_SORT[filter]}
                          toolbarCls="toolbar pane-toolbar"
                          toolbar=${ClientFilters({ ident, setIdent, q, setQ })} />
        </div>
      </section>
    </div>`;
}
