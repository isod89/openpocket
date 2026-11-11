// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket web editor: what its sections share. The state of the session (the pedal, its catalogue, its current
// sound, its 52 slots), events between the sections, the messages, the questions, and the one long operation at a time.

export const $ = (id) => document.getElementById(id);
export const pad2 = (n) => String(n).padStart(2, "0");
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
export const slotName = (slot) => pad2(slot + 1);

// conn: idle | unsupported | denied | searching | notfound | loading | ready | lost
export const app = {
  access: null, ed: null, info: null, cat: null,
  conn: "idle",
  st: null,                 // { slot, dirty, saved, focus, preset }: the current sound
  slots: [],                // [{ slot, saved, name }]
  bank: [],                 // [{ saved, preset }] per slot, read in the background after the connection
  focus: 5,                 // the block the block editor shows
  ping: null,               // the last PING
  nam: null, irs: null,     // NAM_LIST, IR_LIST
  refs: { nam: null, ir: null },                  // where a preset names a model / IR slot (pxlib.js slotRef)
  held: { nam: new Map(), ir: new Map() },        // slot -> the library's entry for what the pedal holds there
  view: "sound",
};

// ---- events between the sections: conn, state ({ push }), slots, bank, ping, assets, view ----
const subs = new Map();
export function on(ev, fn) { if (!subs.has(ev)) subs.set(ev, []); subs.get(ev).push(fn); }
export function emit(ev, arg) { for (const fn of subs.get(ev) || []) fn(arg); }

// ---- elements ----
export function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === "class") el.className = v;
    else if (k === "text") el.textContent = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k.startsWith("aria-") || k.startsWith("data-") || k === "role" || k === "for" || k === "style" || k === "popovertarget") el.setAttribute(k, v === true ? "true" : v);
    else el[k] = v;
  }
  for (const c of kids.flat()) if (c !== null && c !== undefined && c !== false) el.append(c);
  return el;
}
// children as h() takes them (null, undefined and false are left out), onto an element that is already there
export function fill(el, ...kids) {
  el.textContent = "";
  for (const c of kids.flat()) if (c !== null && c !== undefined && c !== false) el.append(c);
  return el;
}
export function icon(name) {
  const s = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  s.setAttribute("class", "ic"); s.setAttribute("aria-hidden", "true");
  const u = document.createElementNS("http://www.w3.org/2000/svg", "use");
  u.setAttribute("href", `#i-${name}`);
  s.append(u);
  return s;
}
export const plural = (n, one, many = one + "s") => `${n} ${n === 1 ? one : many}`;

// ---- the catalogue ----
export const blk = (b) => app.cat.blocks[b];
export const typeOf = (preset, b) => blk(b).types[preset.type[b]] || blk(b).types[0];
export const blockIndex = (label) => app.cat.blocks.findIndex((x) => x.label === label);

// a setting that picks one of a few named things (a model, an interval, a pattern) is a choice, not a slider:
// its options are the runs of equal texts over 0..100 -> [{ label, lo, hi, value }] (value: the middle, as the pedal's knob)
const NUMERIC = /^[+-]?\d+(\.\d+)?( ?[A-Z%]+)?$/;
export function choices(param) {
  const t = param.texts;
  if (!t || t.some((x) => !x)) return null;
  const runs = [];
  for (let v = 0; v <= 100; v++) {
    const last = runs[runs.length - 1];
    if (last && last.label === t[v]) last.hi = v; else runs.push({ label: t[v], lo: v, hi: v });
  }
  if (runs.length > 40 || (runs.length > 8 && runs.every((r) => NUMERIC.test(r.label)))) return null;
  for (const r of runs) r.value = (r.lo + r.hi) >> 1;
  return runs;
}

// ---- messages ----
// toast(text, { kind: "info" | "ok" | "err", action: { label, run }, progress, sticky }) -> { update, close }
export function toast(text, opt = {}) {
  const box = $("toasts");
  const msg = h("span", { class: "toast-t", text });
  const bar = h("i");
  const el = h("div", { class: `toast ${opt.kind || "info"}` }, msg);
  let timer = null;
  const close = () => { clearTimeout(timer); el.remove(); };
  const arm = (o) => {
    clearTimeout(timer);
    if (!(o.sticky || o.kind === "err" || o.progress !== undefined)) timer = setTimeout(close, o.action ? 9000 : 6000);
  };
  const dress = (o) => {
    el.className = `toast ${o.kind || "info"}`;
    el.setAttribute("role", o.kind === "err" ? "alert" : "status");
    el.querySelectorAll(".toast-a, .toast-x, .toast-p").forEach((x) => x.remove());
    if (o.progress !== undefined) { bar.style.width = `${Math.round((o.progress || 0) * 100)}%`; el.append(h("span", { class: "toast-p" }, bar)); }
    if (o.action) el.append(h("button", { type: "button", class: "btn small toast-a", text: o.action.label, onclick: () => { close(); o.action.run(); } }));
    if (o.progress === undefined) el.append(h("button", { type: "button", class: "toast-x", "aria-label": "Dismiss", onclick: close }, icon("x")));
    arm(o);
  };
  dress(opt);
  el.addEventListener("pointerenter", () => clearTimeout(timer));
  el.addEventListener("pointerleave", () => arm(opt));
  box.append(el);
  while (box.children.length > 4) box.firstElementChild.remove();
  return { close, update(t, o = {}) { msg.textContent = t; opt = o; dress(o); } };
}
export const say = (text, kind = "info", action) => toast(text, { kind, action });

