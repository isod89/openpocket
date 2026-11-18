// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — the install page (docs/index.html) in a real browser (headless Chromium, Playwright), its Web MIDI
// bridged to the simulated pedal (tests/devsim.c): Connect, then Install, as a user would; screenshots in OUTDIR.
//   node tests/test_page.mjs OUTDIR
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, cpSync, readFileSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join, extname } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const { chromium } = require("playwright");

const ROOT = fileURLToPath(new URL("..", import.meta.url));
const OUT = process.argv[2] || mkdtempSync(join(tmpdir(), "oppage-"));
const SITE = mkdtempSync(join(tmpdir(), "opsite-"));
const FIX = mkdtempSync(join(tmpdir(), "opfix-"));
execFileSync("python3", [join(ROOT, "tests", "fixtures.py"), join(FIX, "firmware")], { stdio: "inherit" });
cpSync(join(ROOT, "docs"), SITE, { recursive: true });
cpSync(join(FIX, "firmware"), join(SITE, "firmware"), { recursive: true });

const TYPES = { ".html": "text/html", ".js": "text/javascript", ".json": "application/json", ".fwsc": "application/octet-stream" };
const server = createServer((req, res) => {
  try {
    const p = join(SITE, decodeURIComponent(req.url.split("?")[0]).replace(/\/$/, "/index.html"));
    const body = readFileSync(p);
    res.writeHead(200, { "content-type": TYPES[extname(p)] || "application/octet-stream" });
    res.end(body);
  } catch (_) { res.writeHead(404); res.end(); }
}).listen(0);
const url = `http://127.0.0.1:${server.address().port}/`;

let sim = null;
const simLog = [];
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 820, height: 1100 }, colorScheme: "dark" });
await page.exposeFunction("simSend", (hex) => sim && sim.stdin.write(hex + "\n"));
page.on("console", (m) => { if (m.type() === "error") console.log("page error:", m.text()); });
page.on("pageerror", (e) => console.log("page exception:", e.message));
await page.addInitScript(() => {
  // a Web MIDI access over the simulated pedal; "#port NAME" renames (re-enumerates) its port pair
  const access = { inputs: new Map(), outputs: new Map(), onstatechange: null };
  let port = null;
  window.__simPort = (name) => {
    if (port) { port.input.state = "disconnected"; port.output.state = "disconnected"; }
    access.inputs.clear(); access.outputs.clear(); port = null;
    if (name !== "none") {
      const input = { name, state: "connected", onmidimessage: null, open: async () => {} };
      const output = { name, state: "connected", open: async () => {}, send: (b) => {
        if (output.state === "disconnected") throw new Error("disconnected");
        window.simSend([...b].map((x) => x.toString(16).padStart(2, "0")).join(""));
      } };
      port = { input, output };
      access.inputs.set(name, input); access.outputs.set(name, output);
    }
    if (access.onstatechange) access.onstatechange({});
  };
  window.__simIn = (hex) => {
    if (port && port.input.onmidimessage) port.input.onmidimessage({ data: Uint8Array.from(hex.match(/../g).map((h) => parseInt(h, 16))) });
  };
  navigator.requestMIDIAccess = async () => access;
});
let buf = "";
function startSim(...args) {
sim = spawn(join(ROOT, "build", "devsim"), [join(FIX, "firmware", "flash_old.bin"), ...args], { stdio: ["pipe", "pipe", "inherit"] });
sim.stdout.on("data", (d) => {
  buf += d.toString();
  let i;
  while ((i = buf.indexOf("\n")) >= 0) {
    const ln = buf.slice(0, i); buf = buf.slice(i + 1);
    if (ln.startsWith("#port ")) page.evaluate((n) => window.__simPort(n), ln.slice(6)).catch(() => {});
    else if (ln.startsWith("#log ")) simLog.push(ln.slice(5));
    else if (ln) page.evaluate((h) => window.__simIn(h), ln).catch(() => {});
  }
});
}

let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(64)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };
await page.goto(url);
await page.waitForTimeout(300);
startSim();
await page.waitForTimeout(300);
await page.screenshot({ path: join(OUT, "page_1_start.png"), fullPage: true });
await page.click("#connect");
await page.waitForSelector("#devinfo:not([hidden])", { timeout: 15000 });
const fw = await page.textContent("#d-fw");
ok(/OpenPocket v23/.test(fw), `detected: ${fw}`);
ok(await page.isEnabled("#install"), "Install enabled");
await page.screenshot({ path: join(OUT, "page_2_found.png"), fullPage: true });
await page.click("#install");
await page.waitForFunction(() => /PocketX_924/.test(document.getElementById("msg").textContent) ||
  document.getElementById("msg").classList.contains("err"), null, { timeout: 240000 });
