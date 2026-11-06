#!/bin/sh
# Build OpenPocket -> firmware/openpocket.bin (app), build/loader/ota.bin (update loader),
#                    docs/firmware/*.fwsc (packages for the web installer)   (in the container: tools/op.mjs build)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BSP="${BSP:-$HERE/bsp}"
TC="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
echo "Toolchain : $TC"
echo "BSP       : $BSP"
if [ ! -x "$TC/pi32v2/bin/clang" ]; then
  echo "ERROR: no JieLi toolchain in $TC"
  echo "  -> it is the one you already use to build CLIPPER."
  echo "  -> run again with:  JIELI_TOOLCHAIN=/path/to/toolchain sh firmware/build.sh"
  exit 1
fi
if [ ! -x "$TC/pi32v2/bin/ld" ]; then
  echo "ERROR: linker '$TC/pi32v2/bin/ld' missing (incomplete toolchain)."
  exit 1
fi
O="$HERE/build"; rm -rf "$O"; mkdir -p "$O"
CC="$TC/pi32v2/bin/clang -target pi32v2"
$CC -c "$BSP/chip/crt0.S"    -o "$O/crt0.o"
$CC -c "$BSP/chip/fm1_vec.S" -o "$O/vec.o"
$CC -c "$BSP/chip/fm1_isr.S" -o "$O/isr.o"
# 2nd core (entry + stacks, Felucca): only if openpocket.c enables it (#define DSP_CPU1 1)
CPU1OBJ=""
if grep -q "^#define DSP_CPU1 1" "$HERE/openpocket.c"; then
  $CC -c "$BSP/chip/fm1_cpu1.S" -o "$O/cpu1.o"; CPU1OBJ="$O/cpu1.o"
fi
# A test channel build (OP_CHANNEL=midi-test): its own version number (OP_TEST_VERSION, default 37) and a boot-screen tag, its
# own files (firmware/openpocket-midi-test.bin, docs/firmware/midi-test/). The release files are not touched.
CHAN="${OP_CHANNEL:-}"
BIN="$HERE/openpocket.bin"; PKGDIR="$HERE/../docs/firmware"
: > "$O/channel.h"
if [ -n "$CHAN" ]; then
  case "$CHAN" in *[!a-z0-9-]*|"") echo "ERROR: OP_CHANNEL must be lowercase letters, digits and -"; exit 1;; esac
  TESTVER="${OP_TEST_VERSION:-37}"
  printf '#define OP_VERSION_NUM "%s"\n#define OP_TAG " MIDI TEST"\n' "$TESTVER" > "$O/channel.h"
  BIN="$HERE/openpocket-$CHAN.bin"; PKGDIR="$HERE/../docs/firmware/$CHAN"
  rm -f "$BIN"
  echo "Test channel: $CHAN, version $TESTVER -> $BIN, $PKGDIR"
fi
# AC791N hardware FPU (same options as the JieLi SDK for the WL82) + DSP engine at 44.1 kHz
FP="-mcpu=r3 -mfprev1"
DSPDEF="-DPX_SR=44100.0f -DPX_OVERSAMPLE=4"
INC="-I$HERE/inc -I$HERE/../engine"
$CC $FP -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function $DSPDEF $INC \
    -I"$BSP/chip" -I"$BSP/boards/fm1" -include "$O/channel.h" -c "$HERE/openpocket.c" -o "$O/openpocket.o"
