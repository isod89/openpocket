// SPDX-License-Identifier: GPL-3.0-only
// node tools/op.mjs dsp: the DSP's health per preset, read over USB MIDI (PING, docs/EDITOR-PROTOCOL.md): the load, the
// peak load, the late blocks (a_xrun: the "!" after the load), the blocks at the soft clipper and the safety bypass.
// Each preset is selected (not saved), left to run, then read; the preset the pedal had is selected again at the end.
//   dsp [--presets A,B,...] [--seconds SEC] [--json]
//     --presets  slot numbers (from 1) or parts of preset names; default: every saved preset
//     --seconds  how long each preset runs before it is read (default 6)
// Firmware before v40.1 answers PING with the load and the flags only: the peak is then the highest load read, and the
// late blocks and clips are not known.
import { C, Editor } from "../docs/pxedit.js";

const USAGE = `usage: node tools/op.mjs dsp [--presets A,B,...] [--seconds SEC] [--json]
Exit: 0 every preset ran without a late block or the bypass, 1 one did not, 2 bad arguments or no pedal.`;

function parse(argv) {
  const a = { presets: null, seconds: 6, json: false };
  for (let i = 0; i < argv.length; i++) {
    const v = argv[i];
    if (v === "--json") a.json = true;
    else if (v === "--presets") a.presets = String(argv[++i] || "").split(",").map((s) => s.trim()).filter(Boolean);
    else if (v === "--seconds") { a.seconds = Number(argv[++i]); if (!(a.seconds > 0)) throw new Error("--seconds needs a number"); }
    else throw new Error(`unknown argument ${v}`);
  }
  return a;
}

// the engine's blocks (engine/px_dsp.h PX_B_*), then the NAM network
const PROF = ["GATE", "COMP", "PITCH", "SFX", "DRIVE", "AMP", "CAB", "EQ", "MOD", "DELAY", "REVERB", "SYN", "NAM"];

// PING's reply: load, flags, then (v40.1) peak, peak since boot, late blocks and clips (14 bits each, wrapping)
export function parsePing(r) {
  const p = { load: r[0], bypass: !!(r[1] & 1), secondCore: !!(r[1] & 2) };
  if (r.length >= 8) Object.assign(p, { peak: r[2], peakAll: r[3], xrun: r[4] | r[5] << 7, clip: r[6] | r[7] << 7 });
  if (r.length >= 12) Object.assign(p, { stackFree: (r[8] | r[9] << 7) * 4, irqStackFree: (r[10] | r[11] << 7) * 4 });
  // v40.2: then a count and that many bytes: each block's share of the block time (%, in the engine's block order) and
  // the NAM network's, then the network's way of running (its mode)
  const base = [12, 16].find((b) => r.length > b && r.length === b + 1 + 2 * r[b]);
  if ((base ?? r.length) >= 16) Object.assign(p, { bleHeapFree: (r[12] | r[13] << 7) * 4, bleTaskStackFree: (r[14] | r[15] << 7) * 4 });
  if (base) {
    const f = []; for (let i = base + 1; i + 1 < r.length; i += 2) f.push(r[i] | r[i + 1] << 7);
    p.prof = f.slice(0, PROF.length); p.namMode = f[PROF.length];
    p.namModes = f.slice(PROF.length + 1, PROF.length + 12).map((v) => v / 10);
    p.namParts = f.slice(PROF.length + 12, PROF.length + 21).map((v) => v / 10);
    if (f.length > PROF.length + 21) p.split = f[PROF.length + 21] ? f[PROF.length + 21] - 1 : null;   // v40.2: CPU0's last layer
    if (f.length > PROF.length + 25) [p.cpu0, p.cpu1, p.cpu0Peak, p.cpu1Peak] = f.slice(PROF.length + 22, PROF.length + 26);
    if (f.length > PROF.length + 29) [p.switchMs, p.setMs, p.loadingMs, p.loadings] = [f[PROF.length + 26] / 10, f[PROF.length + 27] / 10, f[PROF.length + 28], f[PROF.length + 29]];   // v40.3
    p.extra = f.slice(f.length > PROF.length + 29 ? PROF.length + 30 : f.length > PROF.length + 25 ? PROF.length + 26 : PROF.length + 22);   // (a development build's own)
  }
  return p;
}
const wrap14 = (a, b) => (b - a + 16384) % 16384;

export function pick(list, wanted) {
  if (!wanted) return list.filter((p) => p.saved);
  return wanted.map((w) => {
    const n = Number(w);
    const p = Number.isInteger(n) && n >= 1 ? list[n - 1] : list.find((q) => q.name.toLowerCase().includes(w.toLowerCase()));
    if (!p) throw new Error(`no preset ${w}`);
    return p;
  });
}

