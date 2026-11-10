// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket web editor: the MIDI section. What is connected; the pedal's own Control Change numbers, named for the
// current preset; controls learned from another MIDI device (a foot controller on this computer), which this page
// passes on to the pedal as editor commands while it is open; and a monitor of what arrives. docs/MIDI.md, docs/pxmidi.js.

import { app, $, h, icon, on, emit, blk, typeOf, slotName, say, confirmDialog } from "./edcore.js";
import { sendParam, setOn, setType } from "./edsound.js";
import { selectPreset, stepPreset } from "./edpresets.js";
import { decode, ccParam, ccOn, ccToggle, ccType, ccTarget, resolve, action, learn, cleanMappings, sameTgt, NPAR } from "./pxmidi.js";

const KEY = "openpocket-midi-v1";
let cfg = { mappings: [], pc: true };
let learning = null;                  // the target waiting for a control: { kind, b, k, step, mode }
let learnt = "";                      // what the last learning ended with, said where the waiting was said
let scope = "all";                    // new controls work in every preset ("all") or only in the current one ("preset")
const log = [];                       // the monitor's lines, newest first
const heard = new Map();              // controller input id -> its name

function loadCfg() {
  try { const j = JSON.parse(localStorage.getItem(KEY) || "{}"); cfg = { mappings: cleanMappings(j.mappings), pc: j.pc !== false }; } catch (_) { }
}
function saveCfg() { try { localStorage.setItem(KEY, JSON.stringify(cfg)); } catch (_) { } }

// ---- names ----
function tgtName(t) {
  if (t.kind === "preset") return t.step > 0 ? "Next preset" : "Previous preset";
  const B = blk(t.b);
  if (t.kind === "on") return `${B.label} on / off`;
  if (t.kind === "type") return `${B.label} type`;
  const p = app.st ? typeOf(app.st.preset, t.b).params[t.k] : null;
  return `${B.label} · ${p ? p.name : `setting ${t.k + 1}`}`;
}
const srcName = (s) => `CC ${s.cc} · ch ${s.ch}`;
// what a message means to the pedal itself (its fixed map), in this preset's words
function pedalMeaning(m) {
  if (m.type === "pc") return m.program < app.slots.length ? `preset ${slotName(m.program)} ${app.slots[m.program].name}` : "no such preset";
  if (m.type !== "cc") return "";
  const t = ccTarget(m.cc);
  if (!t) return "not in the pedal's map";
  if (t.kind === "master") return "MASTER volume";
  if (t.kind === "toggle") return `${blk(t.b).label} flips (on value 0)`;
  return tgtName(t);
}

