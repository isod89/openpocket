// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — the web editor page (docs/editor.html) in a real browser (Playwright / Chromium) against the simulated
// pedal (tests/edsim.c: the pedal's real editor code and preset store), through a fake Web MIDI access with the pedal
// and a second device (a foot controller). TONE3000 is simulated (its pop-up, its sign-in, its lists and files).
//   node tests/test_editor_page.mjs [OUT_DIR]     (screenshots in OUT_DIR, default build/editor)
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, extname } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const { chromium } = require("playwright");   /* the global install (CommonJS) */

const ROOT = fileURLToPath(new URL("..", import.meta.url));
const OUT = process.argv[2] || join(ROOT, "build", "editor");
const TMP = mkdtempSync(join(tmpdir(), "opedpage-"));
mkdirSync(OUT, { recursive: true });
const SITE = join(ROOT, "docs");
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".json": "application/json", ".css": "text/css", ".wav": "audio/wav" };
const server = createServer((req, res) => {
  try {
    const p = join(SITE, decodeURIComponent(req.url.split("?")[0]).replace(/\/$/, "/index.html"));
    res.writeHead(200, { "content-type": TYPES[extname(p)] || "application/octet-stream" });
    res.end(readFileSync(p));
  } catch (_) { res.writeHead(404); res.end(); }
}).listen(0);
const url = `http://127.0.0.1:${server.address().port}/editor.html`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(78)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };

const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: 2, acceptDownloads: true });
const page = await ctx.newPage();
let sim = null, buf = "";
const states = [];
const FLASH = join(TMP, "flash.bin");
await page.exposeFunction("simSend", (hex) => sim && sim.stdin.write(hex + "\n"));
page.on("console", (m) => { if (m.type() === "error" && !/fonts\.g|ERR_TUNNEL|net::|Failed to load resource/.test(m.text())) { console.log("page error:", m.text()); failed++; } });
page.on("pageerror", (e) => { console.log("page exception:", e.message); failed++; });
await page.addInitScript(() => {
  const access = { inputs: new Map(), outputs: new Map(), onstatechange: null };
  const fire = (port) => { if (access.onstatechange) access.onstatechange({ port }); };
  const mkIn = (name) => ({ id: "in-" + name, name, type: "input", state: "connected", onmidimessage: null, open: async () => {} });
  let port = null;
  const ctl = mkIn("Foot Controller");                     // another MIDI device on the computer
  access.inputs.set(ctl.id, ctl);
  window.__simPort = (name) => {
    const input = mkIn(name);
    const output = { id: "out-" + name, name, type: "output", state: "connected", open: async () => {}, send: (b) => window.simSend([...b].map((x) => x.toString(16).padStart(2, "0")).join("")) };
    port = { input, output };
    access.inputs.set(input.id, input); access.outputs.set(output.id, output);
    fire(input);
  };
  window.__simGone = () => { if (!port) return; port.input.state = port.output.state = "disconnected"; access.inputs.delete(port.input.id); access.outputs.delete(port.output.id); const p = port.input; port = null; fire(p); };
  // window.__prof = [13 figures]: PING's reply gets the DSP profile a v40.2 pedal adds (the simulator has none)
  window.__simIn = (hex) => {
    let d = hex.match(/../g).map((h) => parseInt(h, 16));
    if (window.__prof && d[4] === 18 && d.length === 18) d = [...d.slice(0, -1), window.__prof.length, ...window.__prof.flatMap((v) => [v & 127, v >> 7]), 0xF7];
    if (port && port.input.onmidimessage) port.input.onmidimessage({ data: Uint8Array.from(d) });
  };
  window.__ctl = (bytes) => { if (ctl.onmidimessage) ctl.onmidimessage({ data: Uint8Array.from(bytes) }); };
  navigator.requestMIDIAccess = async () => access;
});
function startSim() {
  buf = "";
  sim = spawn(join(ROOT, "build", "edsim"), ["--persist", FLASH], { stdio: ["pipe", "pipe", "inherit"] });
  sim.stdout.on("data", (d) => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const ln = buf.slice(0, i); buf = buf.slice(i + 1);
      if (ln.startsWith("#port ")) page.evaluate((n) => window.__simPort(n), ln.slice(6)).catch(() => {});
      else if (ln.startsWith("#state ")) states.push(ln.slice(7).split("|"));
      else if (ln) page.evaluate((h) => window.__simIn(h), ln).catch(() => {});
    }
  });
}
async function pedalState() {
  states.length = 0; sim.stdin.write("#dump\n");
  for (let i = 0; i < 100 && !states.length; i++) await sleep(10);
  const s = states[0];
  return { slot: +s[0], name: s[1], writesOk: s[2] === "1", focus: +s[3], on: s[4].split(",").map(Number), type: s[5].split(",").map(Number), amp: s[6].split(",").map(Number), order: s[7].split(",").map(Number) };
}
const shot = (n) => page.screenshot({ path: join(OUT, n) });
const text = (sel) => page.textContent(sel);
const row = (slot) => page.locator(`#ps${slot}`);
// a message of the page (they leave by themselves: each wait starts from a clean slate)
const clearToasts = () => page.evaluate(() => { document.getElementById("toasts").textContent = ""; });
const toast = (re, ms = 8000) => page.waitForFunction((r) => new RegExp(r).test(document.getElementById("toasts").textContent), re.source, { timeout: ms }).then(() => true, () => false);
const dialog = async () => { await page.waitForSelector("#dlg[open]", { timeout: 4000 }); await sleep(220); };
const menuItem = (label) => page.locator("#menu button", { hasText: label }).first().click();
const go = (view) => page.click(`#tabs [data-go="${view}"]`);
// an option by its text, spaces counted loosely: an option's label has its runs of spaces collapsed, and Playwright
// versions differ in whether { label } compares it so
const pickOpt = async (sel, re) => page.selectOption(sel, await page.$$eval(`${sel} option`, (o, src) => (o.find((x) => new RegExp(src).test(x.textContent)) || {}).value, re.source));
const slide = async (sel, frac) => { const b = await page.locator(sel).boundingBox(); await page.mouse.click(b.x + b.width * frac, b.y + b.height / 2); await sleep(300); };

