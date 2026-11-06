// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket — a fake WebMIDI access over one simulated pedal (tests/devsim.c), shared by the installer and flasher tests.
import { spawn } from "node:child_process";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const SIM = process.env.DEVSIM || join(fileURLToPath(new URL("..", import.meta.url)), "build", "devsim");

// a fake WebMIDI access over one simulated pedal: its port pair is renamed when the device re-enumerates
export function simAccess(args) {
  const child = spawn(SIM, args, { stdio: ["pipe", "pipe", "inherit"] });
  const log = [];
  const access = { inputs: new Map(), outputs: new Map() };
  let port = null, buf = "", readyFn;
  const ready = new Promise((r) => { readyFn = r; });
  const mkPorts = (name) => {
    if (port) { port.input.state = "disconnected"; port.output.state = "disconnected"; }
    access.inputs.clear(); access.outputs.clear();
    if (name === "none") { port = null; return; }
    const input = { name, state: "connected", onmidimessage: null, open: async () => {} };
    const output = { name, state: "connected", open: async () => {},
      send: (bytes) => {
        if (output.state === "disconnected") throw new Error("disconnected");
        child.stdin.write(Buffer.from(bytes).toString("hex") + "\n");
      } };
    port = { input, output };
    access.inputs.set(name, input); access.outputs.set(name, output);
  };
  child.stdout.on("data", (d) => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const ln = buf.slice(0, i); buf = buf.slice(i + 1);
      if (ln.startsWith("#port ")) { mkPorts(ln.slice(6)); readyFn(); }
      else if (ln.startsWith("#log ")) log.push(ln.slice(5));
      else if (port && port.input.onmidimessage && ln) port.input.onmidimessage({ data: Uint8Array.from(Buffer.from(ln, "hex")) });
    }
  });
  const done = new Promise((r) => child.on("exit", (code) => r(code)));
  return { access, log, ready, end: async () => { child.stdin.end(); return done; } };
}

// a pedal that restarts in a loop, over a simulated pedal's access: its USB ports are there for `up` ms, then gone for
// `down` ms, and while they are there the pedal says nothing until cycle `answerOn` (1 = the first). From that cycle on it
// answers and stays up (stopping its sound processing ended the loop), unless `vanishAfterSends` cuts it off once, after
// that many MIDI messages to it in the answering cycle. Returns { access, cycles(), end }.
export function loopAccess(sim, { up = 600, down = 300, answerOn = 1, vanishAfterSends = 0 } = {}) {
  const state = { cycle: 0, visible: false, locked: false, sends: 0, vanished: false };
  let timer = null, stopped = false;
  const phase = () => {
    if (stopped || state.locked) return;
    state.visible = !state.visible;
    if (state.visible) { state.cycle++; if (state.cycle >= answerOn) state.locked = true; }
    timer = setTimeout(phase, state.visible ? up : down);
  };
  phase();
  const proxies = new WeakMap();
  const wrap = (real, kind) => {
    if (!proxies.has(real)) {
      const p = {
        get name() { return real.name; },
        get state() { return state.visible && real.state === "connected" ? "connected" : "disconnected"; },
        open: async () => {},
        get onmidimessage() { return real.onmidimessage; },
        set onmidimessage(f) { real.onmidimessage = f; },
      };
      if (kind === "out") {
        p.send = (bytes) => {
          if (!state.visible) throw new Error("disconnected");
          if (!state.locked) return;                       // restarting firmware: no answer
          if (vanishAfterSends && !state.vanished && ++state.sends > vanishAfterSends) {
            state.vanished = true; state.locked = false; state.visible = false;
            timer = setTimeout(phase, down);               // back after `down` ms, and then up for good
            state.cycle = answerOn - 1;
            throw new Error("disconnected");
          }
          real.send(bytes);
        };
      }
      proxies.set(real, p);
    }
    return proxies.get(real);
  };
  const access = {
    inputs: { values: () => (state.visible ? [...sim.access.inputs.values()].map((r) => wrap(r, "in")) : [])[Symbol.iterator]() },
    outputs: { values: () => (state.visible ? [...sim.access.outputs.values()].map((r) => wrap(r, "out")) : [])[Symbol.iterator]() },
  };
  return { access, cycles: () => state.cycle, end: async () => { stopped = true; clearTimeout(timer); return sim.end(); } };
}
