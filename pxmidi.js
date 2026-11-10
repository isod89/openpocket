// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket web editor: musical MIDI. The pedal's own Control Change map (docs/MIDI.md, firmware/px_midi.h: the same
// numbers and the same scaling), a decoder for the messages a port delivers, and the editor's learned mappings: a control
// of another MIDI device (a foot controller plugged into the computer) -> something on the pedal. No DOM, no port here.

export const NBLK = 12, NPAR = 5;
export const CC_MASTER = 7, CC_TOGGLE0 = 8, CC_ON0 = 20, CC_TYPE0 = 52;

// 60 settings over three runs of free numbers (px_cc_param)
export function ccParam(b, k) {
  const i = b * NPAR + k;
  return i < 5 ? 33 + i : i < 18 ? 34 + i : i < 46 ? 70 + (i - 18) : 102 + (i - 46);
}
export const ccOn = (b) => CC_ON0 + b;
export const ccToggle = (b) => CC_TOGGLE0 + b;
export const ccType = (b) => CC_TYPE0 + b;

// what a Control Change number means to the pedal: { kind: "master" | "toggle" | "on" | "type" | "param", b, k } or null
export function ccTarget(cc) {
  if (cc === CC_MASTER) return { kind: "master" };
  if (cc >= CC_TOGGLE0 && cc < CC_TOGGLE0 + NBLK) return { kind: "toggle", b: cc - CC_TOGGLE0 };
  if (cc >= CC_ON0 && cc < CC_ON0 + NBLK) return { kind: "on", b: cc - CC_ON0 };
  if (cc >= CC_TYPE0 && cc < CC_TYPE0 + NBLK) return { kind: "type", b: cc - CC_TYPE0 };
  for (let b = 0; b < NBLK; b++) for (let k = 0; k < NPAR; k++) if (ccParam(b, k) === cc) return { kind: "param", b, k };
  return null;
}

// value scaling (pxm_to_cc, pxm_from_cc, pxm_type_to_cc, pxm_type_from_cc)
export const toCc = (v) => Math.floor((v * 127 + 50) / 100);
export const fromCc = (c) => Math.floor((c * 100 + 63) / 127);
export const typeToCc = (t, n) => Math.floor((t * 128 + 64) / n);
export const typeFromCc = (c, n) => Math.floor(c * n / 128);

// one MIDI message as a port delivers it -> { type, ch (1..16), ... } or null for what is not musical (SysEx, running data)
export function decode(d) {
  if (!d || !d.length || d[0] < 0x80) return null;
  const s = d[0];
  if (s === 0xF8) return { type: "clock" };
  if (s === 0xFA) return { type: "start" };
  if (s === 0xFB) return { type: "continue" };
  if (s === 0xFC) return { type: "stop" };
  if (s >= 0xF0) return null;
  const ch = (s & 15) + 1, kind = s & 0xF0;
  if (kind === 0xB0 && d.length >= 3) return { type: "cc", ch, cc: d[1] & 127, value: d[2] & 127 };
  if (kind === 0xC0 && d.length >= 2) return { type: "pc", ch, program: d[1] & 127 };
  if (kind === 0x90 && d.length >= 3) return { type: d[2] ? "note" : "noteoff", ch, note: d[1] & 127, value: d[2] & 127 };
  if (kind === 0x80 && d.length >= 3) return { type: "noteoff", ch, note: d[1] & 127, value: d[2] & 127 };
  return { type: "other", ch };
}

// ---- learned mappings ----
// A mapping: { id, src: { ch, cc }, tgt, slot }. tgt is one of
//   { kind: "param", b, k }          the control's 0..127 sets the setting's 0..100
//   { kind: "on", b, mode }          mode "follow": 64 and above is on; "toggle": each press (a value of 64 or more) flips it
//   { kind: "type", b }              the control's range is cut in as many slices as the block has models
//   { kind: "preset", step }         a press goes step presets on (1 or -1)
// slot: null = in every preset, n = only while preset slot n (0-based) is the current one.
export const sameSrc = (a, b) => a.ch === b.ch && a.cc === b.cc;
export const sameTgt = (a, b) => a.kind === b.kind && a.b === b.b && a.k === b.k && a.step === b.step;

// the mappings a Control Change triggers in preset `slot`: one of this preset wins over one for every preset
export function resolve(mappings, msg, slot) {
  const hit = mappings.filter((m) => sameSrc(m.src, msg) && (m.slot === null || m.slot === slot));
  return hit.some((m) => m.slot === slot) ? hit.filter((m) => m.slot === slot) : hit;
}

// what a mapping does with a value: { do: "param", b, k, v } | { do: "on", b, v } | { do: "type", b, t } |
// { do: "preset", step } | null (nothing: a pedal released, a toggle's 0). ctx: { on(b), ntypes(b) }
export function action(m, value, ctx) {
  const t = m.tgt, press = value >= 64;
  if (t.kind === "param") return { do: "param", b: t.b, k: t.k, v: fromCc(value) };
  if (t.kind === "on") {
    if (t.mode === "toggle") return press ? { do: "on", b: t.b, v: ctx.on(t.b) ? 0 : 1 } : null;
    return { do: "on", b: t.b, v: press ? 1 : 0 };
  }
  if (t.kind === "type") return { do: "type", b: t.b, t: typeFromCc(value, ctx.ntypes(t.b)) };
  if (t.kind === "preset") return press ? { do: "preset", step: t.step } : null;
  return null;
}

// learn: `src` now drives `tgt` in `slot`. A control drives one thing per scope, and a target has one control per scope.
export function learn(mappings, src, tgt, slot, id) {
  const keep = mappings.filter((m) => !(m.slot === slot && (sameSrc(m.src, src) || sameTgt(m.tgt, tgt))));
  return [...keep, { id, src: { ch: src.ch, cc: src.cc }, tgt: { ...tgt }, slot }];
}

// stored mappings (localStorage JSON) -> the ones that are well formed; anything else is dropped
export function cleanMappings(list) {
  if (!Array.isArray(list)) return [];
  const int = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
  return list.filter((m) => m && m.src && int(m.src.ch, 1, 16) && int(m.src.cc, 0, 127) && m.tgt &&
    (m.slot === null || int(m.slot, 0, 126)) &&
    ((m.tgt.kind === "param" && int(m.tgt.b, 0, NBLK - 1) && int(m.tgt.k, 0, NPAR - 1)) ||
     (m.tgt.kind === "on" && int(m.tgt.b, 0, NBLK - 1) && (m.tgt.mode === "follow" || m.tgt.mode === "toggle")) ||
     (m.tgt.kind === "type" && int(m.tgt.b, 0, NBLK - 1)) ||
     (m.tgt.kind === "preset" && (m.tgt.step === 1 || m.tgt.step === -1))));
}
