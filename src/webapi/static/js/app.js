// Application root: auth gate, responsive shell (header + status bar) and
// the hash router. Everything below is preact components rendered with htm.
//
//  - App decides auth state (session cookie) -> Login or Shell.
//  - Shell tracks the current #/<section> hash and renders Header,
//    StatusBar and the lazily-imported view for that section.
//  - Views live in views/<section>.js and default-export a component;
//    a missing/failed module shows a friendly placeholder.

import { api, bulkFailures, setUnauthorizedHandler } from "./api.js";
import { data } from "./events.js";
import { searches } from "./searches.js";
import { chats } from "./chats.js";
import { html, render, useState, useEffect, useStore } from "./dom.js";
import { toast, Placeholder } from "./components.js";
import { formatSpeed, formatInt } from "./format.js";
import { t, terr } from "./i18n.js";
import { Icon } from "./icons.js";
import { Login } from "./views/login.js";

// Toolbar pages, ordered like the aMule desktop ("Networks" folds the ED2K
// server list, Kad, and the log panels together).
const ROUTES = [
  { key: "networks", label: t("app_nav_networks") },
  { key: "search", label: t("app_nav_search") },
  { key: "downloads", label: t("app_nav_downloads") },
  { key: "shared", label: t("app_nav_shared") },
  { key: "clients", label: t("app_nav_clients") },
  { key: "messages", label: t("app_nav_messages") },
  { key: "stats", label: t("app_nav_stats") },
  { key: "preferences", label: t("app_nav_preferences") },
  { key: "about", label: t("app_nav_about") },
];
// Routable but not shown in the toolbar (reached from in-page links).
const HIDDEN_ROUTES = [];
const DEFAULT_ROUTE = "downloads";

