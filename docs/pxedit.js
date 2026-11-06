// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — the web editor's protocol over WebMIDI (docs/EDITOR-PROTOCOL.md; the pedal side: firmware/px_editor.h).
// Request / reply, one at a time; STATE frames the pedal pushes while watched go to onState.

import { PORT_RE } from "./pxota.js";

const H = [0x7D, 0x4F, 0x50];
export const C = { INFO: 1, CAT_BLOCK: 2, CAT_TYPE: 3, TEXTS: 4, STATE: 5, SET: 6, ORDER: 7, NAME: 8, PUT: 9, SELECT: 10,
  FOCUS: 11, LIST: 12, SLOT_GET: 13, SLOT_PUT: 14, COMMIT: 15, SAVE: 16, WATCH: 17, PING: 18, DIAG: 19 };
export const P_SIZE = 110;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function fail(code, msg) { const e = new Error(msg); e.code = code; return e; }

// a reply's arguments, read in order
class Reader {
  constructor(a) { this.a = a; this.i = 0; }
  b() { if (this.i >= this.a.length) throw fail("short", "reply too short"); return this.a[this.i++]; }
  s() { let t = ""; for (;;) { const c = this.b(); if (!c) return t; t += String.fromCharCode(c); } }
  bytes(n) { const r = this.a.slice(this.i, this.i + n); if (r.length < n) throw fail("short", "reply too short"); this.i += n; return r; }
}

// preset <-> the 110 bytes of px_preset_t
export function decodePreset(b, nblk = 12, npar = 5) {
  let i = 0;
  const name = String.fromCharCode(...b.slice(0, 14)).replace(/\0.*$/s, "");
  i = 14;
  const on = [...b.slice(i, i + nblk)].map((x) => x ? 1 : 0); i += nblk;
  const type = [...b.slice(i, i + nblk)]; i += nblk;
  const p = []; for (let k = 0; k < nblk; k++) { p.push([...b.slice(i, i + npar)]); i += npar; }
  const order = [...b.slice(i, i + nblk)];
  return { name, on, type, p, order };
}
export function encodePreset(q, nblk = 12, npar = 5) {
  const out = [];
  const nm = cleanName(q.name);
  for (let i = 0; i < 14; i++) out.push(i < nm.length ? nm.charCodeAt(i) : 0);
  for (let k = 0; k < nblk; k++) out.push(q.on[k] ? 1 : 0);
  for (let k = 0; k < nblk; k++) out.push(q.type[k] & 0x7F);
  for (let k = 0; k < nblk; k++) for (let j = 0; j < npar; j++) out.push(Math.max(0, Math.min(100, q.p[k][j] | 0)));
  for (let k = 0; k < nblk; k++) out.push(q.order[k] & 0x7F);
  return out;
}
export function cleanName(s, max = 13) {
  return String(s || "").replace(/[^\x20-\x7E]/g, "?").slice(0, max);
}
export const clonePreset = (q) => ({ name: q.name, on: [...q.on], type: [...q.type], p: q.p.map((r) => [...r]), order: [...q.order] });

export class Editor {
  constructor(access, opts = {}) {
    this.access = access;
    this.timeout = opts.timeout || 1500;
    this.onState = null;          // (state) => {}: pushed by the pedal
    this.onLost = null;           // () => {}: the pedal stopped answering
    this.link = null;
    this.queue = Promise.resolve();
    this.waiting = null;          // { cmd, resolve, reject, timer }
    this.info = null;
    this.cat = null;
  }

