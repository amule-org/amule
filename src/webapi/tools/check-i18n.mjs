// Dictionary consistency check: every locale key must exist in en.json (the
// source) and keep its {placeholders}. Missing or blank ("") keys are fine:
// Weblate drops untranslated/unapproved strings and the UI falls back to
// English. Plural keys may carry any CLDR form (Weblate adds _few/_many for
// e.g. Polish), checked against the English _other; plural forms may drop {n}
// ("Un comentario", Arabic dual). en.json keys must suit
// Weblate's i18next format: no "." or "[]" (nested), and a CLDR suffix only on
// a real _one/_other pair (grouped as a plural). Locale codes (file names and
// LANGS in i18n.js) must be canonical BCP 47, as Intl.PluralRules needs.
// Also checks that every preferences field renders a real label, since those
// keys are derived (prefs_field_<category>_<key>) rather than written out --
// a renamed field with no matching dictionary entry would otherwise render
// the raw key with no error anywhere.
// With I18N_BASE set to a commit, also fails on a locale key added since that
// commit whose value is the English text. New strings go into en.json only:
// Weblate reads any non-empty value as a translation, so an English copy lands
// in that language as if it were translated. A value the locale already had
// in the base is allowed, so a renamed key can carry a kept-English word
// ("Upload") along. CI sets I18N_BASE on pull requests except Weblate's own,
// where a value equal to English is a translator's choice.
// Run: node src/webapi/tools/check-i18n.mjs
import { execFileSync } from "node:child_process";
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import assert from "node:assert";

const here = dirname(new URL(import.meta.url).pathname);
const dir = join(here, "..", "static", "i18n");
const placeholders = (s) => (s.match(/\{[a-z_]+\}/g) || []).sort().join(",");

// JSON.parse silently keeps the last value for a repeated key, so a duplicate
// is invisible to a parsed comparison. The catalogs are flat, one "key": per
// line, so the raw keys are reliable to scan.
const load = (f) => {
  const raw = readFileSync(join(dir, f), "utf8");
  const seen = new Set(), dupes = new Set();
  for (const [, k] of raw.matchAll(/^\s*"((?:[^"\\]|\\.)*)"\s*:/gm))
    (seen.has(k) ? dupes : seen).add(k);
  assert.deepStrictEqual([...dupes], [], f + ": duplicate keys");
  return JSON.parse(raw);
};

const PLURAL = /^(.*)_(zero|one|two|few|many|other)$/;
const en = load("en.json");
for (const k of Object.keys(en)) {
  assert.ok(!/[.[\]]/.test(k), "en.json: \"" + k + "\" contains . or []");
  const m = k.match(PLURAL);
  if (m) assert.ok(m[1] + "_one" in en && m[1] + "_other" in en,
    "en.json: \"" + k + "\" has a plural suffix but no _one/_other pair");
}
const bcp47 = (code, where) => {
  let canon;
  try { canon = Intl.getCanonicalLocales(code)[0]; } catch (_) {}
  assert.strictEqual(code, canon, where + ": \"" + code + "\" is not a canonical BCP 47 code");
};
const files = readdirSync(dir).filter((f) => f.endsWith(".json") && f !== "en.json");
const i18nSrc = readFileSync(join(here, "..", "static", "js", "i18n.js"), "utf8");
const langsSrc = i18nSrc.match(/export\s+const\s+LANGS\s*=\s*(\[[^\]]*\])/);
assert.ok(langsSrc, "i18n.js: cannot find `export const LANGS = [...]`");
const LANGS = new Function("return " + langsSrc[1])();
for (const code of LANGS) {
  bcp47(code, "i18n.js LANGS");
  assert.ok(code === "en" || files.includes(code + ".json"), "i18n.js LANGS: no " + code + ".json");
}

