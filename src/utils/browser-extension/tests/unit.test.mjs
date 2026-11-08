//
// This file is part of the aMule Project.
//
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
//

import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import { chromium } from "playwright-core";
const source = fs.readFileSync(
  process.env.ED2K_UNIT_SOURCE || new URL("../rewrite.js", import.meta.url),
  "utf8",
);
let browser;
before(async () => {
  const env = { ...process.env };
  for (const k of [
    "HTTP_PROXY",
    "HTTPS_PROXY",
    "ALL_PROXY",
    "http_proxy",
    "https_proxy",
    "all_proxy",
    "LD_PRELOAD",
  ])
    delete env[k];
  browser = await chromium.launch({
    executablePath: process.env.AMULE_BROWSER_EXECUTABLE || undefined,
    headless: true,
    env,
  });
});
after(async () => {
  await browser?.close();
});
async function fixture(run) {
  const page = await browser.newPage();
  try {
    await page.setContent("<main></main>");
    await run(page);
  } finally {
    await page.close();
  }
}
const cases = [
  [
    "file delimiters",
    "ed2k://|file|sample.bin|123|0123456789abcdef0123456789abcdef|/",
    "ed2k://%7Cfile%7Csample.bin%7C123%7C0123456789abcdef0123456789abcdef%7C/",
  ],
  [
    "server delimiters",
    "ed2k://|server|127.0.0.1|4661|/",
    "ed2k://%7Cserver%7C127.0.0.1%7C4661%7C/",
  ],
  [
    "server-list delimiters",
    "ed2k://|serverlist|https://example.invalid/server.met|/",
    "ed2k://%7Cserverlist%7Chttps://example.invalid/server.met%7C/",
  ],
  ["uppercase scheme", "ED2K://|file|x|/", "ED2K://%7Cfile%7Cx%7C/"],
  ["leading whitespace", " \tED2K://|file|x|/", " \tED2K://%7Cfile%7Cx%7C/"],
  [
    "encoded filename pipe and space",
    "ed2k://|file|a%7Cb%20c.bin|/",
    "ed2k://%7Cfile%7Ca%7Cb%20c.bin%7C/",
  ],
  [
    "unicode filename",
    "ed2k://|file|你好.bin|/",
    "ed2k://%7Cfile%7C你好.bin%7C/",
  ],
  [
    "mixed escaped delimiters",
    "ed2k://%7cfile|x%7C/",
    "ed2k://%7cfile%7Cx%7C/",
  ],
  [
    "encoded delimiters unchanged",
    "ed2k://%7Cfile%7Cx%7C/",
    "ed2k://%7Cfile%7Cx%7C/",
  ],
  [
    "lowercase escapes unchanged",
    "ed2k://%7cfile%7cx%7c/",
    "ed2k://%7cfile%7cx%7c/",
  ],
  [
    "double-encoded escape unchanged",
    "ed2k://|file|a%257Cb|/",
    "ed2k://%7Cfile%7Ca%257Cb%7C/",
  ],
  [
    "other escapes preserved",
    "ed2k://|file|a%2Fb%25c%26d|/",
    "ed2k://%7Cfile%7Ca%2Fb%25c%26d%7C/",
  ],
  [
    "https unchanged",
    "https://example.invalid/a|b",
    "https://example.invalid/a|b",
  ],
  [
    "http unchanged",
    "http://example.invalid/a|b",
    "http://example.invalid/a|b",
  ],
  [
    "magnet unchanged",
    "magnet:?xt=urn:ed2k:123|456",
    "magnet:?xt=urn:ed2k:123|456",
  ],
  ["javascript unchanged", 'javascript:void("a|b")', 'javascript:void("a|b")'],
  ["relative URL unchanged", "/ed2k://|file|x", "/ed2k://|file|x"],
  [
    "embedded scheme unchanged",
    "https://example.invalid/?q=ed2k://|x",
    "https://example.invalid/?q=ed2k://|x",
  ],
  ["different scheme unchanged", "xed2k://|file|x", "xed2k://|file|x"],
  ["single-slash spelling unchanged", "ed2k:/|file|x", "ed2k:/|file|x"],
  ["empty href unchanged", "", ""],
  ["no pipes unchanged", "ed2k://abc", "ed2k://abc"],
];
for (const [name, input, expected] of cases)
  test(name, () =>
    fixture(async (page) => {
      await page.evaluate((href) => {
        const a = document.createElement("a");
        a.id = "subject";
        a.setAttribute("href", href);
        document.body.append(a);
      }, input);
      await page.addScriptTag({ content: source });
      assert.equal(
        await page.locator("#subject").getAttribute("href"),
        expected,
      );
    }),
  );