const msg = await page.textContent("#msg");
ok(/PocketX_924/.test(msg), `result: ${msg}`);
ok(/OpenPocket v24/.test(msg) && await page.isVisible("#next") && /WHATS-NEW-v24\.md$/.test(await page.getAttribute("#notes", "href")), "the result names the version and links the editor and the release notes");
ok(await page.$$eval("#steps span", (s) => s.length > 0 && s.every((e) => e.className === "done")), "every stage shows done, none still lit");
await page.waitForTimeout(1500);
ok(/OpenPocket v24/.test(await page.textContent("#d-fw")), "the page now sees OpenPocket v24");
await page.screenshot({ path: join(OUT, "page_3_done.png"), fullPage: true });
ok(simLog.some((l) => /step2 rc=0/.test(l)), "the simulated pedal wrote the new app");

// rescue mode: armed with no pedal; the install starts by itself when the pedal appears (issue #1)
sim.stdin.end(); sim = null; simLog.length = 0; buf = "";
await page.reload();
await page.waitForTimeout(300);
ok(!(await page.isVisible("#rescueOn")), "rescue mode sits in the advanced section, folded");
await page.click("details summary[data-t=rescue]");
await page.check("#rescueOn");
await page.waitForFunction(() => /rescue mode armed/.test(document.getElementById("log").textContent), null, { timeout: 15000 });
await page.click("#connect");
await page.waitForTimeout(300);
ok(!/PocketX_9/.test(await page.textContent("#msg")), "rescue: armed, nothing happens without a pedal");
startSim();
await page.waitForFunction(() => /PocketX_924/.test(document.getElementById("msg").textContent), null, { timeout: 240000 });
ok(/PocketX_924/.test(await page.textContent("#msg")), "rescue: the pedal appears, the install runs by itself");
ok(!(await page.isChecked("#rescueOn")), "rescue: the mode turns itself off once done");
await page.screenshot({ path: join(OUT, "page_4_rescue.png"), fullPage: true });

// V21 on its own is not proof that the factory bank is readable.
sim.stdin.end(); sim = null; simLog.length = 0; buf = "";
await page.reload();
startSim("--stock", "BlackBox_021");
await page.click("#connect");
await page.waitForSelector("#verify-factory:enabled", { timeout: 15000 });
ok(await page.isVisible("#v21-factory"), "BlackBox V21: the factory sound controls are visible");
await page.click("#verify-factory");
await page.waitForSelector("#msg.err", { timeout: 15000 });
ok(/sound bank/.test(await page.textContent("#msg")), "BlackBox V21: no bank reply means verification failed");
await page.click("#factory-uboot");
await page.waitForFunction(() => /USB recovery requested/.test(document.getElementById("msg").textContent));
ok(simLog.includes("uboot") && !simLog.some((l) => /step[12]/.test(l)), "BlackBox V21: recovery asks for UBOOT without running an OTA");

// an official firmware newer than any the page knows: a warning and a checkbox. Install stays disabled until it is
// ticked; once ticked the install completes on the simulated pedal
sim.stdin.end(); sim = null; simLog.length = 0; buf = "";
await page.reload();
startSim("--stock", "PocketX_017");
await page.click("#connect");
await page.waitForSelector("#newerconfirm:not([hidden])", { timeout: 15000 });
ok(/official firmware 017.*precaution/s.test(await page.textContent("#newerwarn")), "official PocketX_017 (unknown): the warning names the firmware and calls the check a precaution");
ok(await page.isDisabled("#install") && !simLog.some((l) => /step[12]/.test(l)), "official PocketX_017: Install stays disabled until confirmed, nothing sent to the pedal");
await page.screenshot({ path: join(OUT, "page_4b_untested.png"), fullPage: true });
await page.check("#newerok");
await page.waitForSelector("#install:enabled", { timeout: 15000 });
await page.click("#install");
await page.waitForFunction(() => /PocketX_924/.test(document.getElementById("msg").textContent), null, { timeout: 240000 });
ok(simLog.some((l) => /step2 rc=0/.test(l)), "official PocketX_017, confirmed: the install completes on the simulated pedal");