// ---- first run, no pedal, then the pedal appears ----
await page.goto(url);
await sleep(300);
ok(await page.isVisible("#connect") && /Connect your pedal/.test(await text("#gate")), "first run: what to do, one Connect button");
await shot("editor_1_idle.png");
await page.click("#connect");
await page.waitForSelector('body[data-conn="notfound"]', { timeout: 5000 });
ok(/No pedal answered/.test(await text("#gate")) && /Foot Controller/.test(await text("#seen")), "no pedal: the reasons to check, and the MIDI devices the browser does see");
await shot("editor_1_no_pedal.png");
startSim();
await page.waitForSelector('body[data-conn="ready"]', { timeout: 20000 });
ok(true, "a pedal plugged in afterwards is found without another click");
ok(/PocketX · OpenPocket v39/.test(await text("#status")), `status: ${await text("#status")}`);
ok(await page.locator("#list .row").count() === 52, "52 presets listed");
ok(await page.inputValue("#name") === "TWIN CLEAN" && await text("#num") === "01", "header: 01 TWIN CLEAN");
ok(await page.locator("#tiles .tile").count() === 12, "12 blocks in the chain");
await page.waitForFunction(() => /42%/.test(document.getElementById("loadv").textContent), null, { timeout: 4000 }).catch(() => {});
ok(/42%/.test(await text("#loadv")) && await page.getAttribute("#load", "data-level") === "ok", "top bar: the DSP load (PING)");
await page.waitForFunction(() => /TWIN/.test(document.querySelector("#ps0 .sum").textContent), null, { timeout: 8000 }).catch(() => {});
ok(/TWIN · CMP REV/.test(await text("#ps0 .sum")), `the list says what each preset is made of: ${await text("#ps0 .sum")}`);
await shot("editor_2_connected.png");
await page.click("#load");
ok(/Room to spare/.test(await text("#loadpop")) && /Peak, this preset/.test(await text("#loadpop")), "DSP load details: the peaks, late blocks, clipping");
await page.keyboard.press("Escape");
// a v40.2 pedal also says where the time goes: each block's share, and the NAM network's
await page.evaluate(() => { window.__prof = [1, 2, 0, 0, 0, 3, 9, 1, 0, 0, 7, 0, 31]; });
await page.waitForFunction(() => parseFloat(document.querySelector('#tiles .tile[data-b="6"] .share').style.width) > 0, null, { timeout: 4000 }).catch(() => {});
await page.click("#load");
ok(/NAM network/.test(await text("#loadpop")) && /31 %/.test(await text("#loadpop")) && /CAB/.test(await text("#loadpop .prof")) && !/PIT/.test(await text("#loadpop .prof")),
  "with a v40.2 PING: where the DSP time goes, block by block (and a mark under each block)");
await page.keyboard.press("Escape");
await page.evaluate(() => { window.__prof = null; });

// ---- a setting in the page -> the pedal ----
await page.click('#tiles .tile[data-b="5"] .tsel');
await sleep(150);
ok((await pedalState()).focus === 5, "click on the AMP block: the pedal shows the AMP page");
ok(await page.locator("#p0").evaluate((e) => e.tagName) === "SELECT" && await page.locator("#p0 option").count() === 28, "NAM's MODEL is a list of the 28 models, not a slider");
await page.selectOption("#p0", { label: "JCM900" });
await sleep(250);
let ps = await pedalState();
ok(Math.floor(ps.amp[0] * 28 / 101) === 4, `MODEL JCM900 chosen: the pedal's knob is at ${ps.amp[0]} (model 4)`);
await slide("#p1", 0.8);
ps = await pedalState();
ok(Math.abs(ps.amp[1] - 80) <= 3, `AMP slider 2 clicked at 80 %: the pedal has ${ps.amp[1]}`);
ok(await text("#star") === "Edited" && await page.isEnabled("#save"), "the header says Edited, Save is ready");
await page.focus("#p2");
await page.keyboard.press("ArrowRight");
await page.keyboard.press("ArrowRight");
await sleep(250);
ps = await pedalState();
ok(ps.amp[2] === +(await page.inputValue("#p2")), `keyboard: slider 3 = ${await page.inputValue("#p2")} in the page, ${ps.amp[2]} on the pedal`);
const before3 = ps.amp[3];
await page.locator(".prm").nth(3).locator(".nudge").nth(1).click();
await sleep(250);
ok((await pedalState()).amp[3] === before3 + 1, "the + beside a slider: one step up");

// ---- the pedal -> the page ----
sim.stdin.write("#pedal set 5 2 7\n");
await page.waitForFunction(() => document.getElementById("p2").value === "7", null, { timeout: 3000 }).catch(() => {});
ok(await page.inputValue("#p2") === "7", "a knob turned on the pedal moves the slider in the page");
sim.stdin.write("#pedal focus 9\n");
await page.waitForFunction(() => /DELAY/.test(document.getElementById("bname").textContent), null, { timeout: 3000 }).catch(() => {});
ok(/DELAY/.test(await text("#bname")), "the DLY button on the pedal opens the delay in the page");