$CC $FP -O2 -fno-builtin $DSPDEF $INC -c "$HERE/../engine/px_dsp.c" -o "$O/px_dsp.o"
$CC $FP -O2 -fno-builtin -c "$HERE/px_math.c" -o "$O/px_math.o"
# official JieLi I2S driver (LLVM bitcode from the AC79 SDK, Apache-2.0) -> pi32v2 object
$CC -Os -c -x ir "$HERE/sdk/iis.bc" -o "$O/iis.o"
"$TC/pi32v2/bin/ld" -T "$HERE/app.ld" "$O/crt0.o" "$O/vec.o" "$O/isr.o" $CPU1OBJ "$O/openpocket.o" "$O/px_dsp.o" "$O/px_math.o" "$O/iis.o" -o "$O/openpocket.elf"
# symbol map: used to translate a crash address (PC) into a function name
"$TC/common/bin/objdump" -t "$O/openpocket.elf" 2>/dev/null | sort > "$O/symbols.txt" || true
"$TC/common/bin/objcopy" -O binary -j .text     "$O/openpocket.elf" "$O/text.bin"
"$TC/common/bin/objcopy" -O binary -j .data     "$O/openpocket.elf" "$O/data.bin"
"$TC/common/bin/objcopy" -O binary -j .ram_text "$O/openpocket.elf" "$O/rt.bin" 2>/dev/null || : > "$O/rt.bin"
python3 - "$TC" "$O" "$BIN" <<'PY'
import re,subprocess,sys
tc,out,dest=sys.argv[1:]
syms=subprocess.run([f"{tc}/common/bin/objdump","-t",f"{out}/openpocket.elf"],capture_output=True,text=True,check=True).stdout
def sym(n):
    m=re.search(rf"^([0-9a-f]+) .*\s{n}$",syms,re.M); return int(m.group(1),16) if m else None
load=sym("_data_load"); rt_load=sym("_rt_load"); start=sym("_start")
assert start==0x02000120,hex(start or 0)
img=bytearray(open(f"{out}/text.bin","rb").read())
for at,part in sorted([(rt_load,open(f"{out}/rt.bin","rb").read()),(load,open(f"{out}/data.bin","rb").read())]):
    if part:
        assert at-0x02000120>=len(img),"sections overlap"
        img+=b"\xff"*(at-0x02000120-len(img))+part
img+=b"\xff"*(-len(img)%4)
open(dest,"wb").write(img); print(f"OK -> openpocket.bin: {len(img)} bytes")
PY

# ---- update loader (loader/, felucca-bsp's, Pocket X layout) -> build/loader/ota.bin ----
L="$O/loader"; mkdir -p "$L"
$CC -c "$HERE/../loader/crt0_ldr.S" -o "$L/crt0_ldr.o"
$CC -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function -I"$HERE/inc" -I"$BSP/chip" -I"$BSP/boards/fm1" \
    -c "$HERE/../loader/loader.c" -o "$L/loader.o"
"$TC/pi32v2/bin/ld" --gc-sections -e _start -T "$HERE/../loader/loader.ld" "$L/crt0_ldr.o" "$L/loader.o" -o "$L/loader.elf"
"$TC/common/bin/objcopy" -O binary -j .text "$L/loader.elf" "$L/loader.bin"
# the loader rewrites the flash: nothing of it may live in XIP (0x02xxxxxx)
"$TC/common/bin/objdump" -h "$L/loader.elf" | python3 -c '
import sys
for ln in sys.stdin:
    p = ln.split()
    if len(p) > 4 and p[1].startswith(".") and int(p[2], 16) and int(p[3], 16) >= 0x02000000:
        sys.exit("ERROR: loader section " + p[1] + " outside RAM")'

python3 "$HERE/../tools/mkloader.py" "$L/loader.bin" "$L/ota.bin"

# ---- update packages for the web installer -> docs/firmware/ (PocketX_9NN.fwsc, BlackBox_9NN.fwsc, manifest.json) ----
VER=$(sed -n 's/^#define OP_VERSION_NUM "\([0-9]*\)".*/\1/p' "$HERE/openpocket.c")
[ -n "$CHAN" ] && VER="$TESTVER"
python3 "$HERE/../tools/pxpkg.py" "$BIN" "$L/ota.bin" "$PKGDIR" --version "$VER" \
    --sdk-files "$HERE/sdk/packager"
