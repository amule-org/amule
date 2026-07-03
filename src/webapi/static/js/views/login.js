// Login screen. Password-only (matches aMule Web / the amuleapi auth
// model). On success the session cookie is set by the server and we hand
// control back to the shell via onSuccess(role).

import { api, ApiError } from "../api.js";
import { html, useState, useRef, useEffect } from "../dom.js";
import { t } from "../i18n.js";

// Map the backend's /auth/login error codes (the {error:{code}} envelope) to
// our own translated strings — the raw server message is English-only. Unknown
// codes fall back to the generic login_failed.
const LOGIN_ERROR_KEYS = {
  invalid_credentials: "login_err_invalid_credentials",
  rate_limited: "login_err_rate_limited",
  login_disabled: "login_err_login_disabled",
};

export function Login({ onSuccess }) {
  const [password, setPassword] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const input = useRef(null);

  useEffect(() => { if (input.current) input.current.focus(); }, []);

  const submit = async (e) => {
    e.preventDefault();
    setError("");
    setBusy(true);
    try {
      const res = await api.login(password);
      onSuccess(res.role || "admin");
    } catch (err) {
      const key = (err instanceof ApiError && LOGIN_ERROR_KEYS[err.code]) || "login_failed";
      setError(t(key));
      setBusy(false);
      if (input.current) { input.current.focus(); input.current.select(); }
    }
  };

  return html`
    <div class="login">
      <form class="login-form" onSubmit=${submit}>
        <img class="login-logo" src="img/logo.png" alt="aMule" />
        <h1 class="login-title">${t("login_title")}</h1>
        <label class="sr-only" for="login-password">${t("login_password")}</label>
        <input ref=${input} type="password" id="login-password" class="input"
               autocomplete="current-password" required placeholder=${t("login_password")}
               value=${password} onInput=${(e) => setPassword(e.target.value)} />
        <button type="submit" class="btn btn-primary" disabled=${busy}>
          ${busy ? t("login_signing_in") : t("login_sign_in")}
        </button>
        ${error ? html`<p class="login-error" role="alert">${error}</p>` : null}
      </form>
    </div>`;
}
