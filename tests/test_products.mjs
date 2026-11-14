// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — two products from one base, on the simulated pedal (tests/devsim.c: the pedal's own update code, loader
// and product reply): the flasher and the update library install an alternative firmware, tell it from OpenPocket,
// and put OpenPocket back. Checked: the product a pedal says it runs, the guard that keeps an alternative firmware from
// being installed for OpenPocket, the way back (also for a pedal that restarts in a loop), what the flash keeps through
// both installs (OpenPocket's presets, models and IRs; the alternative firmware's own data), and what it does not.
//   cc -O1 -o build/devsim tests/devsim.c && node tests/test_products.mjs [--built]
// --built: also with the packages the build made (build/apps/starter, docs/firmware); they must exist
// (`node tools/op.mjs all` runs it so). Without it only the made-up packages of tests/fixtures.py are used.
import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { main } from "../tools/flash.mjs";
import { Updater, IS_ALT, OPENPOCKET, firmwareName } from "../docs/pxota.js";
import { logicalImage } from "../docs/pxpkg.js";
import { loopAccess, simAccess } from "./simpedal.mjs";

const ROOT = fileURLToPath(new URL("..", import.meta.url));
const TMP = mkdtempSync(join(tmpdir(), "opprod-"));
let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(100)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };
const FAST = { afterUpgrade: 50, afterStep1: 50, loaderWait: 5000, step1Idle: 3000, step2Idle: 5000, afterWrite: 50,
  backWait: 5000, perRequest: 0, poll: 50, info: 300 };
execFileSync("python3", [join(ROOT, "tests", "fixtures.py"), TMP], { stdio: "ignore" });

// the app the flash holds: its identity, its product descriptor's id, the checksums of the flash's tables; its bytes
const appOf = `
import sys; sys.path.insert(0, sys.argv[2])
import pocketx_fw as fw, pxpkg
d = fw.flash_decrypt(open(sys.argv[1], 'rb').read())
e = fw.flash_parse(d)
bad = [k for k, v in fw.flash_checks(d, e).items() if not v]
app = d[0x4000 + e[1]['addr']:0x4000 + e[1]['addr'] + e[1]['len']]
if len(sys.argv) > 3: open(sys.argv[3], 'wb').write(app)
i = app.find(b'OP-PRODUCT:')
info = pxpkg.app_info(app)
print(app[i + 11:i + 31].split(b'\\0')[0].decode(), info['id'] if info else 'openpocket', 'CRC-OK' if not bad else 'CRC-BAD')`;
const appIn = (dump, out) => execFileSync("python3", ["-c", appOf, dump, join(ROOT, "tools"), ...(out ? [out] : [])]).toString().trim();
const patched = (bin, id, out) => execFileSync("python3", ["-c", `
import sys; sys.path.insert(0, sys.argv[1]); import pxpkg
open(sys.argv[4], 'wb').write(pxpkg.with_product(open(sys.argv[2], 'rb').read(), sys.argv[3]))`, join(ROOT, "tools"), bin, id, out]);
const same = (a, w) => { const x = readFileSync(a), y = readFileSync(w);
  return x.length >= y.length && x.subarray(0, y.length).equals(y) && x.subarray(y.length).every((b) => b === 0xFF); };

// the flasher on a simulated pedal whose flash is the file `flash`; resolves { code, out, dump (a file) }
// mute: the pedal never hears the question "which product are you?" (INFO), so it never says
async function flasher(flash, argv, { loop, mute } = {}) {
  const dump = join(TMP, `dump${Math.random().toString(36).slice(2)}.bin`);
  const sim = simAccess([flash, "--dump", dump]);
  await sim.ready;
  if (mute) {
    const INFO = "f07d4f5001f7", ports = sim.access.outputs, set = ports.set.bind(ports);
    const hush = (o) => { const send = o.send; o.send = (b) => { if (Buffer.from(b).toString("hex") !== INFO) send(b); }; return o; };
    for (const o of ports.values()) hush(o);
    ports.set = (k, o) => set(k, hush(o));               // (the port comes back under a new object after each restart)
  }
  const dev = loop ? loopAccess(sim, loop) : null;
  const lines = [];
  const code = await main(argv, { access: dev ? dev.access : sim.access, say: (l) => lines.push(l), timing: FAST });
  if (dev) await dev.end(); else await sim.end();
  return { code, out: lines.join("\n"), dump };
}
// what a pedal with this flash says it runs (the update library's identify)
async function runs(flash) {
  const sim = simAccess([flash]);
  await sim.ready;
  const d = await new Updater(sim.access, null, FAST).identify();
  await sim.end();
  return d;
}

