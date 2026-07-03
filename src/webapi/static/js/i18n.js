// UI strings. Every user-facing string funnels through t(); the dictionaries
// live in i18n/<lang>.json (flat key -> string, the format Weblate edits
// natively, with en.json as the source). Missing keys fall back to English
// (the es dict is overlaid on en), then to the key itself.
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
export const LANGS = ["en", "es"];

function read() {
  try {
    const v = localStorage.getItem(KEY);
    if (LANGS.includes(v)) return v;
  } catch (_) {}
  const nav = (navigator.language || "en").slice(0, 2).toLowerCase();
  return LANGS.includes(nav) ? nav : "en";
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
  try { dict = { ...dict, ...(await loadDict(lang)) }; }
  catch (e) { console.error("i18n: failed to load " + lang + ".json", e); }
}

export function t(key, params) {
  let s = dict[key] !== undefined ? dict[key] : key;
  if (params) for (const k in params) s = s.replaceAll("{" + k + "}", params[k]);
  return s;
}

// Plural-aware t(): resolves "<key>_one" / "<key>_other" and exposes {n}.
const plural = new Intl.PluralRules(lang);
export function tn(key, n, params) {
  return t(key + "_" + plural.select(n), { n, ...params });
}

export function getLang() { return lang; }

export function setLang(code) {
  try { localStorage.setItem(KEY, LANGS.includes(code) ? code : "en"); } catch (_) {}
  location.reload();
}

// en -> es -> en
export function cycleLang() {
  setLang(LANGS[(LANGS.indexOf(lang) + 1) % LANGS.length]);
}