// ---- the other devices ----
const isPedal = (input) => app.ed && app.ed.link && input === app.ed.link.input;
function listen() {
  heard.clear();
  // only once the pedal is known: while it is being looked for, its own port must keep the handler the search gave it
  if (!app.access || app.conn !== "ready") return;
  for (const input of app.access.inputs.values()) {
    if (isPedal(input) || input.state === "disconnected") continue;
    heard.set(input.id || input.name, input.name || "MIDI device");
    input.onmidimessage = (ev) => fromController(input, ev.data);
    if (input.open) Promise.resolve(input.open()).catch(() => { });
  }
}
function note(from, m, meaning, kind = "") {
  const what = m.type === "cc" ? `CC ${m.cc} = ${m.value}` : m.type === "pc" ? `Program ${m.program}` : m.type;
  log.unshift({ t: new Date(), from, what, ch: m.ch, meaning, kind });
  if (log.length > 60) log.pop();
  if (app.view === "midi") renderMonitor();
}
function fromController(input, data) {
  const m = decode(data);
  if (!m || m.type === "clock" || app.conn !== "ready") return;
  const from = input.name || "Controller";
  if (m.type === "cc" && learning) { bind(m, from); return; }
  if (m.type === "pc") {
    if (cfg.pc && m.program < app.slots.length) { note(from, m, `→ preset ${slotName(m.program)}`, "hit"); selectPreset(m.program, { ask: false }); }
    else note(from, m, cfg.pc ? "no such preset" : "Program Change is switched off below");
    return;
  }
  if (m.type !== "cc") { note(from, m, "not used"); return; }
  const hits = resolve(cfg.mappings, m, app.st.slot);
  if (!hits.length) { note(from, m, "not learned: click a control below, then move this one"); return; }
  for (const map of hits) {
    const a = action(map, m.value, { on: (b) => app.st.preset.on[b], ntypes: (b) => blk(b).types.length });
    note(from, m, a ? `→ ${tgtName(map.tgt)}${a.do === "param" ? ` ${a.v}` : a.do === "on" ? (a.v ? ": on" : ": off") : ""}` : `${tgtName(map.tgt)}: the switch released, nothing to do`, a ? "hit" : "");
    if (!a) continue;
    if (a.do === "param") { sendParam(a.b, a.k, a.v); redrawSoon(); }
    else if (a.do === "on") setOn(a.b, a.v);
    else if (a.do === "type") { if (a.t !== app.st.preset.type[a.b]) setType(a.b, a.t); }
    else if (a.do === "preset") stepPreset(a.step, { ask: false });
  }
}
// a knob turned on the controller sends a stream: the sections redraw once per frame
let redraw = 0;
function redrawSoon() { if (!redraw) redraw = requestAnimationFrame(() => { redraw = 0; if (app.st) emit("state", { push: true }); }); }
// a musical message the pedal sent itself (SEND is on): a setting turned on the pedal names its own number
function fromPedal(data) {
  const m = decode(data);
  if (!m || m.type === "clock") return;
  note("Pedal", m, pedalMeaning(m), "pedal");
  if (m.type === "cc" && app.view === "midi") {
    const t = ccTarget(m.cc);
    const cell = t && t.b !== undefined && document.querySelector(`.cc-cell[data-t="${t.kind === "toggle" ? "on" : t.kind}:${t.b}:${t.k ?? 0}"]`);
    if (cell) { cell.classList.remove("flash"); void cell.offsetWidth; cell.classList.add("flash"); }
  }
}

// ---- learning ----
function startLearn(tgt) {
  learning = learning && sameTgt(learning, tgt) ? null : tgt;
  learnt = "";
  renderMap();
}
function bind(m, from) {
  // a step to the next preset has to work in every preset
  const tgt = learning, slot = scope === "preset" && tgt.kind !== "preset" ? app.st.slot : null;
  learning = null;
  cfg.mappings = learn(cfg.mappings, m, tgt, slot, `m${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`);
  saveCfg();
  note(from, m, `learned: ${tgtName(tgt)}`, "hit");
  learnt = `${tgtName(tgt)} now follows ${srcName(m)} of ${from}, ${slot === null ? "in every preset" : `only in preset ${slotName(slot)}`}.`;
  renderMap();
}
function forget(id) { cfg.mappings = cfg.mappings.filter((m) => m.id !== id); saveCfg(); renderMap(); }

// ---- v42: USB audio (pull request #25; editor protocol 4, docs/USB-AUDIO.md) ----
// The setting is on the pedal (MIDI page, USB AUDIO) and here. A change takes the pedal off USB for a second: it comes
// back as the other USB device, and the editor connects again by itself (editor.js, "reconnect").
let ua = null, uaBusy = false;
async function loadUa() {
  ua = null;
  if (app.conn !== "ready" || !app.ed || !app.ed.hasUsbAudio) return;
  try { const r = await app.ed.usbAudio(); ua = r.rc === 1 ? null : r; } catch (_) { ua = null; }   // (rc 1: no USB audio in this firmware)
  if (app.view === "midi") renderConn();
}
const uaText = (u) => !u.on ? "Off" : u.recording && u.playing ? "On: the computer records and plays"
  : u.recording ? "On: the computer records" : u.playing ? "On: the computer plays" : "On";
