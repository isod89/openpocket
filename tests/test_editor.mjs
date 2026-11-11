// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — end-to-end test of the web editor's protocol (docs/pxedit.js) against a simulated pedal (tests/edsim.c)
// that runs the pedal's real editor code (firmware/px_editor.h), preset store (px_store.h) and engine catalogue.
//   cc -O1 -o build/edsim tests/edsim.c engine/px_dsp.c -lm && node tests/test_editor.mjs
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { Editor, presetToJSON, presetFromJSON, filePresets, parsePresetFile, clonePreset } from "../docs/pxedit.js";
import { parseNam, levelTrim, packModel, readDi, a2Run, KERNELS, DILATIONS, NW } from "../docs/pxnam.js";
import { parseWav, prepareIr, packIr, irName, resample, IR_LEN } from "../docs/pxir.js";

const ROOT = fileURLToPath(new URL("..", import.meta.url));
const SIM = process.env.EDSIM || join(ROOT, "build", "edsim");
const TMP = mkdtempSync(join(tmpdir(), "opedit-"));
globalThis.localStorage = { getItem: () => null, setItem: () => {} };
let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(72)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function sim(args) {
  const child = spawn(SIM, args, { stdio: ["pipe", "pipe", "inherit"] });
  const access = { inputs: new Map(), outputs: new Map() };
  const lines = [];
  let port = null, buf = "", readyFn;
  const ready = new Promise((r) => { readyFn = r; });
  child.stdout.on("data", (d) => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const ln = buf.slice(0, i); buf = buf.slice(i + 1);
      if (ln.startsWith("#port ")) {
        const name = ln.slice(6);
        const input = { name, state: "connected", onmidimessage: null, open: async () => {} };
        const output = { name, state: "connected", open: async () => {}, send: (b) => child.stdin.write(Buffer.from(b).toString("hex") + "\n") };
        port = { input, output };
        access.inputs.set(name, input); access.outputs.set(name, output);
        readyFn();
      } else if (ln.startsWith("#")) lines.push(ln);
      else if (port && port.input.onmidimessage && ln) port.input.onmidimessage({ data: Uint8Array.from(Buffer.from(ln, "hex")) });
    }
  });
  const done = new Promise((r) => child.on("exit", r));
  return {
    access, ready, lines,
    pedal: (l) => child.stdin.write(`#pedal ${l}\n`),
    dump: async () => { lines.length = 0; child.stdin.write("#dump\n"); for (let i = 0; i < 50 && !lines.length; i++) await sleep(10); return lines[0] || ""; },
    end: () => { child.stdin.end(); return done; },
  };
}

// ---- 1. connect, catalogue, current sound ----
const flash1 = join(TMP, "flash1.bin");
let s = sim(["--dump", flash1]);
await s.ready;
let ed = new Editor(s.access);
const info = await ed.connect();
ok(info && info.proto === 4 && info.version === "v39" && info.identity === "PocketX_939", `INFO: ${info && `${info.version} ${info.identity}`}`);
ok(info && info.nblk === 12 && info.npar === 5 && info.nslot === 52 && info.nameMax === 13, "INFO: 12 blocks, 5 parameters, 52 slots, names of 13");
const t0 = Date.now();
const cat = await ed.catalog();
const ntypes = cat.blocks.reduce((n, b) => n + b.types.length, 0);
ok(cat.blocks.length === 12 && cat.blocks[5].label === "AMP" && cat.blocks[5].types.length === 9 && cat.blocks[5].types[8].name === "NAM", `catalogue: 12 blocks, ${ntypes} types (${Date.now() - t0} ms)`);
ok(cat.blocks[11].label === "SYN" && cat.blocks[11].color === "#ff1494", `catalogue: the SYN block and its tile colour (${cat.blocks[11].color})`);
const withTexts = cat.blocks.flatMap((b) => b.types.flatMap((t) => t.params.filter((p) => p.texts))).length;
ok(withTexts > 0 && cat.blocks[9].types[0].params.some((p) => p.texts && p.texts.some((x) => /ms|\d/.test(x))), `catalogue: value texts for ${withTexts} parameters (delay time...)`);
let st = await ed.state();
ok(st.slot === 0 && !st.dirty && st.preset.name.length > 0, `STATE: slot 1 "${st.preset.name}", not changed`);

