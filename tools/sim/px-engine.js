// SPDX-License-Identifier: GPL-3.0-only
// The pedal's engine as WebAssembly (engine/px_wasm.c, tools/sim/px_engine.wasm), on arrays of 128 samples at 44.1 kHz. One
// class for the AudioWorklet (tools/sim/pedal-worklet.js) and the tests. docs/SIMULATOR.md.
export const ENGINE_RATE = 44100;
export const BLOCK = 128;
// tools/calib_levels.py: a guitar at the pedal's level, which a full-scale DI recording reaches at this gain
export const GUITAR_LEVEL = 0.15;
const NAME = 14, NBLK = 12, NPAR = 5;

// `preset` is the sound as `GET /api/state` reports it; the bytes are px_preset_t (engine/px_dsp.h)
export function presetBytes(preset) {
  const b = new Uint8Array(NAME + NBLK * 3 + NBLK * NPAR);
  b.set([...preset.name].slice(0, NAME - 1).map((c) => c.charCodeAt(0)));
  b.set(preset.on, NAME);
  b.set(preset.type, NAME + NBLK);
  b.set(preset.p.flat(), NAME + 2 * NBLK);
  b.set(preset.order, NAME + 2 * NBLK + NBLK * NPAR);
  return b;
}

export class PxEngine {
  constructor(module) {
    this.x = new WebAssembly.Instance(module).exports;
    const view = (ptr) => new Float32Array(this.x.memory.buffer, ptr, BLOCK);
    this.input = view(this.x.px_input());
    this.output = view(this.x.px_output());
    this.x.px_factory(0);
    this.x.px_reset();
  }
  factory(index) { this.x.px_factory(index); this.x.px_apply(); }
  preset(bytes) { new Uint8Array(this.x.memory.buffer, this.x.px_preset(), bytes.length).set(bytes); this.x.px_apply(); }
  volume(master) { this.x.px_volume(master); }
  bypass(on) { this.x.px_bypass(on ? 1 : 0); }
  tunerMute(on) { this.x.px_tuner_mute(on ? 1 : 0); }
  process() { this.x.px_process(); }
}
