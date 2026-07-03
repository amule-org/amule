// Application root: auth gate, responsive shell (header + status bar) and
// the hash router. Everything below is preact components rendered with htm.
//
//  - App decides auth state (session cookie) -> Login or Shell.
//  - Shell tracks the current #/<section> hash and renders Header,
//    StatusBar and the lazily-imported view for that section.
//  - Views live in views/<section>.js and default-export a component;
//    a missing/failed module shows a friendly placeholder.

import { api, setUnauthorizedHandler } from "./api.js";
import { data } from "./events.js";
import { html, render, useState, useEffect, useStore } from "./dom.js";
import { toast, Placeholder, confirmDialog } from "./components.js";
import { formatSpeed, formatInt } from "./format.js";
import { t, getLang, cycleLang } from "./i18n.js";
import { Icon } from "./icons.js";
import { getTheme, cycleTheme } from "./theme.js";
import { Login } from "./views/login.js";

// Toolbar pages, ordered like the aMule desktop ("Networks" folds the ED2K
// server list, Kad, and the log panels together).
const ROUTES = [
  { key: "networks", label: t("app_nav_networks") },
  { key: "search", label: t("app_nav_search") },
  { key: "downloads", label: t("app_nav_downloads") },
  { key: "shared", label: t("app_nav_shared") },
  { key: "stats", label: t("app_nav_stats") },
  { key: "preferences", label: t("app_nav_preferences") },
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

  useEffect(() => {
    let alive = true;
    setUnauthorizedHandler(() => { if (alive) setAuth("out"); });
    api.session()
      .then((s) => { if (alive) setAuth(s.role || "guest"); })
      .catch(() => { if (alive) setAuth("out"); });
    return () => { alive = false; setUnauthorizedHandler(null); };
  }, []);

  if (auth === "checking") return html`<${Placeholder} kind="loading">${t("app_loading")}<//>`;
  if (auth === "out") return html`<${Login} onSuccess=${(role) => setAuth(role)} />`;
  return html`<${Shell} role=${auth} onLogout=${() => setAuth("out")} />`;
}

// --- shell --------------------------------------------------------------
function Shell({ role, onLogout }) {
  const [route, setRoute] = useState(currentRoute());

  useEffect(() => {
    document.body.classList.toggle("role-guest", role === "guest");
    data.ensureStatus();
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
  const status = useStore("status");

  const addEd2k = async () => {
    const value = link.trim();
    if (!value) return;
    try {
      await api.post("downloads", { ed2k_link: value });
      setLink("");
      toast(t("app_toast_link_added"), "success");
      if (currentRoute() === "downloads") data.refresh("downloads");
    } catch (e) {
      toast(e.message || t("app_error"), "error");
    }
  };

  const doLogout = async () => {
    try { await api.logout(); } catch (_) {}
    location.hash = "";
    onLogout();
  };

  return html`
    <header class="app-toolbar">
      <button class="btn btn-ghost nav-toggle" aria-expanded=${menuOpen}
              aria-controls="main-nav" aria-label=${t("app_menu")}
              onClick=${() => setMenuOpen(!menuOpen)}>
        <${Icon} name="menu" size=${20} />
      </button>
      <div class="brand">
        <img class="brand-logo" src="img/logo.png" alt="aMule" />
      </div>
      <span class="route-title">${t("app_nav_" + route)}</span>
      <${ConnectButton} status=${status} />
      ${menuOpen ? html`<div class="nav-backdrop" onClick=${() => setMenuOpen(false)} />` : null}
      <nav class=${"nav" + (menuOpen ? " open" : "")} id="main-nav">
        <${ConnectButton} status=${status} />
        ${ROUTES.map((r) => html`
          <a class=${"tool-btn" + (r.key === route ? " active" : "")}
             href=${"#/" + r.key} data-route=${r.key} title=${r.label}
             onClick=${() => setMenuOpen(false)}>
            <${Icon} name=${r.key} size=${20} />
            <span class="tool-label">${r.label}</span>
          </a>`)}
        <div class="nav-tools">
          <${LangButton} />
          <${ThemeButton} />
          <button class="btn btn-ghost" title=${t("app_logout")} onClick=${doLogout}>
            <${Icon} name="logout" /><span class="sr-only">${t("app_logout")}</span>
          </button>
        </div>
      </nav>
      <div class="header-tools">
        <form class="ed2k-add admin-only" onSubmit=${(e) => { e.preventDefault(); addEd2k(); }}>
          <input class="input ed2k-input" type="text" placeholder="ed2k://|file|…"
                 aria-label=${t("app_add_ed2k_link")} value=${link}
                 onInput=${(e) => setLink(e.target.value)} />
          <button class="btn admin-only" type="submit">${t("app_add")}</button>
        </form>
        <${LangButton} />
        <${ThemeButton} />
        <button class="btn btn-ghost" title=${t("app_logout")} onClick=${doLogout}>
          <${Icon} name="logout" /><span class="sr-only">${t("app_logout")}</span>
        </button>
      </div>
    </header>`;
}

// aMule-style connection button: a coloured plug (red = disconnected, amber =
// connecting, green = connected) that toggles both networks.
function ConnectButton({ status }) {
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
    } catch (e) { toast(e.message || t("app_error"), "error"); }
  };

  return html`
    <button class=${"tool-btn conn-btn admin-only " + cls} title=${label} onClick=${toggle}>
      <${Icon} name="connect" size=${20} />
      <span class="tool-label">${label}</span>
    </button>`;
}