async function uaToggle() {
  if (!ua || uaBusy || !app.ed) return;
  const turnOn = !ua.on;
  if (!await confirmDialog({ title: `Turn USB audio ${turnOn ? "on" : "off"}?`,
    body: "The sound stops for a moment while the pedal saves the setting, then the pedal leaves USB for a second and the editor connects again.",
    confirm: turnOn ? "Turn on" : "Turn off" })) return;
  uaBusy = true;
  try {
    const r = await app.ed.usbAudio(turnOn);
    if (r.rc) { say("The pedal could not save the setting.", "err"); return; }
    say(`USB audio is ${turnOn ? "on" : "off"}. The pedal leaves USB for a second; the editor connects again by itself.`, "ok");
    emit("reconnect");
  } catch (e) { say(`Not changed: ${e.message}`, "err"); }
  finally { uaBusy = false; }
}

// ---- the connection ----
function renderConn() {
  const el = $("midiconn");
  el.textContent = "";
  if (app.conn !== "ready") return;
  const info = app.info, others = [...heard.values()];
  el.append(
    h("div", { class: "card-head" }, h("h2", { id: "midiconn-h", text: "Connection" })),
    h("dl", { class: "facts" },
      h("dt", { text: "Pedal" }), h("dd", { text: `${info.identity.replace(/_9?\d+$/, "")} · OpenPocket ${info.version}` }),
      h("dt", { text: "USB MIDI port" }), h("dd", { text: info.port }),
      h("dt", { text: "Editor protocol" }), h("dd", { text: `version ${info.proto}` }),
      h("dt", { text: "Controllers" }), h("dd", { id: "midictl" }, others.length ? others.map((n) => h("span", { class: "chip ok", text: n })) : h("span", { class: "dim", text: "None. Plug a MIDI controller into this computer and it shows up here." })),
      ua && h("dt", { text: "USB audio" }),
      ua && h("dd", { id: "uarow" }, h("span", { id: "uastate", text: uaText(ua) }), " ",
        h("button", { type: "button", class: "btn small", id: "uabtn", text: ua.on ? "Turn off" : "Turn on", onclick: uaToggle }))),
    ua && h("p", { class: "dim", id: "uanote", text: "With USB audio on, the pedal is also a USB sound card, with no driver to install (stereo, 16 bit, 44.1 kHz). The computer records the pedal's sound, and what the computer plays comes out of the pedal's output." }),
    h("p", { class: "dim" }, "The pedal's own MIDI settings are on the pedal: ", h("b", { text: "hold FX for 0.8 s" }),
      " for CHANNEL (OMNI or 1 to 16), SEND and CLOCK > DELAY. The editor cannot read or change them. ",
      h("a", { href: "https://github.com/isod89/openpocket/blob/main/docs/MIDI.md", target: "_blank", rel: "noopener" }, "The MIDI reference ", icon("ext"))));
}

