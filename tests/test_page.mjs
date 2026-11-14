// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — the install page (docs/index.html) in a real browser (headless Chromium, Playwright), its Web MIDI
// bridged to the simulated pedal (tests/devsim.c): Connect, then Install, as a user would; screenshots in OUTDIR.
//   node tests/test_page.mjs OUTDIR
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, cpSync, readFileSync } from "node:fs";
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
  ok(!(await page.isVisible("#altCard")), "the normal page offers no alternative firmware");
  await ch.close();
}

// an alternative firmware (an app on the base firmware): only through ?app=NAME, in a card of its own, beside OpenPocket;
// the page says which product the pedal runs, and step 2 is the way back
cpSync(join(FIX, "firmware", "alt"), join(SITE, "firmware", "apps", "starter"), { recursive: true });
sim.stdin.end(); sim = null; simLog.length = 0; buf = "";
await page.goto(url + "?app=starter");
await page.waitForFunction(() => /STARTER v01/.test(document.getElementById("altname").textContent), null, { timeout: 5000 });
ok(await page.isVisible("#altCard") && /not OpenPocket/.test(await page.textContent("#altCard")), "?app=starter: its own card names STARTER v01 and says it is not OpenPocket");
ok(/v24/.test(await page.textContent("#pkgver")) && /Install OpenPocket/i.test(await page.textContent("#installCard h2")), "step 2 still offers OpenPocket, from its own folder");
ok(!(await page.isEnabled("#altinstall")), "nothing can be installed before a pedal is connected");
startSim();
await page.waitForTimeout(300);
await page.click("#connect");
await page.waitForSelector("#devinfo:not([hidden])", { timeout: 15000 });
ok(/OpenPocket v23/.test(await page.textContent("#d-fw")) && (await page.textContent("#install")) === "Install" && !(await page.isVisible("#altnow")), "a pedal on OpenPocket: named so, the button of step 2 says Install");
ok((await page.textContent("#altinstall")) === "Install STARTER v01", "the alternative firmware's button names it");
await page.screenshot({ path: join(OUT, "page_6_alt_offered.png"), fullPage: true });
await page.click("#altinstall");
await page.waitForFunction(() => /PocketX_901/.test(document.getElementById("altmsg").textContent) || document.getElementById("altmsg").classList.contains("err"), null, { timeout: 240000 });
const altmsg = await page.textContent("#altmsg");
ok(/the pedal now runs STARTER v01 \(PocketX_901\), an alternative firmware/.test(altmsg) && /To go back to OpenPocket/.test(altmsg), `result, in its own card: ${altmsg}`);
ok((await page.textContent("#msg")) === "" && !(await page.isVisible("#next")), "step 2 says nothing of it: no OpenPocket version, no editor link");
await page.waitForFunction(() => /STARTER/.test(document.getElementById("d-fw").textContent), null, { timeout: 15000 });
ok(/STARTER v01: an alternative firmware, not OpenPocket \(PocketX_901\)/.test(await page.textContent("#d-fw")), `the page now sees: ${await page.textContent("#d-fw")}`);
ok(await page.isVisible("#altnow") && /runs STARTER v01/.test(await page.textContent("#altnow")) && /presets, NAM models and cabinet IRs are still on the pedal/.test(await page.textContent("#altnow")),
  "step 2 says the pedal runs an alternative firmware and that OpenPocket's data is still there");
ok((await page.textContent("#install")) === "Back to OpenPocket" && (await page.textContent("#altinstall")) === "Install again: STARTER v01", "the button of step 2 is now Back to OpenPocket");
ok(await page.isEnabled("#uboot"), "the UBOOT soft key is the base firmware's: offered for every product on it");
await page.screenshot({ path: join(OUT, "page_7_alt_running.png"), fullPage: true });
await page.click("#install");
await page.waitForFunction(() => /PocketX_924/.test(document.getElementById("msg").textContent) || document.getElementById("msg").classList.contains("err"), null, { timeout: 240000 });
ok(/OpenPocket v24 \(PocketX_924\)/.test(await page.textContent("#msg")), `the way back: ${await page.textContent("#msg")}`);
await page.waitForFunction(() => /OpenPocket v24/.test(document.getElementById("d-fw").textContent), null, { timeout: 15000 });
ok((await page.textContent("#install")) === "Install" && !(await page.isVisible("#altnow")), "the pedal is on OpenPocket again, and the page says so");
ok(simLog.filter((l) => /step2 rc=0/.test(l)).length === 2, "the simulated pedal wrote both firmwares");
await page.screenshot({ path: join(OUT, "page_8_back_on_openpocket.png"), fullPage: true });
{
  const no = await browser.newPage({ viewport: { width: 820, height: 900 } });
  await no.goto(url + "?app=nosuch");
  await no.waitForFunction(() => document.getElementById("altmsg").textContent !== "", null, { timeout: 5000 });
  ok(/No alternative firmware by this name/.test(await no.textContent("#altmsg")) && !(await no.isEnabled("#altinstall")), "?app= with a name the site does not have: said so, nothing to install");
  await no.goto(url + "?app=../../etc");
  await no.waitForFunction(() => document.getElementById("altmsg").textContent !== "", null, { timeout: 5000 });
  ok(/: etc$/.test(await no.textContent("#altmsg")), "a path in the name is cleaned (firmware/apps/etc/ is asked for, nothing outside it)");
  await no.close();
}
await browser.close();
if (sim) sim.stdin.end();
server.close();
console.log(failed ? `${failed} FAILED` : "page test passed", "| screenshots:", OUT);
process.exit(failed ? 1 : 0);
