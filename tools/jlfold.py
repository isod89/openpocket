#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# OpenPocket — JieLi's Bluetooth library specialised at build time: a probe in the Bluetooth audio image's build
# (firmware/build.sh, OP_BT), for the flash room Bluetooth MIDI needs beside the A2DP sink.
#
# The library's members (JieLi's LLVM bitcode, third_party/jieli-bt and jieli-ble) read their configuration from other
# files: the log switches (log_tag_const_*), config_asser, and the controller's and host's const settings
# (firmware/bt/bt_cfg.c, firmware/px_audio.h). To the compiler those are external, so it keeps every branch they guard
# and every log string. Folding them: in a text copy of a member's IR, each such external constant becomes an internal
# one with the value the image links anyway (only `const` objects, only when the IR reads them at the width bt_cfg.c
# defines), so -Os drops what they guard. The vendor's files are never changed.
#
#   jlfold.py probe TC CFG.c OUTDIR REPORT MEMBER.bc... -- LINK...
#     builds the library three ways and links each beside the real image (built and linked before, and not touched):
#       R  through text IR, nothing changed: the round trip should give the real image's bytes
#       A  the constants folded
#       B  A, and the print helpers the firmware defines empty (printf, log_print: px_audio.h; puts, put_buf,
#          printf_buf: ble/ble_libc.c) made internal and empty, so their calls and strings go
#     LINK is the real image's link command: its libjl.a and its -o are replaced (the archive keeps its name: the
#     linker script places libjl.a's code). Prints each image's size, the free main RAM, and whether the library's
#     statics kept their places (firmware/bt/bt_a2dp.c reaches two of them); writes the same to REPORT and deletes
#     OUTDIR. A failure is reported, never raised: the build goes on.
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

ENUMS = {"BT_MODULE_CLASSIC": 1, "BT_MODULE_LE": 2, "BT_BTSTACK_CLASSIC": 1, "BT_BTSTACK_LE": 4, "BT_NORMAL": 1}
CTYPE = {"int": "i32", "unsigned": "i32", "uint32_t": "i32", "int32_t": "i32", "uint16_t": "i16", "int16_t": "i16",
         "uint8_t": "i8", "int8_t": "i8", "char": "i8", "unsigned char": "i8"}
BASE = 0x02000120                                   # the image's first byte (firmware/build.sh)
SLOT = 644928                                       # the app slot (tools/pxpkg.py APP_SLOT)
RAM0, RAMLEN = 0x01C08000, 98304

GLOBAL = re.compile(r"^@([\w.$]+) = external((?: (?:hidden|protected|local_unnamed_addr|unnamed_addr))*) constant "
                    r"(i8|i16|i32|i64)(?=[ ,\n])", re.M)
PRINTS = ("printf", "log_print", "puts", "put_buf", "printf_buf")
PRINT = re.compile(r"^declare ((?:[a-z_]+ )*)(void|i32) @(" + "|".join(PRINTS) + r")\((.*)\)[^()\n]*$", re.M)


def cfg_values(path):
    """{name: (IR type, value)}: the const objects bt_cfg.c defines, its log switches, and config_asser (px_audio.h)."""
    src = re.sub(r"/\*.*?\*/", " ", open(path).read(), flags=re.S)
    v = {}
    for m in re.finditer(r"^[ \t]*const[ \t]+(unsigned[ \t]+char|\w+)[ \t]+(\w+)[ \t]*=[ \t]*([^;,{]+);", src, re.M):
        ty = CTYPE.get(re.sub(r"\s+", " ", m.group(1)))
        if not ty:
            continue
        expr = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", m.group(3).strip())
        v[m.group(2)] = (ty, int(eval(expr, {"__builtins__": {}}, dict(ENUMS))))   # (this repository's own file)
    for m in re.finditer(r"\bTAG\((log_tag_const_\w+)\)", src):
        v[m.group(1)] = ("i8", 0)                   # #define TAG(n) const char n = 0;
    v["config_asser"] = ("i32", 0)                  # px_audio.h: const uint32_t config_asser = 0
    return v