// ---- the map ----
const learnedFor = (tgt) => cfg.mappings.filter((m) => sameTgt(m.tgt, tgt) && (m.slot === null || m.slot === app.st.slot));
function cell(tgt, title, cc, extra) {
  const key = `${tgt.kind}:${tgt.b}:${tgt.k ?? 0}`, on_ = learning && sameTgt(learning, tgt), got = learnedFor(tgt);
  return h("button", { type: "button", class: `cc-cell${on_ ? " learning" : ""}${got.length ? " has" : ""}`, "data-t": key, "aria-pressed": on_ ? "true" : "false",
    title: on_ ? "Waiting for a control. Click again to stop." : `Click, then move a control on your controller: it will drive ${tgtName(tgt)}.`, onclick: () => startLearn(tgt) },
    h("span", { class: "cc-n", text: title }),
    h("span", { class: "cc-c", text: on_ ? "Move a control…" : `CC ${cc}${extra ? ` · ${extra}` : ""}` }),
    got.map((m) => h("span", { class: "cc-l", text: `← ${srcName(m.src)}` })));
}
function renderMap() {
  const el = $("midimap");
  el.textContent = "";
  if (app.conn !== "ready" || !app.st) return;
  const p = app.st.preset, cur = `${slotName(app.st.slot)} ${p.name}`;
  const sc = h("select", { id: "midiscope", "aria-label": "Where a learned control works", onchange: () => { scope = sc.value; } },
    h("option", { value: "all", text: "in every preset" }), h("option", { value: "preset", text: `only in ${cur}` }));
  sc.value = scope;
  const pc = h("input", { type: "checkbox", id: "midipc", checked: cfg.pc, onchange: () => { cfg.pc = pc.checked; saveCfg(); } });
  const stepBtn = (step) => {
    const tgt = { kind: "preset", step }, on_ = learning && sameTgt(learning, tgt), got = learnedFor(tgt);
    return h("button", { type: "button", class: `btn small${on_ ? " learning" : ""}`, "data-t": `preset:${step}`, "aria-pressed": on_ ? "true" : "false", onclick: () => startLearn(tgt) },
      on_ ? "Press a switch…" : `${step > 0 ? "Next" : "Previous"} preset${got.length ? `: ${got.map((m) => srcName(m.src)).join(", ")}` : ""}`);
  };
  const rows = app.cat.blocks.map((B, b) => {
    const T = typeOf(p, b);
    return h("tr", { style: `--c:${B.color}` },
      h("th", { scope: "row" }, h("i", { class: `led${p.on[b] ? " on" : ""}` }), h("span", { text: B.label }), h("small", { text: T.name })),
      h("td", {}, cell({ kind: "on", b, mode: "toggle" }, "On / off", ccOn(b), `flip ${ccToggle(b)}`)),
      h("td", {}, cell({ kind: "type", b }, "Type", ccType(b))),
      Array.from({ length: NPAR }, (_, k) => h("td", {}, T.params[k] ? cell({ kind: "param", b, k }, T.params[k].name, ccParam(b, k))
        : h("span", { class: "cc-cell none", title: `${T.name} has no setting ${k + 1}: the pedal stores CC ${ccParam(b, k)} and ignores it.` }, h("span", { class: "cc-n", text: "–" }), h("span", { class: "cc-c", text: `CC ${ccParam(b, k)}` })))));
  });
  const learned = cfg.mappings;
  el.append(
    h("div", { class: "card-head" }, h("h2", { id: "midimap-h", text: "Controls" })),
    h("div", { class: "ways" },
      h("p", {}, h("b", { text: "Straight to the pedal. " }), "A DAW or a controller that reaches the pedal's USB port uses the CC numbers below: they are fixed in the firmware. The names are this preset's: a setting's meaning follows the block's type."),
      h("p", {}, h("b", { text: "Through this page. " }), "For a controller plugged into this computer: click a control below, then move a knob or press a switch on the controller. The page passes it on while it is open.")),
    h("div", { class: "learnbar" },
      h("label", {}, "A control I teach works ", sc),
      h("span", { class: "grow" }),
      stepBtn(-1), stepBtn(1),
      h("label", { class: "check" }, pc, " Program Change loads presets")),
    h("p", { class: `learnstate${learning ? " on" : ""}`, id: "learnstate", role: "status", text: learning
      ? `Waiting for a control for ${tgtName(learning)}: move a knob or press a switch on your controller${heard.size ? "" : " (none is connected yet)"}. Esc stops.` : learnt }),
    h("div", { class: "cc-wrap" }, h("table", { class: "cc" },
      h("thead", {}, h("tr", {}, h("th", { scope: "col", text: "Block" }), h("th", { scope: "col", text: "Switch" }), h("th", { scope: "col", text: "Type" }), h("th", { scope: "col", colSpan: NPAR, text: `Settings in ${cur}` }))),
      h("tbody", {}, rows))),
    h("p", { class: "dim small", text: "CC 7 is the MASTER volume. Values: a setting is 0 to 127 for 0 to 100; a switch is on from 64; a flip number acts on value 0, so a footswitch that sends 127 then 0 flips once." }),
    h("h3", { text: "Learned controls" }),
    learned.length ? h("div", { class: "learned", id: "learned" }, learned.map((m) => {
      const where = h("select", { "aria-label": `Where ${srcName(m.src)} works`, onchange: () => { m.slot = where.value === "all" ? null : +where.value; saveCfg(); renderMap(); } },
        h("option", { value: "all", text: "Every preset" }),
        m.slot !== null && m.slot !== app.st.slot && h("option", { value: m.slot, text: `Only ${slotName(m.slot)} ${app.slots[m.slot] ? app.slots[m.slot].name : ""}` }),
        h("option", { value: app.st.slot, text: `Only ${cur}` }));
      where.value = m.slot === null ? "all" : String(m.slot);
      const mode = m.tgt.kind === "on" && h("select", { "aria-label": "How the switch acts", onchange: () => { m.tgt.mode = mode.value; saveCfg(); } },
        h("option", { value: "toggle", text: "Each press flips it" }), h("option", { value: "follow", text: "On while the value is 64 or more" }));
      if (mode) mode.value = m.tgt.mode;
      return h("div", { class: "lrow" }, h("span", { class: "chip", text: srcName(m.src) }), h("span", { class: "arrow", "aria-hidden": "true", text: "→" }), h("b", { text: tgtName(m.tgt) }), h("span", { class: "grow" }), mode, where,
        h("button", { type: "button", class: "btn icon small", "aria-label": `Forget ${srcName(m.src)} for ${tgtName(m.tgt)}`, onclick: () => forget(m.id) }, icon("x")));
    })) : h("p", { class: "dim empty-note", text: "None yet. A learned control is kept in this browser and works while this page is open and connected." }));
}