// Cycles en -> es; the page reloads so module-level t() calls re-resolve.
function LangButton() {
  const lang = getLang();
  return html`
    <button class="btn btn-ghost" title=${t("app_language") + ": " + lang.toUpperCase()}
            onClick=${cycleLang}>
      <${Icon} name="language" />
      <span class="lang-code">${lang.toUpperCase()}</span>
      <span class="sr-only">${t("app_language")}</span>
    </button>`;
}

// Cycles system -> light -> dark and shows the matching icon.
function ThemeButton() {
  const [pref, setPref] = useState(getTheme());
  const labels = { system: t("app_theme_system"), light: t("app_theme_light"), dark: t("app_theme_dark") };
  return html`
    <button class="btn btn-ghost" title=${t("app_theme") + ": " + labels[pref]}
            onClick=${() => setPref(cycleTheme())}>
      <${Icon} name=${"theme-" + pref} />
      <span class="sr-only">${labels[pref]}</span>
    </button>`;
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
  const hasNet = kNet.users != null || kNet.files != null
              || eNet.users != null || eNet.files != null;

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
          ${i > 0 ? html`<span class="status-sep" aria-hidden="true"></span>` : null}
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
      e2Cls = ed2k.low_id ? "low" : "ok";
      e2Text = (ed2k.server_name || ed2k.server_ip || t("app_connected")) +
        " · " + (ed2k.low_id ? t("app_low_id") : t("app_high_id"));
    } else if (ed2k.state === "connecting") {
      e2Cls = "warn"; e2Text = t("app_connecting");
    }
  }

  const kad = status && status.kad;
  let kText = t("app_disconnected"), kCls = "off";
  if (kad && kad.state === "connected") {
    kCls = kad.firewalled ? "warn" : "ok";
    kText = t("app_connected") + (kad.firewalled ? " · " + t("app_firewalled") : "");
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
  const eHas = e && (e.users != null || e.files != null);
  const kHas = k && (k.users != null || k.files != null);
  if (!eHas && !kHas) return null;

  const both = eHas && kHas;
  const net = eHas ? e : k;
  const usersVal = both
    ? html`<b>E:</b> ${formatInt(e.users)} <b>K:</b> ${formatInt(k.users)}`
    : html`${formatInt(net.users)}`;
  const filesVal = both
    ? html`<b>E:</b> ${formatInt(e.files)} <b>K:</b> ${formatInt(k.files)}`
    : html`${formatInt(net.files)}`;

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
      <span class="speed dl"><${Icon} name="down" /> ${formatSpeed(sp.download_bps)}</span>
      <span class="speed ul"><${Icon} name="up" /> ${formatSpeed(sp.upload_bps)}</span>
    </span>`;
}

// --- router -------------------------------------------------------------
function RouteView({ route, role }) {
  const [View, setView] = useState(null);
  const [missing, setMissing] = useState(false);

  useEffect(() => {
    let alive = true;
    setView(null);
    setMissing(false);
    import("./views/" + route + ".js")
      .then((mod) => {
        if (!alive) return;
        const Component = mod.default || mod.View;
        if (Component) setView(() => Component); else setMissing(true);
      })
      .catch(() => { if (alive) setMissing(true); });
    return () => { alive = false; };
  }, [route]);

  if (missing) return html`<${Placeholder} kind="info">${t("app_coming_soon")}<//>`;
  if (!View) return html`<${Placeholder} kind="loading">${t("app_loading")}<//>`;
  // key=route forces a fresh mount (and cleanup) when switching sections.
  return html`<${View} key=${route} role=${role} isGuest=${role === "guest"} />`;
}

render(html`<${App} />`, document.getElementById("app"));