def fold(ll, vals, prints):
    """The member's IR text with the constants folded (and the print helpers emptied): (text, folded, skipped, emptied)."""
    folded, skipped, emptied = [0], [], [0]

    def g(m):
        name, ty = m.group(1), m.group(3)
        if name not in vals:
            return m.group(0)
        want, val = vals[name]
        if want != ty:                              # read at another width than defined: left to the linker
            skipped.append(f"{name} ({ty}, defined {want})")
            return m.group(0)
        folded[0] += 1
        return f"@{name} = internal constant {ty} {val}"

    ll = GLOBAL.sub(g, ll)
    if prints:
        def p(m):
            emptied[0] += 1
            attrs, ret, name, params = m.groups()
            val = "void" if ret == "void" else "i32 0"
            return f"define internal {attrs}{ret} @{name}({params}) {{\n  ret {val}\n}}"
        ll = PRINT.sub(p, ll)
    return ll, folded[0], skipped, emptied[0]


def elf(path):
    """(sections [(name, type, flags, addr, offset, size)], symbols {name: [(value, size)]}, raw bytes) of a 32-bit ELF."""
    d = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    sh = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    names = d[sh[shstrndx][4]:sh[shstrndx][4] + sh[shstrndx][5]]
    nm = lambda tab, o: tab[o:tab.index(b"\0", o)].decode("latin-1")
    secs, syms = [], {}
    for s in sh:
        secs.append((nm(names, s[0]), s[1], s[2], s[3], s[4], s[5]))
        if s[1] == 2:                               # SHT_SYMTAB
            st = sh[s[6]]
            tab = d[st[4]:st[4] + st[5]]
            for off in range(s[4], s[4] + s[5], 16):
                n, val, size = struct.unpack_from("<III", d, off)
                if n:
                    syms.setdefault(nm(tab, n), []).append((val, size))
    return secs, syms, d


def code_const(path):
    """An object's code and constants (allocated, read-only, with contents) and its data, in bytes."""
    secs, _, _ = elf(path)
    cc = sum(s[5] for s in secs if s[2] & 2 and not s[2] & 1 and s[1] != 8)         # SHF_ALLOC, not SHF_WRITE, not NOBITS
    data = sum(s[5] for s in secs if s[2] & 2 and s[2] & 1 and s[1] != 8)
    return cc, data


def image(path):
    """What firmware/build.sh would package from an ELF: its size, the free main RAM, the layout check, a digest."""
    sl, syms, d = elf(path)
    secs = {}
    for name, *rest in sl:
        secs.setdefault(name, tuple(rest))           # (type, flags, addr, offset, size); a linked image's names are unique
    one = lambda n: syms[n][0][0] if len(syms.get(n, [])) == 1 else None
    end = 0
    for sec, load in ((".text", None), (".ram_text", "_rt_load"), (".data", "_data_load")):
        if sec in secs and secs[sec][4]:
            at = secs[sec][2] if load is None else one(load)
            if at is not None:
                end = max(end, at + secs[sec][4])
    size = end - BASE + (-(end - BASE) % 4)
    h = hashlib.sha256()
    for sec in (".text", ".ram_text", ".data"):
        if sec in secs and secs[sec][0] != 8:
            h.update(d[secs[sec][3]:secs[sec][3] + secs[sec][4]])
    be = one("_bss_end")
    ram = RAMLEN - (be - RAM0) if be else None
    want = ("lmp_acl_link", "local_private_key", "a2dp_mutual_toggle", "channel", "current_conn")
    if all(len(syms.get(n, [])) == 1 for n in want):
        (la, ls), (pk, _), (mt, _), (ch, _), (cc, _) = (syms[n][0] for n in want)
        lay = "layout ok" if ls == 0x250 and pk == la + 0x254 and ch == mt + 8 and cc == mt + 4 else \
            f"layout MOVED (lmp_acl_link {la:#x} {ls:#x}, local_private_key {pk:#x}, toggle {mt:#x}, channel {ch:#x})"
    else:
        lay = "layout: " + ", ".join(f"{n} x{len(syms.get(n, []))}" for n in want)
    return size, ram, lay, h.hexdigest()[:16]