// ---- type, on/off, order ----
await page.selectOption("#type", "3");
await sleep(200);
ok((await pedalState()).type[9] === 3, "type chosen in the page -> the pedal");
await page.click("#tnext");
await sleep(200);
ok((await pedalState()).type[9] === 4, "the > button: next type");
ok(await page.locator("#p3").evaluate((e) => e.tagName) === "SELECT" && await page.locator("#p3 option").count() === 7, "a setting with named values (SPACE ECHO's HEADS: 1, 2, 1+2 ... ALL) is a list of them");
const wasOn = (await pedalState()).on[9];
await page.click("#onoff");
await sleep(200);
ok((await pedalState()).on[9] === (wasOn ? 0 : 1) && await page.getAttribute("#onoff", "aria-checked") === (wasOn ? "false" : "true"), "the block's switch: on/off, and it says so");
const modOn = (await pedalState()).on[8];
await page.click('#tiles .tile[data-b="8"] .pw');
await sleep(200);
ok((await pedalState()).on[8] === (modOn ? 0 : 1) && new RegExp(modOn ? "OFF" : "ON").test(await text('#tiles .tile[data-b="8"] .pw')), `the switch on the MOD block switches it ${modOn ? "off" : "on"} (and reads ${modOn ? "OFF" : "ON"})`);
const before = (await pedalState()).order;
const t0 = await page.locator("#tiles .tile").nth(0).boundingBox();
const t3 = await page.locator("#tiles .tile").nth(3).boundingBox();
await page.mouse.move(t0.x + t0.width / 2, t0.y + 14);
await page.mouse.down();
await page.mouse.move(t0.x + t0.width / 2 + 20, t0.y + 14, { steps: 4 });
await page.mouse.move(t3.x + t3.width / 2, t3.y + 14, { steps: 10 });
await shot("editor_3_drag.png");
await page.mouse.up();
await sleep(300);
const after = (await pedalState()).order;
ok(after[3] === before[0] && after[0] === before[1], `drag GT to the 4th place: the pedal's order ${after.join(",")}`);
await page.click('#tiles .tile[data-b="9"] .tsel');
await page.click("#mvl");
await sleep(250);
const a2 = (await pedalState()).order;
ok(a2.indexOf(9) === after.indexOf(9) - 1, "the earlier button: the delay one place earlier");
// the chain by keyboard: arrows choose, Space switches, Alt + arrows move
await page.focus('#tiles .tile[data-b="9"] .tsel');
await page.keyboard.press("ArrowRight");
await sleep(200);
const nextB = a2[a2.indexOf(9) + 1];
ok((await pedalState()).focus === nextB, "keyboard: the right arrow chooses the next block of the chain");
const onB = (await pedalState()).on[nextB];
await page.keyboard.press("Space");
await sleep(200);
ok((await pedalState()).on[nextB] === (onB ? 0 : 1), "keyboard: Space switches the block");
await page.keyboard.press("Alt+ArrowRight");
await sleep(300);
const a3 = (await pedalState()).order;
ok(a3.indexOf(nextB) === a2.indexOf(nextB) + 1, "keyboard: Alt + right arrow moves the block one place later");
// a setting with two named values is a pair of buttons (the rotary's SPEED)
await page.click('#tiles .tile[data-b="8"] .tsel');
await page.selectOption("#type", { label: "ROTARY" });
await sleep(250);
ok(await page.locator("#p0").getAttribute("role") === "radiogroup", "ROTARY's SPEED (SLOW / FAST) is two buttons");
await page.locator("#p0 button", { hasText: "FAST" }).click();
await sleep(250);
ok(await page.locator("#p0 button", { hasText: "FAST" }).getAttribute("aria-checked") === "true", "FAST chosen");

// ---- name, save, undo, compare ----
await page.fill("#name", "MY BIG TONE");
await page.press("#name", "Enter");
await sleep(200);
ok((await pedalState()).name === "MY BIG TONE", "renamed on the pedal");
await clearToasts();
await page.keyboard.press("Control+s");
ok(await toast(/Saved as 01 MY BIG TONE/), "Ctrl + S saves");
ok(await text("#star") === "" && /Yours/.test(await row(0).textContent()), "saved: no more Edited, 01 is marked Yours");
await shot("editor_4_saved.png");
await page.locator("#toasts .toast-a").click();
ok(await toast(/Undone/), "Undo after the save");
ok(!/Yours/.test(await row(0).textContent()) && await text("#star") === "Edited" && (await pedalState()).name === "MY BIG TONE", "undone: 01 is the factory preset again, and the edited sound is still playing");
await clearToasts();
await page.click("#save");
ok(await toast(/Saved as 01/), "saved again");
await page.click('#tiles .tile[data-b="5"] .tsel');
const savedGain = (await pedalState()).amp[1];
await slide("#p1", 0.2);
const editedGain = (await pedalState()).amp[1];
await page.click("#compare");
await page.waitForFunction(() => /Hearing the saved preset/.test(document.getElementById("star").textContent), null, { timeout: 3000 });
ok((await pedalState()).amp[1] === savedGain && await page.locator("#block").evaluate((e) => e.inert), `Compare: the pedal plays the saved preset (gain ${savedGain}), the editor waits`);
await page.click("#compare");
await sleep(300);
ok((await pedalState()).amp[1] === editedGain && await text("#star") === "Edited", `Compare again: back to the edited sound (gain ${editedGain})`);
await page.click("#revert");
await sleep(300);
ok((await pedalState()).amp[1] === savedGain && await text("#star") === "", "Revert: the saved preset again");

// ---- presets: load, follow the pedal, unsaved changes ----
await row(6).click();
await sleep(300);
ok((await pedalState()).slot === 6 && await text("#num") === "07", "click on 07 in the list: the pedal loads it");
sim.stdin.write("#pedal preset 2\n");
await page.waitForFunction(() => document.getElementById("num").textContent === "03", null, { timeout: 3000 }).catch(() => {});
ok(await text("#num") === "03" && await row(2).getAttribute("aria-selected") === "true", "footswitch on the pedal: the page follows (03)");
await page.click('#tiles .tile[data-b="5"] .tsel');
await slide("#p1", 0.25);
await row(3).click();
await dialog();
ok(/not saved/.test(await text("#dlg")), "leaving an unsaved sound asks first: save, drop or stay");
await shot("editor_5_unsaved.png");
await page.locator("#dlg button", { hasText: "Cancel" }).click();
ok((await pedalState()).slot === 2, "Cancel: still on the same preset");

// ---- save as, with what it replaces said first ----
await page.click("#saveas");
await dialog();
await page.click("#pick-9");
ok(/factory preset BRIT CRUNCH/.test(await text(".pick-note")) && /Save as 10/.test(await text("#dlg-ok")), "Save as: choosing 10 says what is there (a factory preset) before anything is written");
await clearToasts();
await page.click("#dlg-ok");
ok(await toast(/Saved as 10/), "Save as 10");
ok((await pedalState()).slot === 9 && /Yours/.test(await row(9).textContent()), "saved in 10, which is now the current preset");
const name10 = await page.inputValue("#name");
await page.click("#saveas");
await dialog();
await page.click("#pick-0");
ok(/your preset MY BIG TONE/.test(await text(".pick-note")) && /Replace 01 MY BIG TONE/.test(await text("#dlg-ok")), "Save as over a preset of yours: the button names what it replaces");
await page.locator("#dlg button", { hasText: "Cancel" }).click();