function renderMonitor() {
  const el = $("midimon");
  el.textContent = "";
  if (app.conn !== "ready") return;
  el.append(
    h("div", { class: "card-head" }, h("h2", { id: "midimon-h", text: "Monitor" }), h("span", { class: "grow" }),
      log.length ? h("button", { type: "button", class: "btn small", text: "Clear", onclick: () => { log.length = 0; renderMonitor(); } }) : null),
    log.length ? h("ol", { class: "mon", id: "monlist", reversed: true }, log.slice(0, 40).map((l) =>
      h("li", { class: l.kind }, h("span", { class: "mon-w", text: `${l.what}${l.ch ? ` · ch ${l.ch}` : ""}` }), h("span", { class: "mon-m", text: l.meaning }),
        h("span", { class: "mon-s" }, h("time", { text: l.t.toLocaleTimeString([], { hour12: false }) }), ` · ${l.from}`))))
      : h("p", { class: "dim empty-note", text: "Nothing received yet. Messages from a controller show here, and the pedal's own when a setting is changed on it (with SEND on)." }));
}
const renderAll = () => { renderConn(); renderMap(); renderMonitor(); };

export function initMidi() {
  loadCfg();
  on("conn", () => { learning = null; if (app.conn === "ready") { listen(); app.ed.onMidi = fromPedal; loadUa(); } else ua = null; renderAll(); });
  on("ports", () => { listen(); if (app.view === "midi") { renderConn(); renderMap(); } });
  on("state", () => { if (app.view === "midi" && app.st) renderMap(); });
  on("view", () => { if (app.view === "midi") renderAll(); else { learning = null; learnt = ""; } });
  addEventListener("keydown", (e) => { if (e.key === "Escape" && learning) { learning = null; renderMap(); } });
}
