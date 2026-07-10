// About view: application info from GET /api/v0/version (versions + daemon
// update-availability), plus static project links and license. See app.js
// VersionBanner for the mismatch/update banners driven by the same endpoint.

import { api } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder, toast } from "../components.js";
import { t, terr } from "../i18n.js";
import { Icon } from "../icons.js";

const LINKS = [
  { key: "about_website", href: "https://www.amule.org", text: "www.amule.org" },
  { key: "about_source", href: "https://github.com/amule-project/amule", text: "github.com/amule-project/amule" },
  { key: "about_license", href: "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html", text: "GPLv2" },
];

export default function About() {
  const [info, setInfo] = useState(null);
  const [err, setErr] = useState("");
  const [checking, setChecking] = useState(false);

  const load = () => api.get("version")
    .then((v) => setInfo(v))
    .catch((e) => setErr(terr(e) || t("about_load_error")));

  useEffect(() => {
    let alive = true;
    api.get("version")
      .then((v) => { if (alive) setInfo(v); })
      .catch((e) => { if (alive) setErr(terr(e) || t("about_load_error")); });
    return () => { alive = false; };
  }, []);

  // The daemon runs the check asynchronously; re-fetch /version shortly after
  // it accepts (202) so the panel reflects the fresh result.
  const checkNow = async () => {
    setChecking(true);
    try {
      await api.post("version/check");
      toast(t("about_update_check_started"), "success");
      setTimeout(() => load().finally(() => setChecking(false)), 2000);
    } catch (e) {
      toast(terr(e) || t("about_load_error"), "error");
      setChecking(false);
    }
  };

  if (err) return html`<${Placeholder} kind="error">${err}<//>`;
  if (!info) return html`<${Placeholder} kind="loading">${t("app_loading")}<//>`;

  const daemon = info.daemon_version || t("about_daemon_disconnected");
  const u = info.update || {};

  return html`
    <div class="card">
      <h3>aMule Web</h3>
      <div class="form-grid form-grid-2">
        <div class="field"><label>${t("about_amuleapi_version")}</label><span>${info.amule_version}</span></div>
        <div class="field"><label>${t("about_daemon_version")}</label><span>${daemon}</span></div>
        <div class="field"><label>${t("about_api_version")}</label><span>${info.api_version}</span></div>
      </div>
    </div>
    ${u.check_enabled ? html`
    <div class="card">
      <h3>${t("about_update_section")}</h3>
      <div class="form-grid form-grid-2">
        <div class="field">
          <label>${t("about_update_status")}</label>
          <span class=${"update-status" + (u.update_available ? " available" : "")}>
            ${!u.checked ? t("about_update_not_checked")
              : u.update_available ? t("about_update_available") : t("about_update_uptodate")}
          </span>
        </div>
        ${u.checked && u.latest_version ? html`
        <div class="field"><label>${t("about_update_latest")}</label><span>${u.latest_version}</span></div>` : null}
        ${u.last_checked ? html`
        <div class="field">
          <label>${t("about_update_last_checked")}</label>
          <span>${new Date(u.last_checked * 1000).toLocaleString()}</span>
        </div>` : null}
      </div>
      <div class="toolbar update-actions">
        <button class="btn admin-only" disabled=${checking} onClick=${checkNow}>
          <${Icon} name="polling" size=${16} />
          ${checking ? t("about_update_checking") : t("about_update_check_now")}
        </button>
      </div>
    </div>` : null}
    <div class="card">
      <div class="form-grid form-grid-2">
        ${LINKS.map((l) => html`
          <div class="field">
            <label>${t(l.key)}</label>
            <a href=${l.href} target="_blank" rel="noopener">${l.text}</a>
          </div>`)}
      </div>
    </div>`;
}
