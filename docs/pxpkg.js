// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — .fwsc packages in the browser: the identity, the image the pedal reads during an update, and the
// check of an official package for the way back. Adapted from Felucca's web/fm1pkg.js (GPL-3.0, Leo Kuroshita,
// Hügelton Instruments).

const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;

// the package identity ("PocketX_924"): one marker byte after each of the first 20 blocks
export function productOf(fwsc) {
  if (fwsc.length < BLOCKS * BLK) throw new Error("not a .fwsc package (too short)");
  let s = "";
  for (let i = 0; i < BLOCKS; i++) {
    const m = fwsc[i * BLK + KEEP];
    if (m !== 0x7D) s += String.fromCharCode((m - i - 1) & 0xFF);
  }
  return s;
}

// the image the pedal addresses during an update: the .fwsc without its 20 marker bytes
export function logicalImage(fwsc) {
  const out = new Uint8Array(fwsc.length - BLOCKS);
  for (let i = 0; i < BLOCKS; i++) out.set(fwsc.subarray(i * BLK, i * BLK + KEEP), i * KEEP);
  out.set(fwsc.subarray(BLOCKS * BLK), BLOCKS * KEEP);
  return out;
}

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, "0")).join("");
}

// JieLi UFW header (ENC 0xFFFF, CRC16): the entries of a package, or null when it is not a well-formed one
function enc(b, key = 0xFFFF) {
  for (let i = 0; i < b.length; i++) {
    b[i] ^= key & 0xFF;
    key = ((key << 1) ^ (key & 0x8000 ? 0x1021 : 0)) & 0xFFFF;
  }
  return b;
}
function crc16(b, c = 0) {
  for (const x of b) {
    c ^= x << 8;
    for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  return c;
}
export function ufwEntries(logical) {
  if (logical.length < 0x400) return null;
  const h = enc(logical.slice(0, 0x40));
  const dv = new DataView(h.buffer);
  const n = dv.getUint16(8, true);
  if (crc16(h.subarray(2, 0x40)) !== dv.getUint16(0, true) || !n || n > 11) return null;
  if (crc16(logical.subarray(0x40, 0x40 + n * 0x50)) !== dv.getUint16(2, true)) return null;
  const out = [];
  for (let i = 0; i < n; i++) {
    const e = enc(logical.slice(0x40 + i * 0x50, 0x40 + (i + 1) * 0x50));
    const v = new DataView(e.buffer);
    const name = new TextDecoder().decode(e.subarray(0x40)).replace(/\0[\s\S]*$/, "");
    const off = v.getUint32(8, true), len = v.getUint32(12, true);
    if (off + len > logical.length) return null;
    out.push({ type: v.getUint16(0, true), name, off, len, crcOk: crc16(logical.subarray(off, off + len)) === v.getUint16(4, true) });
  }
  return out;
}

// The official packages known to this page (M-VAVE / Donner downloads). The SHA-256 pins every byte; the file name
// proves nothing.
export const OFFICIAL = [
  { model: "PocketX", product: "PocketX_016", size: 692244, sha256: "192a997f42ecb9ec0a136d4089f3d3b1fdc96b19f1d76fc237dca64f2119b5d9" },
  { model: "BlackBox", product: "BlackBox_020", size: 700436, sha256: "c0fef191b860d3b20f77fc492a24986eef2d12df44bb5e38355684820e26cb08" },
];

// an official package for the way back: { product, model, known, image } or an Error.
// known = its SHA-256 is one of OFFICIAL; otherwise it must at least be a well-formed official package (identity
// Model_0NN, flash.bin and ota.bin present, their CRCs right): the page asks for a confirmation then
export async function checkOfficial(bytes) {
  const product = productOf(bytes);
  const m = /^(PocketX|BlackBox)_(\d+)$/.exec(product);
  if (!m || parseInt(m[2], 10) >= 900) throw new Error(`not an official Pocket X / BlackBox package (identity "${product}")`);
  const sha = await sha256hex(bytes);
  const known = OFFICIAL.some((k) => k.size === bytes.length && k.sha256 === sha && k.product === product);
  const image = logicalImage(bytes);
  if (!known) {
    const ents = ufwEntries(image);
    const fl = ents && ents.find((e) => e.type === 0), ota = ents && ents.find((e) => e.type === 100);
    if (!fl || !ota || !fl.crcOk || !ota.crcOk) throw new Error("the package is damaged (UFW / CRC)");
  }
  return { product, model: m[1], known, image, sha256: sha };
}

// an OpenPocket package against its manifest entry ({ product, size, sha256 }), before anything is sent to a pedal:
// size, SHA-256, identity (Model_9NN) and the UFW structure. Returns { product, model, image } or throws an Error.
export async function verifyPackage(bytes, entry) {
  if (bytes.length !== entry.size) throw new Error(`the package is ${bytes.length} bytes, the manifest says ${entry.size}`);
  const sha = await sha256hex(bytes);
  if (sha !== entry.sha256) throw new Error(`SHA-256 mismatch: the package is ${sha}, the manifest says ${entry.sha256}`);
  const product = productOf(bytes);
  const m = /^(PocketX|BlackBox)_9\d\d$/.exec(product);
  if (!m || product !== entry.product) throw new Error(`the package identity is "${product}", the manifest says ${entry.product}`);
  const image = logicalImage(bytes);
  const ents = ufwEntries(image);
  const fl = ents && ents.find((e) => e.type === 0), ota = ents && ents.find((e) => e.type === 100);
  if (!fl || !ota || !fl.crcOk || !ota.crcOk) throw new Error("the package is damaged (UFW / CRC)");
  return { product, model: m[1], image };
}
