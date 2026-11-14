// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — an app on the base firmware other than OpenPocket, in the hosted simulator (tools/sim.mjs --app starter,
// docs/ALT-FIRMWARE.md). The program under test is the starter app's own C file (firmware/apps/starter/starter.c), its
// app.h and its engine, compiled for this computer with tests/host as the chip: the base firmware's start and main
// loop, controls, audio stages, store, MIDI reader and product reply are the pedal's own code.
// Driven over the simulator's HTTP interface: who it says it is, each control, its sound at the two outputs, its MIDI
// map, its own editor requests, a save that survives a restart and stays inside its flash range, safe mode.
//   node tests/test_app_sim.mjs
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { startSim } from "../tools/sim.mjs";
import { parseIdentity, parseInfo, IS_ALT, Updater } from "../docs/pxota.js";
import { Editor } from "../docs/pxedit.js";

const TMP = mkdtempSync(join(tmpdir(), "opapp-"));
let failed = 0;
const ok = (c, what) => { console.log(`${what.padEnd(96)} ${c ? "ok" : "FAIL"}`); if (!c) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const flash = join(TMP, "flash.bin");
let sim = await startSim({ port: 0, flash, app: "starter" });
const get = async (path) => (await fetch(sim.url + path)).json();
const act = async (path, body) => { const r = await fetch(sim.url + path, { method: "POST", body }); assert.equal(r.status, 200, `${path}: ${await r.clone().text()}`); return r.json(); };
const sysex = async (hex) => { const { reply } = await act("/api/sysex", hex); return reply ? Uint8Array.from(Buffer.from(reply, "hex")) : null; };
const midi = (hex) => act("/api/midi", hex).catch(() => null);
const screen = async () => Buffer.from(await (await fetch(sim.url + "/api/screen.rgb565")).arrayBuffer());
// the app's own STATE request (app.h): slot, flags, the selected setting, the sound (name 12, LEVEL, WIDTH, PAN, on)
async function sound() {
  const m = await sysex("f07d4f5005f7");
  const a = [...m.slice(5, -1)];
  return { slot: a[0], dirty: a[1] & 1, saved: a[1] >> 1 & 1, sel: a[2], name: String.fromCharCode(...a.slice(3, 15)).replace(/\0.*$/, ""),
    level: a[15], width: a[16], pan: a[17], on: a[18] };
}
const level = (hz = 440) => act(`/api/sound/${hz}`);        // a sine as the guitar: the two outputs' RMS, of full scale
const near = (v, want, tol) => Math.abs(v - want) <= tol;

// ---- who it is ----
let info = await get("/api/info");
ok(info.kind === "app" && info.app === "starter" && info.port === "OpenPocket" && info.controls.includes("push_long"), "the simulator runs the app \"starter\" on the USB port every product has (OpenPocket)");
let st = await get("/api/state");
ok(st.app === "starter" && st.name === "STARTER" && st.version === "v01" && st.identity === "PocketX_901", `it is ${st.name} ${st.version}, identity ${st.identity}`);
const id = parseIdentity(await sysex("f000324500000040 7ff7".replace(" ", "")));
ok(id && id.text === "PocketX_901" && id.model === "PocketX", "the installer's identity query is answered by the pedal's update code: PocketX_901");
const p = parseInfo(await sysex("f07d4f5001f7"));
ok(p && IS_ALT(p) && p.app === "starter" && p.name === "STARTER" && p.version === "v01" && p.identity === "PocketX_901" && p.proto === 0,
  "INFO says it is not OpenPocket (protocol 0) and which product it is");
ok(p && p.caps.join() === "stereo,midi,store" && p.editor === 1, "and what it can do, as the app declares it to the base: stereo, midi, store; its own editor protocol 1");
ok((await sysex("f07d4f50020000f7")) === null, "a request of OpenPocket's editor that it does not have (CAT_BLOCK) gets no reply");

// ---- the web tools' own code against it: a MIDI access whose port is the simulated pedal ----
{
  const input = { name: "OpenPocket", state: "connected", onmidimessage: null, open: async () => {} };
  const output = { name: "OpenPocket", state: "connected", open: async () => {},
    send: (bytes) => { sysex(Buffer.from(bytes).toString("hex")).then((m) => { if (m && input.onmidimessage) input.onmidimessage({ data: m }); }); } };
  const access = { inputs: new Map([["p", input]]), outputs: new Map([["p", output]]) };
  const d = await new Updater(access, null, { info: 800 }).identify();
  ok(d && d.id.text === "PocketX_901" && IS_ALT(d.product) && d.product.name === "STARTER", "the installer's library finds the pedal and learns it runs STARTER, an alternative firmware");
  const ed = new Editor(access);
  const info = await ed.connect();
  ok(info === null && ed.other && ed.other.app === "starter" && ed.other.name === "STARTER" && ed.other.version === "v01",
    "OpenPocket's web editor does not connect to it, and knows which product answered");
}

// ---- the start: its sound is held off for 1.5 s, then on ----
ok(st.safe === 0 && st.hold === 1 && st.guard_pending === 1, "a normal start: not in safe mode; the sound is held off at first, the start not counted as finished yet");
await act("/api/wait/1600");
st = await get("/api/state");
ok(st.hold === 0 && st.guard_pending === 1 && st.start === "FIRST 6 S", "1.6 s on: the sound is on; the start still counts as unfinished (a reset now would make the next start safe)");
await act("/api/wait/4600");
st = await get("/api/state");
ok(st.guard_pending === 0 && st.start === "OK", "6 s on: the boot guard is told that the start finished");
let s = await sound();
ok(s.slot === 0 && s.name === "CENTRE" && s.level === 50 && s.width === 0 && s.pan === 50 && s.on === 1 && !s.dirty && !s.saved, "it starts on its first factory sound: CENTRE, LEVEL 50, WIDTH 0, PAN 50");

// ---- the sound at the two outputs (the pedal's own audio stages, MASTER at 80) ----
const dry = 0.25 * 0.5 * Math.SQRT1_2 * 0.64;                 // a quarter of full scale, halved at the input, RMS, MASTER 80
let l = await level();
ok(near(l.left, dry, 0.003) && near(l.right, dry, 0.003), `CENTRE: the guitar on both outputs alike (${l.left}, ${l.right}; the dry level is ${dry.toFixed(4)})`);
await act("/api/press/cab");                                   // PAN
await act("/api/knob/-4?fast");
s = await sound();
ok(s.sel === 2 && s.pan === 34 && s.dirty === 1, "CAB selects PAN; the knob turned quickly: one step, then steps of 5 (PAN 34); the sound is marked as edited");
await act("/api/knob/-8?fast");
l = await level();
ok(near(l.left, dry, 0.003) && l.right < 0.001, `PAN 0: the left output as before, the right silent (${l.left}, ${l.right})`);
await act("/api/knob/22?fast");
l = await level();
ok(l.left < 0.001 && near(l.right, dry, 0.003), `PAN 100: the other way round (${l.left}, ${l.right})`);
await sysex("f07d4f50060232f7");                               // (its own SET request: PAN 50)
await act("/api/press/mod");                                   // LEVEL
await act("/api/knob/11?fast");
s = await sound(); l = await level();
ok(s.sel === 0 && s.level === 100 && near(l.left, dry * 4, 0.01) && near(l.right, dry * 4, 0.01), `MOD selects LEVEL; LEVEL 100 is four times as loud (${l.left})`);
await sysex("f07d4f50060032f7");                               // (LEVEL 50)
await act("/api/press/home");
s = await sound(); l = await level();
ok(s.on === 0 && near(l.left, dry, 0.003) && near(l.right, dry, 0.003), "HOME turns the effect off: the guitar as it is on both outputs");
await act("/api/press/home");
await act("/api/volume/0");
l = await level();
ok(l.left < 0.001 && l.right < 0.001, "MASTER at 0: silence");
await act("/api/volume/80");
st = await get("/api/state");
ok(near(st.volume, 80, 1), `MASTER back at 80 (it reads ${st.volume})`);

// ---- a NaN from the app's sound: the base silences it, resets the app's sound and brings it back ----
await act("/api/nan");
st = await get("/api/state");
l = await level();
ok(st.nans === 1 && st.nan_now === 0 && near(l.left, dry, 0.003) && near(l.right, dry, 0.003), "a NaN in the sound: counted, the app's sound reset by the main loop, and the sound is back");

// ---- the controls: PUSH steps the settings, the footswitches the presets ----
await act("/api/press/push");
s = await sound();
ok(s.sel === 1, "PUSH: the next setting (WIDTH)");
await act("/api/press/amp"); await act("/api/knob/7");
s = await sound();
ok(s.sel === 1 && s.width === 7, "AMP selects WIDTH; the knob turned slowly goes one step a detent: WIDTH 7");
l = await level();
ok(near(l.left, dry, 0.003) && near(l.right, dry, 0.003), "WIDTH delays the right side, it does not change its level");
const before = await screen();
await act("/api/press/fs_up");
s = await sound();
ok(s.slot === 1 && s.name === "WIDE" && s.width === 40 && !s.dirty, "the right footswitch: the next preset (WIDE), the unsaved changes dropped");
ok(!before.equals(await screen()), "the screen was drawn again");
await act("/api/press/fs_dn"); await act("/api/press/fs_dn");
s = await sound();
ok(s.slot === 7 && s.name === "OFF" && s.on === 0, "the left footswitch twice: back past the first preset to the last (OFF)");

// ---- MIDI: the app's map over the base's reader ----
await midi("c002"); await sleep(120);
s = await sound();
ok(s.slot === 2 && s.name === "WIDER", "Program Change 2: the third preset (WIDER)");
await midi("b0077f"); await midi("b00a00"); await midi("b00c40"); await sleep(120);
s = await sound();
ok(s.level === 100 && s.pan === 0 && s.width === 50, "Control Change 7, 10 and 12: LEVEL 100, PAN 0, WIDTH 50");
await midi("b05000"); await sleep(120);
ok((await sound()).on === 0, "Control Change 80 below 64: the effect off");
await midi("b0507f"); await midi("c07f"); await midi("903c40"); await sleep(120);
s = await sound();
ok(s.on === 1 && s.slot === 2, "Control Change 80 at 127: on again; a Program Change it has no preset for and a note change nothing");
for (let k = 0; k < 120; k++) await midi("b0077f");            // more than the ring holds at once: it is read every pass
ok(parseInfo(await sysex("f07d4f5001f7")) !== null, "after 120 more messages the pedal still answers SysEx: the ring is read");

// ---- its own editor requests ----
let m = await sysex("f07d4f5006020af7");
ok(m && m[4] === 6 && m[5] === 2 && m[6] === 10 && (await sound()).pan === 10, "its SET request: PAN 10, answered with the value taken");
m = await sysex("f07d4f5006007ff7");
ok(m && m[6] === 100 && (await sound()).level === 100, "a value above 100 is taken as 100");

// ---- a save, and a restart ----
await act("/api/press/fs_up");                                 // slot 4 (LEFT)
await act("/api/press/amp"); await act("/api/knob/9");
s = await sound();
ok(s.slot === 3 && s.width === 9 && s.dirty === 1 && s.saved === 0, "preset 4 edited: WIDTH 9, not saved yet");
await act("/api/press/push_long");
s = await sound();
ok(s.dirty === 0 && s.saved === 1, "PUSH held 1.2 s: saved (the sound is its slot's now)");
st = await get("/api/state");
ok(st.writes_ok === true, "every flash write was inside the app's own range, with the sound held");
await sim.close();
const nor = readFileSync(flash);
const outside = [...nor.subarray(0, 0xE5000), ...nor.subarray(0xEF000)].every((b) => b === 0xFF);
ok(nor.length === 0x100000 && outside, "the flash outside 0xE5000..0xEF000 is untouched: not the firmware, not OpenPocket's presets, models or IRs");
ok(nor.readUInt32LE(0xE5000) === 0x52545350 && nor.subarray(0xE5F00, 0xE6000).every((b) => b === 0xFF), "its store's first copy is there, the sector's last 256 bytes erased");
sim = await startSim({ port: 0, flash, app: "starter" });
s = await sound();
ok(s.slot === 3 && s.name === "LEFT" && s.width === 9 && s.saved === 1 && !s.dirty, "after a restart it starts on the saved preset, with the saved sound");
await act("/api/press/fs_up");
ok((await sound()).name === "RIGHT", "and the other presets are still their factory sounds");
await sim.close();

// ---- safe mode: the start before did not finish ----
sim = await startSim({ port: 0, flash, app: "starter", failedStart: true });
await act("/api/wait/7000");
st = await get("/api/state");
ok(st.safe === 1 && st.hold === 1, "after a start that did not finish: safe mode, the app's sound stays off");
l = await level();
ok(near(l.left, dry, 0.003) && near(l.right, dry, 0.003), "the guitar passes on both outputs");
ok(parseInfo(await sysex("f07d4f5001f7")) !== null && parseIdentity(await sysex("f0003245000000407ff7")) !== null, "and USB answers: the installer can put any firmware on it");
await sim.close();

console.log(failed ? `\n${failed} FAILED` : "\nall app simulator tests passed");
process.exit(failed ? 1 : 0);
