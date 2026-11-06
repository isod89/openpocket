// SPDX-License-Identifier: GPL-3.0-only
// The pedal's sound thread: each 128 frames go through the engine as the firmware's I2S interrupt sends them. Mono in, the
// same signal on both outputs (a_dec in firmware/px_audio.h). docs/SIMULATOR.md.
import { BLOCK, ENGINE_RATE, PxEngine } from "/sim/px-engine.js";

const REPORT_BLOCKS = 16;                              // about 46 ms: the meters
const LOAD_BLOCKS = 172;                               // about 0.5 s, as often as the pedal's status text changes
const BLOCK_MS = BLOCK / ENGINE_RATE * 1000;
const now = () => globalThis.performance?.now() ?? Date.now();
const peak = (a) => { let m = 0; for (const v of a) { const x = Math.abs(v); if (x > m) m = x; } return m; };

class PedalProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.engine = new PxEngine(options.processorOptions.module);
    this.heard = null;
    this.inPeak = this.outPeak = this.blocks = this.busyMs = this.busyBlocks = 0;
    this.load = 0;
    this.port.onmessage = ({ data }) => {
      if (data.preset) { this.engine.preset(data.preset); this.heard = data.name; }
      if (data.volume !== undefined) this.engine.volume(data.volume);
      if (data.bypass !== undefined) this.engine.bypass(data.bypass);
      if (data.tunerMute !== undefined) this.engine.tunerMute(data.tunerMute);
    };
  }
  process(inputs, outputs) {
    const e = this.engine, input = inputs[0][0];
    if (input) e.input.set(input); else e.input.fill(0);
    const t0 = now();
    e.process();
    this.busyMs += now() - t0;
    for (const channel of outputs[0]) channel.set(e.output);
    this.inPeak = Math.max(this.inPeak, peak(e.input));
    this.outPeak = Math.max(this.outPeak, peak(e.output));
    if (++this.busyBlocks === LOAD_BLOCKS) {
      this.load = Math.round(this.busyMs / (LOAD_BLOCKS * BLOCK_MS) * 100);
      this.busyMs = this.busyBlocks = 0;
    }
    if (++this.blocks === REPORT_BLOCKS) {
      this.port.postMessage({ input: this.inPeak, output: this.outPeak, load: this.load, heard: this.heard });
      this.inPeak = this.outPeak = this.blocks = 0;
    }
    return true;
  }
}
registerProcessor("pedal", PedalProcessor);
