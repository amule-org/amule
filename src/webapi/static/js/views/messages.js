// Messages view (the desktop's src/ChatWnd.cpp): friends left, conversations
// right. Rendering only — conversations live in ../chats.js so they survive
// navigation; friends are an ordinary live collection.

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useRef, useStore } from "../dom.js";
import { Placeholder, Tabs, toast, confirmDialog } from "../components.js";
import { Icon } from "../icons.js";
import { chats } from "../chats.js";
import { searches } from "../searches.js";
import { t, terr } from "../i18n.js";

const TAB_LABEL_MAX = 22;

// undefined until the first snapshot lands, [] once there are no friends.
function useFriends() {
  useEffect(() => {
    data.register({ key: "friends", eventPrefix: "friend", id: "ecid",
      list: () => api.get("friends").then((r) => r.friends || []) });
    data.ensure("friends");
  }, []);
  return useStore("friends");
}

export default function Messages({ isGuest }) {
  const reg = useStore("chats") || { tabs: [], activePeer: "", unread: 0 };
  const rawFriends = useFriends();
  const [addOpen, setAddOpen] = useState(false);

  useEffect(() => { chats.ensure(); }, []);
  useEffect(() => { chats.markRead(); }, [reg.activePeer]);

  const active = reg.tabs.find((x) => x.peer === reg.activePeer) || null;

  return html`
    <div class="fill-view msg-split">
      <${FriendsPane} friends=${rawFriends} isGuest=${isGuest}
                      activePeer=${reg.activePeer} onAdd=${() => setAddOpen(true)} />
      <${ChatPane} reg=${reg} active=${active} isGuest=${isGuest} />
      ${addOpen ? html`<${AddFriendModal} onClose=${() => setAddOpen(false)} />` : null}
    </div>`;
}

// --- friends -------------------------------------------------------------

