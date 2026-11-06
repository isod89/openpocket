// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — the command-line flasher (tools/flash.mjs, behind `node tools/op.mjs flash`) against the simulated pedal
// (tests/devsim.c: the real ota.c and loader code), plus its Node MIDI adapter against a fake MIDI library. No hardware.
//   cc -O1 -o build/devsim tests/devsim.c && node tests/test_flash.mjs
import { execFileSync, spawnSync } from "node:child_process";
import { copyFileSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { main } from "../tools/flash.mjs";
import { Updater } from "../docs/pxota.js";
import { logicalImage } from "../docs/pxpkg.js";
import { nodeMidiAccess } from "../tools/midi-node.mjs";
import { loopAccess, simAccess } from "./simpedal.mjs";

const ROOT = fileURLToPath(new URL("..", import.meta.url));
const TMP = mkdtempSync(join(tmpdir(), "opflash-"));
let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(70)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };
const FAST = { afterUpgrade: 50, afterStep1: 50, loaderWait: 5000, step1Idle: 3000, step2Idle: 5000, afterWrite: 50,
  backWait: 5000, perRequest: 0, poll: 50 };
execFileSync("python3", [join(ROOT, "tests", "fixtures.py"), TMP], { stdio: "inherit" });

const man = join(TMP, "manifest.json"), px = join(TMP, "PocketX_924.fwsc"), bb = join(TMP, "BlackBox_924.fwsc");
const old = join(TMP, "flash_old.bin");
const appOf = `
import sys; sys.path.insert(0, sys.argv[2])
import pocketx_fw as fw
d = fw.flash_decrypt(open(sys.argv[1], 'rb').read())
e = fw.flash_parse(d)
bad = [k for k, v in fw.flash_checks(d, e).items() if not v]
app = d[0x4000 + e[1]['addr']:0x4000 + e[1]['addr'] + e[1]['len']]
i = app.find(b'OP-PRODUCT:')
print(app[i + 11:i + 31].split(b'\\0')[0].decode(), 'CRC-OK' if not bad else 'CRC-BAD')`;
const appIn = (dump) => execFileSync("python3", ["-c", appOf, dump, join(ROOT, "tools")]).toString().trim();

// run the flasher on a simulated pedal; resolves { code, out, log, dump }
async function run(argv, { send, flash = old } = {}) {
  const dump = join(TMP, `dump${Math.random().toString(36).slice(2)}.bin`);
  const sim = simAccess([flash, "--dump", dump]);
  await sim.ready;
  if (send) for (const o of sim.access.outputs.values()) { const s = o.send; o.send = (b) => send(b, s); }
  const lines = [];
  const code = await main(argv, { access: sim.access, say: (l) => lines.push(l), timing: FAST });
  await sim.end();
  return { code, out: lines.join("\n"), log: sim.log, dump: readFileSync(dump) };
}
const untouched = (r) => r.dump.equals(readFileSync(old)) && !r.log.some((l) => /step[12]/.test(l));
const notContacted = { inputs: { values() { throw new Error("MIDI was touched"); } }, outputs: { values() { throw new Error("MIDI was touched"); } } };
const say = (lines) => (l) => lines.push(l);

// 1. no confirmation: refused before any MIDI
{
  const lines = [];
  const code = await main(["--package", px, "--manifest", man], { access: notContacted, say: say(lines), timing: FAST });
  ok(code === 2 && /--confirm-flash/.test(lines.join("\n")), "no --confirm-flash: refused, MIDI never touched");
}

// 2. dry run: detects the pedal, checks the package, writes nothing
{
  const r = await run(["--dry-run", "--manifest", man]);
  ok(r.code === 0 && /pedal: PocketX_923/.test(r.out) && /PocketX_924\.fwsc.*matches the manifest/.test(r.out) && /dry run ok/.test(r.out), "dry run: pedal found, its model's package checked");
  ok(untouched(r), "dry run: the pedal's flash is unchanged and no update step ran");
  const both = await run(["--dry-run", "--confirm-flash", "--package", px, "--manifest", man]);
  ok(both.code === 0 && untouched(both), "--dry-run wins over --confirm-flash");
}

// 3. the update: 923 -> 924, version verified afterwards
{
  const r = await run(["--confirm-flash", "--package", px, "--manifest", man]);
  ok(r.code === 0 && /done: the pedal reports PocketX_924/.test(r.out), `update 923 -> 924 reports the new version ${r.code ? "\n" + r.out : ""}`);
  const f = join(TMP, "after.bin"); writeFileSync(f, r.dump);
  ok(appIn(f) === "PocketX_924 CRC-OK", "flash now holds app PocketX_924, every CRC right");
}

// 4. wrong model: refused, nothing sent
{
  const r = await run(["--confirm-flash", "--package", bb, "--manifest", man]);
  ok(r.code === 2 && /wrong model/.test(r.out) && untouched(r), "BlackBox package on a Pocket X: refused, flash unchanged");
}