function currentRoute() {
  const h = (location.hash || "").replace(/^#\/?/, "");
  const key = h.split("?")[0];
  const known = ROUTES.some((r) => r.key === key) || HIDDEN_ROUTES.includes(key);
  return known ? key : DEFAULT_ROUTE;
}

// --- root ---------------------------------------------------------------
function App() {
  // "checking" while we probe the session; "out" when logged out; otherwise
  // the role string ("admin" | "guest").
  const [auth, setAuth] = useState("checking");
  // Why we landed back on the login screen, when it wasn't the user's doing.
  const [notice, setNotice] = useState("");

  useEffect(() => {
    let alive = true;
    setUnauthorizedHandler((reason) => {
      // The session is gone — an amuleapi restart invalidates the cookie.
      // Stop the live-data layer before anything else: its poll timer and SSE
      // stream are module-level and outlive the shell unmounting below, and
      // each retry is another failed-auth strike the server counts towards
      // banning this IP.
      data.stop();
      searches.reset(); // its poll timer and tabs are module-level too
      chats.reset();
      if (!alive) return;
      setNotice(reason === "rate_limited" ? t("login_err_rate_limited") : t("login_session_expired"));
      setAuth("out");
    });
    api.session()
      .then((s) => { if (alive) setAuth(s.role || "guest"); })
      .catch(() => { if (alive) setAuth("out"); });
    return () => { alive = false; setUnauthorizedHandler(null); };
  }, []);

  const onLoggedIn = (role) => { setNotice(""); setAuth(role); };

  if (auth === "checking") return html`<${Placeholder} kind="loading">${t("app_loading")}<//>`;
  if (auth === "out") return html`<${Login} notice=${notice} onSuccess=${onLoggedIn} />`;
  return html`<${Shell} role=${auth} onLogout=${() => setAuth("out")} />`;
}

// --- shell --------------------------------------------------------------
function Shell({ role, onLogout }) {
  const [route, setRoute] = useState(currentRoute());

  useEffect(() => {
    document.body.classList.toggle("role-guest", role === "guest");
    data.ensureStatus();
    // Shell-wide, not per-view, so the unread badge lights up from any section.
    chats.ensure();
    if (!location.hash) location.hash = "#/" + DEFAULT_ROUTE;
    const onHash = () => setRoute(currentRoute());
    window.addEventListener("hashchange", onHash);
    setRoute(currentRoute());
    return () => window.removeEventListener("hashchange", onHash);
  }, [role]);

  return html`
    <${Toolbar} route=${route} onLogout=${onLogout} />
    <main class="view" id="view">
      <${RouteView} route=${route} role=${role} />
    </main>
    <${StatusBar} />`;
}

function Toolbar({ route, onLogout }) {
  const [link, setLink] = useState("");
  const [menuOpen, setMenuOpen] = useState(false);
  const unread = (useStore("chats") || {}).unread || 0;
  // One-shot /version check. A red dot on About (and, on mobile, on the
  // hamburger) flags a version mismatch or an available update; the message
  // itself lives in the About view. daemon_version is empty without EC.
  const [aboutAlert, setAboutAlert] = useState(false);

  useEffect(() => {
    let alive = true;
    api.get("version")
      .then((v) => {
        if (!alive) return;
        const mismatch = v.daemon_version && v.amuleapi_version !== v.daemon_version;
        const update = v.update && v.update.check_enabled && v.update.available === true;
        if (mismatch || update) setAboutAlert(true);
      })
      .catch(() => {});
    return () => { alive = false; };
  }, []);

  const addEd2k = async () => {
    const value = link.trim();
    if (!value) return;

    const links = [];
    const regex = /(ed2k:\/\/\|file\|.+?\|\/|magnet:\?.+?(?=\s*(?:ed2k:\/\/|magnet:|$)))/gi;
    let match;
    while ((match = regex.exec(value)) !== null) {
      links.push(match[1].trim());
    }
    // `links` is the only body form the endpoint takes. Where the regex found
    // nothing, send the raw input as a one-item array anyway so the daemon
    // answers with its own verdict on the string the user typed.
    const payload = { links: links.length > 0 ? links : [value] };

    try {
      // POST /downloads returns the bulk `results` envelope keyed by link; on a
      // 207 partial some links were rejected, so report them and keep the input.
      const failed = bulkFailures(await api.post("downloads", payload));
      if (failed.length) {
        toast(t("common_bulk_partial", { failed: failed.length,
                total: payload.links.length,
                message: terr(failed[0].error) }), "warn");
      } else {
        setLink("");
        toast(t("app_toast_link_added"), "success");
      }
      if (currentRoute() === "downloads") data.refresh("downloads");
    } catch (e) {
      toast(terr(e) || t("app_error"), "error");
    }
  };

  const doLogout = async () => {
    try { await api.logout(); } catch (_) {}
    data.stop(); // the SSE stream and poll timer outlive this component
    searches.reset(); // ...and so do the search tabs and their poll timer
    chats.reset();
    location.hash = "";
    onLogout();
  };

  return html`
    <header class="app-toolbar">
      <button class="btn btn-ghost nav-toggle" aria-expanded=${menuOpen}
              aria-controls="main-nav" aria-label=${unread || aboutAlert ? t("app_menu_alert") : t("app_menu")}
              onClick=${() => setMenuOpen(!menuOpen)}>
        <${Icon} name="menu" size=${20} />
        ${unread || aboutAlert ? html`<span class="nav-toggle-dot"></span>` : null}
      </button>
      <div class="brand">
        <img class="brand-logo" src="img/logo.png" alt="aMule" />
      </div>
      <span class="route-title">${t("app_nav_" + route)}</span>
      ${menuOpen ? html`<div class="nav-backdrop" onClick=${() => setMenuOpen(false)} />` : null}
      <nav class=${"nav" + (menuOpen ? " open" : "")} id="main-nav">
        ${ROUTES.map((r) => html`
          <a class=${"tool-btn" + (r.key === route ? " active" : "")}
             href=${"#/" + r.key} data-route=${r.key} title=${r.label}
             onClick=${() => setMenuOpen(false)}>
            <${Icon} name=${r.key} size=${20} />
            <span class="tool-label">${r.label}</span>
            ${r.key === "messages" && unread
              ? html`<span class="tool-badge" title=${t("messages_unread_tip", { n: unread })}>${unread}</span>`
              : null}
            ${r.key === "about" && aboutAlert
              ? html`<span class="tool-dot" title=${t("about_alert_tip")}></span>`
              : null}
          </a>`)}
        <div class="nav-tools">
          <form class="ed2k-add admin-only" onSubmit=${(e) => { e.preventDefault(); addEd2k(); }}>
            <input class="input ed2k-input" type="text" name="ed2k_link" placeholder="ed2k://|file|…"
                   aria-label=${t("app_add_ed2k_link")} value=${link}
                   onInput=${(e) => setLink(e.target.value)} />
            <button class="btn admin-only" type="submit">${t("app_add")}</button>
          </form>
          <button class="btn btn-ghost" title=${t("app_logout")} onClick=${doLogout}>
            <${Icon} name="logout" /><span class="sr-only">${t("app_logout")}</span>
          </button>
        </div>
      </nav>
      <div class="header-tools">
        <form class="ed2k-add admin-only" onSubmit=${(e) => { e.preventDefault(); addEd2k(); }}>
          <input class="input ed2k-input" type="text" name="ed2k_link" placeholder="ed2k://|file|…"
                 aria-label=${t("app_add_ed2k_link")} value=${link}
                 onInput=${(e) => setLink(e.target.value)} />
          <button class="btn admin-only" type="submit">${t("app_add")}</button>
        </form>
        <button class="btn btn-ghost" title=${t("app_logout")} onClick=${doLogout}>
          <${Icon} name="logout" /><span class="sr-only">${t("app_logout")}</span>
        </button>
      </div>
    </header>`;
}

// --- status bar ---------------------------------------------------------
// The live/polling badge sits on the LEFT; everything else is right-aligned in
// amulegui's footer order (Users | Speed | Connection), groups split by
// vertical separators. Status is text-only (we have no status icons).
function StatusBar() {
  const status = useStore("status");
  const live = useStore("live");
  const polling = useStore("polling");

  const badge = polling
    ? html`<span class="status-chip warn" title=${t("app_offline_polling")}>${t("app_polling")}</span>`
    : (live ? html`<span class="status-chip ok">${t("app_live")}</span>` : null);

  const kNet = (status && status.kad && status.kad.network) || {};
  const eNet = (status && status.ed2k && status.ed2k.network) || {};
  const hasNet = kNet.user_count != null || kNet.file_count != null
              || eNet.user_count != null || eNet.file_count != null;

  const groups = [
    hasNet ? html`<${NetworkInfo} status=${status} />` : null,
    html`<${Speeds} status=${status} />`,
    html`<${ConnectionStatus} status=${status} />`,
  ].filter(Boolean);

  return html`
    <div class="statusbar" id="statusbar">
      <span class="status-left">${badge}</span>
      <div class="status-right">
        ${groups.map((g, i) => html`
          ${i > 0 ? html`<span class="vsep" aria-hidden="true"></span>` : null}
          ${g}`)}
      </div>
    </div>`;
}

// Combined eD2k + Kad status, mirroring amulegui's "eD2k: … | Kad: …" label:
// plain colored text (no badges), joined by a divider.
function ConnectionStatus({ status }) {
  const ed2k = status && status.ed2k;
  let e2Text = t("app_not_connected"), e2Cls = "off";
  if (ed2k) {
    if (ed2k.state === "connected") {
      e2Cls = ed2k.high_id ? "ok" : "low";
      const e2Server = ed2k.server_name ||
        (ed2k.server_ip ? ed2k.server_ip + ":" + ed2k.server_port : "");
      e2Text = (e2Server || t("app_connected")) +
        " · " + (ed2k.high_id ? t("app_high_id") : t("app_low_id"));
    } else if (ed2k.state === "connecting") {
      e2Cls = "warn"; e2Text = t("app_connecting");
    }
  }

  const kad = status && status.kad;
  let kText = t("app_not_connected"), kCls = "off";
  if (kad && kad.state === "connected") {
    kCls = kad.firewalled_tcp ? "warn" : "ok";
    kText = t("app_connected") + (kad.firewalled_tcp ? " · " + t("app_firewalled") : "");
  }

  return html`
    <span class="status-item conn">
      <b>${t("app_ed2k")}: </b><span class=${"conn-state " + e2Cls}>${e2Text}</span>
      <span class="conn-div" aria-hidden="true">|</span>
      <b>${t("app_kad")}: </b><span class=${"conn-state " + kCls}>${kText}</span>
    </span>`;
}

// Network users/files, like amulegui's userLabel (CamuleApp::ShowUserCount):
// both networks connected -> "Users: E: x K: y | Files: E: x K: y", a single
// network -> "Users: x | Files: x". Selection is by presence of each network's
// totals (the status object carries no enabled-pref flag).
function NetworkInfo({ status }) {
  const e = (status && status.ed2k && status.ed2k.network) || null;
  const k = (status && status.kad && status.kad.network) || null;
  const eHas = e && (e.user_count != null || e.file_count != null);
  const kHas = k && (k.user_count != null || k.file_count != null);
  if (!eHas && !kHas) return null;

  const both = eHas && kHas;
  const net = eHas ? e : k;
  const usersVal = both
    ? html`<b>E:</b> ${formatInt(e.user_count)} <b>K:</b> ${formatInt(k.user_count)}`
    : html`${formatInt(net.user_count)}`;
  const filesVal = both
    ? html`<b>E:</b> ${formatInt(e.file_count)} <b>K:</b> ${formatInt(k.file_count)}`
    : html`${formatInt(net.file_count)}`;

  return html`
    <span class="status-item status-extra">
      <b>${t("app_users")}: </b>${usersVal}
      <span class="conn-div" aria-hidden="true">|</span>
      <b>${t("app_files")}: </b>${filesVal}
    </span>`;
}

function Speeds({ status }) {
  const sp = (status && status.speeds) || {};
  return html`
    <span class="status-item speeds">
      <span class="speed dl"><${Icon} name="down" /> ${formatSpeed(sp.download_speed_bytes_per_second)}</span>
      <span class="speed ul"><${Icon} name="up" /> ${formatSpeed(sp.upload_speed_bytes_per_second)}</span>
    </span>`;
}

// --- router -------------------------------------------------------------
function RouteView({ route, role }) {
  const [View, setView] = useState(null);
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    let alive = true;
    setView(null);
    setFailed(false);
    import("./views/" + route + ".js")
      .then((mod) => {
        if (!alive) return;
        if (mod.default) setView(() => mod.default);
        else { console.error("View '" + route + "' has no default export"); setFailed(true); }
      })
      .catch((e) => {
        if (!alive) return;
        console.error("Failed to load view '" + route + "':", e);
        setFailed(true);
      });
    return () => { alive = false; };
  }, [route]);

  if (failed) return html`<${Placeholder} kind="error">${t("app_view_error")}<//>`;
  if (!View) return html`<${Placeholder} kind="loading">${t("app_loading")}<//>`;
  // key=route forces a fresh mount (and cleanup) when switching sections.
  return html`<${View} key=${route} role=${role} isGuest=${role === "guest"} />`;
}

render(html`<${App} />`, document.getElementById("app"));
