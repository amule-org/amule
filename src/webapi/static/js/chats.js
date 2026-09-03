// Conversation registry, kept outside the view so tabs and incoming messages
// survive navigation (like searches.js). Publishes a light tab list on store
// "chats" and the active transcript on "chat:<peer>".
//
// `peer` is "<ip>:<port>" and goes into the URL verbatim — amuleapi does not
// percent-decode path captures, so an encoded colon is a 400.

import { api } from "./api.js";
import { data } from "./events.js";
import { store } from "./store.js";
import { toast } from "./components.js";
import { t, terr } from "./i18n.js";

const TABS_SYNC_MS = 500;
const LOG_SYNC_MS = 300;
const POLL_MS = 2000; // fallback only, while SSE is down
const HISTORY_LIMIT = 200; // the daemon's per-conversation retention

const convs = new Map();
let activePeer = "";
let started = false;
let unsupported = false;
let myNick = "";
let pollTimer = null;
let tabsTimer = 0;
let logTimer = 0;
let offs = [];
let lastMessage = null, lastClosed = null, lastResync = null;

// The API's `name` for a peer the core has no nick for.
function fallbackName(ip, port) { return "IP: " + ip + " Port: " + port; }

function pageVisible() {
  return (location.hash || "").replace(/^#\/?/, "").split("?")[0] === "messages";
}

// Don't let the API fallback overwrite a real name a caller gave us.
function betterName(conv, name) {
  return name && name !== fallbackName(conv.ip, conv.port) ? name : conv.name;
}

function newConv({ peer, ip, port, name = "", clientEcid = 0, friendEcid = 0 }) {
  return {
    peer, ip, port,
    name: name || fallbackName(ip, port),
    clientEcid, friendEcid,
    messages: new Map(),
    lastMsgId: 0,
    unread: 0,
    // The daemon only holds a conversation once its first message goes out;
    // false for one opened locally.
    known: false,
    loaded: false,
    fetching: false,
  };
}

function messageList(conv) {
  return Array.from(conv.messages.values()).sort((a, b) => a.id - b.id);
}

// Resolve the API fallback name against the live friends list, so a friend's
// conversation shows the friend's name.
function displayName(conv) {
  if (conv.name !== fallbackName(conv.ip, conv.port)) return conv.name;
  const friends = store.get("friends") || [];
  const f = friends.find((x) => (conv.friendEcid && x.ecid === conv.friendEcid) ||
                                (x.ip === conv.ip && x.port === conv.port));
  return f && f.name ? f.name : conv.name;
}

function tabList() {
  return Array.from(convs.values()).map((c) => ({
    peer: c.peer, name: displayName(c),
    clientEcid: c.clientEcid, friendEcid: c.friendEcid,
    online: c.clientEcid !== 0, unread: c.unread,
  }));
}

function unreadTotal() {
  let n = 0;
  for (const c of convs.values()) n += c.unread;
  return n;
}

function publishTabs() {
  if (tabsTimer) { clearTimeout(tabsTimer); tabsTimer = 0; }
  store.set("chats", { tabs: tabList(), activePeer, unread: unreadTotal(), unsupported, myNick });
}
function publishTabsSoon() {
  if (!tabsTimer) tabsTimer = setTimeout(() => { tabsTimer = 0; publishTabs(); }, TABS_SYNC_MS);
}
function publishLog(peer) {
  // Bail before touching the timer: the pending publish is the active
  // conversation's, so clearing it here would drop it.
  if (peer !== activePeer) return;
  if (logTimer) { clearTimeout(logTimer); logTimer = 0; }
  const conv = convs.get(peer);
  store.set("chat:" + peer, conv ? messageList(conv) : []);
}
function publishLogSoon(peer) {
  if (peer !== activePeer || logTimer) return;
  logTimer = setTimeout(() => { logTimer = 0; publishLog(peer); }, LOG_SYNC_MS);
}

// Chat routes answer 503 ec_unsupported on an amuled predating chat. Latch it
// so the view reports it once instead of every call toasting.
function noteError(e) {
  if (e && e.code === "ec_unsupported" && !unsupported) { unsupported = true; publishTabs(); }
  return e;
}

// --- reads ---------------------------------------------------------------

async function loadMessages(peer) {
  const conv = convs.get(peer);
  if (!conv || conv.fetching) return;
  // Nothing held and nothing on the daemon: the read would 404 by construction.
  if (!conv.known && !conv.messages.size) return;
  conv.fetching = true;
  try {
    const q = conv.loaded ? "?since_message_id=" + conv.lastMsgId : "?tail=" + HISTORY_LIMIT;
    const r = await api.get("chats/" + peer + "/messages" + q);
    const cur = convs.get(peer);
    if (!cur) return;
    for (const m of r.messages || []) addMessage(cur, m);
    cur.loaded = true;
    publishTabs();
    publishLog(peer);
  } catch (e) {
    noteError(e);
    if (e && e.status === 404) { const cur = convs.get(peer); if (cur) cur.loaded = true; }
  } finally {
    const cur = convs.get(peer);
    if (cur) cur.fetching = false;
  }
}

// Keyed by id (monotonic, never reused) so a send's optimistic echo and the
// SSE frame that follows collapse into one line.
function addMessage(conv, m) {
  conv.messages.set(m.id, m);
  if (m.id > conv.lastMsgId) conv.lastMsgId = m.id;
  if (conv.messages.size > HISTORY_LIMIT) {
    const ids = Array.from(conv.messages.keys()).sort((a, b) => a - b);
    for (const id of ids.slice(0, conv.messages.size - HISTORY_LIMIT)) conv.messages.delete(id);
  }
}

// Adopt conversations any client opened and drop ones the daemon no longer
// holds. Transcripts load lazily on activation, not here.
async function adopt() {
  let list;
  try { list = (await api.get("chats")).chats || []; }
  catch (e) { noteError(e); return; }
  let added = false;
  for (const s of list) {
    const cur = convs.get(s.address);
    if (cur) {
      cur.known = true;
      const name = betterName(cur, s.name);
      if (name !== cur.name) { cur.name = name; added = true; }
      // null when offline / not a friend; 0 is this module's "none".
      const cEcid = s.client_ecid || 0, fEcid = s.friend_ecid || 0;
      if (cEcid !== cur.clientEcid) { cur.clientEcid = cEcid; added = true; }
      if (fEcid !== cur.friendEcid) { cur.friendEcid = fEcid; added = true; }
      if (cur.loaded && s.last_message_id > cur.lastMsgId) loadMessages(s.address);
      continue;
    }
    const conv = newConv({
      peer: s.address, ip: s.ip, port: s.port, name: s.name,
      clientEcid: s.client_ecid || 0, friendEcid: s.friend_ecid || 0,
    });
    conv.known = true;
    convs.set(s.address, conv);
    added = true;
  }
  // Absent = closed elsewhere or evicted by the daemon's cap. Skip local-only
  // ones, not listed yet by definition.
  for (const peer of Array.from(convs.keys())) {
    const conv = convs.get(peer);
    if (conv && conv.known && !list.some((s) => s.address === peer)) { drop(peer, false); added = true; }
  }
  // The daemon lists most-recently-active first.
  if (!activePeer && list.length) { setActive(list[0].address); return; }
  if (added) publishTabs();
}

// --- writes --------------------------------------------------------------

// `read` is false only when auto-selecting a just-arrived conversation off-page:
// it becomes the selected tab but keeps its unread until Messages is opened.
function setActive(peer, read = true) {
  activePeer = peer || "";
  const conv = convs.get(activePeer);
  if (conv && read) conv.unread = 0;
  publishTabs();
  if (!conv) return;
  publishLog(activePeer);
  loadMessages(activePeer);
}

// Select or create a conversation without sending — from the friend list / Clients.
function open({ peer, ip, port, name, friendEcid = 0, clientEcid = 0 }) {
  const conv = convs.get(peer);
  if (!conv) {
    convs.set(peer, newConv({ peer, ip, port, name, friendEcid, clientEcid }));
  } else {
    // The caller's links/name may be fresher than the listing.
    if (name) conv.name = name;
    if (friendEcid) conv.friendEcid = friendEcid;
    if (clientEcid) conv.clientEcid = clientEcid;
  }
  setActive(peer);
  return peer;
}

function drop(peer, notify) {
  const conv = convs.get(peer);
  if (!conv) return;
  // Next tab: right, else left. Map order is strip order.
  const keys = Array.from(convs.keys());
  const next = keys[keys.indexOf(peer) + 1] || keys[keys.indexOf(peer) - 1];
  convs.delete(peer);
  store.set("chat:" + peer, []);
  if (notify) toast(t("messages_toast_closed", { name: conv.name }), "info");
  if (activePeer === peer) {
    activePeer = "";
    if (next) { setActive(next); return; }
  }
  publishTabs();
}

// --- SSE routing ---------------------------------------------------------

function onMessage(p) {
  if (!p || p === lastMessage) return;
  lastMessage = p;
  if (!p.address || !p.message) return;
  let conv = convs.get(p.address);
  if (!conv) {
    // No "conversation started" event; the first message implies a new one.
    conv = newConv({
      peer: p.address, ip: p.ip, port: p.port, name: p.name,
      clientEcid: p.client_ecid || 0, friendEcid: p.friend_ecid || 0,
    });
    convs.set(p.address, conv);
  } else {
    conv.name = betterName(conv, p.name);
    conv.clientEcid = p.client_ecid || 0;
    conv.friendEcid = p.friend_ecid || 0;
  }
  conv.known = true;
  const seen = conv.messages.has(p.message.id);
  addMessage(conv, p.message);
  // Not unread: our own outbound line, or an id already held (a send's echo).
  if (!seen && p.message.direction === "in" && (!pageVisible() || p.address !== activePeer)) {
    conv.unread++;
  }
  // The strip must always have one selected; off-page, don't clear its unread.
  if (!activePeer) { setActive(p.address, pageVisible()); return; }
  publishTabsSoon();
  publishLogSoon(p.address);
}

function onClosed(p) {
  if (!p || p === lastClosed) return;
  lastClosed = p;
  // Closing is global — any client can do it.
  if (p.address) drop(p.address, p.address === activePeer);
}

function onResync(v) {
  if (!v || v === lastResync) return;
  lastResync = v;
  // A gap may have dropped a chat_message, and a restart may have added chat
  // support, so re-reconcile rather than stay latched.
  unsupported = false;
  adopt();
  if (activePeer) loadMessages(activePeer);
}

// A friend's online state changes via friend_updated, with no chat event to
// match (there is no chat_updated), so reconcile linked conversations from the
// friends collection. Non-friend conversations have no such signal.
function onFriends() {
  const friends = store.get("friends") || [];
  for (const conv of convs.values()) {
    if (!conv.friendEcid) continue;
    const f = friends.find((x) => x.ecid === conv.friendEcid);
    if (f) conv.clientEcid = f.client_ecid || 0;
  }
  publishTabsSoon();
}

// --- fallback polling ----------------------------------------------------

function startPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(() => {
    if (data.isLive()) return; // SSE is the normal path; this is the fallback
    adopt();
    if (activePeer) loadMessages(activePeer);
  }, POLL_MS);
}

