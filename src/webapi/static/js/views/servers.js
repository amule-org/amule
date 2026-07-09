// Servers panel: ed2k server list with connect/remove, add server, update
// the list from a URL, and disconnect. Live via the SSE "servers" channel.
// Rendered as the ED2K tab inside the Networks view (no own page header).

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Badge, Placeholder, toast, confirmDialog } from "../components.js";
import { VirtualTable, sortRows } from "../table.js";
import { formatInt } from "../format.js";
import { Icon } from "../icons.js";
import { t, terr } from "../i18n.js";

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
    catch (e) { setConnectingEcid(null); toast(terr(e) || t("networks_server_error"), "error"); }
  };
  const remove = async (s) => {
    if (!(await confirmDialog(t("networks_server_confirm_remove", { name: s.name })))) return;
    try { await api.del("servers/" + s.ecid); data.refresh("servers"); }
    catch (e) { toast(terr(e) || t("networks_server_error"), "error"); }
  };
  const addServer = async (e) => {
    e.preventDefault();
    const address = addr.trim();
    if (!address) { toast(t("networks_server_toast_enter_host_port"), "warn"); return; }
    const body = { address };
    if (name.trim()) body.name = name.trim();
    try { await api.post("servers", body); setAddr(""); setName(""); toast(t("networks_server_toast_added"), "success"); data.refresh("servers"); }
    catch (err) { toast(terr(err) || t("networks_server_error"), "error"); }
  };
  const updateFromUrl = async (e) => {
    e.preventDefault();
    const servers_url = url.trim();
    if (!servers_url) { toast(t("networks_server_toast_enter_url"), "warn"); return; }
    try { await api.post("servers/update", { servers_url }); toast(t("networks_server_toast_updating"), "success"); setTimeout(() => data.refresh("servers"), 2000); }
    catch (err) { toast(terr(err) || t("networks_server_error"), "error"); }
  };
  const disconnect = async () => {
    try { await api.post("networks/disconnect", { network: "ed2k" }); toast(t("networks_server_disconnected"), "success"); }
    catch (e) { toast(terr(e) || t("networks_server_error"), "error"); }
  };

  // Match the connected server by IPv4 + port. The two sides format the address
  // differently: the server list ships `address` as "ip:port", while
  // status.ed2k.server_ip comes from EC_IPv4_t::StringIP() — "[ip:port]" with
  // brackets — so pull the dotted quad out of each before comparing.
  const ipv4 = (v) => { const m = String(v || "").match(/\d+\.\d+\.\d+\.\d+/); return m ? m[0] : ""; };
  const isConnected = (s) =>
    ed2k && ed2k.state === "connected"
    && ipv4(ed2k.server_ip) !== "" && ipv4(ed2k.server_ip) === ipv4(s.address)
    && ed2k.server_port === s.port;

  const columns = [
    { key: "name", label: t("networks_server_name"), cls: "name", sortable: true,
      sortVal: (s) => (s.name || "").toLowerCase(),
      // flex cell so a long name ellipsizes without hiding the "static" badge
      cell: (s) => html`<div class="name-cell" title=${s.name}><span class="name-text">${s.name}</span>${s.static ? html`<${Badge} title=${t("networks_server_badge_static_title")}>${t("networks_server_badge_static")}<//>` : null}</div>` },
    { key: "description", label: t("networks_server_description"), width: "180px", sortable: true,
      sortVal: (s) => (s.description || "").toLowerCase(), cell: (s) => s.description || "" },
    { label: t("networks_server_version"), width: "90px", cell: (s) => s.version || "" },
    { label: t("networks_server_address"), num: true, width: "180px",
      cell: (s) => s.address && s.address.includes(":") ? s.address : (s.address + ":" + s.port) },
    { key: "users", label: t("networks_server_users"), num: true, width: "130px", sortable: true,
      sortVal: (s) => s.users || 0,
      cell: (s) => formatInt(s.users) + (s.max_users ? " / " + formatInt(s.max_users) : "") },
    { key: "files", label: t("networks_server_files"), num: true, width: "110px", sortable: true,
      sortVal: (s) => s.files || 0, cell: (s) => formatInt(s.files) },
    { key: "ping", label: t("networks_server_ping"), num: true, width: "90px", sortable: true,
      sortVal: (s) => s.ping_ms || 0, cell: (s) => s.ping_ms ? s.ping_ms + " ms" : "—" },
    { label: t("networks_server_priority"), width: "90px", cell: (s) => s.priority || "" },
    ...(isGuest ? [] : [{ label: t("networks_server_actions"), cls: "row-actions admin-only", width: "90px",
      cell: (s) => html`
        <button class="btn btn-icon btn-sm" title=${t("networks_server_connect")} onClick=${() => connect(s.ecid)}>
          <${Icon} name="connect" />
        </button>
        <button class="btn btn-icon btn-sm btn-danger" title=${t("networks_server_remove")} onClick=${() => remove(s)}>
          <${Icon} name="remove" />
        </button>` }]),
  ];

  const list = sortRows(servers, columns, sortKey, sortDir);
  const rowClass = (s) => isConnected(s) ? "connected" : connectingEcid === s.ecid ? "connecting" : "";

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
    <${VirtualTable} columns=${columns} rows=${list} rowKey=${(s) => s.ecid} rowClass=${rowClass}
                     sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                     empty=${html`<${Placeholder} kind="info">${t("networks_server_empty")}<//>`} />`;
}
