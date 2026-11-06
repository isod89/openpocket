// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — what a key, a drag or a wheel on the pedal skin (pedal.html) means to the simulated pedal, and what its
// state means to the backlights. No DOM, so tests/test_pedal_input.mjs runs it in Node. Hold timing is not here:
// tests/edsim.c times a held button as firmware/openpocket.c does.

export const BUTTONS = ["home", "fx", "amp", "mod", "dly", "rev", "cab", "fs_dn", "fs_up"];
export const BLOCK = { amp: 5, cab: 6, mod: 8, dly: 9, rev: 10 };
export const FX_BLOCKS = [4, 3, 11, 2, 1, 0, 7];        // ui_fx_cycle: DRV SFX SYN PIT CMP GT EQ

export const FAST_MS = 45;                              // openpocket.c: detents closer than this are a fast turn
export const MASTER_STEP = 2;                           // openpocket.c: the volume's hysteresis, the smallest move it follows
const MASTER_SWEEP = 270;                               // degrees from 0 to 100
export const DETENT_DEGREES = 15;                       // 24 detents a turn: an assumption, the firmware only counts them
export const PRESS_MS = 120;                            // PARA: still for this long, a press and not a turn
export const SLOP_PX = 4;                               // PARA: moved this far, a turn and not a press

const WHEEL_NOTCH_PX = 100;
const LINE_PX = 33;
const DETENTS_PER_KNOB_WIDTH = 10;                      // a drag as long as the knob is wide
const MASTER_PER_KNOB_WIDTH = 40;

const hold = (control) => ({ hold: control });
const turn = (detents) => ({ turn: detents });
const master = (delta) => ({ master: delta });

const KEYS = {
  ArrowUp: hold("fs_up"), ArrowDown: hold("fs_dn"), ArrowLeft: turn(-1), ArrowRight: turn(1),
  Enter: hold("push"), Escape: hold("home"),
  h: hold("home"), f: hold("fx"), a: hold("amp"), m: hold("mod"), d: hold("dly"), r: hold("rev"), c: hold("cab"),
  "[": master(-MASTER_STEP), "]": master(MASTER_STEP),
};
const FOCUSED_MASTER = {
  ArrowUp: master(MASTER_STEP), ArrowRight: master(MASTER_STEP), ArrowDown: master(-MASTER_STEP), ArrowLeft: master(-MASTER_STEP),
  PageUp: master(10), PageDown: master(-10), Home: { masterTo: 0 }, End: { masterTo: 100 },
};
const FOCUSED_PARA = { ArrowUp: turn(1), ArrowDown: turn(-1), " ": hold("push") };

// The action of a key, given the control that has the focus (null: none): {hold}, {turn}, {master}, {masterTo} or null
export function keyAction(key, focused = null) {
  const k = key.length === 1 ? key.toLowerCase() : key;
  if (focused === "master" && FOCUSED_MASTER[k]) return FOCUSED_MASTER[k];
  if (focused === "para" && FOCUSED_PARA[k]) return FOCUSED_PARA[k];
  if (BUTTONS.includes(focused) && (k === "Enter" || k === " ")) return hold(focused);
  return KEYS[k] ?? null;
}

// Detents a drag has reached so far on PARA: right and up turn it clockwise
export const dragDetents = (dx, dy, knobPx) => Math.trunc((dx - dy) * DETENTS_PER_KNOB_WIDTH / knobPx);
export function wheelNotches(rest, deltaY, deltaMode = 0) {
  const travel = rest - deltaY * [1, LINE_PX, WHEEL_NOTCH_PX][deltaMode], notches = Math.trunc(travel / WHEEL_NOTCH_PX);
  return { notches, rest: travel - notches * WHEEL_NOTCH_PX };
}
export const isFast = (lastMs, nowMs, detents) => (nowMs - lastMs) / Math.abs(detents) < FAST_MS;

const clampMaster = (v) => Math.min(100, Math.max(0, Math.round(v)));
export const masterFromDrag = (start, dx, dy, knobPx) => clampMaster(start + (dx - dy) * MASTER_PER_KNOB_WIDTH / knobPx);
// What the firmware's volume does with a new reading: it stays unless it moved by the hysteresis or reached an end
export function masterFollow(current, reading) {
  const next = clampMaster(reading);
  return Math.abs(next - current) >= MASTER_STEP || (next !== current && (next === 0 || next === 100)) ? next : current;
}
export const masterAngle = (volume) => (volume / 100 - 0.5) * MASTER_SWEEP;

// PARA is both a knob and a button: a gesture starts undecided, and only a press reaches the pedal as a held button
const PARA_GESTURE = {
  idle: { down: { to: "undecided", send: [] } },
  undecided: {
    far: { to: "turning", send: [] }, still: { to: "pressed", send: ["down"] }, up: { to: "idle", send: ["down", "up"] },
  },
  pressed: { up: { to: "idle", send: ["up"] } },
  turning: { up: { to: "idle", send: [] } },
};
export const paraStep = (gesture, event) => PARA_GESTURE[gesture][event] ?? { to: gesture, send: [] };

// The firmware drives no LEDs, so this is the skin's own reading of the state: "off", "on" or "focus" for each button
export function backlights(state) {
  const paged = state.mode === "block" || state.mode === "edit";
  const light = (blocks) => {
    if (paged && blocks.includes(state.block)) return "focus";
    return blocks.some((b) => state.preset.on[b]) ? "on" : "off";
  };
  const lit = { home: state.mode === "chain" ? "focus" : "on", fx: light(FX_BLOCKS) };
  for (const [name, block] of Object.entries(BLOCK)) lit[name] = light([block]);
  return lit;
}