def probe(argv):
    tc, cfg, out, report = argv[:4]
    k = argv.index("--")
    members, link = argv[4:k], argv[k + 1:]
    clang = [f"{tc}/pi32v2/bin/clang", "-target", "pi32v2"]
    ar = f"{tc}/pi32v2/bin/ar"
    elfsym = os.path.join(os.path.dirname(os.path.abspath(__file__)), "elfsym.py")
    lines = []

    def say(s):
        print("fold probe: " + s, flush=True)
        lines.append(s)

    def run(cmd):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(" ".join(os.path.basename(c) for c in cmd[:3]) + ": " + (r.stderr or r.stdout).strip()[-600:])
        return r

    try:
        for sub in ("pi32v2/bin", "common/bin", "pi32v2/lib"):
            try:
                say(f"toolchain {sub}: " + " ".join(sorted(os.listdir(os.path.join(tc, sub)))))
            except OSError as e:
                say(f"toolchain {sub}: {e.strerror}")
        say("clang: " + (run(clang + ["--version"]).stdout.splitlines() or ["?"])[0])
        vals = cfg_values(cfg)
        say(f"{len(vals)} constants known (bt_cfg.c, px_audio.h)")
        real = link[link.index("-o") + 1]
        shutil.rmtree(out, ignore_errors=True)
        objs = {v: [] for v in "RAB"}
        for v in "RAB":
            os.makedirs(os.path.join(out, v))
        sizes, nfold, nprint, skipped = {}, 0, 0, set()
        for b in members:
            name = os.path.basename(os.path.dirname(b)) + "_" + os.path.basename(b)[:-3]
            ll = os.path.join(out, name + ".ll")
            run(clang + ["-S", "-emit-llvm", "-x", "ir", b, "-o", ll])
            text = open(ll).read()
            for v in "RAB":
                src = ll
                if v != "R":
                    t, nf, sk, npr = fold(text, vals, v == "B")
                    if v == "B":
                        nfold += nf
                        nprint += npr
                        skipped.update(sk)
                    src = os.path.join(out, f"{name}.{v}.ll")
                    open(src, "w").write(t)
                o = os.path.join(out, v, name + ".o")
                run(clang + ["-Os", "-c", "-x", "ir", src, "-o", o])
                run([sys.executable, elfsym, o, "memcpy=ble_memcpy", "memset=ble_memset"])
                objs[v].append(o)
                sizes.setdefault(name, {})[v] = code_const(o)
                if src != ll:
                    os.remove(src)
            os.remove(ll)
        say(f"{len(members)} members through text IR; {nfold} constants folded, {nprint} print helpers emptied"
            + (f"; left to the linker: {', '.join(sorted(skipped))}" if skipped else ""))
        rs, rr, rl, rh = image(real)
        say(f"real image {rs:,} B of {SLOT:,} ({SLOT - rs:,} free), main RAM {rr} free, {rl}")
        for v, what in (("R", "round trip only"), ("A", "constants folded"), ("B", "constants + print helpers")):
            lib = os.path.join(out, v, "libjl.a")
            run([ar, "rcs", lib] + objs[v])
            args = [lib if a.endswith("/libjl.a") or a == "libjl.a" else a for a in link]
            args[args.index("-o") + 1] = os.path.join(out, v, "openpocket.elf")
            try:
                run(args)
            except RuntimeError as e:
                say(f"{v} ({what}): the link failed: {e}")
                continue
            s, r, lay, hh = image(os.path.join(out, v, "openpocket.elf"))
            same = "the real image's bytes" if hh == rh else f"bytes differ ({hh})"
            say(f"{v} ({what}): {s:,} B ({s - rs:+,}; {SLOT - s:,} free), main RAM {r} free, {lay}, {same}")
        tot = {v: sum(x[v][0] for x in sizes.values()) for v in "RAB"}
        say(f"library code+constants: R {tot['R']:,}, A {tot['A']:,} ({tot['A'] - tot['R']:+,}), "
            f"B {tot['B']:,} ({tot['B'] - tot['R']:+,}) B")
        for name, x in sorted(sizes.items(), key=lambda i: i[1]["B"][0] - i[1]["R"][0])[:15]:
            say(f"  {name:36s} {x['R'][0]:7,} -> A {x['A'][0]:7,} -> B {x['B'][0]:7,}  ({x['B'][0] - x['R'][0]:+,})")
    except Exception as e:                          # a probe: report and let the build go on
        say(f"stopped: {type(e).__name__}: {e}")
    finally:
        shutil.rmtree(out, ignore_errors=True)
        try:
            os.makedirs(os.path.dirname(report), exist_ok=True)
            open(report, "w").write("\n".join(lines) + "\n")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "probe" and "--" in sys.argv:
        sys.exit(probe(sys.argv[2:]))
    raise SystemExit("usage: jlfold.py probe TC CFG.c OUTDIR REPORT MEMBER.bc... -- LINK...")
