// Networks view: mirrors the aMule desktop "Networks" page (NetDialog) — a
// top notebook with ED2K (server list) and Kad tabs, and a bottom notebook
// with the aMule log and Server info. Composed from the existing panels.

import { html, useState, useStore } from "../dom.js";
import { api } from "../api.js";
import { Tabs, toast, confirmDialog } from "../components.js";
import { t, terr } from "../i18n.js";
import { Icon } from "../icons.js";
import { useSplitHeight } from "./split-detail.js";
import { ServersPanel } from "./servers.js";
import { KadPanel, KadInfoPanel } from "./kad.js";
import { Ed2kInfoPanel } from "./ed2k.js";
import { AmuleLogPanel, ServerInfoPanel } from "./logs.js";

export default function Networks({ isGuest }) {
  const [top, setTop] = useState("ed2k");
  const [bottom, setBottom] = useState("amulelog");
  // Draggable divider between the top (servers/Kad) and bottom (log/info)
  // panes, like the desktop NetDialog's wxSplitterWindow. On phones the CSS
  // hides the splitter and both panes flow (see .net-split in app.css).
  const { height, containerRef, splitterProps } = useSplitHeight("net:split");

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
    <div class="net-split fill-view" ref=${containerRef}>
      <section class="net-pane pane-fill">
        <${Tabs} tabs=${topTabs} active=${top} onSelect=${setTop}
                 extra=${html`<${ConnectButton} />`} />
        <div class="net-pane-body">
          ${top === "ed2k"
            ? html`<${ServersPanel} isGuest=${isGuest} />`
            : html`<${KadPanel} />`}
        </div>
      </section>

      <div class="splitter" ...${splitterProps}></div>

      <section class="net-pane" style=${{ height: height + "px" }}>
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

// aMule-style connection button: a coloured plug (red = disconnected, amber =
// connecting, green = connected) that toggles both networks. Lives on the
// right of the ED2K/Kad tab row.
function ConnectButton() {
  const status = useStore("status");
  const ed2k = (status && status.ed2k) || {};
  const kad = (status && status.kad) || {};
  const ed2kConn = ed2k.state === "connected";
  // "connecting" wins over "connected": switching ed2k servers while Kad
  // stays up (or vice versa) should still surface as a transition, not get
  // masked by the other network already being connected.
  const connecting = ed2k.state === "connecting" || kad.state === "connecting";
  const connected = !connecting && (ed2kConn || kad.state === "connected");

  // Colour and label follow the actual current state, not the action the
  // click would perform.
  const cls = connected ? "connected" : connecting ? "connecting" : "disconnected";
  const label = t("app_" + cls);
  const toggle = async () => {
    try {
      if (connected) {
        if (!(await confirmDialog(t("app_confirm_disconnect_both"),
              { okLabel: t("app_disconnect") }))) return;
        await api.post("networks/disconnect", { network: "both" });
        toast(t("app_toast_disconnecting"), "success");
      } else {
        await api.post("networks/connect", { network: "both" });
        toast(t("app_toast_connecting"), "success");
      }
    } catch (e) { toast(terr(e) || t("app_error"), "error"); }
  };

  return html`
    <button class=${"tool-btn conn-btn admin-only " + cls} title=${label} onClick=${toggle}>
      <${Icon} name="connect" size=${20} />
      <span class="tool-label">${label}</span>
    </button>`;
}