  // ---- transport ----
  _onMessage(data) {
    const d = data instanceof Uint8Array ? data : Uint8Array.from(data);
    if (d.length < 6 || d[0] !== 0xF0 || d[1] !== H[0] || d[2] !== H[1] || d[3] !== H[2] || d[d.length - 1] !== 0xF7) return;
    const cmd = d[4], args = d.slice(5, d.length - 1);
    if (this.waiting && this.waiting.cmd === cmd) {
      const w = this.waiting; this.waiting = null; clearTimeout(w.timer); w.resolve(args);
      return;
    }
    if (cmd === C.STATE && this.onState) { try { this.onState(this._state(args)); } catch (_) { } }
  }
  _send(cmd, args) {
    if (!this.link) throw fail("closed", "not connected");
    this.link.output.send([0xF0, ...H, cmd, ...args.map((x) => x & 0x7F), 0xF7]);
  }
  _once(cmd, args, timeout) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.waiting = null; reject(fail("timeout", "the pedal did not answer")); }, timeout);
      this.waiting = { cmd, resolve, reject, timer };
      try { this._send(cmd, args); } catch (e) { clearTimeout(timer); this.waiting = null; reject(e); }
    });
  }
  // one request at a time; one retry after a timeout
  request(cmd, args = [], timeout = this.timeout) {
    const run = async () => {
      try { return await this._once(cmd, args, timeout); }
      catch (e) {
        if (e.code !== "timeout") throw e;
        await sleep(50);
        try { return await this._once(cmd, args, timeout); }
        catch (e2) { if (e2.code === "timeout" && this.onLost) this.onLost(); throw e2; }
      }
    };
    const p = this.queue.then(run, run);
    this.queue = p.catch(() => {});
    return p;
  }

  // the first OpenPocket port that answers INFO
  async connect() {
    const outs = [...this.access.outputs.values()];
    for (const input of this.access.inputs.values()) {
      if (input.state === "disconnected" || !PORT_RE.test(input.name || "")) continue;
      const output = outs.find((o) => o.name === input.name) || (outs.length === 1 ? outs[0] : null);
      if (!output) continue;
      try { await input.open(); await output.open(); } catch (_) { continue; }
      this.link = { input, output };
      input.onmidimessage = (ev) => this._onMessage(ev.data);
      try {
        const r = new Reader(await this.request(C.INFO, [], 600));
        const info = { proto: r.b(), version: r.s(), identity: r.s(), nblk: r.b(), npar: r.b(), nslot: r.b(), nameMax: r.b(), port: input.name };
        if (info.proto >= 1) { this.info = info; return info; }
      } catch (_) { }
      input.onmidimessage = null;
      this.link = null;
    }
    return null;
  }
  // the log channel: its text lines; a timeout when this firmware has none (only the Bluetooth test image answers)
  async diag() {
    const r = new Reader(await this.request(C.DIAG, [], 600));
    return Array.from({ length: r.b() }, () => r.s());
  }
  close() {
    if (this.link) { try { this.link.input.onmidimessage = null; } catch (_) { } }
    this.link = null;
  }

  // ---- the catalogue: blocks, types, parameter names and value texts (cached per firmware) ----
  async catalog(progress = () => {}) {
    const key = `openpocket-cat-${this.info.identity}-${this.info.version}`;
    try { const c = JSON.parse(localStorage.getItem(key) || "null"); if (c && c.blocks && c.blocks.length === this.info.nblk) { this.cat = c; return c; } } catch (_) { }
    const blocks = [];
    for (let b = 0; b < this.info.nblk; b++) {
      const r = new Reader(await this.request(C.CAT_BLOCK, [b]));
      r.b();
      const label = r.s(), name = r.s(), ntypes = r.b();
      const c = r.b() | r.b() << 7 | r.b() << 14;
      const rgb = `#${(((c >> 11) & 31) * 255 / 31 | 0).toString(16).padStart(2, "0")}${(((c >> 5) & 63) * 255 / 63 | 0).toString(16).padStart(2, "0")}${((c & 31) * 255 / 31 | 0).toString(16).padStart(2, "0")}`;
      blocks.push({ label, name, color: rgb, types: new Array(ntypes) });
    }
    const total = blocks.reduce((s, b) => s + b.types.length, 0);
    let done = 0;
    for (let b = 0; b < blocks.length; b++) {
      for (let t = 0; t < blocks[b].types.length; t++) {
        const r = new Reader(await this.request(C.CAT_TYPE, [b, t]));
        r.b(); r.b();
        const name = r.s(), np = r.b(), mask = r.b();
        const params = [];
        for (let k = 0; k < np; k++) params.push({ name: r.s(), texts: null });
        for (let k = 0; k < np; k++) {
          if (!(mask >> k & 1)) continue;
          const q = new Reader(await this.request(C.TEXTS, [b, t, k]));
          q.b(); q.b(); q.b();
          const texts = []; for (let v = 0; v <= 100; v++) texts.push(q.s());
          params[k].texts = texts;
        }
        blocks[b].types[t] = { name, params };
        progress(++done / total);
      }
    }
    this.cat = { blocks };
    try { localStorage.setItem(key, JSON.stringify(this.cat)); } catch (_) { }
    return this.cat;
  }

  // ---- the current sound ----
  _state(args) {
    const r = new Reader(args);
    const slot = r.b(), flags = r.b(), focus = r.b();
    return { slot, dirty: !!(flags & 1), saved: !!(flags & 2), focus, preset: decodePreset(r.bytes(P_SIZE), this.info.nblk, this.info.npar) };
  }
  async state() { return this._state(await this.request(C.STATE)); }
  async set(block, what, k, v) { const r = await this.request(C.SET, [block, what, k, v]); return r[3]; }
  setParam(block, k, v) { return this.set(block, 0, k, v); }
  setOn(block, on) { return this.set(block, 1, 0, on ? 1 : 0); }
  setType(block, t) { return this.set(block, 2, 0, t); }
  async order(order) { return (await this.request(C.ORDER, order))[0]; }
  async name(s) { return new Reader(await this.request(C.NAME, [...cleanName(s, this.info.nameMax)].map((c) => c.charCodeAt(0)).concat([0]))).s(); }
  async put(q) { return (await this.request(C.PUT, encodePreset(q, this.info.nblk, this.info.npar)))[0]; }
  async select(slot) { return (await this.request(C.SELECT, [slot]))[1]; }
  async focus(block) { await this.request(C.FOCUS, [block]); }

  // ---- the slots ----
  async list() {
    const out = [];
    for (let s = 0; s < this.info.nslot; s += 26) {
      const r = new Reader(await this.request(C.LIST, [s, Math.min(26, this.info.nslot - s)]));
      r.b(); const n = r.b();
      for (let i = 0; i < n; i++) out.push({ slot: s + i, saved: !!r.b(), name: r.s() });
    }
    return out;
  }
  async slotGet(slot) {
    const r = new Reader(await this.request(C.SLOT_GET, [slot]));
    r.b(); const saved = !!r.b();
    return { slot, saved, preset: decodePreset(r.bytes(P_SIZE), this.info.nblk, this.info.npar) };
  }
  // q null: back to the factory preset. commit: write the flash now
  async slotPut(slot, q, commit) {
    const args = [slot, (q ? 1 : 0) | (commit ? 2 : 0)];
    if (q) args.push(...encodePreset(q, this.info.nblk, this.info.npar));
    return (await this.request(C.SLOT_PUT, args, commit ? 4000 : this.timeout))[1];
  }
  async commit() { return (await this.request(C.COMMIT, [], 4000))[0]; }
  async save(slot) { return (await this.request(C.SAVE, [slot], 4000))[1]; }
  async watch(on) { return (await this.request(C.WATCH, [on ? 1 : 0]))[0]; }
  async ping() { const r = await this.request(C.PING); return { load: r[0], bypass: !!(r[1] & 1), c1: !!(r[1] & 2) }; }
}