test("image-map area link", () =>
  fixture(async (p) => {
    await p.setContent(
      '<map><area id="subject" href="ed2k://|server|host|1|/"></map>',
    );
    await p.addScriptTag({ content: source });
    assert.equal(
      await p.locator("#subject").getAttribute("href"),
      "ed2k://%7Cserver%7Chost%7C1%7C/",
    );
  }));
test("ignores missing href and non-link elements", () =>
  fixture(async (p) => {
    await p.setContent(
      '<a id="missing">plain</a><div id="div" href="ed2k://|x">text</div>',
    );
    await p.addScriptTag({ content: source });
    assert.equal(await p.locator("#missing").getAttribute("href"), null);
    assert.equal(await p.locator("#div").getAttribute("href"), "ed2k://|x");
  }));
test("mutation observer handles nested additions and text nodes", () =>
  fixture(async (p) => {
    await p.addScriptTag({ content: source });
    await p.evaluate(() => {
      const section = document.createElement("section");
      section.innerHTML =
        '<div><a id="late" href="ed2k://|file|late|/">late</a></div>';
      document.body.append("text", section);
    });
    await p.waitForFunction(
      () =>
        document.querySelector("#late").getAttribute("href") ===
        "ed2k://%7Cfile%7Clate%7C/",
    );
  }));
test("href changes between schemes stay correct", () =>
  fixture(async (p) => {
    await p.setContent(
      '<a id="subject" href="https://example.invalid/">link</a>',
    );
    await p.addScriptTag({ content: source });
    await p
      .locator("#subject")
      .evaluate((a) => a.setAttribute("href", "ed2k://|file|changed|/"));
    await p.waitForFunction(
      () =>
        document.querySelector("#subject").getAttribute("href") ===
        "ed2k://%7Cfile%7Cchanged%7C/",
    );
    await p
      .locator("#subject")
      .evaluate((a) => a.setAttribute("href", "https://example.invalid/a|b"));
    await p.evaluate(() => new Promise((r) => setTimeout(r, 0)));
    assert.equal(
      await p.locator("#subject").getAttribute("href"),
      "https://example.invalid/a|b",
    );
  }));
for (const type of [
  "pointerdown",
  "click",
  "auxclick",
  "contextmenu",
  "keydown",
])
  test(type + " repairs shadow-root anchor without cancelling event", () =>
    fixture(async (p) => {
      await p.addScriptTag({ content: source });
      const r = await p.evaluate((type) => {
        const host = document.createElement("div");
        document.body.append(host);
        const shadow = host.attachShadow({ mode: "open" });
        shadow.innerHTML =
          '<a href="ed2k://|file|shadow|/"><span>nested</span></a>';
        const event = new Event(type, {
          bubbles: true,
          composed: true,
          cancelable: true,
        });
        const allowed = shadow.querySelector("span").dispatchEvent(event);
        return {
          href: shadow.querySelector("a").getAttribute("href"),
          allowed,
          cancelled: event.defaultPrevented,
        };
      }, type);
      assert.deepEqual(r, {
        href: "ed2k://%7Cfile%7Cshadow%7C/",
        allowed: true,
        cancelled: false,
      });
    }),
  );
test("rewriting stabilizes without repeated href writes", () =>
  fixture(async (p) => {
    await p.addScriptTag({ content: source });
    const writes = await p.evaluate(async () => {
      const a = document.createElement("a");
      a.setAttribute("href", "ed2k://|file|x|/");
      let n = 0;
      const observer = new MutationObserver((records) => (n += records.length));
      observer.observe(a, { attributes: true, attributeFilter: ["href"] });
      document.body.append(a);
      await new Promise((r) => setTimeout(r, 20));
      observer.disconnect();
      return n;
    });
    assert.equal(writes, 1);
  }));