// ---- move, copy, rename, back to factory ----
await clearToasts();
await row(9).locator(".more").click();
await menuItem("Move up");
ok(await toast(/is preset 09 now/), "Move up");
ok((await pedalState()).slot === 8 && (await row(8).textContent()).includes(name10) && /KLON CRUNCH/.test(await row(9).textContent()), "the preset is 09 now, the one that was there is 10, and the pedal followed");
await clearToasts();
await page.focus("#list");
await page.keyboard.press("Alt+ArrowDown");
ok(await toast(/is preset 10 now/), "keyboard: Alt + down arrow in the list moves the preset back");
ok(/KLON CRUNCH/.test(await row(8).textContent()) && !/Yours/.test(await row(8).textContent()), "a factory preset moved away and back is the factory preset again, not a copy of yours");
await clearToasts();
await row(9).locator(".more").click();
await menuItem("Move to…");
await dialog();
await page.click("#pick-19");
ok(/becomes preset 20/.test(await text(".pick-note")), "Move to…: it says where the preset goes and what moves");
await page.click("#dlg-ok");
ok(await toast(/is preset 20 now/), "moved to 20");
ok((await row(19).textContent()).includes(name10) && /70S ROCK/.test(await row(9).textContent()) && (await pedalState()).slot === 19, "20 holds it, the presets between moved up by one");
await page.locator("#toasts .toast-a").click();
ok(await toast(/Undone: the move/), "Undo of the move");
ok((await row(9).textContent()).includes(name10) && /70S ROCK/.test(await row(10).textContent()) && !/Yours/.test(await row(10).textContent()), "everything is back where it was, the factory presets factory again");
await clearToasts();
await row(9).locator(".more").click();
await menuItem("Copy to…");
await dialog();
await page.click("#pick-40");
await page.click("#dlg-ok");
ok(await toast(/is also preset 41 now/), "Copy to 41");
ok((await row(40).textContent()).includes(name10), "41 holds the copy");
await clearToasts();
await row(40).locator(".more").click();
await menuItem("Rename…");
await dialog();
await page.fill("#dlg-input", "THE COPY");
await page.click("#dlg-ok");
ok(await toast(/Preset 41 is THE COPY now/), "Rename a preset that is not the current one");
await clearToasts();
await row(40).locator(".more").click();
await menuItem("Back to factory");
await dialog();
ok(/THE COPY leaves preset 41/.test(await text("#dlg")), "Back to factory asks, and names the preset that goes");
await page.click("#dlg-ok");
ok(await toast(/factory .* again/), "41 is the factory preset again");
ok(!/Yours/.test(await row(40).textContent()), "41 no longer marked Yours");

// ---- search, filter ----
await page.keyboard.press("/");
ok(await page.evaluate(() => document.activeElement.id === "psearch"), "the / key goes to the search field");
await page.fill("#psearch", "jcm900");
const found = await page.locator("#list .row").allTextContents();
ok(found.length > 0 && found.length < 52 && found.every((t) => /JCM900/.test(t)), `search "jcm900": ${found.length} presets whose amp is the JCM900`);
await page.fill("#psearch", "zzzz");
ok(/No preset matches/.test(await text("#listempty")), "a search with no result says so");
await page.fill("#psearch", "");
await page.click('#pfilter [data-f="user"]');
ok(await page.locator("#list .row").count() === 2, "filter Yours: the two presets saved here");
await page.click('#pfilter [data-f="all"]');

// ---- export, import ----
const [dl1] = await Promise.all([page.waitForEvent("download"), (async () => { await row(9).locator(".more").click(); await menuItem("Export"); })()]);
const one = JSON.parse(readFileSync(await dl1.path(), "utf8"));
ok(one.format === "openpocket-presets" && one.presets.length === 1 && one.presets[0].name === name10, `Export: ${dl1.suggestedFilename()}`);
const [dl2] = await Promise.all([page.waitForEvent("download"), page.click("#expall")]);
const all = JSON.parse(readFileSync(await dl2.path(), "utf8"));
ok(all.presets.length === 52 && all.presets[0].name === "MY BIG TONE" && all.presets[0].factory === false, `Back up all: 52 presets (${dl2.suggestedFilename()})`);
// a file of my own: two presets, renamed, into 30 and 01 (01 holds a preset of mine)
const mine = { format: "openpocket-presets", version: 1, presets: [
  { ...all.presets[0], slot: 30, name: "IMPORTED ONE", factory: false },
  { ...all.presets[8], slot: 1, name: "IMPORTED TWO", factory: false } ] };
const f2 = join(TMP, "two.json");
writeFileSync(f2, JSON.stringify(mine));
await page.setInputFiles("#file", f2);
await dialog();
ok(/replaces your MY BIG TONE/.test(await text("#dlg")) && /over factory/.test(await text("#dlg")) && /Import 2 presets/.test(await text("#dlg-ok")), "Import: the plan, preset by preset, with what each replaces");
await shot("editor_5_import.png");
await clearToasts();
await page.click("#dlg-ok");
ok(await toast(/2 presets imported/), "imported");
ok((await row(29).textContent()).includes("IMPORTED ONE") && (await row(0).textContent()).includes("IMPORTED TWO"), "the two presets are in 30 and 01");
await page.locator("#toasts .toast-a").click();
ok(await toast(/Undone: the import/), "Undo of the import");
ok((await row(0).textContent()).includes("MY BIG TONE") && !/Yours/.test(await row(29).textContent()), "01 is MY BIG TONE again, 30 factory again");
const f1 = join(TMP, "one.json");
writeFileSync(f1, JSON.stringify({ ...one, presets: [{ ...one.presets[0], name: "FROM A FILE" }] }));
await clearToasts();
await page.setInputFiles("#file", f1);
ok(await toast(/FROM A FILE is playing now/), "Import one preset: it becomes the current sound");
ok((await pedalState()).name === "FROM A FILE" && await text("#star") === "Edited", "playing, not saved anywhere yet");
const bad = join(TMP, "bad.json");
writeFileSync(bad, "{ not json");
await clearToasts();
await page.setInputFiles("#file", bad);
ok(await toast(/not an OpenPocket preset file/), "a file that is not a preset file is refused, with its name");
ok((await pedalState()).writesOk, "the flash was written only inside the preset store");
await page.click("#revert");
await sleep(200);

