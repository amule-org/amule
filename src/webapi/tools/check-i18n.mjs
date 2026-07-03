// Dictionary consistency check: every locale must have exactly the keys of
// en.json (the source), and each translation must keep the same {placeholders}.
// Run: node src/webapi/tools/check-i18n.mjs
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import assert from "node:assert";

const dir = join(dirname(new URL(import.meta.url).pathname), "..", "static", "i18n");
const load = (f) => JSON.parse(readFileSync(join(dir, f), "utf8"));
const placeholders = (s) => (s.match(/\{[a-z_]+\}/g) || []).sort().join(",");

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
