// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — the web editor's own logic, without a browser or a pedal: the Control Change map and the learned
// controls (docs/pxmidi.js) against docs/MIDI.md, and the model library and slot arithmetic (docs/pxlib.js) against
// the engine's rule (engine/px_dsp.c px_nam_model / px_nam_knob). Run by tests/run_tests.sh.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { ccParam, ccOn, ccToggle, ccType, ccTarget, toCc, fromCc, typeToCc, typeFromCc, decode, learn, resolve, action, cleanMappings, NBLK, NPAR } from "../docs/pxmidi.js";
import { knobIndex, indexKnob, slotRef, presetSlot, retarget, usage, packedName, renamePacked, cleanSlotName, Library, pedalKey, planMove } from "../docs/pxlib.js";

const ROOT = fileURLToPath(new URL("..", import.meta.url));
const check = async (what, fn) => { await fn(); console.log(`${what.padEnd(76)} ok`); };

// ---- MIDI ----
await check("the Control Change map is the one docs/MIDI.md prints", () => {
  const md = readFileSync(`${ROOT}docs/MIDI.md`, "utf8");
  const rows = [...md.matchAll(/^\| (GATE|COMP|PITCH|SFX|DRIVE|AMP|CAB|EQ|MOD|DELAY|REVERB|SYN) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \|$/gm)];
  assert.equal(rows.length, NBLK);
  rows.forEach((r, b) => {
    assert.equal(ccOn(b), +r[2]); assert.equal(ccType(b), +r[3]);
    for (let k = 0; k < NPAR; k++) assert.equal(ccParam(b, k), +r[4 + k]);
  });
  assert.equal(ccToggle(5), 13);
});
await check("every number of the map is unique, and ccTarget is its inverse", () => {
  const seen = new Set([7]);
  for (let b = 0; b < NBLK; b++) {
    for (const [cc, t] of [[ccOn(b), { kind: "on", b }], [ccToggle(b), { kind: "toggle", b }], [ccType(b), { kind: "type", b }],
      ...Array.from({ length: NPAR }, (_, k) => [ccParam(b, k), { kind: "param", b, k }])]) {
      assert.ok(!seen.has(cc), `CC ${cc} twice`); seen.add(cc);
      assert.deepEqual(ccTarget(cc), t);
    }
  }
  assert.deepEqual(ccTarget(7), { kind: "master" });
  for (const free of [0, 6, 32, 38, 64, 98, 101, 120, 127]) assert.equal(ccTarget(free), null);
});
await check("values scale as the pedal's: 0..100 to 0..127 and back exactly", () => {
  for (let v = 0; v <= 100; v++) assert.equal(fromCc(toCc(v)), v);
  assert.equal(toCc(100), 127); assert.equal(fromCc(64), 50);
  for (const n of [1, 4, 9, 11]) for (let t = 0; t < n; t++) assert.equal(typeFromCc(typeToCc(t, n), n), t);
  assert.equal(typeFromCc(127, 9), 8);
});
await check("messages are decoded; SysEx and data bytes are not musical", () => {
  assert.deepEqual(decode([0xB2, 11, 99]), { type: "cc", ch: 3, cc: 11, value: 99 });
  assert.deepEqual(decode([0xC0, 51]), { type: "pc", ch: 1, program: 51 });
  assert.equal(decode([0xF8]).type, "clock");
  assert.equal(decode([0xF0, 0x7D, 0xF7]), null);
  assert.equal(decode([0x40, 0x10]), null);
  assert.equal(decode([]), null);
});
await check("a learned control: one control per target and one target per control, in its scope", () => {
  let m = learn([], { ch: 1, cc: 11 }, { kind: "param", b: 5, k: 1 }, null, "a");
  m = learn(m, { ch: 1, cc: 12 }, { kind: "param", b: 5, k: 1 }, null, "b");           // the target taught again
  assert.deepEqual(m.map((x) => x.id), ["b"]);
  m = learn(m, { ch: 1, cc: 12 }, { kind: "on", b: 9, mode: "toggle" }, null, "c");      // the control taught again
  assert.deepEqual(m.map((x) => x.id), ["c"]);
  m = learn(m, { ch: 1, cc: 12 }, { kind: "param", b: 10, k: 2 }, 4, "d");               // the same control, for preset 05 only
  assert.deepEqual(m.map((x) => x.id), ["c", "d"]);
  assert.deepEqual(resolve(m, { ch: 1, cc: 12 }, 4).map((x) => x.id), ["d"], "the preset's own wins in that preset");
  assert.deepEqual(resolve(m, { ch: 1, cc: 12 }, 3).map((x) => x.id), ["c"]);
  assert.deepEqual(resolve(m, { ch: 2, cc: 12 }, 3), [], "another channel is another control");
});
await check("what a learned control does: settings, switches (flip or follow), types, preset steps", () => {
  const ctx = { on: (b) => b === 9, ntypes: () => 8 };
  assert.deepEqual(action({ tgt: { kind: "param", b: 5, k: 1 } }, 127, ctx), { do: "param", b: 5, k: 1, v: 100 });
  assert.deepEqual(action({ tgt: { kind: "on", b: 9, mode: "toggle" } }, 127, ctx), { do: "on", b: 9, v: 0 });
  assert.equal(action({ tgt: { kind: "on", b: 9, mode: "toggle" } }, 0, ctx), null, "the release of a footswitch does nothing");
  assert.deepEqual(action({ tgt: { kind: "on", b: 9, mode: "follow" } }, 0, ctx), { do: "on", b: 9, v: 0 });
  assert.deepEqual(action({ tgt: { kind: "on", b: 2, mode: "follow" } }, 64, ctx), { do: "on", b: 2, v: 1 });
  assert.deepEqual(action({ tgt: { kind: "type", b: 9 } }, 127, ctx), { do: "type", b: 9, t: 7 });
  assert.deepEqual(action({ tgt: { kind: "preset", step: -1 } }, 100, ctx), { do: "preset", step: -1 });
  assert.equal(action({ tgt: { kind: "preset", step: 1 } }, 10, ctx), null);
});
await check("stored controls that are not well formed are dropped", () => {
  const good = { id: "a", src: { ch: 1, cc: 11 }, tgt: { kind: "param", b: 5, k: 1 }, slot: null };
  const list = [good, { src: { ch: 0, cc: 11 }, tgt: good.tgt, slot: null }, { src: good.src, tgt: { kind: "param", b: 12, k: 0 }, slot: null },
    { src: good.src, tgt: { kind: "on", b: 1 }, slot: null }, { src: good.src, tgt: { kind: "preset", step: 2 }, slot: null }, null, "x"];
  assert.deepEqual(cleanMappings(list), [good]);
  assert.deepEqual(cleanMappings("nonsense"), []);
});

