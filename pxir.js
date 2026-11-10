// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — cabinet impulse responses for the pedal (v38): read a WAV file (PCM 16 / 24 / 32-bit or float, any rate,
// mono or stereo), bring it to 44.1 kHz, keep the first 1024 samples (23 ms) from its onset with a short fade at the end,
// and pack it for a user slot (firmware/px_irs.h: a 64-byte header, then 1024 16-bit samples). The pedal sets the
// level itself (every IR at the same loudness as the modelled cabinets).

import { crc16 } from "./pxnam.js";

export const IR_LEN = 1024;
export const IR_SR = 44100;
export const IR_BYTES = 64 + IR_LEN * 2;
export const IR_CHUNK = 112;          // bytes per IR_WRITE (firmware/px_irs.h IR_CHUNK)

// a WAV file (ArrayBuffer) -> { sampleRate, channels, bits, x: Float32Array (the first channel) } or throws
export function parseWav(buf) {
  const v = new DataView(buf);
  const id = (p) => String.fromCharCode(v.getUint8(p), v.getUint8(p + 1), v.getUint8(p + 2), v.getUint8(p + 3));
  if (v.byteLength < 12 || id(0) !== "RIFF" || id(8) !== "WAVE") throw new Error("this is not a WAV file");
  let p = 12, fmt = null, data = null;
  while (p + 8 <= v.byteLength) {
    const c = id(p), n = v.getUint32(p + 4, true);
    if (c === "fmt ") fmt = p + 8;
    if (c === "data") { data = [p + 8, Math.min(n, v.byteLength - p - 8)]; break; }
    p += 8 + n + (n & 1);
  }
  if (fmt === null || !data) throw new Error("the WAV file has no format or no data");
  let tag = v.getUint16(fmt, true);
  const ch = v.getUint16(fmt + 2, true), sr = v.getUint32(fmt + 4, true), bits = v.getUint16(fmt + 14, true);
  if (tag === 0xFFFE) tag = v.getUint16(fmt + 24, true);    // WAVE_FORMAT_EXTENSIBLE: the sub-format's first 2 bytes
  if (!(ch >= 1) || !(sr >= 8000 && sr <= 384000)) throw new Error("the WAV file's format is not one the pedal can read");
  const bps = bits >> 3, frame = bps * ch, n = Math.floor(data[1] / frame), x = new Float32Array(n);
  if (!n) throw new Error("the WAV file is empty");
  for (let i = 0; i < n; i++) {
    const o = data[0] + i * frame;
    let s;
    if (tag === 3 && bits === 32) s = v.getFloat32(o, true);
    else if (tag === 3 && bits === 64) s = v.getFloat64(o, true);
    else if (tag === 1 && bits === 16) s = v.getInt16(o, true) / 32768;
    else if (tag === 1 && bits === 24) s = ((v.getUint8(o) | v.getUint8(o + 1) << 8 | v.getInt8(o + 2) << 16)) / 8388608;
    else if (tag === 1 && bits === 32) s = v.getInt32(o, true) / 2147483648;
    else if (tag === 1 && bits === 8) s = (v.getUint8(o) - 128) / 128;
    else throw new Error(`WAV with ${bits}-bit ${tag === 3 ? "float" : "format " + tag} samples: not supported`);
    x[i] = s;
  }
  return { sampleRate: sr, channels: ch, bits, x };
}

// x at rate sr -> the first `count` samples at 44.1 kHz (windowed sinc, 24 zero crossings a side, a low-pass at the lower
// Nyquist when the rate goes down)
export function resample(x, sr, count = IR_LEN) {
  if (sr === IR_SR) return Float32Array.from({ length: count }, (_, i) => (i < x.length ? x[i] : 0));
  const r = IR_SR / sr, fc = Math.min(1, r), Z = 24, out = new Float32Array(count);
  const bessel0 = (t) => { let s = 1, d = 1; for (let k = 1; k < 30; k++) { d *= (t / (2 * k)) ** 2; s += d; } return s; };
  const beta = 8.6, kn = bessel0(beta);
  for (let i = 0; i < count; i++) {
    const t = i / r, c = Math.floor(t), w = Math.ceil(Z / fc);
    let acc = 0;
    for (let k = c - w + 1; k <= c + w; k++) {
      if (k < 0 || k >= x.length) continue;
      const d = (t - k) * fc;
      const sinc = d === 0 ? 1 : Math.sin(Math.PI * d) / (Math.PI * d);
      const u = (t - k) / (w + 1);
      const win = Math.abs(u) >= 1 ? 0 : bessel0(beta * Math.sqrt(1 - u * u)) / kn;
      acc += x[k] * sinc * win;
    }
    out[i] = acc * fc;
  }
  return out;
}

// a parsed WAV -> { h: Float32Array(1024) at 44.1 kHz, note }: from its onset (a pre-delay of silence dropped), faded out
// over its last 64 samples when the response is longer
export function prepareIr(w) {
  const { x, sampleRate: sr } = w;
  let peak = 0;
  for (const v of x) peak = Math.max(peak, Math.abs(v));
  if (!(peak > 1e-6)) throw new Error("the WAV file is silent");
  let on = 0;
  while (on < x.length && Math.abs(x[on]) < peak * 0.01) on++;
  on = Math.max(0, on - Math.round(sr * 0.0001));      // keep 0.1 ms before the onset
  const src = x.subarray(on);
  const h = resample(src, sr, IR_LEN);
  const longer = src.length * IR_SR / sr > IR_LEN;
  if (longer) for (let i = 0; i < 64; i++) h[IR_LEN - 64 + i] *= 0.5 + 0.5 * Math.cos(Math.PI * (i + 1) / 64);
  const ms = (src.length / sr * 1000).toFixed(0);
  const notes = [];
  if (sr !== IR_SR) notes.push(`resampled from ${sr / 1000} kHz`);
  if (longer) notes.push(`its first 23 ms kept (of ${ms} ms)`);
  if (w.channels > 1) notes.push("its left channel used");
  return { h, note: notes.join(", ") };
}

// a user slot's bytes (firmware/px_irs.h ir_hdr_t + 1024 int16, little endian)
export function packIr(name, h) {
  const out = new Uint8Array(IR_BYTES), v = new DataView(out.buffer);
  let peak = 0;
  for (const s of h) peak = Math.max(peak, Math.abs(s));
  const scale = peak > 0 ? peak : 1;
  for (let i = 0; i < IR_LEN; i++) v.setInt16(64 + 2 * i, Math.max(-32767, Math.min(32767, Math.round(h[i] / scale * 32767))), true);
  v.setUint32(0, 0x5249504F, true);           // "OPIR"
  v.setUint16(4, 1, true);
  v.setUint16(6, IR_LEN, true);
  const nm = name.toUpperCase().replace(/[^ -~]/g, "?").slice(0, 11);
  for (let i = 0; i < nm.length; i++) out[8 + i] = nm.charCodeAt(i);
  v.setFloat32(20, scale, true);
  v.setUint16(24, crc16(out.subarray(64)), true);
  return out;
}

// a file name -> the slot's name (11 characters, the pedal's font)
export const irName = (fileName) => (fileName.replace(/\.wav$/i, "").toUpperCase().replace(/[^ -~]/g, "?").replace(/_/g, " ").trim() || "IR").slice(0, 11).trim();
