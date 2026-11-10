// SPDX-License-Identifier: GPL-3.0-only
// OpenPocket web editor: the 52 presets. Reading them, loading one, and everything that writes a slot (save, save as,
// copy, move, rename, back to factory, import), each with what it replaces said beforehand and an Undo afterwards.

import { app, $, h, icon, on, emit, slotName, say, task, act, busy, confirmDialog, promptDialog, pickDialog, menu, download,
  fileSafe, blk, typeOf, blockIndex, plural, dropTarget } from "./edcore.js";
import { presetToJSON, presetFromJSON, filePresets, parsePresetFile, clonePreset, cleanName } from "./pxedit.js";
import { knobIndex } from "./pxlib.js";

// ---- reading the pedal ----
export async function refreshState() { app.st = await app.ed.state(); emit("state", {}); }
export async function refreshSlots() { app.slots = await app.ed.list(); emit("slots"); }
// a slot's factory preset, known once the slot was seen without a saved one: a preset written back to its own factory
// slot (a move there and back) is written as "factory", not as a copy of yours
const factory = [];
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
function keep(s, g) {
  app.bank[s] = { saved: g.saved, preset: g.preset };
  if (!g.saved) factory[s] = g.preset;
}
export async function refreshBank(slots) {
  for (const s of slots) keep(s, await app.ed.slotGet(s));
  emit("bank");
}
// every slot's preset, a few at a time behind the user's own requests (the list's summaries, the models' "used by")
export async function loadBank() {
  const ed = app.ed;
  app.bank = [];
  for (let s = 0; s < app.slots.length; s++) {
    if (app.ed !== ed) return;
    try { keep(s, await ed.slotGet(s)); } catch (_) { return; }
    if (s % 13 === 12) emit("bank");
  }
  emit("bank");
}
export async function entry(slot) {
  if (!app.bank[slot]) await refreshBank([slot]);
  return app.bank[slot];
}
const label = (slot) => `${slotName(slot)} ${app.slots[slot] ? app.slots[slot].name : ""}`.trim();

// ---- compare: hear the slot's saved preset, then come back to the edited sound ----
let edited = null;                              // the edited sound while the saved one plays
export const comparing = () => !!edited;
export async function setCompare(want) {
  if (!app.ed || want === comparing()) return;
  await act(async () => {
    if (want) {
      const saved = (await entry(app.st.slot)).preset;
      edited = clonePreset(app.st.preset);
      if (await app.ed.put(saved)) { edited = null; throw new Error("refused"); }
    } else {
      const q = edited; edited = null;
      await app.ed.put(q);
    }
    await refreshState();
  });
  emit("compare");
}
// on the way out of the page the edited sound goes back to the pedal, without waiting for the answer
export function compareBeforeUnload() { if (edited && app.ed) { try { app.ed.put(edited).catch(() => { }); } catch (_) { } } }
on("conn", () => { if (app.conn !== "ready") edited = null; });
// a preset chosen on the pedal while comparing: the edited sound is gone with it, as on the pedal
export function compareSlotChanged() { if (edited) { edited = null; emit("compare"); } }

// ---- loading a preset ----
export async function selectPreset(slot, opt = {}) {
  if (!app.ed || !app.st || slot === app.st.slot || slot < 0 || slot >= app.slots.length) return;
  if (busy()) { say(`Wait for "${busy()}" to finish.`, "err"); return; }
  await setCompare(false);
  if (app.st.dirty && opt.ask !== false) {
    const r = await confirmDialog({
      title: `Save the changes to ${app.st.preset.name}?`,
      body: `${label(app.st.slot)} has changes that are not saved. Loading ${label(slot)} drops them.`,
      confirm: "Save, then load", other: { label: "Drop the changes", value: "drop" },
    });
    if (r === false) return;
    if (r === true && !(await save())) return;
  }
  await act(async () => { await app.ed.select(slot); await refreshState(); });
}
export const stepPreset = (d, opt) => app.st && selectPreset((app.st.slot + d + app.slots.length) % app.slots.length, opt);