// ---- slots ----
await check("MODEL and IR knobs: the engine's rule (v * n / 101), the knob at the middle of its span", () => {
  for (const n of [28, 8]) {
    for (let v = 0; v <= 100; v++) assert.equal(knobIndex(v, n), Math.floor(v * n / 101));
    for (let i = 0; i < n; i++) {
      const k = indexKnob(i, n);
      assert.equal(knobIndex(k, n), i);
      const span = Array.from({ length: 101 }, (_, v) => v).filter((v) => knobIndex(v, n) === i);
      assert.equal(k, (span[0] + span[span.length - 1]) >> 1);
    }
  }
  assert.equal(knobIndex(200, 28), 27); assert.equal(knobIndex(-3, 28), 0);
});
const cat = { blocks: [{ label: "GT", types: [{ name: "NOISE GATE" }] }, { label: "AMP", types: [{ name: "TWIN" }, { name: "NAM" }] }, { label: "CAB", types: [{ name: "4x12" }, { name: "USER IR" }] }] };
const preset = (ampType, model, cabType = 0, ir = 0) => ({ name: "P", on: [1, 1, 1], type: [0, ampType, cabType], p: [[0, 0, 0, 0, 0], [model, 50, 50, 50, 50], [ir, 0, 0, 0, 0]], order: [0, 1, 2] });
await check("which slot a preset plays, which presets play a slot, and a preset following its slot", () => {
  const nam = slotRef(cat, "nam", 12, 16), ir = slotRef(cat, "ir", 0, 8);
  assert.deepEqual([nam.b, nam.t, nam.n], [1, 1, 28]);
  assert.equal(slotRef({ blocks: [{ label: "AMP", types: [{ name: "TWIN" }] }] }, "nam", 12, 16), null, "a firmware without NAM");
  const u3 = preset(1, indexKnob(12 + 3, 28));
  assert.equal(presetSlot(u3, nam), 3);
  assert.equal(presetSlot(preset(1, indexKnob(4, 28)), nam), -8, "a factory model is no user slot");
  assert.equal(presetSlot(preset(0, indexKnob(15, 28)), nam), -1, "another amp type plays no model");
  assert.equal(presetSlot(preset(0, 0, 1, indexKnob(5, 8)), ir), 5);
  const bank = [{ saved: true, preset: u3 }, { saved: false, preset: u3 }, undefined, { saved: true, preset: preset(1, indexKnob(12, 28)) }, { saved: true, preset: u3 }];
  assert.deepEqual([...usage(bank, nam)], [[3, [0, 4]], [0, [3]]], "saved presets only; holes are skipped");
  const moved = retarget(u3, nam, new Map([[3, 0]]));
  assert.equal(presetSlot(moved, nam), 0);
  assert.equal(u3.p[1][0], indexKnob(15, 28), "the original is not touched");
  assert.equal(retarget(u3, nam, new Map([[5, 0]])), null);
});
await check("a stored file's name: read, cleaned, changed without touching its data", () => {
  const b = new Uint8Array(96); b.set([..."PLEXI LEAD"].map((c) => c.charCodeAt(0)), 8); b[70] = 9;
  assert.equal(packedName(b), "PLEXI LEAD");
  assert.equal(cleanSlotName("  my plexi é lead long "), "MY PLEXI ?");
  const r = renamePacked(b, "My Twin");
  assert.equal(packedName(r), "MY TWIN"); assert.equal(r[70], 9); assert.equal(r[18], 0);
  assert.equal(packedName(b), "PLEXI LEAD");
});
await check("the library (in memory here): no duplicate of the same data, links to the pedal's slots", async () => {
  const lib = await Library.open({});
  assert.equal(lib.persistent, false);
  const bytes = (seed) => { const b = new Uint8Array(96); b[64] = seed; return b; };
  const a = await lib.add({ kind: "nam", name: "ONE", bytes: bytes(1), source: { from: "file", file: "one.nam" } });
  const twin = await lib.add({ kind: "nam", name: "ONE AGAIN", bytes: bytes(1), source: { from: "file", file: "copy.nam" } });
  assert.equal(twin.id, a.id);
  const b = await lib.add({ kind: "nam", name: "TWO", bytes: bytes(2) });
  await lib.add({ kind: "ir", name: "CAB", bytes: bytes(3) });
  assert.deepEqual((await lib.all("nam")).map((e) => e.name), ["ONE", "TWO"]);
  lib.setLink("PocketX", "nam", 0, a.id); lib.setLink("PocketX", "nam", 2, b.id); lib.setLink("PocketX", "nam", 5, "gone");
  const held = await lib.onPedal("PocketX", "nam", [{ slot: 0, used: true, name: "ONE" }, { slot: 2, used: true, name: "RENAMED" }, { slot: 5, used: true, name: "X" }, { slot: 6, used: false, name: "" }]);
  assert.deepEqual([...held.keys()], [0], "a slot that no longer reads the entry's name is not that entry any more");
  assert.deepEqual(lib.links("PocketX", "nam"), { 0: a.id });
  assert.deepEqual(lib.links("BlackBox", "nam"), {});
  await lib.remove(a.id);
  assert.equal(await lib.get(a.id), null);
  assert.equal(pedalKey("PocketX_940"), "PocketX");
});
await check("moving a slot's file: to an empty slot, two changing places, and what cannot move", () => {
  const A = { id: "a" }, B = { id: "b" }, held = new Map([[0, A], [1, B]]), used = (s) => s === 0 || s === 1 || s === 2;
  let p = planMove(0, 4, held, used);
  assert.deepEqual(p.writes, [[4, A], [0, null]]); assert.deepEqual([...p.map], [[0, 4]]);
  p = planMove(0, 1, held, used);
  assert.deepEqual(p.writes, [[1, A], [0, B]]); assert.deepEqual([...p.map], [[0, 1], [1, 0]]);
  assert.ok(planMove(2, 4, held, used).error, "a file the library does not hold cannot move");
  assert.ok(planMove(0, 2, held, used).error, "nor change places with one it does not hold");
  assert.deepEqual(planMove(0, 0, held, used).writes, []);
});
console.log("editor logic test passed");