// ---- one long operation at a time (an import, an upload, a move): its progress, then its result ----
let running = null;
export const busy = () => running;
// fn(progress) returns the text of the success message, or { text, kind, action }; a throw is the failure
export async function task(label, fn) {
  if (running) { say(`Wait for "${running}" to finish.`, "err"); return undefined; }
  running = label;
  document.body.dataset.busy = "1";
  const t = toast(`${label}…`, { progress: 0 });
  try {
    const r = await fn((frac, text) => t.update(text || `${label}…`, { progress: frac }));
    const o = typeof r === "string" ? { text: r } : r || {};
    if (o.text) t.update(o.text, { kind: o.kind || "ok", action: o.action }); else t.close();
    return r === undefined ? true : r;
  } catch (e) {
    t.update(`${label} failed: ${e.message}.`, { kind: "err" });
    return undefined;
  } finally {
    running = null;
    delete document.body.dataset.busy;
  }
}
// a quick live edit: no lock; a pedal that does not answer is said once
export async function act(fn) {
  if (!app.ed) return undefined;
  try { return await fn(); }
  catch (e) { say(`The pedal did not answer (${e.message}).`, "err"); return undefined; }
}

// ---- questions (a dialog: the answer changes what is stored on the pedal) ----
function openDialog(build) {
  const d = $("dlg");
  return new Promise((resolve) => {
    let answer = null;
    d.textContent = "";
    d.append(...build((v) => { answer = v; d.close(); }));
    d.onclose = () => resolve(answer);
    d.onclick = (e) => { if (e.target === d) d.close(); };
    d.showModal();
  });
}
const dlgHead = (title) => h("h2", { id: "dlg-h", text: title });
const dlgFoot = (...btns) => h("div", { class: "dlg-foot" }, ...btns);

// -> true, false (cancelled), or the `other` button's value
export function confirmDialog({ title, body, confirm = "OK", danger = false, other = null }) {
  return openDialog((done) => [
    dlgHead(title),
    h("div", { class: "dlg-body" }, typeof body === "string" ? h("p", { text: body }) : body),
    dlgFoot(
      h("button", { type: "button", class: "btn", text: "Cancel", onclick: () => done(false) }),
      other && h("button", { type: "button", class: "btn", text: other.label, onclick: () => done(other.value) }),
      h("button", { type: "button", class: `btn ${danger ? "danger" : "primary"}`, id: "dlg-ok", text: confirm, autofocus: true, onclick: () => done(true) })),
  ]).then((v) => v ?? false);
}

// -> the text, or null
export function promptDialog({ title, label, value = "", max = 13, confirm = "Rename", hint = "" }) {
  return openDialog((done) => {
    const input = h("input", { id: "dlg-input", value, maxLength: max, spellcheck: false, autocomplete: "off", autofocus: true });
    const form = h("form", { method: "dialog", onsubmit: (e) => { e.preventDefault(); done(input.value); } },
      dlgHead(title),
      h("div", { class: "dlg-body" }, h("label", { class: "field" }, h("span", { text: label }), input), hint && h("p", { class: "dim", text: hint })),
      dlgFoot(h("button", { type: "button", class: "btn", text: "Cancel", onclick: () => done(null) }),
        h("button", { type: "submit", class: "btn primary", id: "dlg-ok", text: confirm })));
    setTimeout(() => input.select(), 0);
    return [form];
  });
}

