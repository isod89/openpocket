// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — `node tools/op.mjs log` (tools/log.mjs): the DIAG request against the pedal's real editor and status-text code
// (tests/edsim.c built with -DOP_BLE_TEST=1), the same against firmware without it, and the states where less can be read.
//   cc -O1 -DOP_BLE_TEST=1 -o build/edsim_diag tests/edsim.c engine/px_dsp.c firmware/px_math.c -lm && node tests/test_log.mjs
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { BOOT_STAGES, RESET_CAUSES, judge, main, parseBle, parseBoot, parseLast } from "../tools/log.mjs";
import { simAccess } from "./simpedal.mjs";

const ROOT = fileURLToPath(new URL("..", import.meta.url));
let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(72)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// a simulated pedal on a fake Web MIDI access; ble() steers its Bluetooth state
function sim(bin) {
  const child = spawn(join(ROOT, "build", bin), [], { stdio: ["pipe", "pipe", "inherit"] });
  const access = { inputs: new Map(), outputs: new Map() };
  let input = null, buf = "", readyFn;
  const ready = new Promise((r) => { readyFn = r; });
  const states = [];
  child.stdout.on("data", (d) => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const ln = buf.slice(0, i); buf = buf.slice(i + 1);
      if (ln.startsWith("#port ")) {
        const name = ln.slice(6);
        input = { name, state: "connected", onmidimessage: null, open: async () => {} };
        access.inputs.set(name, input);
        access.outputs.set(name, { name, state: "connected", open: async () => {}, send: (b) => child.stdin.write(Buffer.from(b).toString("hex") + "\n") });
        readyFn();
      } else if (ln.startsWith("#state ")) states.push(ln);
      else if (ln && input?.onmidimessage) input.onmidimessage({ data: Uint8Array.from(Buffer.from(ln, "hex")) });
    }
  });
  const done = new Promise((r) => child.on("exit", r));
  return { access, ready, states, ble: (l) => child.stdin.write(`#ble ${l}\n`), dump: () => child.stdin.write("#dump\n"), end: () => { child.stdin.end(); return done; } };
}

async function run(access, argv, opts = {}) {
  const out = [];
  const code = await main(argv, { access, say: (l) => out.push(l), ...opts });
  return { code, text: out.join("\n"), last: argv.includes("--json") ? JSON.parse(out[out.length - 1]) : null };
}

// ---- the text formats ----
const adv = "ADV i12 e5 r0 t340 s0 CON 2001 IEN 101 IPND 0 STAT 80 IC45 3 IC29 3 BEN 1";
let b = parseBle(adv);
ok(b.state === "advertising" && b.i === 12 && b.t === 340 && b.CON === 0x2001 && b.IC45 === 3 && b.BEN === 1 && !b.vm, "advertising line, every field");
b = parseBle("ADV i12 e5 r0 t340 s0 V 7 A3 12AB CON 2001 IEN 101 IPND 0 STAT 80 IC45 3 IC29 3 BEN 1");
ok(b.vm?.found === 7 && b.vm.area === 0xA3 && b.vm.trim === 0x12AB && b.CON === 0x2001, "advertising line with the stored radio records");
b = parseBle("FAILED 5 3  step timeout stage radio-init wait 01C08000");
ok(b.state === "failed" && b.step === 5 && b.code === 3 && b.stage === "radio-init" && b.wait === "01C08000", "failure line");
ok(parseBle("FAILED 2 -7  step btctrler_task_init stage init").code === -7, "failure line, negative code");
ok(parseBle("STARTING 4").stage === 4 && parseBle("OFF").state === "off", "starting and off");
ok(parseBle("ADVERTISING?").state === "unknown", "a line this tool does not know stays unknown");
b = parseBoot("UP 12345 SAFE 1 STAGE 7 RST 44 GUARD 1");
ok(b.clockMs === 12345 && b.safe && b.stageName === "FIRST 6 S" && b.resetBy.join() === "WATCHDOG,SOFT" && b.failedBoots === 1, "boot line");
b = parseLast("LAST FAILED STAGE 7 BLOCKS 9 LOAD 42 PEAK 61 NAN 0 CPU1 1 LOOPS 0 CRASH VEC 3F PC 2000DC8 RETS 2001234 DBG 0 EMU 8 UP 4100");
ok(b.failed && b.load === 42 && b.crash.vec === 0x3F && b.crash.pc === 0x2000DC8 && b.crash.emu === 8 && b.crash.clockMs === 4100, "last-boot line with a crash");
ok(!parseLast("LAST OK").failed, "last boot fine");

