#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// The hosted OpenPocket simulator: tests/edsim.c (the pedal's real editor, preset store and screen code, natively) behind
// a local HTTP interface, so a person or an agent can press its footswitches, turn its knobs, read its screen and talk
// SysEx to it. A virtual Web MIDI shim lets the installer and editor pages use it as a pedal. docs/SIMULATOR.md.
//   node tools/op.mjs sim [--port 8766] [--flash build/sim/flash.bin] [--reset]
//   node tools/op.mjs sim --update FLASH.bin [--stock IDENTITY]     the update flow (tests/devsim.c) for the installer
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:http";
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync } from "node:fs";
import { dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { deflateSync } from "node:zlib";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BUILD = join(ROOT, "build", "sim");
const BIN = join(BUILD, `${process.platform}-${process.arch}`);   // the container shares build/ with the host: one binary each
const SIM_FILES = join(ROOT, "tools", "sim");
const SIM_SERVED = new Set(["webmidi-shim.js", "pedal-input.js", "pedal-audio.js", "pedal-worklet.js", "px-engine.js", "px_engine.wasm"]);
const LOOPS = join(ROOT, "assets", "loops");

// ---- the native simulators, built with the host's C compiler (no JieLi toolchain, no Docker) ----
const PROGRAMS = {
  pedal: { src: ["tests/edsim.c", "engine/px_dsp.c", "firmware/px_math.c"], libs: ["-lm"] },
  update: { src: ["tests/devsim.c"], libs: [] },
};
function newest(dirs) {
  let t = 0;
  for (const d of dirs) {
    for (const f of readdirSync(join(ROOT, d))) if (/\.[ch]$/.test(f)) t = Math.max(t, statSync(join(ROOT, d, f)).mtimeMs);
  }
  return t;
}
function compile(kind) {
  const bin = join(BIN, kind === "pedal" ? "edsim" : "devsim");
  mkdirSync(BIN, { recursive: true });
  const stale = !existsSync(bin) || statSync(bin).mtimeMs < newest(["tests", "engine", "firmware", "firmware/bsp/chip", "loader"]);
  if (!stale) return bin;
  const p = PROGRAMS[kind];
  const r = spawnSync(process.env.CC || "cc", ["-O1", "-Wall", "-Wno-unused-function", "-o", bin, ...p.src, ...p.libs], { cwd: ROOT, stdio: "inherit" });
  if (r.error || r.status !== 0) {
    throw new Error(r.error ? `no C compiler (${r.error.message}): install one (Xcode command line tools, build-essential) or set CC` : "the simulator did not compile");
  }
  return bin;
}

// ---- one simulated pedal: the native program on stdin/stdout, one line per MIDI message or '#' command ----
class Pedal {
  constructor(kind, args) {
    this.kind = kind; this.args = args;
    this.port = null; this.log = [];
    this.waiters = new Map();                      // reply word -> FIFO of resolvers
    this.listeners = new Set();                    // (event, value): "midi" (Uint8Array), "port" (name | null)
    this.chain = Promise.resolve();                // SysEx requests go one at a time, as the pedal takes them
  }
  emit(ev, v) { for (const l of this.listeners) l(ev, v); }
  setPort(name) { if (name !== this.port) { this.port = name; this.emit("port", name); } }
  start() {
    const child = spawn(compile(this.kind), this.args, { stdio: ["pipe", "pipe", "inherit"] });
    this.child = child;
    let buf = "", ready;
    this.ready = new Promise((r) => { ready = r; });
    this.exited = new Promise((r) => child.on("exit", (code) => { this.setPort(null); this.failAll(); ready(); r(code); }));
    child.stdin.on("error", () => { });
    child.stdout.on("data", (d) => {
      buf += d.toString();
      let i;
      while ((i = buf.indexOf("\n")) >= 0) { const ln = buf.slice(0, i); buf = buf.slice(i + 1); this.line(ln, ready); }
    });
    return this.ready;
  }
  line(ln, ready) {
    if (!ln) return;
    if (ln.startsWith("#port ")) { const n = ln.slice(6); this.setPort(n === "none" ? null : n); ready(); return; }
    if (ln.startsWith("#log ")) { this.log.push(ln.slice(5)); if (this.log.length > 200) this.log.shift(); return; }
    if (ln.startsWith("#")) {
      const sp = ln.indexOf(" "), word = sp < 0 ? ln.slice(1) : ln.slice(1, sp);
      this.waiters.get(word)?.shift()?.(sp < 0 ? "" : ln.slice(sp + 1));
      return;
    }
    this.emit("midi", Uint8Array.from(Buffer.from(ln, "hex")));
  }
  failAll() { for (const q of this.waiters.values()) for (const r of q.splice(0)) r(null); }
  // a '#' command: resolves with the text after the reply word, null when the program ended or stayed silent
  ask(cmd, word = cmd.slice(1).split(" ")[0], ms = 10000) {
    return new Promise((resolve) => {
      if (this.child.exitCode !== null) return resolve(null);
      const q = this.waiters.get(word) || this.waiters.set(word, []).get(word);
      const t = setTimeout(() => { const i = q.indexOf(res); if (i >= 0) q.splice(i, 1); resolve(null); }, ms);
      const res = (v) => { clearTimeout(t); resolve(v); };
      q.push(res);
      this.child.stdin.write(cmd + "\n");
    });
  }
  midi(bytes) { this.child.stdin.write(Buffer.from(bytes).toString("hex") + "\n"); }
  // a request and its reply: an editor request is answered by the frame with the same command byte
  sysex(bytes, ms = 1000) {
    const run = () => new Promise((resolve) => {
      const editor = bytes[1] === 0x7D && bytes[2] === 0x4F && bytes[3] === 0x50;
      const done = (v) => { clearTimeout(t); this.listeners.delete(l); resolve(v); };
      const l = (ev, m) => { if (ev === "midi" && (!editor || (m[1] === 0x7D && m[2] === 0x4F && m[3] === 0x50 && m[4] === bytes[4]))) done(m); };
      const t = setTimeout(() => done(null), ms);
      this.listeners.add(l);
      this.midi(bytes);
    });
    const p = this.chain.then(run);
    this.chain = p.catch(() => { });
    return p;
  }
  async stop() {
    if (!this.child) return;
    this.child.stdin.end();
    const t = setTimeout(() => this.child.kill(), 3000);
    await this.exited;
    clearTimeout(t);
  }
}

// ---- the screen as a PNG ----
const CRC = Uint32Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
function crc32(b) { let c = ~0; for (const x of b) c = CRC[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; }
function chunk(type, data) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0); out.write(type, 4, "ascii"); data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}
function rgb565(be) {                                  // 240x240 big-endian RGB565 -> RGB888
  const px = Buffer.alloc(240 * 240 * 3);
  for (let i = 0; i < 240 * 240; i++) {
    const c = be.readUInt16BE(i * 2);
    px[i * 3] = ((c >> 11) & 31) * 255 / 31; px[i * 3 + 1] = ((c >> 5) & 63) * 255 / 63; px[i * 3 + 2] = (c & 31) * 255 / 31;
  }
  return px;
}
function png(be, scale = 1) {
  const w = 240 * scale, rgb = rgb565(be), raw = Buffer.alloc((w * 3 + 1) * w);
  for (let y = 0; y < w; y++) {
    const o = y * (w * 3 + 1), sy = Math.floor(y / scale);
    for (let x = 0; x < w; x++) rgb.copy(raw, o + 1 + x * 3, (sy * 240 + Math.floor(x / scale)) * 3, (sy * 240 + Math.floor(x / scale)) * 3 + 3);
  }
  const head = Buffer.alloc(13);
  head.writeUInt32BE(w, 0); head.writeUInt32BE(w, 4); head[8] = 8; head[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", head), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}

// ---- HTTP ----
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".json": "application/json", ".png": "image/png", ".css": "text/css", ".md": "text/plain; charset=utf-8", ".wasm": "application/wasm", ".wav": "audio/wav" };
const BUTTONS = ["fs_up", "fs_dn", "fx", "home", "push", "mod", "amp", "cab", "dly", "rev"];
const CONTROLS = [...BUTTONS, "push_long", "dpush", "home_long", "fx_long", "feet"];
const SHIM_TAG = '<script src="/sim/webmidi-shim.js"></script>';

class HttpError extends Error { constructor(status, message) { super(message); this.status = status; } }
const hex = (s) => { const t = s.replace(/[^0-9a-fA-F]/g, ""); if (!t.length || t.length % 2) throw new HttpError(400, "body: hex bytes (or a JSON array of numbers)"); return Uint8Array.from(Buffer.from(t, "hex")); };
const midiBytes = (body) => {
  const t = body.toString("utf8").trim();
  const bytes = t.startsWith("[") ? Uint8Array.from(JSON.parse(t)) : t.startsWith("{") ? Uint8Array.from(JSON.parse(t).data) : hex(t);
  if (!bytes.length || bytes.some((b) => !(b >= 0 && b < 256))) throw new HttpError(400, "body: bytes 0..255");
  return bytes;
};
async function readBody(req, max = 2 << 20) {
  const parts = []; let n = 0;
  for await (const c of req) { n += c.length; if (n > max) throw new HttpError(413, "body too large"); parts.push(c); }
  return Buffer.concat(parts);
}
const num = (s, lo, hi, what) => { const v = Number(s); if (!Number.isFinite(v) || v < lo || v > hi) throw new HttpError(400, `${what}: a number from ${lo} to ${hi}`); return v; };

export async function startSim(opts = {}) {
  const update = opts.update ? resolve(opts.update) : null;
  const flash = resolve(opts.flash || join(BUILD, "flash.bin"));
  const kind = update ? "update" : "pedal";
  mkdirSync(dirname(flash), { recursive: true });
  const args = () => update ? [update, ...(opts.stock ? ["--stock", opts.stock] : []), "--dump", join(BUILD, "update-dump.bin")] : ["--persist", flash];
  if (!update && opts.reset) rmSync(flash, { force: true });
  let pedal = new Pedal(kind, args());
  const clients = new Set();                          // SSE responses
  const attach = (p) => p.listeners.add((ev, v) => {
    const data = ev === "midi" ? Buffer.from(v).toString("hex") : v || "";
    for (const c of clients) c.write(`event: ${ev}\ndata: ${data}\n\n`);
  });
  attach(pedal);
  let busy = pedal.start();
  const restart = async (afterStop) => {
    await busy;
    await pedal.stop();
    afterStop?.();
    const next = new Pedal(kind, args());
    attach(next);
    pedal = next;
    busy = pedal.start();
    await busy;
  };
  await busy;
  if (pedal.child.exitCode !== null) throw new Error(`the simulator program ended at once (exit ${pedal.child.exitCode}): is ${update || flash} a 1 MiB flash image?`);

  const needPedal = () => { if (kind !== "pedal") throw new HttpError(409, "this is the update simulator (--update): it has no screen or controls"); };
  const reply = async (cmd) => { const r = await pedal.ask(cmd); if (r === null) throw new HttpError(503, "the simulated pedal did not answer"); return r; };
  const ok = async (cmd) => { const r = await reply(cmd); if (r !== "ok") throw new HttpError(400, r.replace(/^err ?/, "") || "refused"); return { ok: true }; };

  async function api(req, res, path, query) {
    const [, , what, a, b] = path.split("/");
    const post = req.method === "POST";
    if (what === "info") return json(res, { kind, port: pedal.port, flash: update || flash, controls: CONTROLS, log: pedal.log });
    if (what === "events") {
      res.writeHead(200, { "content-type": "text/event-stream", "cache-control": "no-store", connection: "keep-alive", ...cors });
      res.write(`event: port\ndata: ${pedal.port || ""}\n\n`);
      clients.add(res);
      const beat = setInterval(() => res.write(": \n\n"), 15000);
      req.on("close", () => { clearInterval(beat); clients.delete(res); });
      return;
    }
    if (what === "midi" && post) { pedal.midi(midiBytes(await readBody(req))); res.writeHead(204, cors); return res.end(); }
    if (what === "sysex" && post) {
      const t0 = Date.now(), m = await pedal.sysex(midiBytes(await readBody(req)), num(query.get("timeout") ?? 1000, 1, 60000, "timeout"));
      return json(res, { reply: m && Buffer.from(m).toString("hex"), ms: Date.now() - t0 });
    }
    if (what === "reset" && post) { await restart(() => { if (kind === "pedal") rmSync(flash, { force: true }); }); return json(res, { ok: true }); }
    if (what === "restart" && post) { await restart(); return json(res, { ok: true }); }
    needPedal();
    await busy;
    if (what === "state") { const s = await reply("#status"); return json(res, JSON.parse(s)); }
    if (what === "screen.png" || what === "screen.rgb565") {
      const h = await reply("#screen");
      const be = Buffer.from(h, "hex");
      if (what === "screen.rgb565") { res.writeHead(200, { "content-type": "application/octet-stream", "cache-control": "no-store", ...cors }); return res.end(be); }
      res.writeHead(200, { "content-type": "image/png", "cache-control": "no-store", ...cors });
      return res.end(png(be, num(query.get("scale") ?? 1, 1, 8, "scale")));
    }
    if (what === "flash") {
      const tmp = join(BUILD, `flash-${process.pid}.tmp`);
      await ok(`#flash ${tmp}`);
      const data = readFileSync(tmp); rmSync(tmp, { force: true });
      res.writeHead(200, { "content-type": "application/octet-stream", ...cors });
      return res.end(data);
    }
    if (!post) throw new HttpError(405, "POST");
    if (what === "press") { if (!CONTROLS.includes(a)) throw new HttpError(400, `control: one of ${CONTROLS.join(" ")}`); return json(res, await ok(`#press ${a}`)); }
    if (what === "down" || what === "up") { if (!BUTTONS.includes(a)) throw new HttpError(400, `button: one of ${BUTTONS.join(" ")}`); return json(res, await ok(`#${what} ${a}`)); }
    if (what === "knob") return json(res, await ok(`#knob ${Math.trunc(num(a, -200, 200, "knob steps"))}${query.has("fast") ? " fast" : ""}`));
    if (what === "volume") return json(res, await ok(`#vol ${Math.trunc(num(a, 0, 100, "volume"))}`));
    if (what === "battery") return json(res, await ok(`#bat ${Math.trunc(num(a, 0, 6000, "battery mV"))} ${b === undefined ? "" : Math.trunc(num(b, 0, 6000, "USB mV"))}`));
    if (what === "tuner") return json(res, await ok(`#hz ${num(a, 0, 5000, "tuner Hz")}`));
    if (what === "load") return json(res, await ok(`#dsp load ${Math.trunc(num(a, 0, 999, "DSP load"))}`));
    if (what === "bypass") return json(res, await ok(`#dsp bypass ${Math.trunc(num(a, 0, 1, "bypass"))}`));
    throw new HttpError(404, "no such endpoint");
  }

  const cors = { "access-control-allow-origin": "*", "access-control-allow-private-network": "true", "access-control-allow-headers": "*", "access-control-allow-methods": "GET, POST, OPTIONS" };
  const json = (res, obj, status = 200) => { res.writeHead(status, { "content-type": "application/json", "cache-control": "no-store", ...cors }); res.end(JSON.stringify(obj)); };
  const file = (res, path, inject) => {
    if (!existsSync(path) || !statSync(path).isFile()) throw new HttpError(404, "not found");
    let data = readFileSync(path);
    if (inject) data = Buffer.from(data.toString("utf8").replace(/<head[^>]*>/i, (m) => m + SHIM_TAG));
    res.writeHead(200, { "content-type": TYPES[extname(path)] || "application/octet-stream", "cache-control": "no-store", ...cors });
    res.end(data);
  };
  const within = (dir, rel) => { const p = resolve(dir, "." + sep + decodeURIComponent(rel)); if (p !== dir && !p.startsWith(dir + sep)) throw new HttpError(404, "not found"); return p; };

  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url, "http://localhost");
      const path = url.pathname;
      if (req.method === "OPTIONS") { res.writeHead(204, cors); return res.end(); }
      if (path.startsWith("/api/")) return await api(req, res, path, url.searchParams);
      if (req.method !== "GET") throw new HttpError(405, "GET");
      if (path === "/") return file(res, join(SIM_FILES, "pedal.html"));
      if (path === "/plain") return file(res, join(SIM_FILES, "page.html"));
      if (path.startsWith("/sim/") && SIM_SERVED.has(path.slice(5))) return file(res, join(SIM_FILES, path.slice(5)));
      if (path.startsWith("/loops/")) return file(res, within(LOOPS, path.slice(7)));
      if (path.startsWith("/site/")) { const p = within(join(ROOT, "docs"), path.slice(6) || "index.html"); return file(res, p, extname(p) === ".html"); }
      throw new HttpError(404, "not found");
    } catch (e) {
      if (!res.headersSent) json(res, { error: e instanceof HttpError || e instanceof SyntaxError ? e.message : String(e) }, e.status || (e instanceof SyntaxError ? 400 : 500));
      else res.end();
    }
  });
  try {
    await new Promise((r, j) => { server.once("error", j); server.listen(opts.port ?? 8766, opts.host || "127.0.0.1", r); });
  } catch (e) {
    await pedal.stop();                                 // the port is taken: do not leave the pedal program behind
    throw e;
  }
  const url = `http://${server.address().address}:${server.address().port}`;
  return {
    url, kind, flash,
    async close() {
      for (const c of clients) c.end();
      server.close(); server.closeAllConnections?.();
      await busy; await pedal.stop();
    },
  };
}