// ---- 2. live editing ----
const amp = 5;
const v1 = await ed.setParam(amp, 0, 77);
st = await ed.state();
ok(v1 === 77 && st.preset.p[amp][0] === 77 && st.dirty, "SET: AMP's 1st knob = 77, the sound is now changed (*)");
ok(await ed.setParam(amp, 1, 120) === 100, "SET: a value above 100 is clamped to 100");
ok(await ed.setType(amp, 99) === 8, "SET: a type out of range is clamped to the last one (NAM)");
await ed.setOn(9, 1);
ok((await ed.state()).preset.on[9] === 1, "SET: the delay switched on");
ok(await ed.name("my toneé longer than thirteen") === "my tone? long", "NAME: 13 characters at most, other characters become ?");
const ord = [...st.preset.order]; [ord[0], ord[11]] = [ord[11], ord[0]];
ok(await ed.order(ord) === 0 && (await ed.state()).preset.order.join() === ord.join(), "ORDER: the block order changed");
ok(await ed.order([0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]) === 1, "ORDER: not a permutation -> refused");
const bad = clonePreset(st.preset); bad.p[0][0] = 100; bad.on[3] = 1; bad.type[4] = 60;
ok(await ed.put(bad) === 1, "PUT: a type that does not exist -> refused");
await ed.focus(9);
ok((await ed.state()).focus === 9, "FOCUS: the pedal shows the delay page");

// ---- 3. pushes: a change on the pedal reaches the editor, the editor's own does not come back ----
const pushes = [];
ed.onState = (x) => pushes.push(x);
await ed.watch(true);
s.pedal(`set ${amp} 2 33`);
for (let i = 0; i < 40 && !pushes.length; i++) await sleep(10);
ok(pushes.length === 1 && pushes[0].preset.p[amp][2] === 33, "WATCH: a knob turned on the pedal is pushed to the editor");
pushes.length = 0;
await ed.setParam(amp, 3, 12);
await sleep(150);
ok(pushes.length === 0, "WATCH: the editor's own change is not echoed back");
s.pedal("preset 7");
for (let i = 0; i < 40 && !pushes.length; i++) await sleep(10);
ok(pushes.length >= 1 && pushes.at(-1).slot === 7, "WATCH: a footswitch (slot 8) is pushed");

// ---- 4. saving, slots ----
await ed.select(2);
await ed.setParam(amp, 0, 11);
await ed.name("SAVED THREE");
ok(await ed.save(4) === 0, "SAVE: the current sound into slot 5");
st = await ed.state();
let list = await ed.list();
ok(st.slot === 4 && !st.dirty && list[4].saved && list[4].name === "SAVED THREE", "SAVE: slot 5 is current, saved, named");
ok(list.length === 52 && list.filter((x) => x.saved).length === 1, "LIST: 52 slots, 1 saved");
const g = await ed.slotGet(10);
ok(!g.saved && g.preset.name === list[10].name, `SLOT_GET: slot 11, factory "${g.preset.name}"`);
const a = clonePreset(g.preset); a.name = "STAGED A";
const b = clonePreset(g.preset); b.name = "STAGED B"; b.p[amp][0] = 99;
ok(await ed.slotPut(20, a, false) === 0 && await ed.slotPut(21, b, false) === 0, "SLOT_PUT: two slots staged in memory");
ok(await ed.commit() === 0, "COMMIT: one flash write");
ok(await ed.slotPut(4, null, true) === 0 && !(await ed.list())[4].saved, "SLOT_PUT: slot 5 back to its factory preset, written at once");
const dumpLine = await s.dump();
ok(dumpLine.split("|")[2] === "1", "the flash was written only inside the preset store");