// ---- preset files (JSON, blocks and types by name) ----
export function presetToJSON(q, cat, slot) {
  return {
    slot: slot === undefined ? undefined : slot + 1,
    v: 36,                                                       // v36 (preset LEVEL); v35 scales (delay TIME up to 2 s)
    name: q.name,
    blocks: cat.blocks.map((b, i) => ({ block: b.label, on: !!q.on[i], type: b.types[q.type[i]] ? b.types[q.type[i]].name : q.type[i], params: [...q.p[i]] })),
    order: q.order.map((i) => cat.blocks[i] ? cat.blocks[i].label : i),
  };
}
export function filePresets(presets) { return { format: "openpocket-presets", version: 1, presets }; }

// v34: amps, drives and cabs were replaced by fitted models (docs/FITTED-MODELS.md); a preset file from v32 / v33
// names the old types: each one -> the nearest new type, the same choice the pedal makes for its saved presets (px_type_v34)
export const V33_TYPES = {"AMP": {"TWIN CLEAN": "TWIN", "DELUXE 65": "TWIN", "PRINCETONE": "TWIN", "TWEED 59": "SHIRLEY", "TWEED CHAMP": "AC15 TB", "AC 30 TB": "AC15 TB", "AC 15": "AC15 TB", "JTM 45": "SHIRLEY", "PLEXI 68": "SHIRLEY", "BRIT 800": "JCM900", "BRIT JVM": "JCM2000", "HIWATT DR": "JCM900", "ORANGE 120": "JCM2000", "CHIME 30": "AC15 TB", "ODS LEAD": "MARK IV", "SLO 100": "PRO MOD", "MARK IIC+": "MARK IV", "RECTIFIED": "5150", "5150 LEAD": "5150", "5150 III": "5150", "ECSTASY": "PRO MOD", "BE LEAD": "PRO MOD", "UBER VH4": "5150", "EURO BALL": "5150", "REVV GEN": "5150", "JC CLEAN": "TWIN", "SUPRO 64": "AC15 TB", "TWO-ROCK": "TWIN", "SVT BASS": "TWIN"}, "DRV": {"TS 808": "TS9", "TS 9": "TS9", "SD-1 OD": "TS9", "KLONE": "KLON", "BD BLUES": "KLON", "OCD DRIVE": "M77 OD", "RAT DIST": "SWEDE HM-2", "DS-1 DIST": "SWEDE HM-2", "DIST PLUS": "M77 OD", "BLUESBREAKER": "KLON", "KING OF TONE": "KLON", "TIMMY": "KLON", "ZEN DRIVE": "TS9", "MUFF PI": "M77 OD", "RAMS HEAD": "M77 OD", "GREEN RUSSIAN": "M77 OD", "FACE GERMAN.": "M77 OD", "FACE SILICON": "M77 OD", "BENDER MK2": "M77 OD", "OCTAFUZZ": "M77 OD", "FUZZ FACTORY": "M77 OD", "TREBLE BOOST": "M77 OD", "EP BOOST": "KLON", "METAL ZONE": "SWEDE HM-2", "PRECISION": "TS9", "BASS DI 7K": "KLON"}, "CAB": {"1x10 OPEN": "1x12 US OPEN", "2x10 VIBRO": "1x12 US OPEN", "2x12 BRIT": "4x12 GREEN", "4x10 TWEED": "1x12 US OPEN", "4x12 H30": "4x12 V30", "1x15 OPEN": "1x12 US OPEN", "8x10 BASS": "4x12 V30", "4x10 HORN": "1x12 US OPEN"}};