const base = process.env.I18N_BASE;
const git = (...args) => execFileSync("git", args, { cwd: dir, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
// Asserted up front: a base that was not fetched would otherwise read as
// "every locale is new" and flag every value that happens to match English.
if (base) {
  try { git("cat-file", "-e", base + "^{commit}"); } catch (_) { assert.fail("I18N_BASE " + base + " is not a fetched commit"); }
}
const copied = [];

for (const file of files) {
  const code = file.slice(0, -5);
  bcp47(code, file);
  const dict = load(file);
  let old = {};
  if (base) {
    try { old = JSON.parse(git("show", base + ":./" + file)); } catch (_) {} // a locale new since base
  }
  const oldValues = new Set(Object.values(old));
  const extra = [];
  for (const [k, v] of Object.entries(dict)) {
    assert.strictEqual(typeof v, "string", file + ": \"" + k + "\" is not a string");
    if (v === "") continue;
    const m = k.match(PLURAL);
    const src = k in en ? k : m && m[1] + "_other" in en ? m[1] + "_other" : null;
    if (src === null) { extra.push(k); continue; }
    if (base && !old[k] && v === en[src] && !oldValues.has(v)) copied.push(file + ": " + k);
    const ph = (s) => m ? placeholders(s).split(",").filter((p) => p !== "{n}").join(",") : placeholders(s);
    assert.strictEqual(ph(v), ph(en[src]),
      file + ": placeholder mismatch in \"" + k + "\"");
  }
  assert.deepStrictEqual(extra, [], file + ": stale keys not in en.json");
  // Count each plural by the forms this language uses (ja: _other only,
  // pl: _one/_few/_many/_other), not by en.json's _one/_other.
  const forms = new Intl.PluralRules(code).resolvedOptions().pluralCategories;
  let missing = 0;
  for (const k of Object.keys(en)) {
    const m = k.match(PLURAL);
    if (!m) missing += !dict[k];
    else if (m[2] === "other") for (const f of forms) missing += !dict[m[1] + "_" + f];
  }
  console.log(file + ": OK (" + Object.keys(dict).length + " keys, " + missing + " untranslated)");
}
assert.deepStrictEqual(copied, [], "English text added to locale files; add new strings to en.json only");
if (base) console.log("locales: OK (no English copies added since " + base + ")");

// --- Preferences field labels resolve -----------------------------------
// The view derives each label key from the field's category and name, so a
// rename has to land in the dictionary too. Lift the literal tables straight
// out of the view (they are plain data) rather than duplicating them here.
const viewSrc = readFileSync(join(here, "..", "static", "js", "views", "preferences.js"), "utf8");
const table = (name) => {
  const m = viewSrc.match(new RegExp("^const " + name + " = \\[[\\s\\S]*?^\\];$", "m"));
  assert.ok(m, "preferences.js: could not lift `" + name + "`");
  return m[0];
};
const { TABS } = new Function(
  [table("PROXY_TYPES"), table("SEE_SHARES"), table("GEOIP_SOURCES"), table("TABS"),
   "return { TABS };"].join("\n"),
)();

const missingLabels = [];
for (const tab of TABS) {
  if (!(tab.labelKey in en)) missingLabels.push(tab.labelKey);
  if (tab.noteKey && !(tab.noteKey in en)) missingLabels.push(tab.noteKey);
  for (const grp of tab.groups) {
    if (!(grp.legendKey in en)) missingLabels.push(grp.legendKey);
    for (const f of grp.fields) {
      // hidden fields are capability flags loaded only to gate others; the
      // view never renders a label for them.
      if (f.hidden) continue;
      const cat = f.cat || tab.cat;
      const key = f.labelKey || "prefs_field_" + cat.replace(/\./g, "_") + "_" + f.key;
      if (!(key in en)) missingLabels.push(key);
      for (const o of f.options || []) {
        if (!(o.labelKey in en)) missingLabels.push(o.labelKey);
      }
      // Action buttons carry their own toast key and, when they show one, a
      // tooltip key, equally derived from the table and invisible when absent.
      if (f.action && !(f.action.toastKey in en)) missingLabels.push(f.action.toastKey);
      if (f.action?.titleKey && !(f.action.titleKey in en)) missingLabels.push(f.action.titleKey);
    }
  }
}
assert.deepStrictEqual(missingLabels, [], "preferences.js: label keys absent from en.json");
console.log("preferences.js: OK (all field/group/tab labels resolve)");