// ---- 5. files ----
const q = (await ed.slotGet(21)).preset;
const file = JSON.stringify(filePresets([presetToJSON(q, cat, 21)]));
const back = parsePresetFile(file);
const r = presetFromJSON(back[0], cat, (await ed.slotGet(0)).preset);
ok(r.warn.length === 0 && JSON.stringify(r.preset) === JSON.stringify(q), "file: a preset survives export -> import exactly");
const odd = presetFromJSON({ name: "X", blocks: [{ block: "AMP", on: true, type: "NO SUCH AMP", params: [1, 2, 3] }, { block: "FOO" }] }, cat, q);
ok(odd.warn.length === 2 && odd.preset.p[amp][0] === 1, "file: unknown type and block are reported, the rest comes through");
const v33 = presetFromJSON({ name: "OLD", blocks: [{ block: "AMP", on: true, type: "RECTIFIED" }, { block: "DRV", on: true, type: "MUFF PI" }, { block: "CAB", on: true, type: "4x12 H30" }] }, cat, q);
const tn = (b, t) => cat.blocks[b].types[t].name;
ok(v33.warn.length === 3 && tn(amp, v33.preset.type[amp]) === "5150" && tn(4, v33.preset.type[4]) === "M77 OD" && tn(6, v33.preset.type[6]) === "4x12 V30",
   "file: a v33 preset file -> the nearest v34 amp / drive / cab, each one reported");
const dl = cat.blocks.findIndex((b) => b.label === "DLY");
const oldDly = presetFromJSON({ name: "ECHO", blocks: [{ block: "DLY", on: true, type: "TAPE EP-3", params: [50, 30, 20, 50, 50] }] }, cat, q);
const newDly = presetFromJSON(presetToJSON(oldDly.preset, cat), cat, q);
ok(oldDly.preset.p[dl][0] === 40 && oldDly.warn.length === 1 && newDly.preset.p[dl][0] === 40 && newDly.warn.length === 0,
   "file: a delay TIME from before v35 keeps its time (50 -> 40 = 510 ms); a v35 file is taken as is");
// ---- v42: USB AUDIO (protocol 4): off until asked for; kept in the store ----
{
  const u0 = await ed.usbAudio();
  const u1 = await ed.usbAudio(true);
  ok(ed.hasUsbAudio && !u0.on && u0.rc === 0 && u1.on && u1.rc === 0, "USB audio: off by default; switched on");
}
await s.end();

// ---- 6. after a power cycle: the store is read back from the flash ----
s = sim([flash1]);
await s.ready;
ed = new Editor(s.access);
await ed.connect();
list = await ed.list();
{
  let u = await ed.usbAudio();
  ok(u.on && u.rc === 0, "USB audio: still on after a power cycle");
  ok(!u.recording && !u.playing && Object.keys(u.counts).length === 20, "USB audio: the streams' state and 20 counts");
  u = await ed.usbAudio(false);
  ok(!u.on && u.rc === 0 && !(await ed.usbAudio()).on, "USB audio: switched off again");
}