function FriendsPane({ friends, isGuest, activePeer, onAdd }) {
  const list = (friends || []).slice()
    .sort((a, b) => (a.name || "").localeCompare(b.name || ""));

  // Nudge both readers rather than wait for the next-tick SSE events: the
  // friends collection, and the friend link conversations carry (chat has no
  // friend-link event of its own).
  const mutate = async (fn, okKey) => {
    try { await fn(); toast(t(okKey), "success"); }
    catch (e) { toast(terr(e) || t("messages_error"), "error"); }
    data.refresh("friends");
    chats.adopt();
  };

  const toggleSlot = (f) => mutate(
    () => api.patch("friends/" + f.ecid, { friend_slot: !f.friend_slot }),
    "messages_toast_friend_slot");

  const remove = async (f) => {
    if (!(await confirmDialog(t("messages_confirm_remove_friend")))) return;
    mutate(() => api.del("friends/" + f.ecid), "messages_toast_friend_removed");
  };

  // Friend-addressed browse: a friend's stored address reaches one that is offline.
  const viewFiles = async (f) => {
    try { await searches.browseFriend(f.ecid, f.name); }
    catch (e) { toast(terr(e) || t("messages_error"), "error"); }
  };

  const row = (f) => {
    // No ip (added by hash alone) means no conversation key, so disable the
    // open action rather than build one the chat routes would 400 on.
    const peer = f.ip && f.port ? f.ip + ":" + f.port : "";
    // A real <button>, not an <li> click handler, for keyboard/a11y.
    return html`
      <li class=${"friend-row" + (peer && peer === activePeer ? " active" : "")} key=${f.ecid}>
        <button type="button" class=${"friend-open" + (f.online ? " online" : "")} disabled=${!peer}
                title=${f.name + (peer ? " — " + peer : "") + " · " + (f.online ? t("messages_online") : t("messages_offline"))}
                onClick=${() => chats.open({ peer, ip: f.ip, port: f.port, name: f.name, friendEcid: f.ecid, clientEcid: f.client_ecid || 0 })}>
          <span class=${"friend-dot" + (f.online ? " online" : "")}></span>
          <span class="friend-name">${f.name}</span>
        </button>
        ${isGuest ? null : html`
          <span class="row-actions admin-only">
            <button class=${"btn btn-icon btn-sm" + (f.friend_slot ? " active" : "")}
                    title=${t("messages_friend_slot")} disabled=${!f.online}
                    onClick=${() => toggleSlot(f)}><${Icon} name="star" /></button>
            <button class="btn btn-icon btn-sm" title=${t("messages_view_files")}
                    onClick=${() => viewFiles(f)}><${Icon} name="search" /></button>
            <button class="btn btn-icon btn-sm btn-danger" title=${t("messages_remove_friend")}
                    onClick=${() => remove(f)}><${Icon} name="trash" /></button>
          </span>`}
      </li>`;
  };

  return html`
    <section class="msg-pane">
      <h3 class="msg-pane-head"><${Icon} name="clients" size=${16} /> ${t("messages_friends")}</h3>
      <div class="msg-pane-body friend-list-wrap">
        ${friends === undefined
          ? html`<${Placeholder} kind="loading">${t("app_loading")}<//>`
          : list.length
            ? html`<ul class="friend-list">${list.map(row)}</ul>`
            : html`<${Placeholder} kind="info">${t("messages_friends_empty")}<//>`}
      </div>
      <div class="msg-pane-foot">
        <button class="btn btn-sm admin-only" onClick=${onAdd}>${t("messages_add_friend")}</button>
      </div>
    </section>`;
}

function AddFriendModal({ onClose }) {
  const [ip, setIp] = useState("");
  // Just the eD2k default (DEFAULT_TCP_PORT); the address is stored, never
  // checked against the peer.
  const [port, setPort] = useState("4662");
  const [name, setName] = useState("");
  const [hash, setHash] = useState("");

  const submit = async (e) => {
    e.preventDefault();
    const p = Number(port);
    if (!ip.trim() || !p || p < 1 || p > 65535) { toast(t("messages_err_ip_port"), "warn"); return; }
    const body = { ip: ip.trim(), port: p };
    if (name.trim()) body.name = name.trim();
    if (hash.trim()) body.user_hash = hash.trim();
    try {
      await api.post("friends", body);
      toast(t("messages_toast_friend_added"), "success");
      onClose();
    } catch (err) {
      toast(terr(err) || t("messages_error"), "error");
    }
    data.refresh("friends");
  };

  const field = (label, value, setter, extra) => html`
    <label class="field">
      <span>${label}</span>
      <input class="input" value=${value} onInput=${(e) => setter(e.target.value)} ...${extra || {}} />
    </label>`;

  return html`
    <div class="modal-overlay" onClick=${onClose}>
      <div class="modal" onClick=${(e) => e.stopPropagation()}>
        <div class="modal-header"><h3>${t("messages_add_friend_title")}</h3></div>
        <form onSubmit=${submit}>
          <div class="form-grid form-grid-2">
            ${field(t("messages_field_ip"), ip, setIp, { placeholder: "0.0.0.0", required: true })}
            ${field(t("messages_field_port"), port, setPort, { type: "number", min: "1", max: "65535", required: true })}
            ${field(t("messages_field_username"), name, setName)}
            ${field(t("messages_field_userhash"), hash, setHash, { maxlength: "32" })}
          </div>
          <div class="modal-actions">
            <button class="btn" type="button" onClick=${onClose}>${t("common_cancel")}</button>
            <button class="btn btn-primary" type="submit">${t("messages_add")}</button>
          </div>
        </form>
      </div>
    </div>`;
}

// --- chat ----------------------------------------------------------------

function ChatPane({ reg, active, isGuest }) {
  const [draft, setDraft] = useState("");

  const tabItems = reg.tabs.map((x) => ({
    key: x.peer,
    label: x.name.length > TAB_LABEL_MAX ? x.name.slice(0, TAB_LABEL_MAX - 1) + "…" : x.name,
    title: x.name + " — " + x.peer,
    badge: x.unread || null,
    cls: x.online ? "online" : "",
    closeLabel: t("messages_tab_close"),
  }));

  const send = (e) => {
    e.preventDefault();
    const text = draft.trim();
    if (!text || !active) return; // dropped silently, like the desktop
    setDraft("");
    chats.send(active.peer, text);
  };

  const addToFriends = async () => {
    try {
      // Promoting the live peer, so only reachable while it is connected.
      await api.post("friends", { client_ecid: active.clientEcid });
      toast(t("messages_toast_friend_added"), "success");
    } catch (e) { toast(terr(e) || t("messages_error"), "error"); }
    data.refresh("friends");
    chats.adopt(); // picks up the new friend_ecid, which retires this button
  };

  const extra = reg.tabs.length ? html`
    <button class="btn btn-sm admin-only" onClick=${() => chats.closeAll()}>${t("messages_close_all")}</button>` : null;

  return html`
    <section class="msg-pane">
      <h3 class="msg-pane-head"><${Icon} name="messages" size=${16} /> ${t("messages_messages")}</h3>
      ${reg.tabs.length ? html`
        <${Tabs} cls="msg-tabs" tabs=${tabItems} active=${reg.activePeer}
                 onSelect=${(p) => chats.setActive(p)}
                 onClose=${isGuest ? null : (p) => chats.close(p)}
                 extra=${extra} />` : null}
      <div class="msg-pane-body chat-body">
        ${reg.unsupported
          ? html`<${Placeholder} kind="info">${t("messages_unsupported")}<//>`
          : active
            ? html`
              <div class="chat-peer">
                <strong>${active.name}</strong>
                <span class="mono">${active.peer}</span>
                <span class=${"status-chip " + (active.online ? "ok" : "off")}>
                  ${active.online ? t("messages_online") : t("messages_offline")}
                </span>
                ${!active.friendEcid && active.online ? html`
                  <button class="btn btn-sm admin-only" onClick=${addToFriends}>
                    ${t("messages_add_to_friends")}
                  </button>` : null}
              </div>
              <${ChatLog} key=${active.peer} peer=${active.peer}
                          peerName=${active.name} myNick=${reg.myNick} />`
            : html`<${Placeholder} kind="info">${t("messages_empty")}<//>`}
      </div>
      <form class="chat-compose admin-only" onSubmit=${send}>
        <input class="input" type="text" maxlength="1024" placeholder=${t("messages_ph")}
               aria-label=${t("messages_ph")} disabled=${!active} value=${draft}
               onInput=${(e) => setDraft(e.target.value)} />
        <button class="btn btn-primary" type="submit" title=${t("messages_send_tip")}
                disabled=${!active}>${t("messages_send")}</button>
        <button class="btn" type="button" title=${t("messages_close_tip")} disabled=${!active}
                onClick=${() => chats.close(active.peer)}>${t("messages_close")}</button>
      </form>
      ${isGuest ? html`<p class="hint">${t("messages_guest_readonly")}</p>` : null}
    </section>`;
}

const two = (n) => (n < 10 ? "0" + n : String(n));
// From the message's own core timestamp, not the wall clock the wx GUI stamps.
function hhmmss(ts) {
  const d = new Date(ts * 1000);
  return two(d.getHours()) + ":" + two(d.getMinutes()) + ":" + two(d.getSeconds());
}

// Flat coloured text log like the desktop, "[HH:MM:SS] Nick: text", but with
// theme tokens instead of its fixed #0000FF (unreadable on a dark background).
function ChatLog({ peer, peerName, myNick }) {
  const messages = useStore("chat:" + peer) || [];
  const boxRef = useRef(null);
  const stickRef = useRef(true);

  // Key on the array, not its length: at HISTORY_LIMIT each new message evicts
  // one, so a length-keyed effect would stop scrolling to the bottom.
  useEffect(() => {
    const box = boxRef.current;
    if (box && stickRef.current) box.scrollTop = box.scrollHeight;
  }, [messages, peer]);

  const onScroll = (e) => {
    const box = e.target;
    stickRef.current = box.scrollHeight - box.scrollTop - box.clientHeight < 30;
  };

  return html`
    <div class="chatlog" ref=${boxRef} onScroll=${onScroll}>
      ${messages.map((m) => html`
        <div class="chat-line" key=${m.id}>
          <span class="chat-time">[${hhmmss(m.sent_at)}]</span>
          <span class=${"chat-nick " + m.direction}>
            ${m.direction === "out" ? (myNick || t("messages_me")) : peerName}
          </span>${": "}<span class="chat-text">${m.text}</span>
        </div>`)}
    </div>`;
}
