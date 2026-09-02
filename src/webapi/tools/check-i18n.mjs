// Dictionary consistency check: every locale must have exactly the keys of
// en.json (the source), and each translation must keep the same {placeholders}.
// Also checks that every preferences field renders a real label, since those
// keys are derived (prefs_field_<category>_<key>) rather than written out --
// a renamed field with no matching dictionary entry would otherwise render
// the raw key with no error anywhere.
// Run: node src/webapi/tools/check-i18n.mjs
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

const en = load("en.json");
for (const file of readdirSync(dir).filter((f) => f.endsWith(".json") && f !== "en.json")) {
  const dict = load(file);
  const missing = Object.keys(en).filter((k) => !(k in dict));
  const extra = Object.keys(dict).filter((k) => !(k in en));
  assert.deepStrictEqual(missing, [], file + ": missing keys");
  assert.deepStrictEqual(extra, [], file + ": stale keys not in en.json");
  for (const k of Object.keys(en)) {
    assert.strictEqual(placeholders(dict[k]), placeholders(en[k]),
      file + ": placeholder mismatch in \"" + k + "\"");
  }
  console.log(file + ": OK (" + Object.keys(dict).length + " keys)");
}

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
      // Action buttons carry their own tooltip + toast keys, equally derived
      // from the table and equally invisible when absent.
      for (const k of ["titleKey", "toastKey"]) {
        if (f.action && !(f.action[k] in en)) missingLabels.push(f.action[k]);
      }
    }
  }
}
assert.deepStrictEqual(missingLabels, [], "preferences.js: label keys absent from en.json");
console.log("preferences.js: OK (all field/group/tab labels resolve)");
