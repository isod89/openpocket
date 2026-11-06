// SPDX-License-Identifier: GPL-3.0-only
// The simulated pedal's sound: the WebAssembly engine (tools/sim/px_engine.wasm) on known signals, against the native build,
// against the pedal's preset as the simulator reports it; and the bundled loops against their manifest. Run by tests/run_tests.sh.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { startSim } from "../tools/sim.mjs";
import { BLOCK, ENGINE_RATE, PxEngine, presetBytes } from "../tools/sim/px-engine.js";
import { engineModule, readWav, run, wavBytes } from "../tools/sim/render.mjs";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const TMP = mkdtempSync(join(tmpdir(), "opaudio-"));
const read = (p) => readFileSync(join(ROOT, p));
const sha256 = (...buffers) => { const h = createHash("sha256"); for (const b of buffers) h.update(b); return h.digest("hex"); };
const check = (what, fn) => { fn(); console.log(`${what.padEnd(76)} ok`); };

check("the committed engine is built from these sources (else: node tools/op.mjs wasm)", () => {
  const sources = ["engine/px_wasm.c", "engine/px_dsp.c", "firmware/px_math.c", "engine/px_dsp.h", "engine/px_trim.h"];
  assert.equal(sha256(...sources.map(read)), read("tools/sim/px_engine.wasm.sources").toString().trim());
});

const module = engineModule();
const tone = (hz, seconds, level) => Float32Array.from({ length: Math.round(seconds * ENGINE_RATE / BLOCK) * BLOCK }, (_, i) => level * Math.sin(2 * Math.PI * hz * i / ENGINE_RATE));
const engine = (preset = 0, master = 100) => { const e = new PxEngine(module); e.factory(preset); e.volume(master); return e; };
const peak = (a) => a.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
const finite = (a) => a.every(Number.isFinite);
const tail = (a) => a.subarray(a.length >> 1);
const power = (a, hz) => {                             // Goertzel: the power at one frequency
  let s1 = 0, s2 = 0;
  const k = 2 * Math.cos(2 * Math.PI * hz / ENGINE_RATE);
  for (const v of a) { const s = v + k * s1 - s2; s2 = s1; s1 = s; }
  return (s1 * s1 + s2 * s2 - k * s1 * s2) / a.length ** 2;
};
const harmonics = (a, hz) => Array.from({ length: 8 }, (_, n) => power(a, hz * (n + 1)));
const IN = tone(220, 1, 0.15);

check("a tone through the first preset is heard, without a NaN", () => {
  const out = run(engine(0), IN);
  assert.ok(finite(out));
  assert.ok(peak(tail(out)) > 0.02, `peak ${peak(tail(out))}`);
});
check("every factory preset: tone, silence and overload stay finite", () => {
  const count = 52, silence = new Float32Array(IN.length), loud = tone(82.4, 1, 4);
  for (let p = 0; p < count; p++) {
    const e = engine(p);
    for (const input of [IN, silence, loud]) assert.ok(finite(run(e, input)), `preset ${p + 1}`);
  }
});
check("bypass passes the dry input, at MASTER", () => {
  const e = engine(15, 80);
  e.bypass(true);
  const out = run(e, IN);
  out.forEach((v, i) => assert.ok(Math.abs(v - IN[i] * 0.64) < 1e-6));
});
check("a preset change changes the spectrum", () => {
  const clean = harmonics(tail(run(engine(0), IN)), 220), metal = harmonics(tail(run(engine(15), IN)), 220);
  const share = (h) => h.slice(1).reduce((a, b) => a + b) / h[0];
  assert.ok(share(metal) > 3 * share(clean), `harmonics over fundamental: ${share(clean)} ${share(metal)}`);
});
check("a block switched off changes the sound", () => {
  const s = new PxEngine(module);
  s.factory(15);
  const bytes = new Uint8Array(s.x.memory.buffer, s.x.px_preset(), 110).slice(), amp = 14 + 5;
  const a = run(s, IN);
  bytes[amp] = 0;
  s.preset(bytes);
  const b = run(s, IN);
  assert.notDeepEqual(harmonics(tail(a), 220), harmonics(tail(b), 220));
});
check("MASTER follows the firmware's curve", () => {
  const ratio = peak(tail(run(engine(0, 40), IN))) / peak(tail(run(engine(0, 80), IN)));
  assert.ok(Math.abs(ratio - 0.25) < 0.01, `ratio ${ratio}`);
});
check("the tuner mutes the output with a ramp", () => {
  const e = engine(0);
  run(e, IN);
  e.tunerMute(true);
  const out = run(e, IN);
  assert.ok(peak(out.subarray(0, BLOCK)) > 0, "the first block fades");
  assert.equal(peak(tail(out)), 0);
});
check("output never leaves full scale", () => assert.ok(peak(run(engine(5), tone(110, 1, 8))) <= 1));
// Every factory preset, on a test tone and on each bundled loop (at guitar level, 0.15), through the WebAssembly engine
// (MASTER 50, x0.25, so nothing clips) and the native build of the same code. Both must be finite and agree exactly:
// the code is the same, both use the pedal's px_math.c, and x0.25 is exact in floating point. The largest difference of
// each pair is the evidence (EVIDENCE_DIR: audio-parity.tsv).
const TOLERANCE = 0;
const GAIN = Math.fround(0.15);
check("every factory preset, on a tone and every loop: WebAssembly sounds as the native build", () => {
  const exe = join(TMP, "preset"), input = join(TMP, "in.wav"), output = join(TMP, "out.f32");
  const cc = spawnSync(process.env.CC || "cc", ["-O2", "-fno-builtin", "-ffp-contract=off", "-Iengine", "tests/engine/preset.c", "engine/px_dsp.c", "firmware/px_math.c", "-lm", "-o", exe], { cwd: ROOT, stdio: "inherit" });
  assert.equal(cc.status, 0);
  const signals = [{ name: "tone 220 Hz", wav: wavBytes(IN), gain: 1 }];
  for (const l of JSON.parse(read("assets/loops/manifest.json")).loops) signals.push({ name: l.file, wav: read(join("assets/loops", l.file)), gain: GAIN });
  const rows = ["preset\tsignal\tnative_peak\twasm_peak\tmax_abs_diff\tdiff_over_native_peak"];
  for (const { name, wav, gain } of signals) {
    writeFileSync(input, wav);
    const samples = readWav(wav).samples, heard = samples.map((v) => v * gain);
    for (let preset = 0; preset < 52; preset++) {
      assert.equal(spawnSync(exe, [String(preset), input, output, String(gain)]).status, 0);
      const b = readFileSync(output), native = new Float32Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.length)), e = engine(preset, 50);
      e.x.px_reset();                                  // as the harness: one px_engine_set() on a new engine
      const wasm = run(e, heard).subarray(0, native.length);
      assert.ok(finite(native) && finite(wasm), `preset ${preset + 1}, ${name}: not finite`);
      const scale = peak(native);
      assert.ok(scale > 0.001 && scale < 4, `preset ${preset + 1}, ${name}: peak ${scale}`);
      let worst = 0;
      wasm.forEach((v, i) => { worst = Math.max(worst, Math.abs(v - native[i] * 0.25)); });
      assert.ok(worst <= TOLERANCE * scale, `preset ${preset + 1}, ${name}: differs by ${worst} (native peak ${scale})`);
      rows.push([preset + 1, name, scale, peak(wasm), worst, worst / scale].join("\t"));
    }
  }
  if (process.env.EVIDENCE_DIR) writeFileSync(join(process.env.EVIDENCE_DIR, "audio-parity.tsv"), rows.join("\n") + "\n");
});