// ---- the start: a pedal on an older OpenPocket, with something in every part of the flash that must outlive an install
const f0 = readFileSync(join(TMP, "flash_old.bin"));
for (const [at, s] of [[0xF0000, "OPENPOCKET PRESETS"], [0xC8000, "OPENPOCKET NAM MODELS"], [0xF4000, "OPENPOCKET IRS"]]) f0.write(s, at, "latin1");
// an update record, as the loader and the boot code know one (80 bytes at a 4 KiB boundary - 256): "AT" at 6, its CRC16 first
const crc16 = (b) => { let c = 0; for (const x of b) { c ^= x << 8; for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF; } return c; };
const record = Buffer.alloc(80, 0x5A); record[6] = 0x41; record[7] = 0x54; record.writeUInt16LE(crc16(record.subarray(2)), 0);
// the alternative firmware's own data, where firmware/base/store.h keeps it (0xE5000..0xEF000): every sector full of
// such records up to 0xF00, its last 256 bytes erased
for (let s = 0xE5000; s < 0xEF000; s += 0x1000) { f0.fill(0xFF, s, s + 0x1000); for (let o = 0; o + 80 <= 0xF00; o += 80) record.copy(f0, s + o); }
// and, to show what the erased bytes are for: the sector above the store, ending in a record
f0.fill(0xFF, 0xEF000, 0xF0000); record.copy(f0, 0xEFF00);
const start = join(TMP, "start.bin");
writeFileSync(start, f0);
const KEPT = [[0xC0000, 0xE0000, "OpenPocket's NAM models"], [0xF0000, 0xF4000, "OpenPocket's presets"], [0xF4000, 0xFC000, "OpenPocket's IRs"],
  [0xE5000, 0xEF000, "the alternative firmware's own data"]];
const kept = (dump) => KEPT.filter(([a, b]) => !readFileSync(dump).subarray(a, b).equals(f0.subarray(a, b))).map((k) => k[2]);
const unchanged = (dump, flash) => readFileSync(dump).equals(readFileSync(flash));

const P = { alt: join(TMP, "alt"), op: join(TMP, "op42"), alt42: join(TMP, "alt42") };
const altPkg = join(P.alt, "starter-PocketX_901.fwsc"), altMan = join(P.alt, "manifest.json"), opMan = join(P.op, "manifest.json");

// ---- the guard: an alternative firmware is never installed for OpenPocket
let r = await flasher(start, ["--confirm-flash", "--package", altPkg, "--manifest", altMan]);
ok(r.code === 2 && /STARTER v01, an alternative firmware, not OpenPocket/.test(r.out) && /--app starter/.test(r.out) && unchanged(r.dump, start),
  "an alternative firmware's package without --app: refused, its name and the option given, nothing written");
r = await flasher(start, ["--confirm-flash", "--manifest", altMan]);
ok(r.code === 2 && /not OpenPocket/.test(r.out) && unchanged(r.dump, start), "its manifest alone, without --app: refused the same way");
r = await flasher(start, ["--confirm-flash", "--app", "chroma", "--manifest", altMan]);
ok(r.code === 2 && /not chroma/.test(r.out) && unchanged(r.dump, start), "--app with another firmware's name: refused");
r = await flasher(start, ["--confirm-flash", "--app", "starter", "--manifest", opMan]);
ok(r.code === 2 && /lists OpenPocket, not the alternative firmware starter/.test(r.out) && unchanged(r.dump, start), "--app with OpenPocket's manifest: refused");
r = await flasher(start, ["--confirm-flash", "--app", "starter", "--package", join(P.alt, "starter-BlackBox_901.fwsc"), "--manifest", altMan]);
ok(r.code === 2 && /wrong model/.test(r.out) && unchanged(r.dump, start), "its BlackBox package on a Pocket X: refused, nothing written");

