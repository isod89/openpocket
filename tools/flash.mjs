// SPDX-License-Identifier: GPL-3.0-only
// node tools/op.mjs flash: write a built OpenPocket package to a connected pedal over USB MIDI, with no browser.
// The protocol is docs/pxota.js and the package checks are docs/pxpkg.js, the very modules the installer page runs;
// this file only adds the command line: arguments, the checks that come first, and the report.
//   flash [--dry-run | --confirm-flash] [--rescue [--timeout SECONDS]] [--package FILE] [--manifest FILE] [--app NAME]
// A package is OpenPocket's unless its manifest says it is an alternative firmware (an app on the base firmware,
// docs/ALT-FIRMWARE.md). That one is written only when --app names it, so no one installs it for OpenPocket.
import { readFileSync } from "node:fs";
import { webcrypto } from "node:crypto";
import { basename, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { Updater, IS_OTA, OUR_LOADER, MODELS, TIMING, OPENPOCKET, firmwareName } from "../docs/pxota.js";
import { verifyPackage } from "../docs/pxpkg.js";

globalThis.crypto ??= webcrypto;                             // Node 18 has no global; the page's browser does

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const USAGE = `usage: node tools/op.mjs flash --dry-run [--package FILE] [--manifest FILE] [--app NAME]
       node tools/op.mjs flash --confirm-flash [--package FILE] [--manifest FILE] [--app NAME]
       node tools/op.mjs flash --rescue --confirm-flash [--timeout SECONDS] [--package FILE] [--manifest FILE]
  --dry-run        find the pedal and check the package; write nothing
  --confirm-flash  really write the package to the pedal (required: there is no default)
  --package FILE   the .fwsc to write (default: the manifest's package for the pedal's model)
  --rescue         for a pedal that restarts in a loop: wait for it to answer (plug it in, or let it restart), stop its
                   sound processing the moment it does, and install. Only a pedal whose firmware answers the MIDI
                   identity query for a moment is caught; the chip's own boot loader (WL80UBOOT) is not reachable.
  --timeout SECONDS  how long --rescue waits for the pedal (default 180); an install that has started is never cut short
  --manifest FILE  the manifest that lists the package and its SHA-256 (default: docs/firmware/manifest.json)
  --app NAME       install the alternative firmware NAME (built with \`node tools/op.mjs app NAME\`: its manifest is
                   build/apps/NAME/manifest.json) in place of OpenPocket. Without it only OpenPocket is installed,
                   which is also the way back from an alternative firmware`;
const RECOVERY = `Recovery: if the pedal does not start, power-cycle it and run this again. If it stays dead, use the SD-card
recovery (README.md, "Install": the \`flash/\` folder, fm1buddy on an ESP32-S3, \`make appimage DUMP=fm1-1.bin\` or makeimage.bat).`;

class Refusal extends Error {}                               // a check that came first: nothing was sent to the pedal
const refuse = (msg) => new Refusal(msg);

function parse(argv) {
  const a = { dryRun: false, confirm: false, rescue: false, timeout: 180, package: null, manifest: null, app: null };
  for (let i = 0; i < argv.length; i++) {
    const v = argv[i];
    if (v === "--dry-run") a.dryRun = true;
    else if (v === "--confirm-flash") a.confirm = true;
    else if (v === "--rescue") a.rescue = true;
    else if (v === "--timeout") {
      const n = Number(argv[++i]);
      if (!(n > 0)) throw refuse("--timeout needs a number of seconds above 0");
      a.timeout = n;
    } else if (v === "--package" || v === "--manifest") {
      if (i + 1 >= argv.length) throw refuse(`${v} needs a file`);
      a[v.slice(2)] = resolve(argv[++i]);
    } else if (v === "--app") {
      a.app = argv[++i];
      if (!/^[a-z0-9-]+$/.test(a.app || "")) throw refuse("--app needs the name of an alternative firmware (a folder of firmware/apps)");
    } else throw refuse(`unknown argument ${v}`);
  }
  a.manifest ??= resolve(ROOT, a.app ? `build/apps/${a.app}/manifest.json` : "docs/firmware/manifest.json");
  return a;
}

// the manifest entry for a package file (by its name) or for a model, and the product the manifest's packages are:
// `app` is the manifest's "app" (an alternative firmware: its id, name, version), null for OpenPocket.
// want: the --app given (null: OpenPocket). A manifest of the other kind is refused: nothing was asked of the pedal
function manifestEntry(manifestPath, { file, model }, want) {
  let man;
  try { man = JSON.parse(readFileSync(manifestPath, "utf8")); } catch (e) { throw refuse(`cannot read the manifest ${manifestPath}: ${e.message}${want ? ` (build it first: node tools/op.mjs app ${want})` : ""}`); }
  const app = man.app && typeof man.app.id === "string" ? man.app : null;
  if (app && want !== app.id) {
    throw refuse(`${manifestPath} lists ${app.name} ${app.version}, an alternative firmware, not OpenPocket${want ? `, and not ${want}` : ""}. ` +
      `To install it in place of OpenPocket, add --app ${app.id}. Nothing was sent.`);
  }
  if (!app && want) throw refuse(`${manifestPath} lists OpenPocket, not the alternative firmware ${want}. Nothing was sent.`);
  const entry = (man.packages || []).find((p) => (file ? p.file === file : p.model === model));
  if (!entry) throw refuse(file ? `${file} is not listed in ${manifestPath}: refusing an unlisted package` : `${manifestPath} has no package for ${model}`);
  return { entry, app };
}

async function loadPackage(args, { file, model }) {
  const { entry, app } = manifestEntry(args.manifest, { file: file && basename(file), model }, args.app);
  const path = file || resolve(dirname(args.manifest), entry.file);
  let bytes;
  try { bytes = new Uint8Array(readFileSync(path)); } catch (e) { throw refuse(`cannot read the package ${path}: ${e.message}`); }
  try { return { ...(await verifyPackage(bytes, entry)), path, sha256: entry.sha256, app }; } catch (e) { throw refuse(`${basename(path)}: ${e.message}`); }
}
// what a package is, in words, and the product the pedal must say it runs once the package is on it
const pkgName = (p) => `${p.product} (${p.app ? `${p.app.name} ${p.app.version}, an alternative firmware` : `OpenPocket v${Number(p.product.split("_")[1]) - 900}`})`;
const devName = (d) => `${d.id.text} (${firmwareName(d.id, d.product)})`;
const pkgApp = (p) => (p.app ? p.app.id : OPENPOCKET);
const wayBack = (p, say) => { if (p.app) say("To go back to OpenPocket: node tools/op.mjs flash --confirm-flash (your OpenPocket presets, NAM models and IRs were not touched)."); };

// the one pedal on the bus, or an error that says why not. Each MIDI input is asked alone (read-only identity query).
async function scan(access, timing) {
  const found = [];
  const outputs = access.outputs;
  for (const input of [...access.inputs.values()]) {
    if (input.state === "disconnected") continue;
    const probe = new Updater({ inputs: new Map([[input.name, input]]), outputs }, null, timing);
    const d = await probe.identify();
    if (d && (MODELS.includes(d.id.model) || IS_OTA(d.id))) found.push(d);
  }
  return { found, seen: new Updater(access, null, timing).seen() };
}

async function detect(access, timing) {
  const { found, seen } = await scan(access, timing);
  if (!found.length) throw refuse(`no OpenPocket-compatible pedal found${seen}. Plug the pedal in over USB, close the browser installer and any MIDI program, and try again.`);
  if (found.length > 1) throw refuse(`${found.length} pedals answered (${found.map((d) => `${d.id.text} on "${d.name}"`).join(", ")}). Connect only one.`);
  return found[0];
}

const WHY = {
  notfound: "the pedal was not found", model: "wrong model", stopped: "the transfer stopped", noloader: "the update loader did not appear",
  lost: "the pedal was disconnected", noreturn: "the pedal did not come back", mismatch: "the pedal reports another version",
  timeout: "the pedal never answered", pkg: "no package for this pedal",
  badreq: "the pedal asked for data outside the package", foreign: "the pedal is in another firmware's update mode",
};

const describe = (p) => `${basename(p.path)}: ${pkgName(p)}, SHA-256 ${p.sha256.slice(0, 16)}… matches the manifest`;

// --rescue: the pedal restarts in a loop, so it is waited for (the installer page's rescue mode, Updater.rescueWithin):
// the moment it answers the identity query its sound processing is stopped and the package of its model is installed.
// The states reached are the page's: the running firmware's MIDI port, and an OpenPocket update loader left half-done.
async function rescue(args, given, access, t, say) {
  const pkgs = {};
  if (given) pkgs[given.model] = given;
  else for (const m of MODELS) { try { pkgs[m] = await loadPackage(args, { model: m }); } catch (e) { if (args.app || /alternative firmware/.test(e.message)) throw e; /* else: no package for this model */ } }
  if (!Object.keys(pkgs).length) throw refuse(`${args.manifest} lists no package for ${MODELS.join(" or ")}`);
  if (!given) for (const p of Object.values(pkgs)) say(`package ${describe(p)}`);
  const secs = (ms) => Math.ceil(ms / 1000);
  say(`rescue: waiting up to ${args.timeout} s for the pedal to answer; plug it in over USB or let it restart. Ctrl-C stops.`);
  const end = Date.now() + args.timeout * 1000;

  if (args.dryRun) {                                         // read-only: wait until one pedal answers, then check the model
    for (;;) {
      const { found } = await scan(access, t);
      if (found.length > 1) throw refuse(`${found.length} pedals answered. Connect only one.`);
      if (found.length) {
        const dev = found[0];
        say(`pedal answered: ${devName(dev)} on "${dev.name}"`);
        const want = pkgs[dev.id.model];
        if (IS_OTA(dev.id)) {
          if (!given) throw refuse(`the pedal is in update mode (${dev.id.text}), which does not tell its model: add --package FILE to finish the update. Nothing was sent.`);
          if (!OUR_LOADER(dev.id)) throw refuse(`the pedal is in the update mode of another firmware (${dev.id.text}); a rescue only finishes an OpenPocket update. Nothing was sent.`);
          say(`it is in update mode (${dev.id.text}); a real rescue would finish the update with ${given.product}.`);
        } else if (!want) throw refuse(`wrong model: the pedal is a ${dev.id.text}, no package given for it. Nothing was sent.`);
        say(`dry run ok: a rescue would install ${pkgName(want || given)} on ${dev.id.text}. Nothing was written.`);
        return 0;
      }
      if (Date.now() >= end) throw Object.assign(new Error(`no pedal answered in ${args.timeout} s`), { code: "timeout" });
      await new Promise((r) => setTimeout(r, t.rescuePoll));
    }
  }

  let lastLeft = 0;
  const step = (s, n) => {
    if (s === "start") say(`pedal answered: ${n}: stopping its sound processing and installing; do not unplug the pedal`);
    else if (s === "verify" || s === "write") { if (n % 250 === 0) say(`  ${s}: ${n} requests`); }
    else say(`  ${s}`);
  };
  const onEvent = (kind, x) => {
    if (kind === "retry") say(`  the pedal went away before the update was complete (${x.message}); waiting for it again`);
    else if (lastLeft - x >= 5000 || lastLeft === 0) { lastLeft = x; say(`  still waiting for the pedal: ${secs(x)} s left`); }
  };
  let reported;
  try {
    reported = await new Updater(access, null, t).rescueWithin((m) => (pkgs[m] ? { image: pkgs[m].image, product: pkgs[m].product, app: pkgApp(pkgs[m]) } : null),
      step, { timeoutMs: Math.max(0, end - Date.now()), onEvent, loader: given ? { image: given.image, product: given.product, app: pkgApp(given) } : null });
  } catch (e) {
    if (e.code === "pkg") throw refuse(`wrong model: the pedal is a ${e.detail}, the package is ${Object.values(pkgs).map((p) => p.product).join(", ")}. Nothing was sent.`);
    throw e;
  }
  if (reported === null) throw Object.assign(new Error(`no pedal answered in ${args.timeout} s. A pedal that never answers the MIDI identity query (a crash before USB is up, or the chip's boot loader WL80UBOOT) cannot be reached this way. A pedal left in update mode by an interrupted install is only finished when --package names its file`), { code: "timeout" });
  const want = Object.values(pkgs).map((p) => p.product);
  if (!want.includes(reported)) throw new Error(`the pedal reports ${reported}, expected ${want.join(" or ")}`);
  const done = Object.values(pkgs).find((p) => p.product === reported);
  say(`done: the pedal reports ${pkgName(done)}.`);
  wayBack(done, say);
  return 0;
}

// main(argv, { access, say, timing }): the exit code. access defaults to the real USB MIDI ports.
export async function main(argv, { access, say = console.log, timing } = {}) {
  let node = null;
  try {
    const args = parse(argv);
    if (!args.dryRun && !args.confirm) throw refuse(`this writes to the pedal; add --confirm-flash to do it, or --dry-run to only check.\n${USAGE}`);
    const mode = args.dryRun ? "dry run: nothing will be written" : args.rescue ? "FLASH, rescue mode" : "FLASH";
    say(`flash (${mode})`);

    // the package first, when named: a damaged one never reaches the pedal, not even a query
    let pkg = args.package ? await loadPackage(args, { file: args.package }) : null;
    if (pkg) say(`package ${describe(pkg)}`);

    if (!access) {
      const midi = await import("@julusian/midi").then((m) => m.default || m, () => { throw refuse("the MIDI library is not installed: run `npm ci --prefix tools` once (docs/BUILDING.md)."); });
      node = (await import("./midi-node.mjs")).nodeMidiAccess(midi);
      access = node;
    }
    const t = { ...TIMING, ...timing };
    if (args.rescue) return await rescue(args, pkg, access, t, say);
    const dev = await detect(access, t);
    say(`pedal: ${devName(dev)} on "${dev.name}"`);
    if (IS_OTA(dev.id)) throw refuse(`the pedal is in update mode (${dev.id.text}), not running firmware. Power-cycle it, then try again; the installer page can also finish an interrupted update.`);

    if (!pkg) { pkg = await loadPackage(args, { model: dev.id.model }); say(`package ${describe(pkg)}`); }
    if (pkg.model !== dev.id.model) throw refuse(`wrong model: the pedal is a ${dev.id.text}, the package is ${pkg.product}. Nothing was sent.`);

    if (args.dryRun) { say(`dry run ok: would update ${devName(dev)} -> ${pkgName(pkg)}. Nothing was written.`); return 0; }

    say(`updating ${devName(dev)} -> ${pkgName(pkg)}: do not unplug the pedal`);
    const step = (s, n) => {
      if (s === "verify" || s === "write") { if (n % 250 === 0) say(`  ${s}: ${n} requests`); }
      else say(`  ${s}`);
    };
    const reported = await new Updater(access, null, t).install(pkg.image, pkg.product, step, { app: pkgApp(pkg) });
    if (reported !== pkg.product) throw new Error(`the pedal reports ${reported}, expected ${pkg.product}`);
    say(`done: the pedal reports ${pkgName(pkg)}.`);
    wayBack(pkg, say);
    return 0;
  } catch (e) {
    if (e instanceof Refusal) { say(`REFUSED: ${e.message}`); return 2; }
    say(`FAILED: ${WHY[e.code] || "error"}: ${e.message}${e.detail ? ` [${e.detail}]` : ""}`);
    say(RECOVERY);
    return 1;
  } finally {
    if (node) node.close();
  }
}