// ---- writing slots ----
// changes: [{ slot, preset | null (its factory preset) }] in one flash write. sound: "follow" (the current sound reloads
// from its slot unless it has unsaved changes), "keep", "reload". -> the changes that put everything back
export async function writeSlots(changes, sound = "follow") {
  const before = [];
  for (const c of changes) before.push({ slot: c.slot, ...(await entry(c.slot)) });
  changes = changes.map((c) => (c.preset && factory[c.slot] && same(c.preset, factory[c.slot]) ? { slot: c.slot, preset: null } : c));
  const dirty = app.st.dirty;
  let n = 0;
  try {
    for (const c of changes) { if (await app.ed.slotPut(c.slot, c.preset, false)) throw new Error(`the pedal refused preset ${slotName(c.slot)}`); n++; }
    if (await app.ed.commit()) throw new Error("the pedal could not write its memory");
  } catch (e) {
    for (const b of before.slice(0, n)) await app.ed.slotPut(b.slot, b.saved ? b.preset : null, false).catch(() => { });
    throw e;
  }
  await refreshSlots();
  await refreshBank(changes.map((c) => c.slot));
  if (changes.some((c) => c.slot === app.st.slot) && (sound === "reload" || (sound === "follow" && !dirty))) await app.ed.select(app.st.slot);
  await refreshState();
  return before.map((b) => ({ slot: b.slot, preset: b.saved ? b.preset : null }));
}
const undo = (what, restore, sound) => ({ label: "Undo", run: () => task(`Undoing ${what}`, async () => { await writeSlots(restore, sound); return `Undone: ${what}.`; }) });

export function save(slot = app.st.slot) {
  return task("Saving", async () => {
    await setCompare(false);
    const prev = await entry(slot);
    if (await app.ed.save(slot)) throw new Error("the pedal could not write its memory");
    await refreshState(); await refreshSlots(); await refreshBank([slot]);
    return { text: `Saved as ${label(slot)}.`, action: undo(`the save to ${slotName(slot)}`, [{ slot, preset: prev.saved ? prev.preset : null }], "keep") };
  });
}
export function revert() {
  return act(async () => {
    await setCompare(false);
    await app.ed.select(app.st.slot); await refreshState();
    say(`Back to the saved ${label(app.st.slot)}.`, "ok");
  });
}

// the 52 slots as a list to pick from; `what(slot)` says what happens to the one picked
const slotItems = (skip) => app.slots.map((s) => ({ value: s.slot, n: slotName(s.slot), name: s.name, tag: s.saved ? "Yours" : "Factory", disabled: s.slot === skip, why: "This is the preset itself." }));
function replaces(slot, verb) {
  const s = app.slots[slot];
  if (s.saved) return { text: `${slotName(slot)} holds your preset ${s.name}. It is replaced; Undo brings it back.`, confirm: `Replace ${slotName(slot)} ${s.name}`, danger: true };
  return { text: `${slotName(slot)} holds the factory preset ${s.name}. "Back to factory" brings that one back at any time.`, confirm: `${verb} ${slotName(slot)}` };
}
export async function saveAs() {
  const cur = app.st.slot;
  const to = await pickDialog({
    title: `Save ${app.st.preset.name} as…`, intro: "Choose the preset number it is saved under.", items: slotItems(-1), initial: cur,
    consequence: (it) => it.value === cur ? { text: "Its own number: the same as Save.", confirm: `Save as ${slotName(cur)}` } : replaces(it.value, "Save as"),
  });
  if (to !== null) await save(to);
}
export async function duplicate(from) {
  const src = await entry(from);
  const to = await pickDialog({ title: `Copy ${label(from)} to…`, intro: "The copy takes the place of the preset you choose.", items: slotItems(from), initial: null, consequence: (it) => replaces(it.value, "Copy to") });
  if (to === null) return;
  await task("Copying", async () => {
    const restore = await writeSlots([{ slot: to, preset: clonePreset(src.preset) }]);
    return { text: `${src.preset.name} is also preset ${slotName(to)} now.`, action: undo(`the copy to ${slotName(to)}`, restore) };
  });
}
// the preset of `from` takes place `to`; the ones between move one place, as in any list
export function movePreset(from, to) {
  if (from === to || to < 0 || to >= app.slots.length) return undefined;
  return task("Moving", async () => {
    await setCompare(false);
    const lo = Math.min(from, to), hi = Math.max(from, to), step = from < to ? 1 : -1;
    const e = [];
    for (let s = lo; s <= hi; s++) e[s] = await entry(s);
    const changes = [];
    for (let s = from; s !== to; s += step) changes.push({ slot: s, preset: e[s + step].preset });
    changes.push({ slot: to, preset: e[from].preset });
    const cur = app.st.slot, dirty = app.st.dirty, sound = clonePreset(app.st.preset), name = e[from].preset.name;
    const restore = await writeSlots(changes, "keep");
    const now = cur === from ? to : cur >= lo && cur <= hi ? cur - step : cur;     // the current sound travels with its preset
    if (now !== cur || !dirty) { await app.ed.select(now); if (dirty) await app.ed.put(sound); }
    await refreshState();
    return { text: `${name} is preset ${slotName(to)} now.`, action: undo(`the move of ${name}`, restore) };
  });
}
export async function moveTo(from) {
  const to = await pickDialog({
    title: `Move ${label(from)} to…`, intro: "It takes that place; the presets between move by one. Nothing is replaced.", items: slotItems(from), initial: null,
    consequence: (it) => ({ text: `${app.slots[from].name} becomes preset ${it.n}. ${plural(Math.abs(it.value - from), "preset")} between ${Math.abs(it.value - from) === 1 ? "moves" : "move"} one place ${it.value > from ? "up" : "down"}.`, confirm: `Move to ${it.n}` }),
  });
  if (to !== null) await movePreset(from, to);
}
export async function rename(slot) {
  if (slot === app.st.slot) { emit("view-go", "sound"); const n = $("name"); n.focus(); n.select(); return; }
  const src = await entry(slot);
  const name = await promptDialog({ title: `Rename ${label(slot)}`, label: "Name (13 characters at most)", value: src.preset.name });
  if (name === null || cleanName(name) === src.preset.name || !cleanName(name)) return;
  await task("Renaming", async () => {
    const restore = await writeSlots([{ slot, preset: { ...clonePreset(src.preset), name: cleanName(name) } }]);
    return { text: `Preset ${slotName(slot)} is ${cleanName(name)} now.`, action: undo("the rename", restore) };
  });
}
export async function restoreFactory(slot) {
  const s = app.slots[slot];
  if (!(await confirmDialog({ title: `Put ${slotName(slot)} back to its factory preset?`, body: `Your preset ${s.name} leaves preset ${slotName(slot)}. Undo brings it back.`, confirm: "Back to factory", danger: true }))) return;
  await task("Restoring", async () => {
    await setCompare(false);
    const restore = await writeSlots([{ slot, preset: null }], "reload");
    return { text: `Preset ${slotName(slot)} is the factory ${app.slots[slot].name} again.`, action: undo("the factory restore", restore) };
  });
}

