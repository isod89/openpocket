// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — NAM A2 models for the pedal (v37): read a .nam file, check it is an A2 network, take its A2-Lite part,
// measure its level and pack it for a user slot (firmware/px_models.h). The network itself runs here only to measure
// the level, in float, with the same maths as tools/nam_a2.py and engine/px_nam.c.

export const KERNELS = [6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6];
export const DILATIONS = [1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239];
export const HEAD_K = 16;
export const NW = 1871;                 // A2-Lite weights (head_scale included)
export const REF_LUFS = -20;            // tools/nam_level.py REF: every model at this loudness after its trim
export const SLOT_BYTES = 64 + NW * 4;  // header + weights
const SLOPE = 0.01;
const FILMS = ["conv_pre_film", "conv_post_film", "input_mixin_pre_film", "input_mixin_post_film", "activation_pre_film",
  "activation_post_film", "layer1x1_post_film", "head1x1_post_film"];

const eqList = (a, b) => Array.isArray(a) && a.length === b.length && a.every((v, i) => v === b[i]);

// channels of an A2 WaveNet entry (3 = A2-Lite, 8 = A2-Full), 0 if it is not one
export function a2Channels(m) {
  if (!m || m.architecture !== "WaveNet" || !m.config) return 0;
  const cfg = m.config;
  if (cfg.head != null || !Array.isArray(cfg.layers) || cfg.layers.length !== 1) return 0;
  const L = cfg.layers[0], c = L.channels, head = L.head || {};
  const ok = eqList(L.kernel_sizes, KERNELS) && eqList(L.dilations, DILATIONS) && L.bottleneck === c &&
    L.input_size === 1 && L.condition_size === 1 && head.kernel_size === HEAD_K && head.out_channels === 1 &&
    head.bias === true && !(L.head1x1 || {}).active && (L.layer1x1 || { active: true }).active !== false &&
    (L.gating_mode || ["none"]).every((g) => g == null || g === "none") &&
    (L.activation || [null]).every((a) => a && a.type === "LeakyReLU" && Math.abs((a.negative_slope ?? 0) - SLOPE) < 1e-9) &&
    FILMS.every((k) => !(L[k] || {}).active);
  return ok ? c : 0;
}

// A .nam file (text) -> { name, weights: Float32Array(1871), sampleRate, note } or throws with a reason a person reads
export function parseNam(text, fileName = "") {
  let j;
  try { j = JSON.parse(text); } catch (_) { throw new Error("this is not a .nam file (not JSON)"); }
  const subs = j.architecture === "SlimmableContainer" ? ((j.config || {}).submodels || []).map((s) => s.model) : [j];
  const found = subs.map((m) => [m, a2Channels(m)]).filter(([, c]) => c);
  if (!found.length) {
    const arch = j.architecture || "?";
    throw new Error(`not an A2 model (${arch}${arch === "WaveNet" ? ", an A1 or custom WaveNet" : ""}). ` +
      "The pedal runs NAM's A2 captures (A2-Lite): on TONE3000, pick the A2 version of a capture.");
  }
  found.sort((a, b) => a[1] - b[1]);
  const [m, c] = found[0];
  if (c !== 3) throw new Error(`this A2 file has no A2-Lite part (only ${c} channels): the pedal needs A2-Lite`);
  const w = m.weights;
  if (!Array.isArray(w) || w.length !== NW) throw new Error(`A2-Lite with ${w ? w.length : 0} weights, ${NW} expected`);
  const weights = new Float32Array(w);
  for (const v of weights) if (!Number.isFinite(v)) throw new Error("the file holds a weight that is not a number");
  const sr = m.sample_rate || j.sample_rate || 48000;
  const meta = m.metadata || j.metadata || {};
  const base = (meta.name || fileName.replace(/\.nam$/i, "") || "MODEL").toUpperCase().replace(/[^ -~]/g, "?");
  const note = Math.round(sr) !== 44100 ? `made at ${Math.round(sr / 100) / 10} kHz: the pedal plays it at 44.1 kHz ` +
    `(its tone sits ${(12 * Math.log2(sr / 44100)).toFixed(1)} semitones lower)` : "";
  return { name: base.slice(0, 11).trim(), weights, sampleRate: sr, note };
}