// v35: delay TIME 20..1000 ms in 100 steps became 20..1000 ms in 80 steps, then up to 2 s (px_dly_time_v35)
export const dlyTimeV35 = (v) => Math.min(80, Math.floor(v * (9.8 / 12.25) + 0.5));

// a preset from its JSON; problems (unknown block or type) are listed, the rest still comes through
export function presetFromJSON(o, cat, base) {
  const q = clonePreset(base), warn = [];
  q.name = cleanName(o.name || q.name);
  const byLabel = new Map(cat.blocks.map((b, i) => [b.label, i]));
  for (const ob of Array.isArray(o.blocks) ? o.blocks : []) {
    const i = byLabel.get(ob.block);
    if (i === undefined) { warn.push(`unknown block ${ob.block}`); continue; }
    q.on[i] = ob.on ? 1 : 0;
    let t = typeof ob.type === "number" ? ob.type : cat.blocks[i].types.findIndex((x) => x.name === ob.type);
    const moved = typeof ob.type === "string" && t < 0 && V33_TYPES[ob.block] && V33_TYPES[ob.block][ob.type];
    if (moved) {
      t = cat.blocks[i].types.findIndex((x) => x.name === moved);
      if (t >= 0) warn.push(`${ob.block}: ${ob.type} (before v34) -> ${moved}`);
    }
    if (t < 0 || t >= cat.blocks[i].types.length) { warn.push(`${ob.block}: unknown type ${ob.type}`); t = q.type[i]; }
    q.type[i] = t;
    if (Array.isArray(ob.params)) for (let k = 0; k < q.p[i].length; k++) {
      const v = Number(ob.params[k]);
      if (Number.isFinite(v)) q.p[i][k] = Math.max(0, Math.min(100, Math.round(v)));
    }
    if (!(o.v >= 36) && ob.block === "GT") q.p[i][2] = 50;           // before v36: no preset LEVEL (0 dB)
    if (!(o.v >= 35) && ob.block === "DLY" && cat.blocks[i].types[q.type[i]] && cat.blocks[i].types[q.type[i]].name !== "REVERSE") {
      const old = q.p[i][0], now = dlyTimeV35(old);                // a file from before v35: TIME was 20..1000 ms
      if (now !== old) { q.p[i][0] = now; warn.push(`DLY: TIME ${old} (before v35) -> ${now}, same delay`); }
    }
  }
  if (Array.isArray(o.order)) {
    const ord = o.order.map((l) => typeof l === "number" ? l : byLabel.get(l));
    if (ord.length === q.order.length && new Set(ord).size === ord.length && ord.every((x) => x !== undefined && x >= 0 && x < q.order.length)) q.order = ord;
    else warn.push("block order not usable: default order kept");
  }
  return { preset: q, warn };
}
export function parsePresetFile(text) {
  const o = JSON.parse(text);
  if (o && o.format === "openpocket-presets" && Array.isArray(o.presets)) return o.presets;
  if (o && Array.isArray(o.blocks)) return [o];                // a bare preset
  throw fail("format", "not an OpenPocket preset file");
}