// ---- v37: NAM captures: slots, the library, what uses them ----
const namFile = (seed, name = "") => {
  const KS = [6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6];
  const DS = [1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239];
  let s = seed; const rnd = () => { s = (s * 1103515245 + 12345) >>> 0; return s / 4294967296 - 0.5; };
  const w = Array.from({ length: 1871 }, () => rnd() * 0.6); w[1870] = 0.02;
  return JSON.stringify({ version: "0.7.0", metadata: { name }, architecture: "WaveNet", sample_rate: 44100, weights: w, config: { head: null, head_scale: 0.02, layers: [{
    input_size: 1, condition_size: 1, head: { out_channels: 1, kernel_size: 16, bias: true }, channels: 3, bottleneck: 3,
    kernel_sizes: KS, dilations: DS, activation: KS.map(() => ({ type: "LeakyReLU", negative_slope: 0.01 })),
    head1x1: { active: false }, layer1x1: { active: true }, gating_mode: KS.map(() => "none") }] } });
};
const srow = (kind, slot) => page.locator(`#${kind}list .srow[data-slot="${slot}"]`);
const free = (kind, slot) => page.locator(`#${kind}list .free [data-slot="${slot}"]`);
{
  await go("models");
  ok(await page.locator("#namlist .free .slot").count() === 16 && /0 of 16 slots used/.test(await text("#namcap")), "Models: the 16 user slots, all empty, and how many are used");
  ok(/12 factory models/.test(await text("#nam .factory summary")), "the 12 factory models, apart");
  await shot("editor_6_models_empty.png");
  const fn = join(TMP, "Page Amp.nam"), fb = join(TMP, "Second.nam"), fa1 = join(TMP, "Old A1.nam");
  writeFileSync(fn, namFile(3)); writeFileSync(fb, namFile(11));
  writeFileSync(fa1, JSON.stringify({ version: "0.5.4", architecture: "WaveNet", weights: [0.1], config: { layers: [{ channels: 16, kernel_sizes: [3], dilations: [1] }, { channels: 8 }] } }));
  await clearToasts();
  const [ch] = await Promise.all([page.waitForEvent("filechooser"), page.click("#namadd")]);
  ok(ch.isMultiple(), "Add .nam files takes several files at once");
  await ch.setFiles([fn, fa1, fb]);
  ok(await toast(/2 captures on the pedal, 1 not added/, 30000), "three files: two captures sent to the first free slots, the A1 file refused");
  ok((await srow("nam", 0).textContent()).includes("PAGE AMP") && (await srow("nam", 1).textContent()).includes("SECOND") && /2 of 16 slots used/.test(await text("#namcap")), "U01 PAGE AMP, U02 SECOND");
  ok(/not an A2 model/i.test(await text("#nam .queue")) && /Page Amp\.nam/.test(await srow("nam", 0).textContent()), "the list says why the A1 file was refused; each slot says which file it came from");
  ok((await pedalState()).writesOk, "the flash was written only inside the preset store and the model slots");
  await shot("editor_6_nam.png");
  // a preset that plays U01
  await go("sound");
  await page.click('#tiles .tile[data-b="5"] .tsel');
  await page.selectOption("#type", { label: "NAM" });
  await sleep(200);
  ok(/U01\s+PAGE AMP/.test(await text("#p0")) && await page.locator('#p0 option:disabled').count() === 14, "the AMP block's MODEL list: your captures by slot, the empty slots not selectable");
  await pickOpt("#p0", /^U01\s+PAGE AMP$/);
  await sleep(250);
  ok(Math.floor((await pedalState()).amp[0] * 28 / 101) === 12 && /From the file Page Amp\.nam/.test(await text("#blocknote")), "U01 chosen: the pedal plays model 12, the block says where the capture comes from");
  await clearToasts();
  await page.click("#save");
  await toast(/Saved as/);
  const user = (await pedalState()).slot;
  await go("models");
  await page.waitForFunction(() => /1 preset/.test(document.querySelector('#namlist .srow[data-slot="0"]').textContent), null, { timeout: 5000 }).catch(() => {});
  ok(/1 preset/.test(await srow("nam", 0).textContent()) && /Not used/.test(await srow("nam", 1).textContent()), "U01 is used by 1 preset, U02 by none");
  // move U01 to U05: the preset follows
  await clearToasts();
  await srow("nam", 0).locator(".btn.icon").click();
  await menuItem("Move to another slot…");
  await dialog();
  await page.click("#pick-4");
  ok(/PAGE AMP goes to U05/.test(await text(".pick-note")) && /1 preset is updated/.test(await text(".pick-note")), "Move to U05: it says the preset that uses it will follow");
  await page.click("#dlg-ok");
  ok(await toast(/PAGE AMP is in U05 now\. 1 preset updated/, 30000), "moved");
  ok((await srow("nam", 4).textContent()).includes("PAGE AMP") && await free("nam", 0).count() === 1 && /1 preset/.test(await srow("nam", 4).textContent()), "U05 holds it, U01 is empty, and the preset uses U05");
  ps = await pedalState();
  ok(ps.slot === user && Math.floor(ps.amp[0] * 28 / 101) === 16, `the saved preset now plays model 16 (U05): knob ${ps.amp[0]}`);
  // swap with U02
  await clearToasts();
  await srow("nam", 4).locator(".btn.icon").click();
  await menuItem("Move to another slot…");
  await dialog();
  await page.click("#pick-1");
  ok(/change places/.test(await text(".pick-note")), "moving onto a used slot: the two change places");
  await page.click("#dlg-ok");
  ok(await toast(/PAGE AMP is in U02 now/, 40000), "swapped");
  ok((await srow("nam", 1).textContent()).includes("PAGE AMP") && (await srow("nam", 4).textContent()).includes("SECOND"), "U02 PAGE AMP, U05 SECOND");
  // rename
  await clearToasts();
  await srow("nam", 1).locator(".btn.icon").click();
  await menuItem("Rename…");
  await dialog();
  await page.fill("#dlg-input", "My Plexi");
  await page.click("#dlg-ok");
  ok(await toast(/U02 is MY PLEXI now/, 30000), "Rename a capture (sent again under its new name)");
  ok((await srow("nam", 1).textContent()).includes("MY PLEXI"), "U02 MY PLEXI");
  // remove: what uses it is said first; it stays in the library
  await srow("nam", 1).locator(".btn.icon").click();
  await menuItem("Remove from the pedal");
  await dialog();
  ok(/1 preset uses it/.test(await text("#dlg")) && /without an amp/.test(await text("#dlg")) && /stays in the library/.test(await text("#dlg")), "Remove: the preset that uses it is named, and it stays in the library");
  await shot("editor_6_remove.png");
  await clearToasts();
  await page.click("#dlg-ok");
  ok(await toast(/U02 is empty/), "removed from the pedal");
  ok(/MY PLEXI/.test(await text("#namlib")), "MY PLEXI is in the library, not on the pedal");
  ok(await free("nam", 1).evaluate((e) => e.classList.contains("need")) && /U02 \(\d\d [^)]*\): presets point to an empty slot/.test(await text("#namlist")), "the empty slot a preset still points to is marked, with the preset's name");
  await go("sound");
  await page.click('#tiles .tile[data-b="5"] .tsel');
  ok(/U02 is empty: this preset plays without an amp/.test(await text("#blocknote")), "the AMP block of the preset that used it warns: empty slot");
  await go("models");
  await clearToasts();
  await page.locator("#namlib .srow", { hasText: "MY PLEXI" }).locator("button", { hasText: "Send to the pedal" }).click();
  ok(await toast(/MY PLEXI is in U02/, 30000), "Send to the pedal: back from the library into the slot it left");
  await page.waitForFunction(() => /1 preset/.test(document.querySelector('#namlist .srow[data-slot="1"]')?.textContent || ""), null, { timeout: 5000 }).catch(() => {});
  ok(/1 preset/.test(await srow("nam", 1).textContent()) && !/point to an empty slot/.test(await text("#namlist")), "the preset that named U02 plays the capture again: no warning left");
  // a library kept across a reload of the page
  ok(await page.evaluate(async () => { const r = indexedDB.open("openpocket-library"); const db = await new Promise((res) => { r.onsuccess = () => res(r.result); }); const n = await new Promise((res) => { const q = db.transaction("files").objectStore("files").count(); q.onsuccess = () => res(q.result); }); db.close(); return n; }) === 2, "the library holds the two captures in the browser's own storage");
}