// --- public API ----------------------------------------------------------

export const chats = {
  // First mount of anything showing chat or the unread badge. Idempotent.
  ensure() {
    if (started) return;
    started = true;
    lastMessage = store.get("chat:message");
    lastClosed = store.get("chat:closed");
    lastResync = store.get("resync");
    offs.push(store.subscribe("chat:message", onMessage));
    offs.push(store.subscribe("chat:closed", onClosed));
    offs.push(store.subscribe("resync", onResync));
    offs.push(store.subscribe("friends", onFriends));
    // Our nick for outbound labels; one read, no poll.
    api.get("preferences")
      .then((p) => { myNick = ((p || {}).general || {}).nickname || ""; publishTabs(); })
      .catch(() => {});
    startPolling();
    adopt();
  },

  // Logout / dead session. This state outlives the shell, so without teardown
  // the poll loop keeps hitting a rejected session.
  reset() {
    for (const off of offs) off();
    offs = [];
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
    if (tabsTimer) { clearTimeout(tabsTimer); tabsTimer = 0; }
    if (logTimer) { clearTimeout(logTimer); logTimer = 0; }
    for (const peer of convs.keys()) store.set("chat:" + peer, []);
    convs.clear();
    activePeer = "";
    started = false;
    unsupported = false;
    myNick = "";
    lastMessage = lastClosed = lastResync = null;
    store.set("chats", { tabs: [], activePeer: "", unread: 0, unsupported: false, myNick: "" });
  },

  setActive,
  open,
  adopt,

  // Opening the Messages page marks the visible conversation read.
  markRead() {
    const conv = convs.get(activePeer);
    if (conv && conv.unread) { conv.unread = 0; publishTabs(); }
  },

  // The 202 echo is the one message object with no timestamp, so stamp one
  // locally and render at once; the SSE frame ~1s later replaces it by id.
  async send(peer, text) {
    const conv = convs.get(peer);
    if (!conv || !text) return;
    try {
      const r = await api.post("chats/" + peer + "/messages", { text });
      conv.known = true; // the core creates the conversation on this call
      const m = (r || {}).message;
      if (m) {
        addMessage(conv, {
          id: m.id, direction: "out", text: m.text,
          sent_at: Math.floor(Date.now() / 1000),
        });
        publishTabs();
        publishLog(peer);
      }
    } catch (e) {
      toast(terr(noteError(e)) || t("messages_error"), "error");
    }
  },

  // Closing is global. A 404 means someone else already closed it.
  async close(peer) {
    const conv = convs.get(peer);
    // One the daemon never had is ours alone to forget.
    if (conv && !conv.known) { drop(peer, false); return; }
    try { await api.del("chats/" + peer); }
    catch (e) {
      if (!e || e.status !== 404) { toast(terr(noteError(e)) || t("messages_error"), "error"); return; }
    }
    drop(peer, false);
  },

  async closeAll() {
    // Clear the selection up front so no close re-activates a conversation
    // about to be closed too.
    activePeer = "";
    await Promise.all(Array.from(convs.keys()).map((p) => chats.close(p)));
    // A failed close (non-404) leaves its tab; keep one selected.
    const first = convs.keys().next().value;
    if (first) setActive(first); else publishTabs();
  },
};
