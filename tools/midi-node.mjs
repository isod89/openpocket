// SPDX-License-Identifier: GPL-3.0-only
// The Node side of the flasher's MIDI transport: a Web MIDI shaped access ({ inputs, outputs }, the shape docs/pxota.js's
// Updater takes) over @julusian/midi (RtMidi: CoreMIDI, ALSA, WinMM). Ports are listed afresh on every values() call,
// because the pedal re-enumerates under another name when it enters its update loader.

export function nodeMidiAccess(midi) {
  const probes = { in: new midi.Input(), out: new midi.Output() };
  const live = { in: new Map(), out: new Map() };            // key -> port, for the ports open now

  const names = (probe) => Array.from({ length: probe.getPortCount() }, (_, i) => probe.getPortName(i));

  // key = name, or name#n for the n-th port that shares it
  function keyed(list) {
    const seen = new Map();
    return list.map((name) => { const n = seen.get(name) || 0; seen.set(name, n + 1); return { name, key: n ? `${name}#${n}` : name }; });
  }

  function makePort(kind, name, key) {
    const present = () => keyed(names(probes[kind])).some((p) => p.key === key);
    const index = () => keyed(names(probes[kind])).findIndex((p) => p.key === key);
    let dev = null;
    const port = {
      name,
      get state() { return present() ? "connected" : "disconnected"; },
      async open() {
        if (dev) return;
        const i = index();
        if (i < 0) throw new Error(`MIDI port ${name} is gone`);
        if (kind === "in") {
          dev = new midi.Input();
          dev.ignoreTypes(false, true, true);                // SysEx on, clock and active sensing off
          dev.on("message", (_dt, msg) => { if (port.onmidimessage) port.onmidimessage({ data: Uint8Array.from(msg) }); });
        } else {
          dev = new midi.Output();
        }
        dev.openPort(i);
      },
      send: (bytes) => dev.sendMessage(Array.from(bytes)),
      dispose() { if (dev) { try { dev.closePort(); } catch (_) { /* already gone */ } dev = null; } },
    };
    if (kind === "in") port.onmidimessage = null;
    return port;
  }

  function refresh(kind) {
    const now = keyed(names(probes[kind]));
    const map = live[kind];
    for (const [key, port] of map) if (!now.some((p) => p.key === key)) { port.dispose(); map.delete(key); }
    for (const p of now) if (!map.has(p.key)) map.set(p.key, makePort(kind, p.name, p.key));
    return map;
  }

  return {
    inputs: { values: () => refresh("in").values() },
    outputs: { values: () => refresh("out").values() },
    close() { for (const kind of ["in", "out"]) for (const p of live[kind].values()) p.dispose(); },
  };
}
