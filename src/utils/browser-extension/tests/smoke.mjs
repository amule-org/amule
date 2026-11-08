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

import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";

const root = import.meta.dirname;
const state = process.env.ED2K_TEST_STATE;
assert.equal(process.env.ED2K_PRIVATE_TEST_BUS, "1", "Use npm run test:smoke");
assert.equal(process.env.XDG_CONFIG_HOME, state + "/config");
const extension = state + "/release/ed2k-link-fixer";
const results = [];
const contexts = [];
const record = (name) => {
  results.push(name);
  console.log("PASS", name);
};
const hash = (name) => createHash("md5").update(name).digest("hex");
const link = (name) => `ed2k://|file|${name}.bin|1024|${hash(name)}|/`;
const deliveries = () =>
  fs.existsSync(state + "/handler.jsonl")
    ? fs
        .readFileSync(state + "/handler.jsonl", "utf8")
        .trim()
        .split("\n")
        .filter(Boolean)
        .map(JSON.parse)
    : [];

async function waitFor(check, message) {
  for (let i = 0; i < 100; i++) {
    if (check()) return;
    await delay(150);
  }
  throw new Error(message);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, "http://localhost");
  const name = url.searchParams.get("name") || "fixture";
  assert.match(name, /^[a-z-]+$/);
  res.setHeader("Content-Type", "text/html; charset=utf-8");
  res.end(`<html><title>eD2k extension smoke test</title><body>
    <a id="link" href="${link(name)}"><span>Open ${name}</span></a>
    <a id="ordinary" href="https://example.invalid/a|b">Web link</a>
    <a id="encoded" href="ed2k://%7cfile%7ca%7Cb%20name.bin%7c1024%7c${hash("filename")}%7c/">Encoded</a>
    <div id="dynamic"></div></body></html>`);
});