const fw = readFileSync(join(ROOT, "firmware/openpocket.c"), "utf8");
const strings = (s) => [...s.matchAll(/"([^"]*)"/g)].map((m) => m[1]);
ok(strings(/ST_NAME\[ST_N\] = \{([^}]*)\}/.exec(fw)[1]).join() === BOOT_STAGES.join(), "boot stage names match firmware/openpocket.c");
ok(strings(/N\[7\] = \{([^}]*)\}/.exec(fw)[1]).join() === RESET_CAUSES.join(), "reset causes match firmware/openpocket.c");

// ---- the verdicts on the real firmware text ----
const fast = ["--interval", "0.01", "--json"];
const seen = { ok: true, seconds: 10, hits: 42, others: 9, name: "OpenPocket", rssi: -60, connectable: true };
const nothing = { ok: true, seconds: 10, hits: 0, others: 9, name: null };

let s = sim("edsim_diag");
await s.ready;
let r = await run(s.access, fast);
ok(r.code === 1 && /Bluetooth is off/.test(r.last.verdict.reason), "firmware text: OFF in a normal boot fails");
ok(r.last.level === "full" && r.last.boot.clockMs === 30000 && r.last.lastBoot.failed === false, "DIAG reaches the parser through the real editor code");
ok(/^LINK 1 n2 A 060504030201 I48 /.test(r.last.link), "the link line comes through");

s.ble("failed 5 3");
r = await run(s.access, fast);
ok(r.code === 1 && /still starting after 5 s/.test(r.last.verdict.reason) && /code 3/.test(r.last.verdict.reason), `FAILED in words: ${r.last.verdict.reason}`);

s.ble("adv 100 500");
r = await run(s.access, [...fast, "--count", "1"]);
ok(r.code === 3 && r.last.verdict.result === "pending", "one reading of counters decides nothing");
r = await run(s.access, fast);
ok(r.code === 1 && /no baseband events/i.test(r.last.verdict.reason), "counters that stand still fail");

let n = 100;
const rising = async (scan) => {
  const t = setInterval(() => s.ble(`adv ${n += 10} ${n * 5}`), 5);
  const out = await run(s.access, ["--interval", "0.1", "--json", ...(scan ? ["--scan", "5"] : [])], scan ? { scan: async () => scan } : {});
  clearInterval(t);
  return out;
};
r = await rising();
ok(r.code === 0 && r.last.verdict.result === "running" && /not proof/.test(r.last.verdict.reason), "rising counters: running, and not called proof");
let v = judge([{ level: "full", time: "2026-11-06T13:00:00Z", boot: { stage: 8, clockMs: 178000 }, ble: parseBle("ADV i10 e0 r0 t0 s2 V 0 0 0 CON 6001 IEN 101 IPND 0 STAT 0 IC45 5 IC29 5 BEN 1") },
  { level: "full", time: "2026-11-06T13:00:02Z", boot: { stage: 8, clockMs: 1000 }, ble: parseBle("ADV i30 e0 r0 t0 s2 V 0 0 0 CON 6001 IEN 101 IPND 0 STAT 0 IC45 5 IC29 5 BEN 1") }]);