// a package that is not the manifest's (one byte changed on the site): refused by its SHA-256, before the pedal is asked
sim.stdin.end(); sim = null; simLog.length = 0; buf = "";
{
  const f = join(SITE, "firmware", "PocketX_924.fwsc"), good = readFileSync(f), bad = Buffer.from(good);
  bad[bad.length >> 1] ^= 1;
  writeFileSync(f, bad);
  await page.reload();
  startSim();
  await page.click("#connect");
  await page.waitForSelector("#install:enabled", { timeout: 15000 });
  await page.click("#install");
  await page.waitForSelector("#msg.err", { timeout: 15000 });
  ok(/SHA-256 mismatch/.test(await page.textContent("#msg")), "a package that differs from the manifest: refused (SHA-256)");
  ok(!simLog.some((l) => /step[12]/.test(l)), "the changed package: no update step ran on the pedal");
  writeFileSync(f, good);
}

// the USB rescue for a pedal the page cannot reach: a folded guide in the rescue section, Mac and Windows steps,
// read-only first, the write as one more step, a link to the full guide; it fits a phone's width
{
  ok(/read-only/.test(await page.textContent("[data-t=rescueusb]")) && /How to run the rescue on your computer/.test(await page.textContent("[data-t=rescueon]")),
    "rescue section: the USB rescue is read-only first, and rescue mode points to the guide");
  await page.click("details summary[data-t=rescue]");
  ok(!(await page.isVisible("#rescue-mac")), "rescue guide: folded until opened");
  await page.click("#rescueguide summary");
  const steps = (id) => page.$$eval(`#${id} + ol li, #${id} + p + ol li`, (li) => li.map((e) => e.textContent));
  const mac = await steps("rescue-mac"), win = await steps("rescue-windows");
  ok(mac.some((s) => /brew install libusb/.test(s)) && mac.some((s) => /sudo python3 tools\/uboot_rescue\.py --package BlackBox\.fwsc/.test(s)) &&
     mac.some((s) => /--write --package BlackBox\.fwsc/.test(s)), "rescue guide, Mac: libusb once, the read-only check, then the write");
  ok(win.some((s) => /python\.org/.test(s)) && win.some((s) => /Run as administrator/.test(s)) && win.some((s) => /^Unplug.*RESCUE\.bat --package BlackBox\.fwsc/.test(s)) &&
     win.some((s) => /RESCUE\.bat --write --package BlackBox\.fwsc/.test(s)) && /not been tested on a real pedal/.test(await page.textContent("#rescueguide .warnbox")),
    "rescue guide, Windows: Python once, an administrator Command Prompt, the check, then the write; said to be untested");
  ok(await page.getAttribute("#rescueguide a[href^='RESCUE.md#quick-start']", "href") !== null, "rescue guide: links the full guide in RESCUE.md");
  await page.setViewportSize({ width: 360, height: 800 });
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), "rescue guide: no sideways scrolling at a phone's width");
  await page.screenshot({ path: join(OUT, "page_4c_rescue_guide.png"), fullPage: true });
  await page.setViewportSize({ width: 820, height: 1100 });
}

// a test channel: only through ?channel=NAME, from its own folder, with its banner; the normal page has none
cpSync(join(FIX, "firmware"), join(SITE, "firmware", "midi-test"), { recursive: true });
{
  const ch = await browser.newPage({ viewport: { width: 820, height: 900 } });
  await ch.goto(url + "?channel=midi-test");
  await ch.waitForFunction(() => /midi-test/.test(document.getElementById("pkgver").textContent), null, { timeout: 5000 });
  ok(await ch.isVisible("#testchan"), "test channel: the banner says it is a test build");
  ok(/\(midi-test\)/.test(await ch.textContent("#pkgver")), "test channel: the packages come from firmware/midi-test/");
  await ch.goto(url + "?channel=../../etc");
  await ch.waitForFunction(() => document.getElementById("pkgver").textContent !== "…", null, { timeout: 5000 });
  ok((await ch.textContent("#pkgver")) === "—", "test channel: a path in the name is cleaned (firmware/etc/ is asked for, nothing outside firmware/)");
  await ch.screenshot({ path: join(OUT, "page_5_test_channel.png"), fullPage: true });
  ok(!(await page.isVisible("#testchan")), "the normal page shows no test banner");
}
await browser.close();
if (sim) sim.stdin.end();
server.close();
console.log(failed ? `${failed} FAILED` : "page test passed", "| screenshots:", OUT);
process.exit(failed ? 1 : 0);