// ---- OpenPocket -> the alternative firmware
let d = await runs(start);
ok(d && d.product === null && firmwareName(d.id, d.product) === "OpenPocket v23", "an OpenPocket too old to say which product it is (v23) is taken at its identity");
r = await flasher(start, ["--dry-run", "--app", "starter", "--manifest", altMan]);
ok(r.code === 0 && /would update PocketX_923 \(OpenPocket v23\) -> PocketX_901 \(STARTER v01, an alternative firmware\)/.test(r.out) && unchanged(r.dump, start),
  "dry run with --app: it says what would replace what, and writes nothing");
r = await flasher(start, ["--confirm-flash", "--app", "starter", "--manifest", altMan]);
ok(r.code === 0 && /done: the pedal reports PocketX_901 \(STARTER v01, an alternative firmware\)/.test(r.out), `install with --app: done, and the pedal says it runs STARTER${r.code ? "\n" + r.out : ""}`);
ok(/To go back to OpenPocket: node tools\/op\.mjs flash --confirm-flash/.test(r.out), "the flasher ends with the way back to OpenPocket");
ok(appIn(r.dump) === "PocketX_901 starter CRC-OK", "the flash holds the app with the product descriptor \"starter\", every checksum right");
ok(kept(r.dump).length === 0, `through that install the flash keeps OpenPocket's presets, models and IRs, and the app's own data${kept(r.dump).length ? ": NOT " + kept(r.dump).join(", ") : ""}`);
ok(readFileSync(r.dump).subarray(0xEF000, 0xF0000).every((b) => b === 0xFF), "a sector that ends like an update record is erased by the loader (so the store keeps those bytes erased)");
const onAlt = r.dump;

r = await flasher(start, ["--confirm-flash", "--app", "starter", "--manifest", altMan], { mute: true });
ok(r.code === 1 && /written, but the pedal runs a firmware that does not say which it is/.test(r.out) && !/done:/.test(r.out),
  "the flasher checks the product after an install: a pedal that does not say it runs STARTER is a failure, not a done");

// ---- on the alternative firmware: every tool says which product runs
d = await runs(onAlt);
ok(d && IS_ALT(d.product) && d.product.app === "starter" && d.product.name === "STARTER" && d.product.version === "v01" && d.product.identity === "PocketX_901",
  "the pedal says which product it runs: STARTER v01, id starter");
ok(d && d.product.caps.join() === "stereo,midi,store" && d.product.editor === 1, "and what it can do: a stereo sound, a MIDI map, its own store, its own editor requests");
ok(d && d.id.text === "PocketX_901" && firmwareName(d.id, d.product) === "STARTER v01, an alternative firmware", "its identity is PocketX_901 as before: an installer of any age still finds it");
r = await flasher(onAlt, ["--dry-run", "--manifest", opMan]);
ok(r.code === 0 && /pedal: PocketX_901 \(STARTER v01, an alternative firmware\)/.test(r.out) && /-> PocketX_942 \(OpenPocket v42\)/.test(r.out), "the flasher names it, and OpenPocket as what would replace it");

// ---- the way back
r = await flasher(onAlt, ["--confirm-flash", "--manifest", opMan]);
ok(r.code === 0 && /done: the pedal reports PocketX_942 \(OpenPocket v42\)/.test(r.out) && !/To go back/.test(r.out), `the way back: the flasher without --app installs OpenPocket${r.code ? "\n" + r.out : ""}`);
ok(appIn(r.dump) === "PocketX_942 openpocket CRC-OK" && kept(r.dump).length === 0, "the flash holds OpenPocket again; its presets, models and IRs and the app's own data are as they were");
const onOp = r.dump;
d = await runs(onOp);
ok(d && d.product && d.product.app === OPENPOCKET && !IS_ALT(d.product) && d.product.version === "v42", "and the pedal says it runs OpenPocket v42");

