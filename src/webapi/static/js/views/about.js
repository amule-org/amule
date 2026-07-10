// About view: application info from GET /api/v0/version (versions), plus static
// project links and license. See app.js VersionBanner for the mismatch warning.

import { api } from "../api.js";
import { html, useState, useEffect } from "../dom.js";
import { Placeholder } from "../components.js";
import { t, terr } from "../i18n.js";

const LINKS = [
  { key: "about_website", href: "https://www.amule.org", text: "www.amule.org" },
  { key: "about_source", href: "https://github.com/amule-project/amule", text: "github.com/amule-project/amule" },
  { key: "about_license", href: "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html", text: "GPLv2" },
];

export default function About() {
  const [info, setInfo] = useState(null);
  const [err, setErr] = useState("");

  useEffect(() => {
    let alive = true;
    api.get("version")
      .then((v) => { if (alive) setInfo(v); })
      .catch((e) => { if (alive) setErr(terr(e) || t("about_load_error")); });
    return () => { alive = false; };
  }, []);

  if (err) return html`<${Placeholder} kind="error">${err}<//>`;
  if (!info) return html`<${Placeholder} kind="loading">${t("app_loading")}<//>`;

  const daemon = info.daemon_version || t("about_daemon_disconnected");

  return html`
    <div class="card">
      <h3>aMule Web</h3>
      <div class="form-grid form-grid-2">
        <div class="field"><label>${t("about_amuleapi_version")}</label><span>${info.amule_version}</span></div>
        <div class="field"><label>${t("about_daemon_version")}</label><span>${daemon}</span></div>
        <div class="field"><label>${t("about_api_version")}</label><span>${info.api_version}</span></div>
      </div>
    </div>
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
