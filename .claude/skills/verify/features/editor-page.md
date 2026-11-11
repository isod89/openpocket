# Editor page (docs/editor.html)

## Sub-features
Four sections on one page (`editor.html`, `editor.css`, `editor.js`; one module per section: `edsound.js`, `edpresets.js`,
`edmodels.js`, `edmidi.js`, on `edcore.js`).
- **Presets:** the 52 presets with a summary each, search and filter, load, rename, copy, move, back to factory, JSON
  export / back up / import with a plan, Undo after every write, unsaved-change prompts.
- **Sound:** the current sound (blocks, types, settings as sliders or choices, order by dragging or keyboard, name),
  Compare, the DSP load and its details, live follow of changes made on the pedal (WATCH).
- **Models:** the 16 NAM user slots and the 8 IR slots (`docs/pxnam.js`, `docs/pxir.js`), which presets use a slot,
  rename / move / remove, the browser library (`docs/pxlib.js`, IndexedDB), TONE3000 (`docs/pxt3k.js`, `docs/t3k.html`).
- **MIDI:** the Control Change map for the current preset, learned controls from a second MIDI device, the monitor
  (`docs/pxmidi.js`).
- First run and failures (no Web MIDI, MIDI refused, no pedal, pedal unplugged and back), phone layout.

## How to get to it (user POV)
Open the editor from the installer page, Connect, edit. A section is `#presets`, `#sound`, `#models` or `#midi` in the URL.

## Driving it with Playwright
`sh .claude/skills/verify/scripts/pages.sh`; `tests/test_editor_page.mjs` talks to `build/edsim` (the real `px_editor.h` and `px_store.h`) through a fake Web MIDI that has the pedal and a second device ("Foot Controller": `window.__ctl([0xB0, cc, value])` sends from it). TONE3000 is simulated by route handlers in the test. End state: every line `ok`, `editor page test passed`, `build/editor/editor_1..10*.png`. Without a browser: `node tests/test_editor.mjs` (the protocol, `docs/pxedit.js`) and `node tests/test_editor_logic.mjs` (the Control Change map against `docs/MIDI.md`, learned controls, the library and slot arithmetic), both in `node tools/op.mjs test`.

## Driving it against the hosted simulator
`node tools/op.mjs sim --port 0` serves the page at `/site/editor.html` with a virtual Web MIDI: open it with `chrome-devtools-axi`, press Connect (the port is "OpenPocket"), edit, and compare with `GET /api/state` and `GET /api/screen.png` (the pedal's own screen follows an edit; a footswitch pressed over `POST /api/press/fs_up` moves the editor's selection through WATCH). Skill: `openpocket-simulator`. The real editor code over the same shim runs in `tests/test_sim.mjs`.

## Gotchas
- Protocol is in `docs/EDITOR-PROTOCOL.md`; a change to a command needs both `px_editor.h` and `docs/pxedit.js`.
- Simulated flash only; no pedal.
- The simulator answers SysEx only: it neither takes nor sends Control Change or Program Change, and its PING has no
  per-block profile. The page test stands in for both at the fake Web MIDI: it injects a Control Change as the pedal
  would send it (the monitor's "Pedal" lines) and appends a v40.2 profile to PING's reply (`window.__prof`: the
  per-block load). On the real pedal both need a person.
- Element ids the tests rely on: `#connect`, `#status`, `#list .row` (`#psN`), `#name`, `#num`, `#star`, `#tiles .tile[data-b]`
  (`.tsel`, `.pw`), `#pK` (a setting: a range, a select or a radio group), `#type`, `#onoff`, `#save`, `#toasts`, `#dlg`
  (`#dlg-ok`, `#pick-N`), `#menu`, `#namlist` / `#irlist` (`.srow[data-slot]`, `.free [data-slot]`), `#t3kbrowse`, `#t3kcard`,
  `.cc-cell[data-t]`, `#learned`, `#monlist`.
