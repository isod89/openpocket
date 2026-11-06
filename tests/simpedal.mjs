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
