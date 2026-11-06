# Editor page (docs/editor.html)

## Sub-features
52 presets (load, save, save as, move, factory reset), the current sound (blocks, types, parameters, order by dragging, name), JSON export/import, live follow of changes made on the pedal (WATCH), unsaved-change prompts, phone layout.

## How to get to it (user POV)
Open the editor from the installer page, Connect, edit.

## Driving it with Playwright
`sh .claude/skills/verify/scripts/pages.sh`; `tests/test_editor_page.mjs` talks to `build/edsim` (the real `px_editor.h` and `px_store.h`) through a fake Web MIDI. End state: every line `ok`, `editor page test passed`, `build/editor/editor_1..6*.png`. Protocol only (no browser): `node tools/op.mjs test` runs `tests/test_editor.mjs`.

## Driving it against the hosted simulator
`node tools/op.mjs sim --port 0` serves the page at `/site/editor.html` with a virtual Web MIDI: open it with `chrome-devtools-axi`, press Connect (the port is "OpenPocket"), edit, and compare with `GET /api/state` and `GET /api/screen.png` (the pedal's own screen follows an edit; a footswitch pressed over `POST /api/press/fs_up` moves the editor's selection through WATCH). Skill: `openpocket-simulator`. The real editor code over the same shim runs in `tests/test_sim.mjs`.

## Gotchas
- Protocol is in `docs/EDITOR-PROTOCOL.md`; a change to a command needs both `px_editor.h` and `docs/pxedit.js`.
- Simulated flash only; no pedal.
