// SPDX-License-Identifier: GPL-3.0-only
// node tools/op.mjs flash: write a built OpenPocket package to a connected pedal over USB MIDI, with no browser.
// The protocol is docs/pxota.js and the package checks are docs/pxpkg.js, the very modules the installer page runs;
// this file only adds the command line: arguments, the checks that come first, and the report.
//   flash [--dry-run | --confirm-flash] [--package FILE] [--manifest FILE]
import { readFileSync } from "node:fs";
import { webcrypto } from "node:crypto";
import { basename, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { Updater, IS_OTA, MODELS } from "../docs/pxota.js";
import { verifyPackage } from "../docs/pxpkg.js";

globalThis.crypto ??= webcrypto;                             // Node 18 has no global; the page's browser does

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const USAGE = `usage: node tools/op.mjs flash --dry-run [--package FILE] [--manifest FILE]
       node tools/op.mjs flash --confirm-flash [--package FILE] [--manifest FILE]
  --dry-run        find the pedal and check the package; write nothing
  --confirm-flash  really write the package to the pedal (required: there is no default)
  --package FILE   the .fwsc to write (default: the manifest's package for the pedal's model)
  --manifest FILE  the manifest that lists the package and its SHA-256 (default: docs/firmware/manifest.json)`;
const RECOVERY = `Recovery: if the pedal does not start, power-cycle it and run this again. If it stays dead, use the SD-card
recovery (README.md, "Install": the \`flash/\` folder, fm1buddy on an ESP32-S3, \`make appimage DUMP=fm1-1.bin\` or makeimage.bat).`;
const TIMING = { afterUpgrade: 2000, afterStep1: 3000, loaderWait: 30000, step1Idle: 8000, step2Idle: 180000, afterWrite: 3000, backWait: 40000, perRequest: 10, poll: 1000 };

class Refusal extends Error {}                               // a check that came first: nothing was sent to the pedal
const refuse = (msg) => new Refusal(msg);

function parse(argv) {
  const a = { dryRun: false, confirm: false, package: null, manifest: resolve(ROOT, "docs/firmware/manifest.json") };
  for (let i = 0; i < argv.length; i++) {
    const v = argv[i];
    if (v === "--dry-run") a.dryRun = true;
    else if (v === "--confirm-flash") a.confirm = true;
    else if (v === "--package" || v === "--manifest") {
      if (i + 1 >= argv.length) throw refuse(`${v} needs a file`);
      a[v.slice(2)] = resolve(argv[++i]);
    } else throw refuse(`unknown argument ${v}`);
  }
  return a;
}

// the manifest entry for a package file (by its name) or for a model
function manifestEntry(manifestPath, { file, model }) {
  let man;
  try { man = JSON.parse(readFileSync(manifestPath, "utf8")); } catch (e) { throw refuse(`cannot read the manifest ${manifestPath}: ${e.message}`); }
  const entry = (man.packages || []).find((p) => (file ? p.file === file : p.model === model));
  if (!entry) throw refuse(file ? `${file} is not listed in ${manifestPath}: refusing an unlisted package` : `${manifestPath} has no package for ${model}`);
  return entry;
}

async function loadPackage(manifestPath, { file, model }) {
  const entry = manifestEntry(manifestPath, { file: file && basename(file), model });
  const path = file || resolve(dirname(manifestPath), entry.file);
  let bytes;
  try { bytes = new Uint8Array(readFileSync(path)); } catch (e) { throw refuse(`cannot read the package ${path}: ${e.message}`); }
  try { return { ...(await verifyPackage(bytes, entry)), path, sha256: entry.sha256 }; } catch (e) { throw refuse(`${basename(path)}: ${e.message}`); }
}

// the one pedal on the bus, or an error that says why not. Each MIDI input is asked alone (read-only identity query).
async function detect(access, timing) {
  const found = [];
  const outputs = access.outputs;
  for (const input of [...access.inputs.values()]) {
    if (input.state === "disconnected") continue;
    const probe = new Updater({ inputs: new Map([[input.name, input]]), outputs }, null, timing);
    const d = await probe.identify();
    if (d && (MODELS.includes(d.id.model) || IS_OTA(d.id))) found.push(d);
  }
  const seen = new Updater(access, null, timing).seen();
  if (!found.length) throw refuse(`no OpenPocket-compatible pedal found${seen}. Plug the pedal in over USB, close the browser installer and any MIDI program, and try again.`);
  if (found.length > 1) throw refuse(`${found.length} pedals answered (${found.map((d) => `${d.id.text} on "${d.name}"`).join(", ")}). Connect only one.`);
  return found[0];
}

const WHY = {
  notfound: "the pedal was not found", model: "wrong model", stopped: "the transfer stopped", noloader: "the update loader did not appear",
  lost: "the pedal was disconnected", noreturn: "the pedal did not come back", mismatch: "the pedal reports another version",
  badreq: "the pedal asked for data outside the package", foreign: "the pedal is in another firmware's update mode",
};

// main(argv, { access, say, timing }): the exit code. access defaults to the real USB MIDI ports.
export async function main(argv, { access, say = console.log, timing } = {}) {
  let node = null;
  try {
    const args = parse(argv);
    if (!args.dryRun && !args.confirm) throw refuse(`this writes to the pedal; add --confirm-flash to do it, or --dry-run to only check.\n${USAGE}`);
    const mode = args.dryRun ? "dry run: nothing will be written" : "FLASH";
    say(`flash (${mode})`);

    // the package first, when named: a damaged one never reaches the pedal, not even a query
    let pkg = args.package ? await loadPackage(args.manifest, { file: args.package }) : null;
    if (pkg) say(`package ${basename(pkg.path)}: ${pkg.product}, SHA-256 ${pkg.sha256.slice(0, 16)}… matches the manifest`);

    if (!access) {
      const midi = await import("@julusian/midi").then((m) => m.default || m, () => { throw refuse("the MIDI library is not installed: run `npm ci --prefix tools` once (docs/BUILDING.md)."); });
      node = (await import("./midi-node.mjs")).nodeMidiAccess(midi);
      access = node;
    }
    const t = { ...TIMING, ...timing };
    const dev = await detect(access, t);
    say(`pedal: ${dev.id.text} on "${dev.name}"`);
    if (IS_OTA(dev.id)) throw refuse(`the pedal is in update mode (${dev.id.text}), not running firmware. Power-cycle it, then try again; the installer page can also finish an interrupted update.`);

    if (!pkg) { pkg = await loadPackage(args.manifest, { model: dev.id.model }); say(`package ${basename(pkg.path)}: ${pkg.product}, SHA-256 ${pkg.sha256.slice(0, 16)}… matches the manifest`); }
    if (pkg.model !== dev.id.model) throw refuse(`wrong model: the pedal is a ${dev.id.text}, the package is ${pkg.product}. Nothing was sent.`);

    if (args.dryRun) { say(`dry run ok: would update ${dev.id.text} -> ${pkg.product}. Nothing was written.`); return 0; }

    say(`updating ${dev.id.text} -> ${pkg.product}: do not unplug the pedal`);
    const step = (s, n) => {
      if (s === "verify" || s === "write") { if (n % 250 === 0) say(`  ${s}: ${n} requests`); }
      else say(`  ${s}`);
    };
    const reported = await new Updater(access, null, t).install(pkg.image, pkg.product, step);
    if (reported !== pkg.product) throw new Error(`the pedal reports ${reported}, expected ${pkg.product}`);
    say(`done: the pedal reports ${reported}.`);
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