// ---- a pedal on the alternative firmware that restarts in a loop: rescue installs OpenPocket
r = await flasher(onAlt, ["--rescue", "--confirm-flash", "--timeout", "20", "--manifest", opMan], { loop: { up: 500, down: 250, answerOn: 3 } });
ok(r.code === 0 && /done: the pedal reports PocketX_942 \(OpenPocket v42\)/.test(r.out) && appIn(r.dump) === "PocketX_942 openpocket CRC-OK",
  `rescue of a looping pedal on the alternative firmware: OpenPocket is installed the moment it answers${r.code ? "\n" + r.out : ""}`);

// ---- the same number for both products (an alternative firmware at v42): only the product tells them apart
r = await flasher(onOp, ["--confirm-flash", "--app", "starter", "--manifest", join(P.alt42, "manifest.json")]);
ok(r.code === 0 && /PocketX_942 \(OpenPocket v42\) -> PocketX_942 \(STARTER v01, an alternative firmware\)/.test(r.out) && appIn(r.dump) === "PocketX_942 starter CRC-OK",
  "an alternative firmware with OpenPocket's own number: installed, told apart by its product");
{   // the check after an install: the pedal must run the product that was installed, not only report its number
  const sim = simAccess([onOp]);
  await sim.ready;
  const image = logicalImage(new Uint8Array(readFileSync(join(P.op, "PocketX_942.fwsc"))));
  let err = null;
  try { await new Updater(sim.access, null, FAST).install(image, "PocketX_942", null, { app: "starter" }); } catch (e) { err = e; }
  await sim.end();
  ok(err && err.code === "mismatch" && /runs OpenPocket v42/.test(err.message), "an install that ends on another product than the one asked for is reported, though the number is right");
}

// ---- the packages the build made
if (process.argv.includes("--built")) {
  const app = join(ROOT, "build/apps/starter"), rel = join(ROOT, "docs/firmware");
  const relMan = JSON.parse(readFileSync(join(rel, "manifest.json"), "utf8")), appMan = JSON.parse(readFileSync(join(app, "manifest.json"), "utf8"));
  const relId = relMan.packages.find((p) => p.model === "PocketX").product, appId = appMan.packages.find((p) => p.model === "PocketX").product;
  ok(!relMan.app && appMan.app && appMan.app.id === "starter" && appMan.packages.every((p) => p.file.startsWith("starter-")),
    "built: OpenPocket's manifest names no app; the starter's names it, and its packages carry its name");
  r = await flasher(start, ["--confirm-flash", "--app", "starter"]);
  const a1 = join(TMP, "app1.bin"), w1 = join(TMP, "want1.bin");
  ok(r.code === 0 && appIn(r.dump, a1) === `${appId} starter CRC-OK`, `built: the starter's package installs over OpenPocket (node tools/op.mjs flash --app starter)${r.code ? "\n" + r.out : ""}`);
  patched(join(app, "starter.bin"), appId, w1);
  ok(same(a1, w1) && kept(r.dump).length === 0, "built: the flash holds build/apps/starter/starter.bin byte for byte; OpenPocket's data and the app's own are kept");
  d = await runs(r.dump);
  ok(d && d.product && d.product.app === "starter" && d.product.name === appMan.app.name && d.product.version === appMan.app.version && d.product.caps.join() === appMan.app.caps.join(),
    "built: what the pedal says it runs is what the manifest says the package is");
  const built = r.dump;
  r = await flasher(built, ["--confirm-flash"]);
  const a2 = join(TMP, "app2.bin"), w2 = join(TMP, "want2.bin");
  ok(r.code === 0 && appIn(r.dump, a2) === `${relId} openpocket CRC-OK`, `built: OpenPocket's release package installs back over the starter (node tools/op.mjs flash)${r.code ? "\n" + r.out : ""}`);
  patched(join(ROOT, "firmware/openpocket.bin"), relId, w2);
  ok(same(a2, w2) && kept(r.dump).length === 0, "built: the flash holds firmware/openpocket.bin byte for byte; OpenPocket's data and the app's own are still as they were");
} else if (!existsSync(join(ROOT, "build/apps/starter/manifest.json"))) console.log("(the build's own packages: not built, not checked here; `node tools/op.mjs all` checks them)");

console.log(failed ? `\n${failed} FAILED` : "\nall product tests passed");
process.exit(failed ? 1 : 0);