// ---- command line ----
const USAGE = `usage: node tools/op.mjs sim [--port 8766] [--host 127.0.0.1] [--flash FILE] [--reset]
       node tools/op.mjs sim --update FLASH.bin [--stock IDENTITY] [--port N]
  --flash FILE   the pedal's flash, kept between runs (default build/sim/flash.bin); --reset starts from a blank one
  --update FILE  the update flow (the installer's stand-in) on this 1 MiB flash image (never written) instead of the pedal`;
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const argv = process.argv.slice(2), o = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i], val = () => { if (i + 1 >= argv.length) { console.error(USAGE); process.exit(2); } return argv[++i]; };
    if (a === "--port") o.port = Number(val());
    else if (a === "--host") o.host = val();
    else if (a === "--flash") o.flash = val();
    else if (a === "--reset") o.reset = true;
    else if (a === "--update") o.update = val();
    else if (a === "--stock") o.stock = val();
    else { console.error(USAGE); process.exit(a === "--help" || a === "-h" ? 0 : 2); }
  }
  if (o.update && !existsSync(o.update)) { console.error(`${o.update} not found`); process.exit(2); }
  try {
    const sim = await startSim(o);
    console.log(`OpenPocket simulator (${sim.kind}) listening on ${sim.url}/`);
    console.log(`  the pedal: ${sim.url}/    plain controls: ${sim.url}/plain    installer: ${sim.url}/site/index.html    editor: ${sim.url}/site/editor.html`);
    if (sim.kind === "pedal") console.log(`  flash: ${sim.flash}`);
    const quit = async () => { await sim.close(); process.exit(0); };
    process.on("SIGINT", quit); process.on("SIGTERM", quit);
  } catch (e) { console.error(`ERROR: ${e.message}`); process.exit(1); }
}