// ---- files ----
export async function exportOne(slot) {
  const cur = slot === app.st.slot, p = cur ? app.st.preset : (await entry(slot)).preset;
  download(`${fileSafe(p.name)}.json`, JSON.stringify(filePresets([presetToJSON(p, app.cat, slot)]), null, 1));
  say(`${p.name} exported${cur && app.st.dirty ? ", with its unsaved changes" : ""}.`, "ok");
}
export function exportAll() {
  return task("Backing up the presets", async (progress) => {
    const all = [];
    for (let s = 0; s < app.slots.length; s++) { progress(s / app.slots.length); const e = await entry(s); all.push({ ...presetToJSON(e.preset, app.cat, s), factory: !e.saved }); }
    const d = new Date();
    download(`openpocket-presets-${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}.json`, JSON.stringify(filePresets(all), null, 1));
    return `All ${app.slots.length} presets are in one file. Import it to bring them back.`;
  });
}
export async function importFile(f) {
  let list;
  try { list = parsePresetFile(await f.text()); } catch (_) { say(`${f.name} is not an OpenPocket preset file.`, "err"); return; }
  if (!list.length) { say(`${f.name} holds no preset.`, "err"); return; }
  if (list.length === 1) return importOne(list[0], f.name);
  const ok = list.filter((o) => Number.isInteger(o.slot) && o.slot >= 1 && o.slot <= app.slots.length);
  const mine = ok.filter((o) => !o.factory), lost = mine.filter((o) => app.slots[o.slot - 1].saved);
  if (!ok.length) { say(`${f.name}: none of its ${list.length} presets says which number it belongs to.`, "err"); return; }
  const rows = h("div", { class: "plan" }, mine.slice(0, 60).map((o) => {
    const s = app.slots[o.slot - 1];
    return h("div", { class: "plan-row" }, h("span", { class: "n", text: slotName(o.slot - 1) }), h("span", { class: "nm", text: cleanName(o.name) }),
      h("span", { class: s.saved ? "tag warn" : "tag", text: s.saved ? `replaces your ${s.name}` : `over factory ${s.name}` }));
  }));
  const body = h("div", {},
    h("p", { text: `${f.name} holds ${plural(list.length, "preset")}: ${plural(mine.length, "preset")} of yours` +
      `${ok.length - mine.length ? ` and ${plural(ok.length - mine.length, "factory preset")} (those numbers go back to factory)` : ""}` +
      `${list.length - ok.length ? `; ${list.length - ok.length} without a number are left out` : ""}.` }),
    lost.length ? h("p", { class: "warn", text: `${plural(lost.length, "preset")} of yours on the pedal ${lost.length === 1 ? "is" : "are"} replaced. Undo brings ${lost.length === 1 ? "it" : "them"} back.` }) : null,
    rows);
  if (!(await confirmDialog({ title: "Import these presets?", body, confirm: `Import ${plural(ok.length, "preset")}`, danger: lost.length > 0 }))) return;
  await task("Importing", async (progress) => {
    const changes = [], warn = [];
    for (const [i, o] of ok.entries()) {
      progress(i / ok.length * 0.5);
      const s = o.slot - 1;
      if (o.factory) { changes.push({ slot: s, preset: null }); continue; }
      const r = presetFromJSON(o, app.cat, (await entry(s)).preset);
      warn.push(...r.warn.map((w) => `${slotName(s)}: ${w}`));
      changes.push({ slot: s, preset: r.preset });
    }
    const restore = await writeSlots(changes);
    return { text: `${plural(ok.length, "preset")} imported.${warn.length ? ` Notes: ${warn.slice(0, 4).join("; ")}${warn.length > 4 ? `; and ${warn.length - 4} more` : ""}.` : ""}`,
      kind: warn.length ? "info" : "ok", action: undo("the import", restore) };
  });
}
// one preset: it becomes the current sound, to listen to before it is saved anywhere
async function importOne(o, fileName) {
  if (app.st.dirty && !(await confirmDialog({ title: "Replace the current sound?", body: `${app.st.preset.name} has changes that are not saved. The preset from ${fileName} takes its place.`, confirm: "Replace the sound", danger: true }))) return;
  await act(async () => {
    await setCompare(false);
    const r = presetFromJSON(o, app.cat, app.st.preset);
    if (await app.ed.put(r.preset)) { say("The pedal refused this preset.", "err"); return; }
    await refreshState();
    say(`${r.preset.name} is playing now. Save it to keep it.${r.warn.length ? ` Notes: ${r.warn.join("; ")}.` : ""}`, r.warn.length ? "info" : "ok");
    emit("view-go", "sound");
  });
}