// 5. damaged or unlisted package: refused before any MIDI
{
  const d2 = join(TMP, "PocketX_925.fwsc");
  const bytes = readFileSync(px); bytes[0x20000] ^= 0x55;
  writeFileSync(d2, bytes);
  const lines = [];
  let code = await main(["--confirm-flash", "--package", d2, "--manifest", man], { access: notContacted, say: say(lines), timing: FAST });
  ok(code === 2 && /not listed/.test(lines.join("\n")), "a package the manifest does not list: refused");
  const bytes2 = readFileSync(px); bytes2[0x20000] ^= 0x55;
  const damaged = join(TMP, "damaged"); execFileSync("mkdir", ["-p", damaged]);
  writeFileSync(join(damaged, "PocketX_924.fwsc"), bytes2); copyFileSync(man, join(damaged, "manifest.json"));
  lines.length = 0;
  code = await main(["--confirm-flash", "--package", join(damaged, "PocketX_924.fwsc"), "--manifest", join(damaged, "manifest.json")], { access: notContacted, say: say(lines), timing: FAST });
  ok(code === 2 && /SHA-256 mismatch/.test(lines.join("\n")), "damaged package (one byte): SHA-256 mismatch, MIDI never touched");
}

// 6. pedal problems: none, several
{
  const lines = [];
  const code = await main(["--dry-run", "--manifest", man], { access: { inputs: new Map(), outputs: new Map() }, say: say(lines), timing: FAST });
  ok(code === 2 && /no OpenPocket-compatible pedal found/.test(lines.join("\n")), "no pedal: a clear error");
  const a = simAccess([old]), b = simAccess([old]);
  await a.ready; await b.ready;
  for (const p of [...b.access.inputs.values(), ...b.access.outputs.values()]) p.name = "OpenPocket B";
  const both = { inputs: new Map([["a", [...a.access.inputs.values()][0]], ["b", [...b.access.inputs.values()][0]]]),
    outputs: new Map([["a", [...a.access.outputs.values()][0]], ["b", [...b.access.outputs.values()][0]]]) };
  lines.length = 0;
  const two = await main(["--confirm-flash", "--package", px, "--manifest", man], { access: both, say: say(lines), timing: FAST });
  await a.end(); await b.end();
  ok(two === 2 && /2 pedals answered/.test(lines.join("\n")) && !a.log.concat(b.log).some((l) => /step/.test(l)), "two pedals: refused, nothing written");
}

// 7. a failure midway prints the SD-card recovery pointer
{
  const r = await run(["--confirm-flash", "--package", px, "--manifest", man], { send: (bytes, real) => { if (bytes.length > 100) throw new Error("unplugged"); return real(bytes); } });
  ok(r.code === 1 && /FAILED/.test(r.out) && /SD-card/.test(r.out) && /README\.md/.test(r.out), "a failed update exits 1 and points to the SD-card recovery");
}

// 8. the Node MIDI adapter over a fake @julusian/midi: ports are listed afresh, SysEx goes both ways
{
  const names = ["Other", "OpenPocket"], sent = [], handlers = [];
  const midi = {
    Input: class { getPortCount() { return names.length; } getPortName(i) { return names[i]; } ignoreTypes() {} on(_, f) { this.f = f; handlers.push(f); } openPort(i) { this.i = i; } closePort() {} },
    Output: class { getPortCount() { return names.length; } getPortName(i) { return names[i]; } openPort(i) { this.i = i; } sendMessage(m) { sent.push([this.i, m]); } closePort() {} },
  };
  const acc = nodeMidiAccess(midi);
  const ins = [...acc.inputs.values()], outs = [...acc.outputs.values()];
  ok(ins.map((p) => p.name).join() === "Other,OpenPocket" && ins[0].state === "connected", "adapter: ports listed by name");
  await ins[1].open(); await outs[1].open();
  let got = null; ins[1].onmidimessage = (e) => { got = e.data; };
  handlers[0](0, [0xF0, 1, 2, 0xF7]); outs[1].send(Uint8Array.from([0xF0, 0xF7]));
  ok(got && got.length === 4 && sent.length === 1 && sent[0][0] === 1, "adapter: SysEx received and sent on the right port");
  names.pop(); names.push("OpenPocket Update");
  ok([...acc.inputs.values()].map((p) => p.name).join() === "Other,OpenPocket Update" && ins[1].state === "disconnected", "adapter: a re-enumerated port shows under its new name");
  acc.close();
}

// 9. the command line itself: no confirmation, no library needed, no MIDI
{
  const r = spawnSync(process.execPath, [join(ROOT, "tools/op.mjs"), "flash"], { encoding: "utf8" });
  ok(r.status === 2 && /--confirm-flash/.test(r.stdout), "node tools/op.mjs flash (no flag): refused with exit 2");
}