// pick one of `items` ([{ value, n, name, tag, disabled, why }]) -> its value, or null.
// `consequence(item)` -> { text, confirm, danger }: said before the button is pressed, so nothing is overwritten blind
export function pickDialog({ title, intro, items, initial, consequence, searchable = true }) {
  return openDialog((done) => {
    let cur = items.find((x) => x.value === initial && !x.disabled) || null;
    const note = h("p", { class: "pick-note", "aria-live": "polite" });
    const ok = h("button", { type: "button", class: "btn primary", id: "dlg-ok", onclick: () => cur && done(cur.value) });
    const list = h("div", { class: "pick", role: "listbox", "aria-label": title, tabIndex: 0 });
    const search = searchable && h("input", { type: "search", class: "pick-q", placeholder: "Search", "aria-label": "Search", autocomplete: "off" });
    const shown = () => [...list.children].filter((r) => !r.hidden && r.getAttribute("aria-disabled") !== "true");
    const sync = () => {
      for (const r of list.children) r.setAttribute("aria-selected", cur && r.dataset.v === String(cur.value) ? "true" : "false");
      const c = cur ? consequence(cur) : { text: "Choose one.", confirm: "Choose" };
      note.textContent = c.text; ok.textContent = c.confirm; ok.disabled = !cur;
      ok.className = `btn ${c.danger ? "danger" : "primary"}`;
      if (cur) list.setAttribute("aria-activedescendant", `pick-${cur.value}`); else list.removeAttribute("aria-activedescendant");
    };
    for (const it of items) {
      const r = h("div", { class: "pick-row", role: "option", id: `pick-${it.value}`, "data-v": it.value, "aria-disabled": it.disabled ? "true" : null, title: it.disabled ? it.why : null },
        h("span", { class: "n", text: it.n }), h("span", { class: "nm", text: it.name }), it.tag && h("span", { class: "tag", text: it.tag }));
      if (!it.disabled) { r.onclick = () => { cur = it; sync(); }; r.ondblclick = () => done(it.value); }
      list.append(r);
    }
    list.onkeydown = (e) => {
      const rows = shown(), i = rows.findIndex((r) => cur && r.dataset.v === String(cur.value));
      const go = (j) => { const r = rows[Math.max(0, Math.min(rows.length - 1, j))]; if (r) { cur = items.find((x) => String(x.value) === r.dataset.v); sync(); r.scrollIntoView({ block: "nearest" }); } };
      if (e.key === "ArrowDown") { e.preventDefault(); go(i + 1); }
      else if (e.key === "ArrowUp") { e.preventDefault(); go(i < 0 ? 0 : i - 1); }
      else if (e.key === "Home") { e.preventDefault(); go(0); }
      else if (e.key === "End") { e.preventDefault(); go(rows.length - 1); }
      else if (e.key === "Enter" && cur) { e.preventDefault(); done(cur.value); }
    };
    if (search) search.oninput = () => {
      const q = search.value.trim().toLowerCase();
      for (const r of list.children) r.hidden = !!q && !r.textContent.toLowerCase().includes(q);
    };
    sync();
    setTimeout(() => { const s = list.querySelector('[aria-selected="true"]'); if (s) s.scrollIntoView({ block: "center" }); (search || list).focus(); }, 0);
    return [dlgHead(title), h("div", { class: "dlg-body" }, intro && h("p", { class: "dim", text: intro }), search, list, note),
      dlgFoot(h("button", { type: "button", class: "btn", text: "Cancel", onclick: () => done(null) }), ok)];
  });
}

// ---- a small menu on a button: items [{ label, run, danger, disabled, why }] or "-" ----
export function menu(anchor, items) {
  const m = $("menu");
  m.textContent = "";
  for (const it of items) {
    if (!it) continue;
    if (it === "-") { m.append(h("hr")); continue; }
    m.append(h("button", { type: "button", role: "menuitem", class: it.danger ? "danger" : "", disabled: !!it.disabled, title: it.disabled ? it.why : null,
      text: it.label, onclick: () => { m.hidePopover(); it.run(); } }));
  }
  const r = anchor.getBoundingClientRect();
  m.showPopover();
  const w = m.offsetWidth, hh = m.offsetHeight;
  m.style.left = `${Math.max(8, Math.min(innerWidth - w - 8, r.right - w))}px`;
  m.style.top = `${r.bottom + 6 + hh > innerHeight - 8 ? Math.max(8, r.top - hh - 6) : r.bottom + 6}px`;
  const first = m.querySelector("button:not(:disabled)");
  if (first) first.focus();
  m.onkeydown = (e) => {
    const b = [...m.querySelectorAll("button:not(:disabled)")], i = b.indexOf(document.activeElement);
    if (e.key === "ArrowDown") { e.preventDefault(); b[(i + 1) % b.length].focus(); }
    else if (e.key === "ArrowUp") { e.preventDefault(); b[(i - 1 + b.length) % b.length].focus(); }
  };
  m.addEventListener("toggle", (e) => { if (e.newState === "closed" && document.activeElement === document.body) anchor.focus(); }, { once: true });
}

// ---- files ----
export function download(name, data, type = "application/json") {
  const url = URL.createObjectURL(new Blob([data], { type }));
  const a = h("a", { href: url, download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 5000);
}
export const fileSafe = (s) => (s || "preset").trim().replace(/[^A-Za-z0-9 _-]+/g, "").replace(/\s+/g, "-").toLowerCase() || "preset";

// files dropped on `el` (accept: (file) => boolean) -> onFiles([...]); the element shows it is a target meanwhile
export function dropTarget(el, accept, onFiles) {
  let depth = 0;
  const has = (e) => [...(e.dataTransfer?.types || [])].includes("Files");
  el.addEventListener("dragenter", (e) => { if (!has(e)) return; e.preventDefault(); depth++; el.classList.add("drop"); });
  el.addEventListener("dragover", (e) => { if (has(e)) e.preventDefault(); });
  el.addEventListener("dragleave", () => { if (--depth <= 0) { depth = 0; el.classList.remove("drop"); } });
  el.addEventListener("drop", (e) => {
    if (!has(e)) return;
    e.preventDefault(); depth = 0; el.classList.remove("drop");
    const files = [...e.dataTransfer.files].filter(accept);
    if (files.length) onFiles(files); else say("These files are not of the kind this panel takes.", "err");
  });
}