// ---- the list ----
let filter = "all", query = "", active = -1;     // active: the slot the keyboard is on
// what a preset is made of, in a few words: its amp, then the effects that are on
function summary(e) {
  const p = e.preset, A = blockIndex("AMP"), out = [];
  if (A >= 0 && p.on[A]) { const T = typeOf(p, A), m = app.nam && app.nam.models; out.push(T.name === "NAM" && m ? m[knobIndex(p.p[A][0], m.length)].name : T.name); }
  const fx = p.order.filter((b) => p.on[b] && !["AMP", "CAB", "GT"].includes(blk(b).label)).map((b) => blk(b).label);
  if (fx.length) out.push(fx.join(" "));
  return out.join(" · ");
}
const haystack = (s) => {
  const e = app.bank[s.slot];
  return `${slotName(s.slot)} ${s.name} ${e ? `${summary(e)} ${e.preset.order.filter((b) => e.preset.on[b]).map((b) => `${blk(b).name} ${typeOf(e.preset, b).name}`).join(" ")}` : ""}`.toLowerCase();
};
const visible = () => app.slots.filter((s) => (filter === "all" || (filter === "user") === s.saved) && (!query || haystack(s).includes(query)));

function rowMenu(slot, anchor) {
  const s = app.slots[slot];
  menu(anchor, [
    slot !== app.st.slot && { label: "Load", run: () => selectPreset(slot) },
    { label: "Rename…", run: () => rename(slot) },
    { label: "Copy to…", run: () => duplicate(slot) },
    { label: "Move to…", run: () => moveTo(slot) },
    { label: "Move up", run: () => movePreset(slot, slot - 1), disabled: slot === 0 },
    { label: "Move down", run: () => movePreset(slot, slot + 1), disabled: slot === app.slots.length - 1 },
    { label: "Export", run: () => exportOne(slot) },
    "-",
    { label: "Back to factory", run: () => restoreFactory(slot), danger: true, disabled: !s.saved, why: "This is already the factory preset." },
  ]);
}