const sim = await startSim({ port: 0, flash: join(TMP, "flash.bin") });
try {
  const state = await (await fetch(sim.url + "/api/state")).json();
  check("the pedal's preset, as the simulator reports it, is the engine's", () => {
    const e = engine(state.slot);
    const memory = new Uint8Array(e.x.memory.buffer, e.x.px_preset(), 110);
    assert.deepEqual([...presetBytes(state.preset)], [...memory]);
  });
  const served = await fetch(sim.url + "/sim/px_engine.wasm");
  check("the engine is served as WebAssembly", () => assert.equal(served.headers.get("content-type"), "application/wasm"));
  await served.arrayBuffer();
  const post = (path) => fetch(sim.url + "/api/" + path, { method: "POST" });
  await post("load/17");
  await post("bypass/1");
  const s = await (await fetch(sim.url + "/api/state")).json();
  check("the status follows the measured load and the bypass", () => assert.deepEqual(s.dsp, { load: 17, bypass: 1, core1: 1 }));
} finally {
  await sim.close();
}

const ALLOWED = { "CC0-1.0": "publicdomain/zero/1.0", "CC-BY-4.0": "licenses/by/4.0", "CC-BY-3.0": "licenses/by/3.0", "CC-BY-SA-4.0": "licenses/by-sa/4.0" };
const manifest = JSON.parse(read("assets/loops/manifest.json"));
const attribution = read("assets/loops/ATTRIBUTION.md").toString();
check("four to six loops, a few MB", () => {
  assert.ok(manifest.loops.length >= 4 && manifest.loops.length <= 6);
  const bytes = manifest.loops.reduce((n, l) => n + statSync(join(ROOT, "assets/loops", l.file)).size, 0);
  assert.ok(bytes < 5e6, `${bytes} bytes`);
});
for (const l of manifest.loops) {
  const bytes = read(join("assets/loops", l.file)), { rate, channels, samples } = readWav(bytes);
  check(`${l.file}: licence, checksum, format, level, seam`, () => {
    assert.ok(l.license in ALLOWED, l.license);
    assert.ok(l.license_url.includes(ALLOWED[l.license]), l.license_url);
    assert.ok(l.title && l.author && l.source.startsWith("https://") && l.attribution);
    assert.ok(attribution.includes(l.attribution), "ATTRIBUTION.md");
    for (const field of [l.file, l.author, l.source, l.license_url]) assert.ok(attribution.includes(field), `ATTRIBUTION.md lacks ${field}`);
    assert.ok(["dry", "amped"].includes(l.signal));
    assert.equal(l.sha256, sha256(bytes));
    assert.equal(rate, l.sample_rate);
    assert.equal(rate, ENGINE_RATE);
    assert.equal(channels, 1);
    assert.ok(Math.abs(samples.length / rate - l.seconds) < 0.01);
    const p = peak(samples);
    assert.ok(p > 0.5 && p <= 0.75, `peak ${p}`);
    const largest = peak(samples.slice(1).map((v, i) => v - samples[i]));
    assert.ok(Math.abs(samples[0] - samples.at(-1)) < largest / 2, "the end meets the start");
    assert.ok(finite(run(engine(0), samples.map((v) => v * 0.15))));
  });
}
rmSync(TMP, { recursive: true, force: true });
