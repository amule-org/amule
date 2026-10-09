// UI strings. Every user-facing string funnels through t(); the dictionaries
// live in i18n/<lang>.json (flat key -> string, the format Weblate edits
// natively, with en.json as the source). Missing or blank keys fall back to
// English (the locale dict is overlaid on en), then to the key itself. Keys
// must not contain "." or "[]": Weblate's i18next format nests them.
//
// Key naming: every key is prefixed with the section it belongs to
// (app_, login_, networks_, search_, downloads_, shared_, stats_, prefs_,
// and common_ for the genuinely shared modules — api.js, components.js,
// charts.js). Sub-panels nest under their parent section's prefix
// (networks_kad_*, networks_server_*, downloads_peer_*, downloads_cat_*, …).
// Strings are duplicated per section rather than shared, so a translator can
// give the same English word a different translation in different sections.
//
// Language: localStorage override -> browser language -> English. Changing
// language reloads the page: several modules resolve t() at import time
// (ROUTES in app.js, form-field tables, graph titles), so a live swap could
// not retranslate them. ponytail: reload is 1 line; a live switch would need
// every module-level t() moved into render scope.

const KEY = "amule.lang";
// Bare codes before their regional variants (pt before pt-BR): read() takes
// the first same-language match.
export const LANGS = ["en", "es", "it", "ru"];

function read() {
  try {
    const v = localStorage.getItem(KEY);
    if (LANGS.includes(v)) return v;
  } catch (_) {}
  // Browser languages in preference order: exact code (any case), then same
  // language and script (es-MX -> es, zh-TW -> zh-Hant, not zh-Hans).
  const max = (c) => { try { return new Intl.Locale(c).maximize(); } catch (_) { return null; } };
  for (const nav of navigator.languages?.length ? navigator.languages : [navigator.language || "en"]) {
    const exact = LANGS.find((c) => c.toLowerCase() === nav.toLowerCase());
    if (exact) return exact;
    const want = max(nav);
    const near = want && LANGS.find((c) => {
      const l = max(c);
      return l && l.language === want.language && l.script === want.script;
    });
    if (near) return near;
  }
  return "en";
}

const lang = read();
document.documentElement.lang = lang;

async function loadDict(code) {
  const res = await fetch("i18n/" + code + ".json");
  if (!res.ok) throw new Error("HTTP " + res.status);
  return res.json();
}

// Top-level await: importers (app.js, views) implicitly wait for the
// dictionaries before their module bodies run.
let dict = {};
try { dict = await loadDict("en"); }
catch (e) { console.error("i18n: failed to load en.json", e); }
if (lang !== "en") {
  // Weblate writes an untranslated plural form as "", so skip blanks.
  try { for (const [k, v] of Object.entries(await loadDict(lang))) if (v !== "") dict[k] = v; }
  catch (e) { console.error("i18n: failed to load " + lang + ".json", e); }
}

export function t(key, params) {
  let s = dict[key] !== undefined ? dict[key] : key;
  if (params) for (const k in params) s = s.replaceAll("{" + k + "}", params[k]);
  return s;
}

// Plural-aware t(): resolves "<key>_<CLDR form>" and exposes {n}. en.json only
// has _one/_other, so an untranslated _few/_many falls back to _other.
const plural = new Intl.PluralRules(lang);
export function tn(key, n, params) {
  const k = key + "_" + plural.select(n);
  return t(dict[k] !== undefined ? k : key + "_other", { n, ...params });
}

// ApiError -> localized text. The API contract is English text / C-locale
// numbers (see docs/api/REFERENCE.md), so error strings arrive in English and
// localization is the client's job. Priority: exact known message (the
// wxTRANSLATE strings amuled relays verbatim, keyed without "." and "[]")
// -> generic per error code with the raw detail as {message} -> the raw message.
export function terr(e) {
  if (!e) return "";
  const exact = dict["common_apierr_" + String(e.message).replace(/[.[\]]/g, "")];
  if (exact !== undefined) return exact;
  if (e.code && dict["common_err_" + e.code] !== undefined)
    return t("common_err_" + e.code, { message: e.message || "" });
  return e.message || "";
}

export function getLang() { return lang; }

export function setLang(code) {
  try { localStorage.setItem(KEY, LANGS.includes(code) ? code : "en"); } catch (_) {}
  location.reload();
}

// Language autonym ("English", "español"...), capitalized. Native, no data.
export function langName(code) {
  try {
    const n = new Intl.DisplayNames([code], { type: "language" }).of(code);
    return n ? n[0].toUpperCase() + n.slice(1) : code.toUpperCase();
  } catch (_) { return code.toUpperCase(); }
}
