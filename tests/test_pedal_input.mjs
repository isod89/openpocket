// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — the pedal skin's input mapping (tools/sim/pedal-input.js): keys, drags and the wheel in the simulator's
// terms, the MASTER knob against the firmware's volume, PARA's press or turn, the backlights. No browser.
//   node tests/test_pedal_input.mjs
import assert from "node:assert/strict";
import {
  BLOCK, FAST_MS, FX_BLOCKS, MASTER_STEP, backlights, dragDetents, isFast, keyAction, masterAngle, masterFollow, masterFromDrag, paraStep, wheelNotches,
} from "../tools/sim/pedal-input.js";

const tests = {
  "the page's keys press what they name"() {
    const holds = { ArrowUp: "fs_up", ArrowDown: "fs_dn", Enter: "push", Escape: "home", h: "home", H: "home", f: "fx", a: "amp", m: "mod", d: "dly", r: "rev", c: "cab" };
    for (const [key, control] of Object.entries(holds)) assert.deepEqual(keyAction(key), { hold: control }, key);
    assert.deepEqual(keyAction("ArrowRight"), { turn: 1 });
    assert.deepEqual(keyAction("ArrowLeft"), { turn: -1 });
    assert.deepEqual(keyAction("]"), { master: MASTER_STEP });
    assert.deepEqual(keyAction("["), { master: -MASTER_STEP });
    assert.equal(keyAction("x"), null);
    assert.equal(keyAction("Tab"), null);
  },
  "a focused control takes the keys that belong to it"() {
    assert.deepEqual(keyAction("Enter", "amp"), { hold: "amp" });
    assert.deepEqual(keyAction(" ", "fs_up"), { hold: "fs_up" });
    assert.deepEqual(keyAction("ArrowUp", "master"), { master: MASTER_STEP });
    assert.deepEqual(keyAction("ArrowLeft", "master"), { master: -MASTER_STEP });
    assert.deepEqual(keyAction("Home", "master"), { masterTo: 0 });
    assert.deepEqual(keyAction("End", "master"), { masterTo: 100 });
    assert.deepEqual(keyAction("ArrowUp", "para"), { turn: 1 });
    assert.deepEqual(keyAction("ArrowDown", "para"), { turn: -1 });
    assert.deepEqual(keyAction(" ", "para"), { hold: "push" });
    assert.deepEqual(keyAction("Enter", "para"), { hold: "push" });
  },
  "a drag on PARA counts detents, up and right clockwise, whatever the knob's size"() {
    assert.equal(dragDetents(0, -100, 200), 5);
    assert.equal(dragDetents(100, 0, 200), 5);
    assert.equal(dragDetents(0, 100, 200), -5);
    assert.equal(dragDetents(19, 0, 200), 0);
    assert.equal(dragDetents(50, 0, 100), dragDetents(100, 0, 200));
  },
  "the wheel carries what is short of a notch"() {
    assert.deepEqual(wheelNotches(0, -100), { notches: 1, rest: 0 });
    assert.deepEqual(wheelNotches(0, 300), { notches: -3, rest: 0 });
    assert.deepEqual(wheelNotches(0, -40), { notches: 0, rest: 40 });
    assert.equal(wheelNotches(80, -40).notches, 1);
    assert.equal(wheelNotches(0, -3, 1).notches, 0);
    assert.equal(wheelNotches(0, -1, 2).notches, 1);
  },
  "detents closer than the firmware's 45 ms are a fast turn"() {
    assert.equal(isFast(1000, 1000 + FAST_MS - 1, 1), true);
    assert.equal(isFast(1000, 1000 + FAST_MS, 1), false);
    assert.equal(isFast(1000, 1100, -3), true);
  },
  "MASTER stays in 0..100 and follows a drag"() {
    assert.equal(masterFromDrag(50, 0, -100, 200), 70);
    assert.equal(masterFromDrag(50, -100, 0, 200), 30);
    assert.equal(masterFromDrag(50, 0, 1000, 200), 0);
    assert.equal(masterFromDrag(50, 0, -1000, 200), 100);
    assert.equal(masterAngle(0), -135);
    assert.equal(masterAngle(100), 135);
  },
  "MASTER has the firmware's hysteresis of 2, and always reaches its ends"() {
    assert.equal(masterFollow(50, 51), 50);
    assert.equal(masterFollow(50, 52), 52);
    assert.equal(masterFollow(50, 48), 48);
    assert.equal(masterFollow(1, 0), 0);
    assert.equal(masterFollow(99, 100), 100);
    assert.equal(masterFollow(100, 140), 100);
  },
  "PARA: a tap is one PUSH, a turn is none, a still press is held"() {
    const run = (...events) => {
      let g = "idle";
      const sent = [];
      for (const e of events) { const s = paraStep(g, e); g = s.to; sent.push(...s.send); }
      return { g, sent };
    };
    assert.deepEqual(run("down", "up"), { g: "idle", sent: ["down", "up"] });
    assert.deepEqual(run("down", "far", "still", "up"), { g: "idle", sent: [] });
    assert.deepEqual(run("down", "still", "far", "up"), { g: "idle", sent: ["down", "up"] });
  },
  "the backlights follow the blocks, the page and the focus"() {
    const on = Array(12).fill(0);
    const state = (over) => ({ mode: "chain", block: BLOCK.amp, preset: { on }, ...over });
    assert.deepEqual(backlights(state()), { home: "focus", fx: "off", amp: "off", cab: "off", mod: "off", dly: "off", rev: "off" });
    const some = [...on];
    some[BLOCK.dly] = 1; some[FX_BLOCKS[3]] = 1;
    let lit = backlights(state({ preset: { on: some } }));
    assert.equal(lit.dly, "on"); assert.equal(lit.fx, "on"); assert.equal(lit.rev, "off");
    lit = backlights(state({ mode: "block", block: BLOCK.rev }));
    assert.equal(lit.rev, "focus"); assert.equal(lit.home, "on");
    assert.equal(backlights(state({ mode: "edit", block: FX_BLOCKS[0] })).fx, "focus");
    assert.equal(backlights(state({ mode: "tuner", block: BLOCK.rev })).rev, "off");
  },
};

let failed = 0;
for (const [what, run] of Object.entries(tests)) {
  try { run(); console.log(`${what.padEnd(76)} ok`); } catch (e) { console.log(`${what.padEnd(76)} FAIL\n${e.message}`); failed++; }
}
console.log(failed ? `\n${failed} FAILED` : "\npedal input: ok");
process.exit(failed ? 1 : 0);