// 10. --rescue against a pedal that restarts in a loop (USB ports there for a moment, gone, there again)
async function rescueRun(argv, loop) {
  const dump = join(TMP, `dump${Math.random().toString(36).slice(2)}.bin`);
  const sim = simAccess([old, "--dump", dump]);
  await sim.ready;
  const lp = loopAccess(sim, loop);
  const lines = [];
  const t0 = Date.now();
  const code = await main(argv, { access: lp.access, say: (l) => lines.push(l), timing: FAST });
  const cycles = lp.cycles();
  await lp.end();
  return { code, out: lines.join("\n"), log: sim.log, dump: readFileSync(dump), cycles, secs: (Date.now() - t0) / 1000 };
}
const rescueArgs = ["--rescue", "--confirm-flash", "--package", px, "--manifest", man];
{
  const lines = [];
  const code = await main(["--rescue", "--package", px, "--manifest", man], { access: notContacted, say: say(lines), timing: FAST });
  ok(code === 2 && /--confirm-flash/.test(lines.join("\n")), "rescue without --confirm-flash: refused, MIDI never touched");
  const bad = await main(["--rescue", "--timeout", "0", "--confirm-flash"], { access: notContacted, say: say(lines), timing: FAST });
  ok(bad === 2, "rescue with a timeout of 0: refused");
}
{
  const r = await rescueRun([...rescueArgs, "--timeout", "30"], { answerOn: 3 });
  ok(r.code === 0 && r.cycles === 3 && /done: the pedal reports PocketX_924/.test(r.out), `rescue: the pedal answers on the 3rd restart and is installed (cycles ${r.cycles}, ${r.secs}s) ${r.code ? "\n" + r.out : ""}`);
  ok(/still waiting|pedal answered/.test(r.out) && /pedal answered: PocketX_923/.test(r.out), "rescue: progress lines say when the pedal answered");
  const f = join(TMP, "rescued.bin"); writeFileSync(f, r.dump);
  ok(appIn(f) === "PocketX_924 CRC-OK" && r.log.filter((l) => /step1/.test(l)).length === 1, "rescue: flash holds PocketX_924, every CRC right, one transfer");
}
{
  const r = await rescueRun([...rescueArgs, "--timeout", "2"], { answerOn: 1e9 });
  ok(r.code === 1 && /no pedal answered in 2 s/.test(r.out) && /WL80UBOOT/.test(r.out) && untouched(r) && r.secs < 15, `rescue: a pedal that never answers times out, exit 1, nothing written (${r.secs}s)`);
}
{
  const r = await rescueRun(["--rescue", "--confirm-flash", "--package", bb, "--manifest", man, "--timeout", "10"], { answerOn: 2 });
  ok(r.code === 2 && /wrong model/.test(r.out) && untouched(r), "rescue: a BlackBox package on a Pocket X is refused, nothing sent");
}
{
  const r = await rescueRun(["--rescue", "--confirm-flash", "--manifest", man, "--timeout", "30"], { answerOn: 2 });
  ok(r.code === 0 && /PocketX_924/.test(r.out) && !/BlackBox_924/.test(r.out.split("pedal answered")[1] || ""), "rescue: without --package the pedal's own model's package is picked");
}
{
  const r = await rescueRun(["--rescue", "--dry-run", "--manifest", man, "--timeout", "30"], { answerOn: 2 });
  ok(r.code === 0 && /dry run ok/.test(r.out) && untouched(r), "rescue --dry-run: waits for the pedal, checks the model, writes nothing");
}
{
  const r = await rescueRun([...rescueArgs, "--timeout", "30"], { answerOn: 1, vanishAfterSends: 3 });
  ok(r.code === 0 && /went away/.test(r.out) && /done: the pedal reports PocketX_924/.test(r.out), `rescue: the pedal vanishes mid-transfer, returns, and is installed ${r.code ? "\n" + r.out : ""}`);
}

// 11. a fresh --rescue finds the pedal in the OpenPocket update loader an interrupted install left behind
async function loaderRun(argv) {
  const dump = join(TMP, `dump${Math.random().toString(36).slice(2)}.bin`);
  const sim = simAccess([old, "--dump", dump]);
  await sim.ready;
  const up = new Updater(sim.access, null, FAST);
  try { await up.installOn(await up.find((id) => id.model === "PocketX"), logicalImage(readFileSync(px)), "PocketX_924", (s) => { if (s === "loader") throw new Error("interrupted"); }); } catch (_) { /* left in the loader */ }
  const lines = [];
  const code = await main(argv, { access: sim.access, say: (l) => lines.push(l), timing: FAST });
  await sim.end();
  return { code, out: lines.join("\n"), log: sim.log, dump: readFileSync(dump) };
}
{
  const r = await loaderRun([...rescueArgs, "--timeout", "30"]);
  const f = join(TMP, "loader_rescued.bin"); writeFileSync(f, r.dump);
  ok(r.code === 0 && /done: the pedal reports PocketX_924/.test(r.out) && appIn(f) === "PocketX_924 CRC-OK", `rescue: a pedal left in its update loader is finished with the given package ${r.code ? "\n" + r.out : ""}`);
}
{
  const r = await loaderRun(["--rescue", "--confirm-flash", "--manifest", man, "--timeout", "2"]);
  ok(r.code === 1 && /no pedal answered in 2 s/.test(r.out) && /--package/.test(r.out) && !r.log.some((l) => /step2/.test(l)), "rescue: a loader without --package is not guessed at, the timeout says to name the package");
}

console.log(failed ? `\n${failed} FAILED` : "\nall flasher tests passed");
process.exit(failed ? 1 : 0);
