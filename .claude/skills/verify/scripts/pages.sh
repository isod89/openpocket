#!/bin/sh
# Drives docs/index.html (installer), docs/editor.html (editor) and the pedal skin in headless Chromium against the simulated
# pedals (devsim.c / edsim.c) with a mocked Web MIDI device. Never touches a real pedal. Evidence: build/page/*.png,
# build/editor/*.png, build/pedal/*.png.
#   PW_DIR=/path/with/node_modules sh .claude/skills/verify/scripts/pages.sh      (default PW_DIR: ~/.cache/openpocket-pw)
set -e
cd "$(dirname "$0")/../../../.."
PW_DIR="${PW_DIR:-$HOME/.cache/openpocket-pw}"
if [ ! -d "$PW_DIR/node_modules/playwright" ]; then
  echo "Installing Playwright + Chromium into $PW_DIR (outside the repository, about 100 MB, once)"
  mkdir -p "$PW_DIR" && (cd "$PW_DIR" && npm init -y >/dev/null && npm i playwright >/dev/null && npx playwright install chromium)
fi
mkdir -p build
cc -O1 -Wall -Wno-unused-function -o build/devsim tests/devsim.c
cc -O1 -Wall -Wno-unused-function -o build/edsim tests/edsim.c engine/px_dsp.c firmware/px_math.c -lm
export NODE_PATH="$PW_DIR/node_modules"
node tests/test_page.mjs build/page
node tests/test_editor_page.mjs build/editor
node tests/test_pedal_page.mjs build/pedal
node tests/test_audio_page.mjs build/audio