// ---- v38: cabinet IRs, the same way ----
{
  const wav = (n = 2048, sr = 48000) => { const b = Buffer.alloc(44 + n * 2); b.write("RIFF", 0); b.writeUInt32LE(36 + n * 2, 4); b.write("WAVEfmt ", 8); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write("data", 36); b.writeUInt32LE(n * 2, 40); for (let i = 0; i < n; i++) b.writeInt16LE(Math.round(24000 * Math.exp(-i / 180) * Math.cos(i * 0.21)), 44 + i * 2); return b; };
  const fw = join(TMP, "Green 412.wav");
  writeFileSync(fw, wav());
  await clearToasts();
  const [ch] = await Promise.all([page.waitForEvent("filechooser"), free("ir", 2).click()]);
  await ch.setFiles(fw);
  ok(await toast(/1 IR on the pedal/, 20000), "a WAV file loaded into IR slot 3 from its own row");
  ok((await srow("ir", 2).textContent()).includes("GREEN 412") && /1 of 8 slots used/.test(await text("#ircap")) && /resampled from 48 kHz/.test(await text("#ir .queue")), "IR3 GREEN 412, with what was done to the file");
  ok((await pedalState()).writesOk, "the flash was written only inside the preset store, the model slots and the IR slots");
}

// ---- v37: TONE3000 (their site simulated: the pop-up, PKCE, the lists, a tone, its A2 models, the download) ----
{
  const T3K = "https://www.tone3000.com";
  let challenge = "", authOk = false, tokenOk = false, bearerOk = true, pick = "42";
  const cors = { "access-control-allow-origin": "*", "access-control-allow-headers": "authorization, content-type", "access-control-allow-methods": "GET, POST, OPTIONS" };
  const json = (route, o) => route.fulfill({ status: 200, headers: { ...cors, "content-type": "application/json" }, body: JSON.stringify(o) });
  const tone = (id, title, a2) => ({ id, title, gear: "amp", format: "nam", images: null, a2_models_count: a2, url: `${T3K}/tones/${id}`, description: "A capture for the test.",
    user: { username: "capturer", display_name: null, avatar_url: null, url: `${T3K}/capturer` } });
  await ctx.route(`${T3K}/**`, async (route) => {
    const r = route.request(), u = new URL(r.url());
    if (r.method() === "OPTIONS") return route.fulfill({ status: 204, headers: cors });
    if (u.pathname === "/api/v1/oauth/authorize") {
      const q = u.searchParams;
      challenge = q.get("code_challenge");
      authOk = q.get("client_id") === "page-test" && q.get("prompt") === "select_tone" && q.get("architecture") === "2" &&
        q.get("format") === "nam" && q.get("code_challenge_method") === "S256" && /\/t3k\.html$/.test(q.get("redirect_uri"));
      const back = pick === "cancel" ? `canceled=true` : `tone_id=${pick}`;
      return route.fulfill({ status: 302, headers: { location: `${q.get("redirect_uri")}?code=C0DE&state=${q.get("state")}&${back}` } });
    }
    if (u.pathname === "/api/v1/oauth/token") {
      const f = new URLSearchParams(r.postData() || "");
      const v = createHash("sha256").update(f.get("code_verifier") || "").digest("base64url");
      tokenOk = f.get("grant_type") === "authorization_code" && f.get("code") === "C0DE" && v === challenge;
      return json(route, tokenOk ? { access_token: "AT", refresh_token: "RT", token_type: "bearer", expires_in: 3600 } : { error: "invalid_grant" });
    }
    if (r.headers()["authorization"] !== "Bearer AT") bearerOk = false;
    if (u.pathname === "/api/v1/user") return json(route, { username: "vince", display_name: null, avatar_url: null, url: `${T3K}/vince` });
    if (u.pathname === "/api/v1/tones/42") return json(route, tone(42, "Plexi Lead", 2));
    if (u.pathname === "/api/v1/tones/favorited") return json(route, { data: [tone(42, "Plexi Lead", 2), tone(45, "Old Recto", 0)], page: 1, page_size: 50, total: 2, total_pages: 1 });
    // (a tone whose addresses are not web addresses: the page must not turn them into links)
    if (u.pathname === "/api/v1/tones/trending") return json(route, { data: [{ ...tone(46, "Top Boost", 1), url: "javascript:alert(1)", user: { username: "odd", url: "javascript:alert(2)", avatar_url: "javascript:alert(3)" } }] });
    if (u.pathname === "/api/v1/models") {
      const id = u.searchParams.get("tone_id");
      const data = id === "42" ? [{ id: 7, name: "Plexi Lead A2", size: "standard", architecture_version: "2", model_url: `${T3K}/files/7.nam`, tone_id: 42 },
        { id: 8, name: "Plexi Dimed", size: "standard", architecture_version: "2", model_url: `${T3K}/files/8.nam`, tone_id: 42 }] : [];
      return json(route, { data, page: 1, page_size: 100, total: data.length, total_pages: 1 });
    }
    const f = /^\/files\/(\d+)\.nam$/.exec(u.pathname);
    if (f) return route.fulfill({ status: 200, headers: cors, body: namFile(+f[1] * 7) });
    return route.fulfill({ status: 404, headers: cors });
  });
  await page.evaluate(() => { window.__t3kClientId = "page-test"; });
  ok(await page.isVisible("#t3kbrowse") && /TONE3000/.test(await text("#t3k .t3k-logo")) && /sign in there/.test(await text("#t3k")), "TONE3000: its name, what it is, and a Browse button, before any sign-in");
  await shot("editor_7_tone3000_intro.png");
  await page.click("#t3kbrowse");
  await page.waitForSelector("#t3kcard", { timeout: 10000 });
  ok(authOk && tokenOk, "TONE3000: Select pop-up (A2, .nam), sign-in with PKCE, back to the page");
  await page.waitForFunction(() => document.querySelectorAll("#t3kcard .mrow").length === 2, null, { timeout: 5000 });
  ok(/Plexi Lead/.test(await text("#t3kcard")) && /capturer/.test(await text("#t3kcard")) && /AMP · NAM/.test(await text("#t3kcard")) && /A capture for the test/.test(await text("#t3kcard")), "TONE3000: the tone (title, gear, format, creator, description)");
  ok(/vince/.test(await text("#t3k .card-head")), "TONE3000: who is signed in");
  ok(/Next free slot \(U01\)/.test(await text("#t3kinto")), "TONE3000: the next free slot proposed (U01)");
  await clearToasts();
  await page.locator("#t3kcard .mrow button").first().click();
  ok(await toast(/1 capture on the pedal/, 30000), "TONE3000: one capture got");
  ok((await srow("nam", 0).textContent()).includes("PLEXI LEAD") && /T3K\s+Plexi Lead by capturer/.test(await srow("nam", 0).textContent()) && bearerOk,
    "TONE3000: the A2 model downloaded (with the token) into U01, with where it comes from");
  await shot("editor_7_tone3000.png");
  await page.locator("#t3kcard .back").click();
  await page.waitForSelector("#t3klist .tone", { timeout: 5000 });
  await sleep(100);
  ok(await page.locator("#t3klist .tone").count() === 2 && /2 A2/.test(await text("#t3klist")) && /No A2/.test(await text("#t3klist")), "TONE3000: your favorites, each with how many A2 captures it has");
  await page.locator("#t3k [role=tab]", { hasText: "Trending" }).click();
  await page.waitForFunction(() => /Top Boost/.test(document.getElementById("t3klist").textContent), null, { timeout: 5000 });
  ok(true, "TONE3000: the trending list");
  await page.locator("#t3klist .tone").first().click();
  await page.waitForFunction(() => /no A2 capture/.test(document.getElementById("t3kcard")?.textContent || ""), null, { timeout: 5000 }).catch(() => {});
  ok(/no A2 capture/.test(await text("#t3kcard")), "TONE3000: a tone without an A2 capture says the pedal cannot play it");
  ok(await page.locator('#t3kcard [href^="javascript"], #t3kcard [src^="javascript"]').count() === 0 && /odd/.test(await text("#t3kcard")), "TONE3000: an address that is not a web address is shown as text, never as a link");
  pick = "cancel";
  await page.click("#t3kbrowse");
  await page.waitForSelector("#t3klist", { timeout: 10000 });
  ok(await page.isEnabled("#t3kbrowse"), "TONE3000: their browser closed without a tone: back to the lists, nothing broken");
  await go("sound");
  await page.click('#tiles .tile[data-b="5"] .tsel');
  await pickOpt("#p0", /^U01\s+PLEXI LEAD$/);
  await sleep(250);
  ok(/T3K/.test(await text("#blocknote")) && /Plexi Lead/.test(await text("#blocknote")) && /capturer/.test(await text("#blocknote")), "the AMP block names the TONE3000 tone and its creator");
}