export function renderList() {
  const box = $("list"), rows = visible();
  if (!app.st) return;
  box.textContent = "";
  for (const s of rows) {
    const cur = s.slot === app.st.slot, e = app.bank[s.slot];
    const row = h("div", { class: `row${s.saved ? " user" : ""}${cur ? " cur" : ""}${s.slot === active ? " act" : ""}`, role: "option", id: `ps${s.slot}`, "data-slot": s.slot, "aria-selected": cur ? "true" : "false" },
      h("span", { class: "n", text: slotName(s.slot) }),
      h("span", { class: "nm", text: cur ? app.st.preset.name : s.name }),
      h("span", { class: "sum", text: cur && app.st.dirty ? "Edited, not saved" : e ? summary(cur ? { preset: app.st.preset } : e) : "" }),
      s.saved && h("span", { class: "tag", text: "Yours" }),
      h("button", { type: "button", class: "more", tabIndex: -1, "aria-label": `Actions for ${slotName(s.slot)} ${s.name}`, "aria-haspopup": "menu",
        onclick: (ev) => { ev.stopPropagation(); rowMenu(s.slot, ev.currentTarget); } }, icon("more")));
    row.onclick = () => { active = s.slot; selectPreset(s.slot); };
    box.append(row);
  }
  const empty = $("listempty");
  empty.hidden = rows.length > 0;
  if (!rows.length) empty.textContent = query ? `No preset matches "${$("psearch").value.trim()}".` : filter === "user" ? "No preset of yours yet. Change a sound and press Save: it shows up here." : "Every preset is one of yours.";
  if (active >= 0 && rows.some((s) => s.slot === active)) box.setAttribute("aria-activedescendant", `ps${active}`); else box.removeAttribute("aria-activedescendant");
  const mine = app.slots.filter((s) => s.saved).length;
  for (const b of $("pfilter").children) {
    b.setAttribute("aria-pressed", b.dataset.f === filter ? "true" : "false");
    if (b.dataset.f === "user") b.textContent = `Yours ${mine}`;
    if (b.dataset.f === "factory") b.textContent = `Factory ${app.slots.length - mine}`;
  }
}
const reveal = (slot) => { const r = $(`ps${slot}`); if (r) r.scrollIntoView({ block: "nearest" }); };

export function initPresets() {
  let lastSlot = -1;
  on("state", () => { renderList(); if (app.st && app.st.slot !== lastSlot) { lastSlot = app.st.slot; active = app.st.slot; renderList(); reveal(lastSlot); } });
  on("slots", renderList);
  on("bank", renderList);
  on("assets", renderList);                     // (a summary names the preset's capture)
  $("psearch").oninput = () => { query = $("psearch").value.trim().toLowerCase(); renderList(); };
  $("psearch").onkeydown = (e) => {
    if (e.key === "ArrowDown" || e.key === "Enter") { const v = visible(); if (v.length) { e.preventDefault(); active = v[0].slot; renderList(); $("list").focus(); if (e.key === "Enter") selectPreset(active); } }
    else if (e.key === "Escape" && $("psearch").value) { e.preventDefault(); $("psearch").value = ""; query = ""; renderList(); }
  };
  $("pfilter").onclick = (e) => { const b = e.target.closest("button"); if (b) { filter = b.dataset.f; renderList(); } };
  $("list").onkeydown = (e) => {
    const v = visible(), i = v.findIndex((s) => s.slot === active);
    const go = (j) => { if (!v.length) return; active = v[Math.max(0, Math.min(v.length - 1, j))].slot; renderList(); reveal(active); };
    if (e.altKey && (e.key === "ArrowUp" || e.key === "ArrowDown") && active >= 0) { e.preventDefault(); const to = active + (e.key === "ArrowUp" ? -1 : 1); const p = movePreset(active, to); if (p) p.then((ok) => { if (ok) { active = to; renderList(); reveal(to); } }); }
    else if (e.key === "ArrowDown") { e.preventDefault(); go(i + 1); }
    else if (e.key === "ArrowUp") { e.preventDefault(); go(i < 0 ? 0 : i - 1); }
    else if (e.key === "Home") { e.preventDefault(); go(0); }
    else if (e.key === "End") { e.preventDefault(); go(v.length - 1); }
    else if (e.key === "PageDown") { e.preventDefault(); go(i + 10); }
    else if (e.key === "PageUp") { e.preventDefault(); go(i - 10); }
    else if (e.key === "Enter" && active >= 0) { e.preventDefault(); selectPreset(active); }
    else if (e.key === "F2" && active >= 0) { e.preventDefault(); rename(active); }
    else if ((e.key === "ContextMenu" || (e.shiftKey && e.key === "F10")) && active >= 0) { e.preventDefault(); const r = $(`ps${active}`); if (r) rowMenu(active, r.querySelector(".more")); }
  };
  $("list").onfocus = () => { if (active < 0 && app.st) { active = app.st.slot; renderList(); } };
  $("expall").onclick = exportAll;
  $("imp").onclick = () => $("file").click();
  $("file").onchange = () => { const f = $("file").files[0]; $("file").value = ""; if (f && app.ed) importFile(f); };
  dropTarget($("presets"), (f) => /\.json$/i.test(f.name) || f.type === "application/json", (files) => { if (app.ed) importFile(files[0]); });
}
