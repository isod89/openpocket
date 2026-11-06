// SPDX-License-Identifier: GPL-3.0-only
// The pedal's sound on the page: a source (a bundled loop, a dropped file, an input of the computer) -> the input level ->
// the engine in an AudioWorklet -> the device mute -> the output volume -> the speakers. The context runs at the engine's
// 44.1 kHz, so the browser does every sample-rate conversion. docs/SIMULATOR.md.
import { ENGINE_RATE, GUITAR_LEVEL, presetBytes } from "/sim/px-engine.js";

export const dbfs = (peak) => (peak > 0 ? 20 * Math.log10(peak) : -Infinity);
const tuning = (s) => ({ volume: s.volume, bypass: Boolean(s.dsp.bypass), tunerMute: s.mode === "tuner" });

export class PedalAudio {
  constructor({ onMeter, onLoad, onChange }) {
    Object.assign(this, { onMeter, onLoad, onChange });
    this.ctx = null;
    this.node = null;                                  // the engine
    this.playing = null;                               // { stop(), device: boolean }: the source that sounds now
    this.buffers = new Map();
    this.pedal = null;                                 // the last pedal state seen
    this.sentPreset = "";
    this.sent = {};
    this.levelPercent = 100;
    this.volumePercent = 70;
    this.heard = null;
    this.deviceHeard = false;
    this.repeat = true;
  }
  // must be called from a user gesture: browsers keep a context made outside of one suspended
  async start() {
    this.ctx ??= new AudioContext({ sampleRate: ENGINE_RATE, latencyHint: "interactive" });
    const resumed = this.ctx.resume();
    this.starting ??= this.build().catch((error) => { this.starting = null; throw error; });
    await this.starting;
    await resumed;
  }
  async build() {
    const ctx = this.ctx;
    if (!ctx.audioWorklet) throw new Error("this browser gives audio worklets only to https or localhost pages");
    const [module] = await Promise.all([
      fetch("/sim/px_engine.wasm").then((r) => WebAssembly.compileStreaming(r)),
      ctx.audioWorklet.addModule("/sim/pedal-worklet.js"),
    ]);
    this.level = new GainNode(ctx);
    this.mute = new GainNode(ctx);
    this.out = new GainNode(ctx);
    this.node = new AudioWorkletNode(ctx, "pedal", {
      numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 1, channelCountMode: "explicit",
      channelInterpretation: "speakers", processorOptions: { module },
    });
    this.node.port.onmessage = ({ data }) => {
      this.heard = data.heard;
      this.onMeter(data);
      if (data.load !== this.load) { this.load = data.load; this.onLoad(data.load); }
    };
    this.level.connect(this.node).connect(this.mute).connect(this.out).connect(ctx.destination);
    this.setLevel(this.levelPercent);
    this.setVolume(this.volumePercent);
    if (this.pedal) this.follow(this.pedal);
  }
  // the sound follows the pedal: what `GET /api/state` says now, as audio_poll() in firmware/px_audio.h
  follow(state) {
    this.pedal = state;
    if (!this.node) return;
    const port = this.node.port, preset = JSON.stringify(state.preset);
    if (preset !== this.sentPreset) {
      this.sentPreset = preset;
      port.postMessage({ preset: presetBytes(state.preset), name: state.preset.name });
    }
    for (const [key, value] of Object.entries(tuning(state))) {
      if (this.sent[key] !== value) { this.sent[key] = value; port.postMessage({ [key]: value }); }
    }
  }
  setLevel(percent) { this.levelPercent = percent; if (this.level) this.level.gain.value = percent / 100 * GUITAR_LEVEL; }
  setVolume(percent) { this.volumePercent = percent; if (this.out) this.out.gain.value = (percent / 100) ** 2; }
  setRepeat(on) { this.repeat = on; if (this.playing?.buffer) this.playing.buffer.loop = on; }
  hearDevice(on) { this.deviceHeard = on; this.applyMute(); }
  applyMute() { if (this.mute) this.mute.gain.value = this.playing?.device && !this.deviceHeard ? 0 : 1; }

  async decode(key, bytes) {
    if (!this.buffers.has(key)) this.buffers.set(key, await this.ctx.decodeAudioData(await bytes()));
    return this.buffers.get(key);
  }
  async playBuffer(key, bytes) {
    await this.start();
    const buffer = await this.decode(key, bytes);
    this.stop();
    const node = new AudioBufferSourceNode(this.ctx, { buffer, loop: this.repeat });
    node.connect(this.level);
    const playing = { buffer: node, device: false, stop: () => { node.onended = null; node.stop(); node.disconnect(); } };
    node.onended = () => { if (this.playing === playing) { this.playing = null; this.onChange(); } };
    node.start();
    this.playing = playing;
    this.applyMute();
    this.onChange();
  }
  async playDevice(deviceId) {
    await this.start();
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: { deviceId: deviceId ? { exact: deviceId } : undefined, echoCancellation: false, noiseSuppression: false, autoGainControl: false },
    });
    const close = () => { for (const t of stream.getTracks()) t.stop(); };
    let node;
    try { node = this.ctx.createMediaStreamSource(stream); } catch (e) { close(); throw e; }   // Firefox: another sample rate
    this.stop();
    node.connect(this.level);
    this.playing = { device: true, stop: () => { node.disconnect(); close(); } };
    this.applyMute();
    this.onChange();
  }
  stop() {
    this.playing?.stop();
    this.playing = null;
    this.onChange();
  }
  state() {
    return {
      running: this.ctx?.state === "running", sampleRate: this.ctx?.sampleRate ?? null, playing: Boolean(this.playing),
      device: Boolean(this.playing?.device), muted: this.mute ? this.mute.gain.value === 0 : false, heard: this.heard,
      latencyMs: this.ctx ? Math.round((this.ctx.baseLatency + (this.ctx.outputLatency || 0)) * 1000) : null,
    };
  }
}