// ---- MIDI: the pedal's numbers, and a controller taught through the page ----
{
  await go("midi");
  ok(/Foot Controller/.test(await text("#midictl")) && /hold FX for 0\.8 s/.test(await text("#midiconn")), "MIDI: the controller the browser sees, and where the pedal's own settings are");
  const ampRow = page.locator("#midimap tbody tr").nth(5);
  ok(/AMP/.test(await ampRow.locator("th").textContent()) && /CC 25/.test(await ampRow.textContent()) && /CC 57/.test(await ampRow.textContent()) && /GAIN\s*CC 78/.test(await ampRow.textContent()), "the pedal's own map: AMP on/off 25, type 57, and GAIN (NAM's 2nd setting) 78");
  await shot("editor_8_midi.png");
  await page.click('.cc-cell[data-t="param:5:1"]');
  ok(/Waiting for a control for AMP · GAIN/.test(await text("#learnstate")), "Learn: click a control, the page waits for the controller");
  await page.evaluate(() => window.__ctl([0xB0, 11, 100]));
  await sleep(150);
  ok(/AMP · GAIN now follows CC 11 · ch 1 of Foot Controller, in every preset/.test(await text("#learnstate")), "a knob moved on the controller is learned");
  ok(/CC 11 · ch 1/.test(await text('.cc-cell[data-t="param:5:1"]')) && await page.locator("#learned .lrow").count() === 1, "the control shows it, and it is listed under Learned controls");
  await page.evaluate(() => window.__ctl([0xB0, 11, 127]));
  await sleep(400);
  ok((await pedalState()).amp[1] === 100, "the controller's knob at 127: the pedal's AMP GAIN is 100");
  await page.evaluate(() => window.__ctl([0xB0, 11, 32]));
  await sleep(400);
  ok((await pedalState()).amp[1] === 25, "at 32: 25 (the pedal's own scaling)");
  ok(/AMP · GAIN 25/.test(await text("#monlist")), "the monitor shows what arrived and what it did");
  // a footswitch: each press flips the block
  await page.click('.cc-cell[data-t="on:9:0"]');
  await page.evaluate(() => window.__ctl([0xB0, 80, 127]));
  await sleep(200);
  const d0 = (await pedalState()).on[9];
  await page.evaluate(() => { window.__ctl([0xB0, 80, 0]); window.__ctl([0xB0, 80, 127]); });
  await sleep(400);
  ok((await pedalState()).on[9] === (d0 ? 0 : 1), "a footswitch learned for DLY on / off: a press flips it, the release does nothing");
  // only in this preset
  await page.selectOption("#midiscope", "preset");
  await page.click('.cc-cell[data-t="param:10:2"]');
  await page.evaluate(() => window.__ctl([0xB1, 12, 64]));
  await sleep(200);
  const here = (await pedalState()).slot;
  ok(new RegExp(`Only ${String(here + 1).padStart(2, "0")}`).test(await page.locator("#learned .lrow").nth(2).locator("select").last().evaluate((s) => s.selectedOptions[0].textContent)), "a control taught for this preset only says so");
  // Program Change and the preset steps
  await page.evaluate(() => window.__ctl([0xC0, 4]));
  await page.waitForFunction(() => document.getElementById("num").textContent === "05", null, { timeout: 3000 }).catch(() => {});
  ok((await pedalState()).slot === 4, "Program Change 4 from the controller loads preset 05");
  const rev = (await pedalState()).type[10];
  await page.evaluate(() => window.__ctl([0xB1, 12, 127]));
  await sleep(300);
  ok(/not learned/.test(await page.locator("#monlist li").first().textContent()), "the control taught for another preset does nothing here");
  await page.click('#midimap [data-t="preset:1"]');
  await page.evaluate(() => window.__ctl([0xB0, 81, 127]));
  await sleep(200);
  await page.evaluate(() => window.__ctl([0xB0, 81, 127]));
  await page.waitForFunction(() => document.getElementById("num").textContent === "06", null, { timeout: 3000 }).catch(() => {});
  ok((await pedalState()).slot === 5 && (await pedalState()).type[10] !== undefined && rev !== undefined, "a footswitch learned for Next preset goes to 06");
  await page.locator("#learned .lrow").first().locator(".btn.icon").click();
  ok(await page.locator("#learned .lrow").count() === 3, "Forget removes a learned control");
  ok(await page.evaluate(() => JSON.parse(localStorage.getItem("openpocket-midi-v1")).mappings.length) === 3, "the learned controls are kept in the browser");
  // what the pedal sends itself (SEND on): the monitor names it
  await page.evaluate(() => window.__simIn("b01940"));
  await sleep(200);
  ok(/Pedal/.test(await page.locator("#monlist li").first().textContent()) && /AMP on \/ off/.test(await page.locator("#monlist li").first().textContent()), "a Control Change the pedal sends is named in the monitor");
  await shot("editor_8_midi_learned.png");
}