// the network on x (Float32Array), float: y = A2-Lite(x)
export function a2Run(weights, x) {
  const C = 3, T = x.length;
  let i = 0;
  const take = (n) => { const r = weights.subarray(i, i + n); i += n; return r; };
  const rech = take(C);
  let X = [0, 1, 2].map((c) => { const a = new Float32Array(T); for (let t = 0; t < T; t++) a[t] = rech[c] * x[t]; return a; });
  const hs = [0, 1, 2].map(() => new Float32Array(T));
  const z = [0, 1, 2].map(() => new Float32Array(T));
  for (let l = 0; l < KERNELS.length; l++) {
    const K = KERNELS[l], d = DILATIONS[l];
    const conv = take(C * C * K), cb = take(C), mix = take(C), one = take(C * C), ob = take(C);
    for (let o = 0; o < C; o++) {
      const zo = z[o];
      for (let t = 0; t < T; t++) zo[t] = cb[o] + mix[o] * x[t];
      for (let c = 0; c < C; c++) {
        const xc = X[c];
        for (let k = 0; k < K; k++) {
          const w = conv[(o * C + c) * K + k], s = (K - 1 - k) * d;
          for (let t = s; t < T; t++) zo[t] += w * xc[t - s];
        }
      }
    }
    const Xn = [0, 1, 2].map(() => new Float32Array(T));
    for (let t = 0; t < T; t++) {
      const a0 = z[0][t] >= 0 ? z[0][t] : z[0][t] * SLOPE, a1 = z[1][t] >= 0 ? z[1][t] : z[1][t] * SLOPE,
        a2 = z[2][t] >= 0 ? z[2][t] : z[2][t] * SLOPE;
      hs[0][t] += a0; hs[1][t] += a1; hs[2][t] += a2;
      for (let o = 0; o < C; o++) Xn[o][t] = X[o][t] + one[o * C] * a0 + one[o * C + 1] * a1 + one[o * C + 2] * a2 + ob[o];
    }
    X = Xn;
  }
  const head = take(C * HEAD_K), hb = take(1)[0], scale = take(1)[0];
  const y = new Float32Array(T);
  for (let t = 0; t < T; t++) {
    let v = hb;
    for (let c = 0; c < C; c++) for (let k = 0; k < HEAD_K; k++) { const s = HEAD_K - 1 - k; if (t >= s) v += head[c * HEAD_K + k] * hs[c][t - s]; }
    y[t] = scale * v;
  }
  return y;
}

// ITU BS.1770 loudness at 44.1 kHz (K-weighting, 400 ms blocks, gates): tools/nam_level.py's lufs()
function biquad(x, b, a) {
  const y = new Float64Array(x.length); let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (let n = 0; n < x.length; n++) {
    const v = b[0] * x[n] + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2;
    x2 = x1; x1 = x[n]; y2 = y1; y1 = v; y[n] = v;
  }
  return y;
}
export function lufs(x, SR = 44100) {
  let f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
  let K = Math.tan(Math.PI * f0 / SR); const Vh = 10 ** (G / 20), Vb = Vh ** 0.4996667741545416; let a0 = 1 + K / Q + K * K;
  let y = biquad(x, [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0],
    [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]);
  f0 = 38.13547087602444; Q = 0.5003270373238773; K = Math.tan(Math.PI * f0 / SR); a0 = 1 + K / Q + K * K;
  y = biquad(y, [1, -2, 1], [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]);
  const blk = Math.floor(0.4 * SR), hop = Math.floor(0.1 * SR), z = [];
  for (let i = 0; i + blk <= y.length; i += hop) { let s = 0; for (let k = i; k < i + blk; k++) s += y[k] * y[k]; z.push(s / blk); }
  const L = (m) => -0.691 + 10 * Math.log10(m);
  let g = z.filter((m) => L(m + 1e-20) > -70);
  if (!g.length) return -99;
  const rel = L(g.reduce((a, b) => a + b, 0) / g.length) - 10;
  g = g.filter((m) => L(m) > rel);
  return L(g.reduce((a, b) => a + b, 0) / g.length);
}

// level trim (dB) that brings the model to REF_LUFS on the DI clip (docs/nam_di.wav, 44.1 kHz)
export function levelTrim(weights, di) {
  const t = REF_LUFS - lufs(a2Run(weights, di));
  return Math.max(-24, Math.min(24, t));
}

// docs/nam_di.wav -> Float32Array
export function readDi(buf) {
  const v = new DataView(buf);
  let p = 12, data = null;
  while (p + 8 <= v.byteLength) {
    const id = String.fromCharCode(v.getUint8(p), v.getUint8(p + 1), v.getUint8(p + 2), v.getUint8(p + 3)), n = v.getUint32(p + 4, true);
    if (id === "data") { data = [p + 8, n]; break; }
    p += 8 + n + (n & 1);
  }
  if (!data) throw new Error("DI clip: no data");
  const x = new Float32Array(data[1] >> 1);
  for (let i = 0; i < x.length; i++) x[i] = v.getInt16(data[0] + 2 * i, true) / 32768;
  return x;
}

export function crc16(b, c = 0) {             // CRC16, poly 0x1021, init 0 (ota_crc16)
  for (const x of b) { c ^= x << 8; for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF; }
  return c;
}

// a user slot's bytes (firmware/px_models.h nm_hdr_t + weights, little endian)
export function packModel(name, weights, levelDb) {
  const out = new Uint8Array(SLOT_BYTES), v = new DataView(out.buffer);
  const wb = new Uint8Array(weights.buffer.slice(weights.byteOffset, weights.byteOffset + NW * 4));
  out.set(wb, 64);
  v.setUint32(0, 0x4D4E504F, true);           // "OPNM"
  v.setUint16(4, 1, true);
  v.setUint16(6, NW, true);
  const nm = name.toUpperCase().replace(/[^ -~]/g, "?").slice(0, 11);
  for (let i = 0; i < nm.length; i++) out[8 + i] = nm.charCodeAt(i);
  v.setInt16(20, Math.round(levelDb * 100), true);
  v.setUint16(22, crc16(wb), true);
  return out;
}

// 8-bit bytes -> 7-bit groups (a byte of high bits, then 7 bytes): px_models.h nm_unpack7
export function pack7(b) {
  const o = [];
  for (let i = 0; i < b.length; i += 7) {
    let hi = 0; const g = [];
    for (let j = 0; j < 7 && i + j < b.length; j++) { hi |= (b[i + j] >> 7) << j; g.push(b[i + j] & 0x7F); }
    o.push(hi, ...g);
  }
  return o;
}
