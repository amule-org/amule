// Networks view: mirrors the aMule desktop "Networks" page (NetDialog) — a
// top notebook with ED2K (server list) and Kad tabs, and a bottom notebook
// with the aMule log and Server info. Composed from the existing panels.

import { html, useState } from "../dom.js";
import { Tabs } from "../components.js";
import { t } from "../i18n.js";
import { ServersPanel } from "./servers.js";
import { KadPanel, KadInfoPanel } from "./kad.js";
import { Ed2kInfoPanel } from "./ed2k.js";
import { AmuleLogPanel, ServerInfoPanel } from "./logs.js";

export default function Networks({ isGuest }) {
  const [top, setTop] = useState("ed2k");
  const [bottom, setBottom] = useState("amulelog");

  const topTabs = [
    { key: "ed2k", label: t("networks_tab_ed2k") },
    { key: "kad", label: t("networks_tab_kad") },
  ];
  const bottomTabs = [
    { key: "amulelog", label: t("networks_tab_amule_log") },
    { key: "serverinfo", label: t("networks_tab_server_info") },
    { key: "ed2kinfo", label: t("networks_tab_ed2k_info") },
    { key: "kadinfo", label: t("networks_tab_kad_info") },
  ];

  return html`
    <div class="net-split">
      <section class="net-pane">
        <${Tabs} tabs=${topTabs} active=${top} onSelect=${setTop} />
        <div class="net-pane-body">
          ${top === "ed2k"
            ? html`<${ServersPanel} isGuest=${isGuest} />`
            : html`<${KadPanel} />`}
        </div>
      </section>

      <section class="net-pane">
        <${Tabs} tabs=${bottomTabs} active=${bottom} onSelect=${setBottom} />
        <div class="net-pane-body">
          ${bottom === "amulelog"
            ? html`<${AmuleLogPanel} />`
            : bottom === "serverinfo"
            ? html`<${ServerInfoPanel} />`
            : bottom === "ed2kinfo"
            ? html`<${Ed2kInfoPanel} />`
            : html`<${KadInfoPanel} />`}
        </div>
      </section>
    </div>`;
}