// ---- the pedal unplugged, and back ----
{
  await go("sound");
  sim.stdin.end();
  await page.evaluate(() => window.__simGone());
  await page.waitForSelector('body[data-conn="lost"]', { timeout: 8000 });
  ok(await page.isVisible("#lostbar") && await page.locator("#shell").evaluate((e) => e.inert), "the pedal unplugged: said at the top, the editor held still");
  await shot("editor_9_lost.png");
  startSim();
  await page.waitForSelector('body[data-conn="ready"]', { timeout: 20000 });
  ok(await page.isHidden("#lostbar") && await page.locator("#shell").evaluate((e) => !e.inert), "plugged back in: connected again by itself");
  await go("models");
  ok((await srow("nam", 0).textContent()).includes("PLEXI LEAD") && /T3K/.test(await srow("nam", 0).textContent()), "after the reconnection the slots still know where their captures came from");
}

// ---- v42: USB audio (pull request #25), in the MIDI section: off by default; the button asks, sets it, and the editor
// connects again by itself ----
{
  await go("midi");
  await page.waitForSelector("#uastate", { timeout: 8000 });
  ok(await page.textContent("#uastate") === "Off" && await page.textContent("#uabtn") === "Turn on", "USB audio: shown, off by default");
  await page.click("#uabtn");
  await dialog();
  ok(/Turn USB audio on\?/.test(await text("#dlg")) && /leaves USB/.test(await text("#dlg")), "USB audio: asks first, and says the pedal leaves USB");
  await page.click("#dlg-ok");
  const back = (state) => page.waitForFunction((t) => document.body.dataset.conn === "ready" &&
    (document.getElementById("uastate") || {}).textContent === t, state, { timeout: 20000 });
  await back("On");
  ok(await page.textContent("#uabtn") === "Turn off", "USB audio: on, and the editor connected again");
  await shot("editor_8_usb_audio.png");
  await page.click("#uabtn");
  await dialog();
  await page.click("#dlg-ok");
  await back("Off");
  ok(await page.textContent("#uabtn") === "Turn on", "USB audio: off again");
  await go("sound");
}

// ---- phone width ----
await page.setViewportSize({ width: 390, height: 844 });
await sleep(300);
for (const v of ["presets", "sound", "models", "midi"]) {
  await go(v);
  await sleep(150);
  const sw = await page.evaluate(() => document.documentElement.scrollWidth);
  ok(sw <= 390, `phone width, ${v}: no sideways scroll (${sw} px)`);
  await shot(`editor_10_phone_${v}.png`);
}
await go("sound");
const tw = await page.locator("#tiles .tile .pw").first().boundingBox();
ok(tw.width >= 44 && tw.height >= 24, `phone: a block's switch is ${Math.round(tw.width)} x ${Math.round(tw.height)} px`);

await browser.close();
sim.stdin.end();
server.close();
console.log(failed ? `\n${failed} FAILED` : "\neditor page test passed");
process.exit(failed ? 1 : 0);