ok(v.result === "running" && /10\.0 a second/.test(v.reason) && v.notes.length === 3, `events without timers still run, across a clock wrap, with notes: ${v.notes.length}`);
r = await rising(seen);
ok(r.code === 0 && r.last.verdict.result === "pass" && r.last.scan.hits === 42, "rising counters and a sighting: pass");
r = await rising(nothing);
ok(r.code === 1 && /saw no advertisement/.test(r.last.verdict.reason), "rising counters and no sighting: fail");
r = await rising({ ok: false, why: "the Bluetooth adapter is not usable" });
ok(r.code === 3 && r.last.verdict.result === "unknown", "a scanner that could not run decides nothing");

s.ble("safe");
r = await run(s.access, fast);
ok(r.code === 1 && /last boot failed/i.test(r.last.verdict.reason) && /FIRST 6 S/.test(r.last.verdict.reason) && /WATCHDOG/.test(r.last.verdict.reason) && /safe mode/.test(r.last.verdict.reason), `safe mode in words: ${r.last.verdict.reason}`);
r = await run(s.access, ["--interval", "0.01", "--count", "1"]);
ok(/last boot  FAILED/.test(r.text) && /verdict    FAIL/.test(r.text) && /SAFE MODE/.test(r.text), "the plain report says the same");

s.dump(); await sleep(50);
const before = s.states.at(-1);
await run(s.access, fast);
s.dump(); await sleep(50);
ok(s.states.at(-1) === before && before.split("|")[2] === "1", "reading changes nothing on the pedal");
await s.end();

// ---- what less can be read ----
s = sim("edsim");
await s.ready;
r = await run(s.access, fast);
ok(r.code === 3 && r.last.level === "info" && r.last.ping.load === 42 && /no log channel/.test(r.last.verdict.reason), "firmware without DIAG: version and load, and says what is missing");
await s.end();

const blank = join(mkdtempSync(join(tmpdir(), "oplog-")), "flash.bin");
writeFileSync(blank, Buffer.alloc(0x100000, 0xFF));
const dev = simAccess([blank, "--stock", "BlackBox_016"]);
await dev.ready;
r = await run(dev.access, fast);
ok(r.code === 3 && r.last.level === "identity" && /identity/.test(r.last.verdict.reason), `stock firmware: ${r.last.verdict.reason}`);
await dev.end();

const port = (name) => ({ name, state: "connected", onmidimessage: null, open: async () => {}, send: () => {} });
const mute = { inputs: new Map([["OpenPocket", port("OpenPocket")]]), outputs: new Map([["OpenPocket", port("OpenPocket")]]) };
r = await run(mute, fast);
ok(r.code === 3 && r.last.level === "silent" && /nothing answers/.test(r.last.verdict.reason), "a port that does not answer");
r = await run({ inputs: new Map(), outputs: new Map() }, fast);
ok(r.code === 3 && r.last.level === "none" && /CRASH screen/.test(r.last.verdict.reason), "no port: names the screens this cannot read");

// --wait: the pedal shows up during the wait
s = sim("edsim_diag");
await s.ready;
s.ble("failed 1 -1");
const late = { inputs: new Map(), outputs: new Map() };
setTimeout(() => { for (const k of ["inputs", "outputs"]) for (const [n, p] of s.access[k]) late[k].set(n, p); }, 300);
r = await run(late, ["--wait", "5", "--interval", "0.05", "--count", "1", "--json"]);
ok(r.code === 1 && r.last.ble.step === 1, "--wait reads the pedal once it appears");
await s.end();

r = await run({ inputs: new Map(), outputs: new Map() }, ["--nonsense"]);
ok(r.code === 2 && /usage/.test(r.text), "a bad argument prints the usage");
ok(judge([{ level: "none" }]).result === "unknown", "no reading is never a pass");

if (failed) { console.log(`log test: ${failed} FAILED`); process.exit(1); }
console.log("log test passed");
process.exit(0);