export async function run(ed, args, sleep) {
  const list = await ed.list();
  const presets = pick(list, args.presets);
  const was = (await ed.state()).slot;
  const out = [];
  try {
    for (const p of presets) {
      await ed.select(p.slot);
      await sleep(1.5);                                  // the preset's load (a NAM model, an IR) and its first blocks
      const a = parsePing(await ed.request(C.PING));
      let max = a.load, b = a;
      for (let t = 0; t < args.seconds; t += 0.5) { await sleep(0.5); b = parsePing(await ed.request(C.PING)); max = Math.max(max, b.load); }
      out.push({ slot: p.slot + 1, name: p.name, load: b.load, peak: b.peak ?? max, bypass: b.bypass || a.bypass, secondCore: b.secondCore,
                 xrun: b.xrun === undefined ? null : wrap14(a.xrun, b.xrun), clip: b.clip === undefined ? null : wrap14(a.clip, b.clip),
                 stackFree: b.stackFree ?? null, irqStackFree: b.irqStackFree ?? null,
                 bleHeapFree: b.bleHeapFree ?? null, bleTaskStackFree: b.bleTaskStackFree ?? null,
                 prof: b.prof ?? null, namMode: b.namMode ?? null, namModes: b.namModes ?? null, namParts: b.namParts ?? null,
                 split: b.split ?? null, cpu0: b.cpu0 ?? null, cpu1: b.cpu1 ?? null, cpu0Peak: b.cpu0Peak ?? null,
                 cpu1Peak: b.cpu1Peak ?? null, switchMs: a.switchMs ?? null, setMs: a.setMs ?? null, extra: b.extra ?? null });
    }
  } finally { await ed.select(was).catch(() => {}); }
  return out;
}

export async function main(argv, { access, say = console.log } = {}) {
  let args, node = null;
  try { args = parse(argv); } catch (e) { say(`${e.message}\n${USAGE}`); return 2; }
  const sleep = (s) => new Promise((r) => setTimeout(r, s * 1000));
  try {
    if (!access) {
      const midi = await import("@julusian/midi").then((m) => m.default || m, () => null);
      if (!midi) { say("the MIDI library is not installed: run `npm ci --prefix tools` once (docs/BUILDING.md)."); return 2; }
      node = (await import("./midi-node.mjs")).nodeMidiAccess(midi);
      access = node;
    }
    const ed = new Editor(access);
    const info = await ed.connect();
    if (!info) { say("no pedal answers the editor protocol"); return 2; }
    let rows;
    try { rows = await run(ed, args, sleep); } finally { ed.close(); }
    if (args.json) for (const r of rows) say(JSON.stringify(r));
    else {
      say(`firmware   ${info.version || ""}`.trimEnd());
      say("slot name           load  peak  late  clip  cores  bypass");
      for (const r of rows)
        say(`${String(r.slot).padStart(4)} ${r.name.padEnd(14)} ${String(r.load).padStart(4)}% ${String(r.peak).padStart(4)}% ${String(r.xrun ?? "?").padStart(5)} ${String(r.clip ?? "?").padStart(5)}  ${r.secondCore ? "2   " : "1   "}   ${r.bypass ? "BYP" : "-"}`);
      if (rows.some((r) => r.prof)) {
        say("where the time goes (% of a block, last second):");
        for (const r of rows) if (r.prof) {
          const parts = r.prof.map((v, i) => [PROF[i] ?? `#${i}`, v]).filter(([, v]) => v);
          say(`${String(r.slot).padStart(4)} ${r.name.padEnd(14)} ${parts.map(([n, v]) => `${n} ${v}`).join("  ")}  (mode ${r.namMode})` +
              `${r.split === null ? "" : `  CPU0 ${r.cpu0}/${r.cpu0Peak} %, CPU1 ${r.cpu1}/${r.cpu1Peak} % (layers 0..${r.split} on CPU0)`}`);
        }
      }
      if (rows.some((r) => r.switchMs !== null)) {          // v40.3: the selection is a full change when the type, order or model differs
        say("the switch to it (from the selection to the sound back, ms; the engine's set in it):");
        for (const r of rows) if (r.switchMs !== null) say(`${String(r.slot).padStart(4)} ${r.name.padEnd(14)} ${r.switchMs}  (set ${r.setMs})`);
      }
      const last = rows[rows.length - 1];
      if (last && last.namModes) {
        say(`NAM network at start-up, % of real time by mode: ${last.namModes.map((v, i) => `${i}:${v}`).join(" ")}`);
        say(`  one core (mode 0) parts: ${last.namParts.join(" ")}`);
        if (last.split !== null) say(`  layer split: CPU0 runs layers 0..${last.split} of 23, CPU1 the rest and the head`);
        if (last.extra.length) say(`  extra: ${last.extra.join(" ")}`);
      }
      if (last && last.stackFree !== null) say(`stacks never used since boot: main ${last.stackFree} B, interrupts ${last.irqStackFree} B`);
      if (last && last.bleHeapFree !== null) say(`Bluetooth: heap lowest free ${last.bleHeapFree} B, deepest task stack never used ${last.bleTaskStackFree} B`);
    }
    return rows.some((r) => r.bypass || r.xrun) ? 1 : 0;
  } catch (e) { say(e.message); return 2; } finally { if (node) node.close(); }
}
