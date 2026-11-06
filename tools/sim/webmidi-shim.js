// SPDX-License-Identifier: GPL-3.0-only
// A virtual Web MIDI for the OpenPocket simulator (tools/sim.mjs): after this script runs, navigator.requestMIDIAccess()
// returns the simulated pedal as one input and one output port, in any browser, with no driver. The installer and editor
// pages served at /site/ get it injected. For another page, run once in its console:
//   document.head.append(Object.assign(document.createElement("script"), { src: "http://127.0.0.1:8766/sim/webmidi-shim.js" }))
(() => {
  const base = new URL(document.currentScript.src).origin;
  const access = { inputs: new Map(), outputs: new Map(), sysexEnabled: true, onstatechange: null, simulator: base };
  let input = null, output = null, queue = Promise.resolve();
  const post = (bytes) => { queue = queue.then(() => fetch(`${base}/api/midi`, { method: "POST", body: Array.from(bytes).map((b) => b.toString(16).padStart(2, "0")).join("") }).catch(() => { })); };
  const mkPort = (type, name) => ({
    id: `op-sim-${type}`, name, manufacturer: "OpenPocket simulator", version: "1", type, state: "connected", connection: "closed",
    onstatechange: null, onmidimessage: null,
    open() { this.connection = "open"; return Promise.resolve(this); },
    close() { this.connection = "closed"; return Promise.resolve(this); },
    addEventListener() { }, removeEventListener() { },
  });
  const change = (port) => { for (const f of [access.onstatechange, port.onstatechange]) if (f) try { f({ port, target: access }); } catch (e) { console.error(e); } };
  const setPort = (name) => {
    for (const p of [input, output]) if (p) { p.state = "disconnected"; p.connection = "closed"; }
    const old = [input, output].filter(Boolean);
    access.inputs.clear(); access.outputs.clear();
    input = output = null;
    if (name) {
      input = mkPort("input", name); output = mkPort("output", name);
      output.send = (data) => { if (output.state === "disconnected") throw new DOMException("the port is not connected", "InvalidStateError"); post(data); };
      access.inputs.set(input.id, input); access.outputs.set(output.id, output);
    }
    for (const p of [...old, input, output]) if (p) change(p);
  };
  const events = new EventSource(`${base}/api/events`);
  events.addEventListener("port", (e) => setPort(e.data));
  events.addEventListener("midi", (e) => {
    if (input && input.onmidimessage) input.onmidimessage({ data: Uint8Array.from(e.data.match(/../g).map((h) => parseInt(h, 16))), target: input });
  });
  const first = new Promise((r) => events.addEventListener("port", () => r(), { once: true }));
  Object.defineProperty(navigator, "requestMIDIAccess", { configurable: true, value: async () => { await first; return access; } });
})();