// ---- 7. v37: NAM models (an A2 file -> a user slot -> the AMP block's NAM type) ----
{
  let seed = 7;
  const rnd = () => { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296 - 0.5; };
  const w = Array.from({ length: NW }, () => rnd() * 0.6);
  w[NW - 1] = 0.02;                                                    // head_scale
  const layer = { input_size: 1, condition_size: 1, head: { out_channels: 1, kernel_size: 16, bias: true }, channels: 3,
    kernel_sizes: KERNELS, dilations: DILATIONS, activation: KERNELS.map(() => ({ type: "LeakyReLU", negative_slope: 0.01 })),
    bottleneck: 3, head1x1: { active: false }, layer1x1: { active: true }, gating_mode: KERNELS.map(() => "none") };
  const a2 = { version: "0.7.0", architecture: "WaveNet", config: { layers: [layer], head: null, head_scale: 0.02 }, weights: w, sample_rate: 48000 };
  const file = JSON.stringify({ architecture: "SlimmableContainer", config: { submodels: [{ max_value: 0.5, model: a2 }] } });
  let m = null, why = "";
  try { m = parseNam(file, "My Test Amp.nam"); } catch (e) { why = e.message; }
  ok(m && m.name === "MY TEST AMP" && m.weights.length === NW && /semitones/.test(m.note), `NAM: an A2 file's A2-Lite part is read (${m ? m.name : why})`);
  let a1why = "";
  try { parseNam(JSON.stringify({ architecture: "WaveNet", config: { layers: [{ channels: 16 }] }, weights: [1] }), "a1.nam"); } catch (e) { a1why = e.message; }
  ok(/not an A2 model/.test(a1why), "NAM: an A1 capture is refused, with the reason");
  const di = readDi(readFileSync(join(ROOT, "docs", "nam_di.wav")).buffer);
  const lv = levelTrim(m.weights, di);
  ok(Number.isFinite(lv) && Math.abs(lv) <= 24, `NAM: level measured on the DI clip (${lv.toFixed(2)} dB)`);
  let nl = await ed.namList();
  ok(nl.nfact === 12 && nl.nuser === 16 && nl.models.filter((x) => !x.user && x.used).length === 12 && nl.models.every((x) => !x.user || !x.used),
     "NAM_LIST: 12 factory models, 16 empty user slots");
  const rc = await ed.namUpload(4, packModel(m.name, m.weights, lv));
  nl = await ed.namList();
  ok(rc === 0 && nl.models[12 + 4].used && nl.models[12 + 4].name === "MY TEST AMP", "NAM: the model is written into user slot 5");
  await ed.catalog();
  const nt = cat.blocks[amp].types.findIndex((t) => t.name === "NAM");
  await ed.namTexts();
  const texts = ed.cat.blocks[amp].types[nt].params[0].texts;
  const kv = texts.indexOf("MY TEST ");
  ok(nt === 8 && kv >= 0, `NAM: the AMP block's NAM type offers it as a MODEL (knob ${kv})`);
  await ed.setType(amp, nt);
  ok(await ed.setParam(amp, 0, kv) === kv && (await ed.state()).preset.type[amp] === nt, "NAM: a sound uses it");
  const bad = packModel(m.name, m.weights, lv); bad[64 + 100] ^= 0x55;           // a weight changed: the CRC no longer matches
  ok(await ed.namUpload(5, bad) === 1 && !(await ed.namList()).models[12 + 5].used, "NAM: a damaged model is refused, the slot stays empty");
  ok(await ed.namErase(4) === 0 && !(await ed.namList()).models[12 + 4].used, "NAM: the slot is cleared again");
}
// ---- 8. v38: cabinet IRs (a WAV file -> an IR slot -> the CAB block's USER IR type) ----
{
  const wav = (sr, bits, ch, frames) => {        // a WAV file of `frames` (arrays of channel samples, -1..1)
    const bps = bits >> 3, n = frames.length, b = new ArrayBuffer(44 + n * ch * bps), v = new DataView(b);
    const str = (o, t) => { for (let i = 0; i < 4; i++) v.setUint8(o + i, t.charCodeAt(i)); };
    str(0, "RIFF"); v.setUint32(4, 36 + n * ch * bps, true); str(8, "WAVE"); str(12, "fmt "); v.setUint32(16, 16, true);
    v.setUint16(20, bits === 32 ? 3 : 1, true); v.setUint16(22, ch, true); v.setUint32(24, sr, true);
    v.setUint32(28, sr * ch * bps, true); v.setUint16(32, ch * bps, true); v.setUint16(34, bits, true);
    str(36, "data"); v.setUint32(40, n * ch * bps, true);
    frames.forEach((f, i) => f.forEach((x, c) => {
      const o = 44 + (i * ch + c) * bps;
      if (bits === 16) v.setInt16(o, Math.round(x * 32767), true);
      else if (bits === 24) { const k = Math.round(x * 8388607); v.setUint8(o, k & 255); v.setUint8(o + 1, k >> 8 & 255); v.setInt8(o + 2, k >> 16); }
      else v.setFloat32(o, x, true);
    }));
    return b;
  };
  let seed = 11;
  const rnd = () => { seed = (seed * 1103515245 + 12345) >>> 0; return seed / 4294967296 - 0.5; };
  const resp = (sr, ms) => Array.from({ length: Math.round(sr * ms / 1000) }, (_, i) => [i < 20 ? 0 : rnd() * Math.exp(-(i - 20) / (sr * 0.004)), 0]);
  const w48 = parseWav(wav(48000, 24, 2, resp(48000, 200)));
  ok(w48.sampleRate === 48000 && w48.channels === 2 && w48.bits === 24 && w48.x.length === 9600, "IR: a 24-bit stereo WAV at 48 kHz is read");
  const ir = prepareIr(w48);
  ok(ir.h.length === IR_LEN && Math.abs(ir.h[0]) > 0 && /48 kHz/.test(ir.note) && /23 ms/.test(ir.note) && Math.abs(ir.h[IR_LEN - 1]) < 1e-3,
     `IR: from its onset, at 44.1 kHz, 1024 samples, faded (${ir.note})`);
  const sine = Array.from({ length: 4800 }, (_, i) => Math.sin(2 * Math.PI * 1000 * i / 48000));
  const rs = resample(sine, 48000, 1000), errs = rs.slice(100, 900).map((v, i) => v - Math.sin(2 * Math.PI * 1000 * (i + 100) / 44100));
  ok(Math.max(...errs.map(Math.abs)) < 1e-3, "IR: the resampling keeps a 1 kHz tone (error below -60 dB)");
  let bad = ""; try { parseWav(new ArrayBuffer(40)); } catch (e) { bad = e.message; }
  ok(/not a WAV/.test(bad), "IR: a file that is not a WAV is refused, with the reason");
  const f32 = prepareIr(parseWav(wav(44100, 32, 1, resp(44100, 10).map((f) => [f[0]]))));
  ok(f32.note === "" && f32.h[1000] === 0, "IR: a short float WAV at 44.1 kHz is kept as it is (zeros after its end)");
  let il = await ed.irList();
  ok(il.length === 8 && il.every((x) => !x.used), "IR_LIST: 8 empty slots");
  const name = irName("Mesa_4x12 V30.wav");
  const rc = await ed.irUpload(2, packIr(name, ir.h));
  il = await ed.irList();
  ok(rc === 0 && name === "MESA 4X12 V" && il[2].used && il[2].name === name, `IR: written into slot 3 (${il[2].name})`);
  await ed.catalog();
  const cabB = cat.blocks.findIndex((b) => b.label === "CAB"), it = cat.blocks[cabB].types.findIndex((t) => t.name === "USER IR");
  await ed.irTexts();
  const texts = ed.cat.blocks[cabB].types[it].params[0].texts, kv = texts.indexOf("MESA 4X1");
  ok(it === 7 && kv >= 0 && ed.cat.blocks[cabB].types[it].params.map((p) => p.name).join() === "IR,LOW CUT,HIGH CUT",
     `IR: the CAB block's USER IR type offers it (knob ${kv})`);
  await ed.setType(cabB, it);
  ok(await ed.setParam(cabB, 0, kv) === kv && (await ed.state()).preset.type[cabB] === it, "IR: a sound uses it");
  const dmg = packIr(name, ir.h); dmg[64 + 300] ^= 0x21;
  ok(await ed.irUpload(3, dmg) === 1 && !(await ed.irList())[3].used, "IR: a damaged IR is refused, the slot stays empty");
  ok(await ed.irErase(2) === 0 && !(await ed.irList())[2].used, "IR: the slot is cleared again");
}
ok(list[20].name === "STAGED A" && list[21].name === "STAGED B" && list[20].saved && !list[4].saved, "power cycle: the saved slots are still there");
st = await ed.state();
ok(st.slot === 4, "power cycle: the pedal starts on the last slot SAVEd (imports do not move it)");
await s.end();

console.log(failed ? `\n${failed} FAILED` : "\nall editor tests passed");
process.exit(failed ? 1 : 0);