try {
  // Verify that the packaged bytes are exactly those exercised by unit tests.
  for (const name of [
    "manifest.json",
    "rewrite.js",
    "popup.html",
    "help.html",
    "style.css",
  ]) {
    assert.deepEqual(
      fs.readFileSync(extension + "/" + name),
      fs.readFileSync(path.join(root, "..", name)),
    );
  }
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const origin = `http://127.0.0.1:${server.address().port}`;

  async function browser(name, load, consent) {
    const profile = state + "/" + name;
    fs.mkdirSync(profile + "/Default", { recursive: true });
    if (consent) {
      fs.writeFileSync(
        profile + "/Default/Preferences",
        JSON.stringify({
          protocol_handler: {
            allowed_origin_protocol_pairs: { [origin]: { ed2k: true } },
          },
        }),
      );
    }
    const context = await chromium.launchPersistentContext(profile, {
      executablePath: process.env.AMULE_BROWSER_EXECUTABLE || undefined,
      headless: false,
      ignoreDefaultArgs: ["--disable-extensions"],
      args: [
        "--enable-unsafe-extension-debugging",
        "--force-renderer-accessibility",
        "--lang=en-US",
      ],
    });
    contexts.push(context);
    console.log("Browser:", context.browser().version());
    if (load) {
      const cdp = await context.browser().newBrowserCDPSession();
      context.extensionId = (
        await cdp.send("Extensions.loadUnpacked", { path: extension })
      ).id;
    }
    return context;
  }

  const baseline = await browser("baseline", false, false);
  const control = await baseline.newPage();
  await control.goto(origin + "/?name=control");
  await control.locator("#link").click();
  await waitFor(
    () => control.url() === "about:blank#blocked",
    "Unextended control was not blocked",
  );
  assert.equal(deliveries().length, 0);
  record("literal-pipe control blocked without extension");
  await baseline.close();

  // A clean profile must show native external-app consent. Do not manufacture
  // delivery by calling the handler or pre-populating permission for this case.
  const first = await browser("first-use", true, false);
  const firstPage = await first.newPage();
  await firstPage.goto(origin + "/?name=first-use");
  await firstPage.locator("#link").click();
  await delay(500);
  assert.equal(deliveries().length, 0, "Handler ran before first-use consent");
  const prompt = spawnSync("python3", [root + "/accept-prompt.py", origin], {
    encoding: "utf8",
    timeout: 20000,
  });
  fs.writeFileSync(state + "/native-prompt.log", prompt.stdout + prompt.stderr);
  assert.equal(prompt.status, 0, prompt.stdout + prompt.stderr);
  await waitFor(
    () => deliveries().some((args) => args[0].includes("first-use")),
    "Accepted native prompt did not deliver URI",
  );
  record(
    "first-use native Chrome prompt gates delivery and Open invokes handler",
  );
  await first.close();

  const context = await browser("approved", true, true);
  const page = await context.newPage();
  await page.goto(origin + "/?name=package-click");
  await page.waitForFunction(() =>
    document.querySelector("#link").getAttribute("href").includes("%7C"),
  );
  assert.equal(
    await page.locator("#ordinary").getAttribute("href"),
    "https://example.invalid/a|b",
  );
  assert.equal(
    await page.locator("#encoded").getAttribute("href"),
    `ed2k://%7cfile%7ca%7Cb%20name.bin%7c1024%7c${hash("filename")}%7c/`,
  );
  record(
    "initial links repaired and other schemes and filename escapes preserved",
  );
  await page.evaluate((url) => {
    const a = document.createElement("a");
    a.id = "late";
    a.href = url;
    document.querySelector("#dynamic").append(a);
  }, link("late"));
  await page.waitForFunction(() =>
    document.querySelector("#late").getAttribute("href").includes("%7C"),
  );
  await page
    .locator("#late")
    .evaluate((a, url) => a.setAttribute("href", url), link("changed"));
  await page.waitForFunction(
    () =>
      document
        .querySelector("#late")
        .getAttribute("href")
        .includes("changed") &&
      !document.querySelector("#late").getAttribute("href").includes("|"),
  );
  record("dynamic insertion and href update repaired");
  const frameReady = page.waitForEvent("framenavigated", {
    predicate: (f) => f.parentFrame() !== null,
  });
  await page.evaluate((url) => {
    const f = document.createElement("iframe");
    f.src = url;
    document.body.append(f);
  }, origin + "/?name=frame");
  const frame = await frameReady;
  await frame.waitForFunction(() =>
    document.querySelector("#link")?.getAttribute("href").includes("%7C"),
  );
  record("web-page frame links repaired");

  async function accepted(name, activate) {
    await page.goto(origin + "/?name=" + name);
    await page.waitForFunction(() =>
      document.querySelector("#link").getAttribute("href").includes("%7C"),
    );
    const count = deliveries().length;
    await activate();
    await waitFor(
      () =>
        deliveries().length > count &&
        deliveries().some((args) => args[0].includes(name)),
      name + " did not reach OS handler",
    );
    if (process.env.AMULE_BINARY) {
      await waitFor(
        () =>
          fs.existsSync(state + "/amule/logfile") &&
          fs
            .readFileSync(state + "/amule/logfile", "utf8")
            .includes("Downloading " + name + ".bin"),
        name + " did not reach aMule queue",
      );
    }
    record(
      name +
        " reached " +
        (process.env.AMULE_BINARY
          ? "OS handler and aMule queue"
          : "OS handler"),
    );
  }
  await accepted("package-click", () => page.locator("#link span").click());
  await accepted("package-keyboard", async () => {
    await page.locator("#link").focus();
    await page.keyboard.press("Enter");
  });
  await accepted("package-middle", () =>
    page.locator("#link").click({ button: "middle" }),
  );
  await accepted("package-shadow", async () => {
    await page.evaluate((url) => {
      const host = document.createElement("div");
      host.id = "shadow";
      document.body.append(host);
      host.attachShadow({ mode: "open" }).innerHTML =
        `<a href="${url}">Shadow link</a>`;
    }, link("package-shadow"));
    await page.locator("#shadow a").click();
  });
  const help = await context.newPage();
  await help.goto(`chrome-extension://${context.extensionId}/help.html`);
  assert.equal(
    await help.locator("h1").textContent(),
    "Open eD2k links with a click",
  );
  await help.screenshot({ path: state + "/help.png", fullPage: true });
  await help.goto(`chrome-extension://${context.extensionId}/popup.html`);
  assert.equal(
    await help.locator("a.button").getAttribute("href"),
    "help.html",
  );
  await help.screenshot({ path: state + "/popup.png" });
  record("bundled help and popup render");
} finally {
  // Cleanup must run even if a browser close fails, and must not replace the
  // original test failure with a secondary shutdown exception.
  await Promise.allSettled(contexts.map((context) => context.close()));
  server.close();
  spawnSync("python3", [root + "/cleanup.py"], { stdio: "inherit" });
  fs.writeFileSync(
    state + "/results.json",
    JSON.stringify({ extension, results }, null, 2),
  );
}
