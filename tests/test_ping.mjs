// SPDX-License-Identifier: GPL-3.0-only
// PING's reply as tools/dsp.mjs and docs/pxedit.js read it (docs/EDITOR-PROTOCOL.md): v40.1 (the stacks; the Bluetooth
// image's heap and tasks), v40.2 (then the DSP profile, after its count) and v40.3 (the last switch), on built replies. Run by tests/run_tests.sh.
import assert from "node:assert/strict";
import { parsePing } from "../tools/dsp.mjs";
import { Editor } from "../docs/pxedit.js";

const two = (v) => [v & 127, v >> 7 & 127];
const base = [70, 2, 80, 90, ...two(3), ...two(5), ...two(22244 / 4), ...two(5268 / 4)];
const ble = [...base, ...two(14152 / 4), ...two(3200 / 4)];
const prof = (n) => [n, ...Array.from({ length: n }, (_, i) => two(i === 13 ? 5 : i * 3 + 1)).flat()];
const check = async (what, fn) => { await fn(); console.log(`${what.padEnd(76)} ok`); };
const ping = async (reply) => { const e = Object.create(Editor.prototype); e.request = async () => reply; return e.ping(); };

check("v40.1 release reply: the stacks, no Bluetooth figures, no profile", () => {
  const p = parsePing(base);
  assert.equal(p.stackFree, 22244); assert.equal(p.irqStackFree, 5268);
  assert.equal(p.bleHeapFree, undefined); assert.equal(p.prof, undefined);
});
check("v40.1 Bluetooth reply: its heap and tasks", () => {
  const p = parsePing(ble);
  assert.equal(p.bleHeapFree, 14152); assert.equal(p.bleTaskStackFree, 3200);
});
check("v40.2 release reply: the profile after its count, no Bluetooth figures", () => {
  const p = parsePing([...base, ...prof(35)]);
  assert.equal(p.bleHeapFree, undefined);
  assert.deepEqual(p.prof, Array.from({ length: 13 }, (_, i) => i * 3 + 1));
  assert.equal(p.namMode, 5);
  assert.equal(p.namModes.length, 11); assert.equal(p.namParts.length, 9);
});
check("v40.2 reply with the layer split's figure (0 = off, else 1 + CPU0's last layer)", () => {
  const with_ = (v) => [...base, 35, ...prof(34).slice(1), ...two(v)];
  assert.equal(parsePing(with_(12)).split, 11);
  assert.equal(parsePing(with_(0)).split, null);
  assert.equal(parsePing(with_(12)).namMode, 5);
  assert.deepEqual(parsePing(with_(12)).extra, []);
});
check("v40.2 reply with each core's load and peak after the split", () => {
  const p = parsePing([...base, 39, ...prof(34).slice(1), ...two(12), ...two(55), ...two(57), ...two(63), ...two(66)]);
  assert.equal(p.split, 11); assert.equal(p.cpu0, 55); assert.equal(p.cpu1, 57); assert.equal(p.cpu0Peak, 63); assert.equal(p.cpu1Peak, 66);
  assert.deepEqual(p.extra, []);
});
check("v40.3 reply with the last full change: to the sound back, the set (0.1 ms), LOADING (ms, count)", () => {
  const p = parsePing([...base, 43, ...prof(34).slice(1), ...two(12), ...two(55), ...two(57), ...two(63), ...two(66),
                       ...two(157), ...two(98), ...two(310), ...two(4)]);
  assert.equal(p.cpu1Peak, 66); assert.equal(p.switchMs, 15.7); assert.equal(p.setMs, 9.8); assert.equal(p.loadingMs, 310); assert.equal(p.loadings, 4);
  assert.deepEqual(p.extra, []);
});
check("v40.2 Bluetooth reply: its heap and tasks, then the profile", () => {
  const p = parsePing([...ble, ...prof(35)]);
  assert.equal(p.bleHeapFree, 14152); assert.equal(p.prof.length, 13); assert.equal(p.namMode, 5);
});
await check("the editor page's ping(): the same figures", async () => {
  const a = await ping([...base, ...prof(35)]), b = await ping([...ble, ...prof(35)]);
  assert.equal(a.stackFree, 22244); assert.equal(a.bleHeapFree, undefined); assert.equal(b.bleHeapFree, 14152);
  assert.deepEqual(a.prof, Array.from({ length: 12 }, (_, i) => i * 3 + 1)); assert.equal(a.profNam, 37);   // the blocks, then the network
});
