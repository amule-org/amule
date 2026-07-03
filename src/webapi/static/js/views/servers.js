// Servers panel: ed2k server list with connect/remove, add server, update
// the list from a URL, and disconnect. Live via the SSE "servers" channel.
// Rendered as the ED2K tab inside the Networks view (no own page header).

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Badge, Placeholder, toast, confirmDialog } from "../components.js";
import { formatInt } from "../format.js";
import { Icon } from "../icons.js";
import { t } from "../i18n.js";

export function ServersPanel({ isGuest }) {
  const servers = useStore("servers") || [];
  const status = useStore("status");
  const ed2k = status && status.ed2k;
  const [sortKey, setSortKey] = useState("users");
  const [sortDir, setSortDir] = useState(-1);
  const [addr, setAddr] = useState("");
  const [name, setName] = useState("");
  const [url, setUrl] = useState("");
  const [connectingEcid, setConnectingEcid] = useState(null);

  useEffect(() => {
    data.register({ key: "servers", eventPrefix: "server", id: "ecid",
      list: () => api.get("servers").then((r) => r.servers || []) });
    data.ensure("servers");
  }, []);

  // Clear the optimistic "connecting" row as soon as ed2k leaves that state
  // (connected to it, connected elsewhere, or the attempt failed).
  useEffect(() => {
    if (ed2k && ed2k.state !== "connecting") setConnectingEcid(null);
  }, [ed2k && ed2k.state]);

  const toggleSort = (key) => {
    if (sortKey === key) setSortDir(-sortDir);
    else { setSortKey(key); setSortDir(1); }
  };

  const connect = async (ecid) => {
    setConnectingEcid(ecid);
    try { await api.post("servers/" + ecid + "/connect"); toast(t("networks_server_toast_connecting"), "success"); }
    catch (e) { setConnectingEcid(null); toast(e.message || t("networks_server_error"), "error"); }
  };
  const remove = async (s) => {
    if (!(await confirmDialog(t("networks_server_confirm_remove", { name: s.name })))) return;
    try { await api.del("servers/" + s.ecid); data.refresh("servers"); }
    catch (e) { toast(e.message || t("networks_server_error"), "error"); }
  };
  const addServer = async (e) => {
    e.preventDefault();
    const address = addr.trim();
    if (!address) { toast(t("networks_server_toast_enter_host_port"), "warn"); return; }
    const body = { address };
    if (name.trim()) body.name = name.trim();
    try { await api.post("servers", body); setAddr(""); setName(""); toast(t("networks_server_toast_added"), "success"); data.refresh("servers"); }
    catch (err) { toast(err.message || t("networks_server_error"), "error"); }
  };
  const updateFromUrl = async (e) => {
    e.preventDefault();
    const servers_url = url.trim();
    if (!servers_url) { toast(t("networks_server_toast_enter_url"), "warn"); return; }
    try { await api.post("servers/update", { servers_url }); toast(t("networks_server_toast_updating"), "success"); setTimeout(() => data.refresh("servers"), 2000); }
    catch (err) { toast(err.message || t("networks_server_error"), "error"); }
  };
  const disconnect = async () => {
    try { await api.post("networks/disconnect", { network: "ed2k" }); toast(t("networks_server_disconnected"), "success"); }
    catch (e) { toast(e.message || t("networks_server_error"), "error"); }
  };

  const list = servers.slice().sort((a, b) => sortDir * cmp(sortVal(a, sortKey), sortVal(b, sortKey)));
  const cols = isGuest ? 8 : 9;

  const sortArrow = (key) => key === sortKey
    ? html`<span class="sort-arrow"><${Icon} name=${sortDir > 0 ? "sort-asc" : "sort-desc"} /></span>` : null;
  const Th = (label, key, num) => html`
    <th class=${(key ? "sortable " : "") + (num ? "num" : "")} onClick=${key ? () => toggleSort(key) : null}>
      ${label}${sortArrow(key)}
    </th>`;

  // Match the connected server by IPv4 + port. The two sides format the address
  // differently: the server list ships `address` as "ip:port", while
  // status.ed2k.server_ip comes from EC_IPv4_t::StringIP() — "[ip:port]" with
  // brackets — so pull the dotted quad out of each before comparing.
  const ipv4 = (v) => { const m = String(v || "").match(/\d+\.\d+\.\d+\.\d+/); return m ? m[0] : ""; };
  const isConnected = (s) =>
    ed2k && ed2k.state === "connected"
    && ipv4(ed2k.server_ip) !== "" && ipv4(ed2k.server_ip) === ipv4(s.address)
    && ed2k.server_port === s.port;

  const row = (s) => html`
    <tr class=${isConnected(s) ? "connected" : connectingEcid === s.ecid ? "connecting" : ""}>
      <td class="name" title=${s.name}>
        ${s.name}${s.static ? html`<${Badge} title=${t("networks_server_badge_static_title")}>${t("networks_server_badge_static")}<//>` : null}
      </td>
      <td>${s.description || ""}</td>
      <td>${s.version || ""}</td>
      <td class="num">${s.address && s.address.includes(":") ? s.address : (s.address + ":" + s.port)}</td>
      <td class="num">${formatInt(s.users) + (s.max_users ? " / " + formatInt(s.max_users) : "")}</td>
      <td class="num">${formatInt(s.files)}</td>
      <td class="num">${s.ping_ms ? s.ping_ms + " ms" : "—"}</td>
      <td>${s.priority || ""}</td>
      ${isGuest ? null : html`
        <td class="row-actions admin-only">
          <button class="btn btn-icon btn-sm" title=${t("networks_server_connect")} onClick=${() => connect(s.ecid)}>
            <${Icon} name="connect" />
          </button>
          <button class="btn btn-icon btn-sm btn-danger" title=${t("networks_server_remove")} onClick=${() => remove(s)}>
            <${Icon} name="remove" />
          </button>
        </td>`}
    </tr>`;

  return html`
    <div class="server-toolbars admin-only">
      <form class="toolbar admin-only" onSubmit=${addServer}>
        <input class="input input-sm" placeholder=${t("networks_server_host_port_ph")} value=${addr} onInput=${(e) => setAddr(e.target.value)} />
        <input class="input input-sm" placeholder=${t("networks_server_name_ph")} value=${name} onInput=${(e) => setName(e.target.value)} />
        <button class="btn btn-sm" type="submit">${t("networks_server_add")}</button>
        <div class="spacer"></div>
        <button class="btn btn-sm admin-only" type="button" onClick=${disconnect}>${t("networks_server_disconnect")}</button>
      </form>
      <form class="toolbar admin-only" onSubmit=${updateFromUrl}>
        <input class="input input-sm input-url" placeholder="http(s)://…/server.met"
               value=${url} onInput=${(e) => setUrl(e.target.value)} />
        <button class="btn btn-sm" type="submit">${t("networks_server_update_from_url")}</button>
      </form>
    </div>
    <div class="table-wrap">
      <table class="data">
        <thead>
          <tr>
            ${Th(t("networks_server_name"), "name")}${Th(t("networks_server_description"), "description")}${Th(t("networks_server_version"))}${Th(t("networks_server_address"))}
            ${Th(t("networks_server_users"), "users", true)}${Th(t("networks_server_files"), "files", true)}${Th(t("networks_server_ping"), "ping", true)}${Th(t("networks_server_priority"))}
            ${isGuest ? null : html`<th class="admin-only">${t("networks_server_actions")}</th>`}
          </tr>
        </thead>
        <tbody>
          ${list.length
            ? list.map(row)
            : html`<tr><td colspan=${cols}><${Placeholder} kind="info">${t("networks_server_empty")}<//></td></tr>`}
        </tbody>
      </table>
    </div>`;
}

function sortVal(s, k) {
  switch (k) {
    case "name": return (s.name || "").toLowerCase();
    case "description": return (s.description || "").toLowerCase();
    case "users": return s.users || 0;
    case "files": return s.files || 0;
    case "ping": return s.ping_ms || 0;
    default: return 0;
  }
}
function cmp(a, b) { return a < b ? -1 : a > b ? 1 : 0; }
