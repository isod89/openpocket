#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// The bundled loops (assets/loops) through the WebAssembly engine, to WAV files you can listen to:
//   node tools/sim/render.mjs OUTDIR [preset 1..52]
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { BLOCK, ENGINE_RATE, GUITAR_LEVEL, PxEngine } from "./px-engine.js";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

export const engineModule = () => new WebAssembly.Module(readFileSync(join(ROOT, "tools/sim/px_engine.wasm")));

// a 16-bit PCM WAV -> { rate, channels, samples } (first channel only, floats)
export function readWav(buffer) {
  let at = 12, format = null;
  while (at + 8 <= buffer.length) {
    const id = buffer.toString("latin1", at, at + 4), size = buffer.readUInt32LE(at + 4), body = at + 8;
    if (id === "fmt ") format = { pcm: buffer.readUInt16LE(body), channels: buffer.readUInt16LE(body + 2), rate: buffer.readUInt32LE(body + 4), bits: buffer.readUInt16LE(body + 14) };
    if (id === "data") {
      if (!format || format.pcm !== 1 || format.bits !== 16) throw new Error("only 16-bit PCM WAV");
      const frames = Math.floor(size / (2 * format.channels)), samples = new Float32Array(frames);
      for (let i = 0; i < frames; i++) samples[i] = buffer.readInt16LE(body + i * 2 * format.channels) / 32768;
      return { rate: format.rate, channels: format.channels, samples };
    }
    at = body + size + (size & 1);
  }
  throw new Error("no audio in this WAV");
}
export function wavBytes(samples, rate = ENGINE_RATE) {
  const out = Buffer.alloc(44 + samples.length * 2);
  out.write("RIFF", 0, "latin1"); out.writeUInt32LE(36 + samples.length * 2, 4); out.write("WAVEfmt ", 8, "latin1");
  out.writeUInt32LE(16, 16); out.writeUInt16LE(1, 20); out.writeUInt16LE(1, 22); out.writeUInt32LE(rate, 24); out.writeUInt32LE(rate * 2, 28);
  out.writeUInt16LE(2, 32); out.writeUInt16LE(16, 34); out.write("data", 36, "latin1"); out.writeUInt32LE(samples.length * 2, 40);
  samples.forEach((v, i) => out.writeInt16LE(Math.round(Math.max(-1, Math.min(1, v)) * 32767), 44 + i * 2));
  return out;
}
// samples through the engine, block by block; a last short block is padded with silence
export function run(engine, samples) {
  const out = new Float32Array(Math.ceil(samples.length / BLOCK) * BLOCK);
  for (let at = 0; at < samples.length; at += BLOCK) {
    engine.input.fill(0);
    engine.input.set(samples.subarray(at, at + BLOCK));
    engine.process();
    out.set(engine.output, at);
  }
  return out.subarray(0, samples.length);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [outdir, preset = "1"] = process.argv.slice(2);
  if (!outdir) { console.error("usage: node tools/sim/render.mjs OUTDIR [preset 1..52]"); process.exit(2); }
  mkdirSync(outdir, { recursive: true });
  const module = engineModule();
  const { loops } = JSON.parse(readFileSync(join(ROOT, "assets/loops/manifest.json")));
  for (const loop of loops) {
    const { samples } = readWav(readFileSync(join(ROOT, "assets/loops", loop.file)));
    const twice = new Float32Array(samples.length * 2);
    twice.set(samples); twice.set(samples, samples.length);
    const engine = new PxEngine(module);
    engine.factory(Number(preset) - 1);
    engine.volume(80);
    const path = join(outdir, `${basename(loop.file, ".wav")}-preset${preset}.wav`);
    writeFileSync(path, wavBytes(run(engine, twice.map((v) => v * GUITAR_LEVEL))));
    console.log(path);
  }
}
