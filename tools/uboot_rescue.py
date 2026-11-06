#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""uboot_rescue.py - restore the official firmware of a crash-looping M-VAVE BlackBox from a Mac, with no extra hardware.

Between crashes the pedal sits about 44 s in the JieLi mask-ROM USB boot state (USB 4C4A:8057, "WL80UBOOT1.00"): a USB
Mass Storage device with vendor SCSI commands. This tool waits for it, loads JieLi's RAM loader (wl82loader.bin) and
rewrites the app area of the flash from the official package. See docs/RESCUE.md for the full walkthrough.

  tools/uboot_rescue.py                      read-only: steps 1 to 4 below, nothing is ever written to the flash
  tools/uboot_rescue.py --write              the rescue (needs the read-only run to have passed)
  options: --package FILE.fwsc (default ~/Documents/BlackBox (2).fwsc, BlackBox_016)  |  --image flash.bin
           --loader FILE  --backup-dir DIR (default ~/OpenPocket-rescue)  --attempts N (default 10)

One-time install (macOS): `brew install libusb`. Nothing else: Python 3 and its standard library only (libusb is called
through ctypes; no pip packages). macOS keeps its own mass-storage driver on the pedal, so libusb needs root to take the
interface: start the tool with `sudo` (it waits for the pedal by itself, start it BEFORE the pedal reaches UBOOT).

Steps, in this order (a rerun starts again at step 1 and resumes: only the sectors that still differ are written):
  1 wait for 4C4A:8057 and start the moment it appears; SCSI INQUIRY must answer WL82 / UBOOT1.00
  2 upload the pinned wl82loader.bin to RAM (SHA-256 checked) and run it; read the chip key (0x980F) and the flash id
  3 back up the whole flash to a file, before anything is written
  4 check that the pedal's flash [0, 0x4000) is the head of the image
  5 --write only: for each 4 KiB sector in [0x4000, end of the image) that differs: erase, write, read back, compare
  6 run-app: the pedal restarts into its firmware
Guards that cannot be switched off: no write outside [0x4000, end of the image) with the end never above 0xFF000; a
whitelist of loader commands (no chip erase FB02, no key burn FC12); the chip key and the flash size must match.
"""
import argparse, ctypes, ctypes.util, hashlib, os, pwd, struct, sys, time, urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pocketx_fw
from pocketx_fw import crc16

VID, PID = 0x4C4A, 0x8057
LOADER_URL = ("https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/"
              "c32b28e7a04f873f526ba929cb1bae9f05442fe9/data/loaderblobs/usb/wl82loader.bin")
LOADER_SHA256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"
LOADER_SIZE = 24064
LOADER_AT = 0x1C02000
CHIP_KEY = pocketx_fw.CHIPKEY
FLASH_SIZE = 1 << 20
SECTOR = 0x1000
IO = 512
WRITE_FROM = 0x4000
WRITE_LIMIT = 0xFF000

def user_home():
    """The home of the person who typed sudo, not root's."""
    name = os.environ.get("SUDO_USER")
    return Path(pwd.getpwnam(name).pw_dir) if os.geteuid() == 0 and name else Path.home()


def hand_back(path):
    """Under sudo, give a file or folder we create back to the person who typed it."""
    uid, gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if os.geteuid() == 0 and uid and gid:
        os.chown(path, int(uid), int(gid))


def make_dir(path):
    missing = [p for p in reversed(path.parents) if not p.exists()] + ([path] if not path.exists() else [])
    path.mkdir(parents=True, exist_ok=True)
    for p in missing:
        hand_back(p)


DEFAULT_PACKAGE = user_home() / "Documents" / "BlackBox (2).fwsc"
DEFAULT_BACKUPS = user_home() / "OpenPocket-rescue"
LOADER_CACHE = ROOT / "build" / "cache" / "wl82loader.bin"

# the commands of the mask ROM and of the RAM loader (kagaimiq/jl-uboot-tool docs, fm1buddy), and the only ones this
# tool can send
WRITE_MEMORY, JUMP = 0xFB06, 0xFB08
READ_KEY, ONLINE_DEVICE, USB_BUFFER, READ_FLASH, RUN_APP = 0xFC09, 0xFC0A, 0xFC14, 0xFD05, 0xFC0C
ERASE_SECTOR, WRITE_FLASH = 0xFB01, 0xFB04
READ_ONLY_OPS = frozenset({WRITE_MEMORY, JUMP, READ_KEY, ONLINE_DEVICE, USB_BUFFER, READ_FLASH, RUN_APP})
WRITE_OPS = frozenset({ERASE_SECTOR, WRITE_FLASH})

# JieLi "CrcDecode" cipher of the chip key answer (key 0xFFFFFFFF); the magic is "孟黎我爱你，玉林" in GB2312
MAGIC = bytes.fromhex("c3cfc0e8ced2b0aec4e3a3acd3f1c1d6")


class RescueError(Exception):
    """Setup problem (no libusb, no access, no image): fix it and rerun."""


class GuardError(Exception):
    """A safety check refused to go on. Nothing more is sent to the pedal."""


class ScsiError(Exception):
    """A command failed; the pedal may be back after a reset."""


class DeviceGone(ScsiError):
    """The pedal left the bus (it rebooted)."""


def cipher(data):
    crc = crc16(b"\xff\xff", 0xFFFF)
    out = bytearray(data)
    for i in range(len(out)):
        crc = crc16(MAGIC[i % 16:i % 16 + 1], crc)
        out[i] ^= crc & 0xFF
    return bytes(out)


@dataclass(frozen=True)
class Window:
    """The only flash range this run may erase or write."""
    lo: int
    hi: int

    @staticmethod
    def for_image(size):
        if size % SECTOR or not WRITE_FROM < size <= WRITE_LIMIT:
            raise GuardError(f"the image is {size:#x} bytes: it must be whole 4 KiB sectors and end in "
                             f"({WRITE_FROM:#x}, {WRITE_LIMIT:#x}]")
        return Window(WRITE_FROM, size)

    def holds(self, addr, n):
        return self.lo <= addr and addr + n <= self.hi


@dataclass
class Chip:
    key: int
    flash_id: int
    size: int
    io: int


class Uboot:
    """The mask ROM / loader protocol over a mass-storage transport: msc.execute(cdb, out, in_len, timeout_ms) -> bytes.
    Without a window nothing can be erased or written."""

    def __init__(self, msc, window=None, clock=time.monotonic):
        self.msc, self.window, self.clock = msc, window, clock

    def _command(self, op, args, out=None, in_len=0, answer=False, tries=3, timeout=2000):
        if op not in READ_ONLY_OPS and not (self.window and op in WRITE_OPS):
            raise GuardError(f"command {op:04X} is not allowed in this run")
        cdb = (op.to_bytes(2, "big") + args).ljust(16, b"\xff")
        err = None
        for _ in range(tries):
            try:
                data = self.msc.execute(cdb, out, 16 if answer else in_len, timeout)
            except DeviceGone:
                raise
            except ScsiError as e:
                err = e
                continue
            if answer and (len(data) != 16 or int.from_bytes(data[:2], "big") != op):
                err = ScsiError(f"command {op:04X}: wrong answer")
                continue
            if not answer and len(data) != in_len:
                err = ScsiError(f"command {op:04X}: {len(data)} of {in_len} bytes")
                continue
            return data[2:] if answer else data
        raise err

    def inquiry(self):
        data = self.msc.execute(b"\x12\x00\x00\x00\x24\x00", None, 36, 1000)
        text = lambda a, b: data[a:b].decode("ascii", "replace").strip()
        return text(8, 16), text(16, 32), text(32, 36)

    def mem_write(self, addr, data):
        if not (LOADER_AT <= addr and addr + len(data) <= LOADER_AT + LOADER_SIZE):
            raise GuardError(f"memory write at {addr:#x}: only the loader's RAM may be written")
        crc = crc16(data)
        self._command(WRITE_MEMORY, struct.pack(">IHB", addr, len(data), 0) + crc.to_bytes(2, "little"), out=data)

    def jump(self):
        try:  # the answer may be lost; the loader runs when READ_KEY answers
            self._command(JUMP, struct.pack(">IH", LOADER_AT, 1), answer=True, tries=1)
        except ScsiError:
            pass

    def chip_key(self):
        a = self._command(READ_KEY, struct.pack(">I", 0xAC6900), answer=True)
        k = cipher(bytes([a[5], a[4]]))
        return k[0] | k[1] << 8

    def online_device(self):
        return int.from_bytes(self._command(ONLINE_DEVICE, b"", answer=True)[2:6], "little")

    def usb_buffer(self):
        return int.from_bytes(self._command(USB_BUFFER, b"", answer=True, tries=2, timeout=1000)[:4], "big")

    keepalive = online_device

    def flash_read(self, addr, n):
        return self._command(READ_FLASH, struct.pack(">IH", addr, n), in_len=n)

    def erase_sector(self, addr):
        if self.window is None or addr % SECTOR or not self.window.holds(addr, SECTOR):
            raise GuardError(f"erase at {addr:#x} refused")
        self._command(ERASE_SECTOR, struct.pack(">I", addr), answer=True, timeout=5000)

    def flash_write(self, addr, data):
        if self.window is None or not data or (addr % SECTOR) + len(data) > SECTOR or not self.window.holds(addr, len(data)):
            raise GuardError(f"write at {addr:#x} refused")
        self._command(WRITE_FLASH, struct.pack(">IHB", addr, len(data), 0) + crc16(data).to_bytes(2, "little"), out=data)

    def run_app(self):
        try:  # it may restart before it answers
            self._command(RUN_APP, struct.pack(">I", 1), answer=True, tries=1, timeout=500)
        except ScsiError:
            pass

    def read_range(self, addr, n, io):
        out = bytearray()
        for a in range(addr, addr + n, io):
            out += self.flash_read(a, min(io, addr + n - a))
        return bytes(out)

    def write_sector(self, addr, data):
        """Erase, write in 512-byte pieces, read back and compare. NOR flash: a failed attempt is simply repeated."""
        for attempt in range(3):
            self.erase_sector(addr)
            for off in range(0, SECTOR, IO):
                self.flash_write(addr + off, data[off:off + IO])
            if self.read_range(addr, SECTOR, IO) == data:
                return
        raise GuardError(f"sector {addr:#x} reads back differently after 3 attempts")


def attach(dev, loader, log):
    """Steps 1 and 2: INQUIRY, loader into RAM, run it, chip key and flash id. Refuses the wrong device, key or size."""
    for _ in range(50):
        try:
            vendor, product, rev = dev.inquiry()
            break
        except DeviceGone:
            raise
        except ScsiError:
            time.sleep(0.2)
    else:
        raise ScsiError("the pedal does not answer INQUIRY")
    log(f"INQUIRY: {vendor} {product} {rev}")
    if vendor != "WL82" or not product.startswith("UBOOT1.00"):
        raise GuardError(f"this is not the WL82 UBOOT1.00 boot state ({vendor!r} {product!r})")
    for off in range(0, len(loader), IO):
        dev.mem_write(LOADER_AT + off, loader[off:off + IO])
    key = None
    for _ in range(3):
        dev.jump()
        try:
            key = dev.chip_key()
            break
        except DeviceGone:
            raise
        except ScsiError:
            pass
    if key is None:
        raise ScsiError("the loader did not start")
    flash_id = dev.online_device()
    size = 1 << (flash_id & 0xFF) if 0x10 <= flash_id & 0xFF <= 0x18 else 0
    log(f"loader running: chip key {key:#06x}, flash id {flash_id:#08x} ({size // 1024} KiB)")
    if key != CHIP_KEY:
        raise GuardError(f"chip key {key:#06x} is not {CHIP_KEY:#06x}: this is not a BlackBox, nothing was written")
    if size != FLASH_SIZE:
        raise GuardError(f"flash id {flash_id:#08x} is not a 1 MiB flash: nothing was written")
    io = IO
    try:  # as big as the loader's USB buffer allows (fm1buddy: two readings of 1 MiB at 512 B took 8 s)
        top = min(dev.usb_buffer(), 0x8000) // IO * IO
    except ScsiError:
        top = IO
    while top > IO:
        try:
            if len(dev.flash_read(0, top)) == top:
                io = top
                break
        except DeviceGone:
            raise
        except ScsiError:
            pass
        top //= 2
    return Chip(key, flash_id, size, io)


def rescue_pass(dev, loader, image, write, save_backup, log):
    """One try at steps 1 to 6 on an open device. Returns the number of sectors that differ (and were written)."""
    chip = attach(dev, loader, log)
    flash = dev.read_range(0, chip.size, chip.io)
    log(f"flash read: {len(flash) // 1024} KiB")
    save_backup(flash)
    dev.keepalive()
    if flash[:WRITE_FROM] != image[:WRITE_FROM]:
        raise GuardError("the flash head [0, 0x4000) is not the image's head: this package belongs to another layout; "
                         "nothing was written")
    log("head [0, 0x4000) matches the image")
    window = dev.window or Window.for_image(len(image))
    todo = [a for a in range(window.lo, window.hi, SECTOR) if flash[a:a + SECTOR] != image[a:a + SECTOR]]
    log(f"{len(todo)} of {(window.hi - window.lo) // SECTOR} sectors in [{window.lo:#x}, {window.hi:#x}) differ")
    if write:
        for n, addr in enumerate(todo, 1):
            dev.write_sector(addr, image[addr:addr + SECTOR])
            if n % 16 == 0 or n == len(todo):
                log(f"  written {n}/{len(todo)}")
    dev.run_app()
    log("run-app sent: the pedal restarts")
    return len(todo)


def rescue(open_device, loader, image, write, backup_dir, attempts, log, clock=time.monotonic):
    """Run the passes until one completes (each pass is safe to repeat: only differing sectors are written)."""
    saved = []

    def save_backup(flash):
        if saved:
            return
        make_dir(backup_dir)
        path = backup_dir / f"blackbox-{datetime.now():%Y%m%d-%H%M%S}.dump.bin"
        with open(path, "wb") as f:
            f.write(flash)
            f.flush()
            os.fsync(f.fileno())
        hand_back(path)
        if path.stat().st_size != len(flash):
            raise RescueError(f"the backup {path} is incomplete")
        saved.append(path)
        log(f"backup saved: {path}")

    window = Window.for_image(len(image)) if write else None
    for attempt in range(1, attempts + 1):
        msc = open_device()
        try:
            return rescue_pass(Uboot(msc, window, clock), loader, image, write, save_backup, log), saved
        except ScsiError as e:
            log(f"the pedal went away or stopped answering ({e}); waiting for it again ({attempt}/{attempts})")
        finally:
            msc.close()
    raise RescueError(f"gave up after {attempts} tries; rerun the command, it resumes where it stopped")


# ---------------------------------------------------------------- the image and the loader
def load_image(package=None, image=None):
    """flash.bin of the official package (every CRC verified first) or a raw flash image."""
    if image:
        try:
            data = Path(image).read_bytes()
        except OSError as e:
            raise RescueError(f"cannot read {image}: {e.strerror}")
    else:
        path = Path(package or DEFAULT_PACKAGE).expanduser()
        if not path.exists():
            raise RescueError(f"{path} not found: pass --package FILE.fwsc (the official BlackBox package) or --image flash.bin")
        _, L, _, u = pocketx_fw.load(path)
        bad = [k for k, v in pocketx_fw.package_checks(L, u).items() if not v]
        if bad:
            raise RescueError(f"{path}: CRC check failed ({', '.join(bad)}); not using it")
        entry = next(e for e in u["ents"] if e["name"] == "flash.bin")
        data = L[entry["off"]:entry["off"] + entry["len"]]
    Window.for_image(len(data))
    return data


def get_loader(path=None):
    """The pinned wl82loader.bin: --loader FILE, else the git-ignored cache, else downloaded; SHA-256 checked each time."""
    def checked(blob, where):
        if hashlib.sha256(blob).hexdigest() != LOADER_SHA256 or len(blob) != LOADER_SIZE:
            raise RescueError(f"{where} is not the pinned wl82loader.bin (SHA-256 {LOADER_SHA256}); not using it")
        return blob
    if path:
        try:
            blob = Path(path).read_bytes()
        except OSError as e:
            raise RescueError(f"cannot read {path}: {e.strerror}")
        return checked(blob, path)
    if LOADER_CACHE.exists() and hashlib.sha256(LOADER_CACHE.read_bytes()).hexdigest() == LOADER_SHA256:
        return LOADER_CACHE.read_bytes()
    try:
        with urllib.request.urlopen(LOADER_URL, timeout=30) as r:
            blob = r.read()
    except OSError as e:
        raise RescueError(f"could not download the loader ({e}); download {LOADER_URL} and pass --loader FILE")
    checked(blob, LOADER_URL)
    make_dir(LOADER_CACHE.parent)
    LOADER_CACHE.write_bytes(blob)
    hand_back(LOADER_CACHE)
    return blob


# ---------------------------------------------------------------- USB: libusb through ctypes, Bulk-Only transport
def cbw(tag, cdb, data_len, data_in):
    return struct.pack("<4sIIBBB16s", b"USBC", tag, data_len, 0x80 if data_in else 0, 0, len(cdb), cdb.ljust(16, b"\0"))


def parse_csw(raw, tag):
    sig, rtag, residue, status = struct.unpack("<4sIIB", raw)
    if sig != b"USBS" or rtag != tag:
        raise ScsiError("bad status wrapper")
    return residue, status


def mass_storage_endpoints(config):
    """(interface, bulk in, bulk out) of the Mass Storage / SCSI / Bulk-Only interface in a raw configuration descriptor."""
    iface = None
    eps = {}
    i = 0
    while i + 2 <= len(config) and config[i]:
        kind = config[i + 1]
        if kind == 4:
            iface = config[i + 2] if config[i + 5:i + 8] == bytes([8, 6, 0x50]) else None
        elif kind == 5 and iface is not None and config[i + 3] & 3 == 2:
            eps["in" if config[i + 2] & 0x80 else "out"] = config[i + 2]
        i += config[i]
    if iface is None or len(eps) != 2:
        raise RescueError("no Mass Storage Bulk-Only interface found on the device")
    return iface, eps["in"], eps["out"]


class DevDesc(ctypes.Structure):
    _fields_ = [("bLength", ctypes.c_uint8), ("bDescriptorType", ctypes.c_uint8), ("bcdUSB", ctypes.c_uint16),
                ("bDeviceClass", ctypes.c_uint8), ("bDeviceSubClass", ctypes.c_uint8), ("bDeviceProtocol", ctypes.c_uint8),
                ("bMaxPacketSize0", ctypes.c_uint8), ("idVendor", ctypes.c_uint16), ("idProduct", ctypes.c_uint16),
                ("bcdDevice", ctypes.c_uint16), ("iManufacturer", ctypes.c_uint8), ("iProduct", ctypes.c_uint8),
                ("iSerialNumber", ctypes.c_uint8), ("bNumConfigurations", ctypes.c_uint8)]


E_ACCESS, E_NO_DEVICE, E_BUSY, E_TIMEOUT, E_PIPE, E_NOT_SUPPORTED = -3, -4, -6, -7, -9, -12
SUDO_HINT = ("macOS keeps its mass-storage driver on the pedal and libusb needs root to take the interface: run the "
             "command again with sudo in front of it (docs/RESCUE.md)")
ROOT_HINT = ("macOS did not let go of the pedal even for root. Nothing was sent to it; send this output to whoever helps "
             "you (the other routes are in docs/RESCUE.md)")


class Libusb:
    def __init__(self):
        self.lib = self._load()
        v, p, i, u8, u16 = ctypes.c_void_p, ctypes.POINTER, ctypes.c_int, ctypes.c_uint8, ctypes.c_uint16
        sig = {"libusb_init": ([p(v)], i), "libusb_exit": ([v], None), "libusb_get_device_list": ([v, p(p(v))], ctypes.c_ssize_t),
               "libusb_free_device_list": ([p(v), i], None), "libusb_get_device_descriptor": ([v, p(DevDesc)], i),
               "libusb_open": ([v, p(v)], i), "libusb_close": ([v], None), "libusb_detach_kernel_driver": ([v, i], i),
               "libusb_claim_interface": ([v, i], i), "libusb_release_interface": ([v, i], i),
               "libusb_clear_halt": ([v, u8], i), "libusb_error_name": ([i], ctypes.c_char_p),
               "libusb_control_transfer": ([v, u8, u8, u16, u16, v, u16, ctypes.c_uint], i),
               "libusb_bulk_transfer": ([v, u8, v, i, p(i), ctypes.c_uint], i)}
        for name, (args, res) in sig.items():
            f = getattr(self.lib, name)
            f.argtypes, f.restype = args, res
        self.ctx = ctypes.c_void_p()
        self.check(self.lib.libusb_init(ctypes.byref(self.ctx)), "libusb_init")

    @staticmethod
    def _load():
        for path in [os.environ.get("LIBUSB_PATH"), "/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib"]:
            if path and os.path.exists(path):
                return ctypes.CDLL(path)
        if ctypes.util.find_library("usb-1.0"):
            return ctypes.CDLL(ctypes.util.find_library("usb-1.0"))
        raise RescueError("libusb not found: run `brew install libusb` once (or set LIBUSB_PATH)")

    def name(self, code):
        return self.lib.libusb_error_name(code).decode()

    def check(self, code, what):
        if code < 0:
            raise RescueError(f"{what}: {self.name(code)}")
        return code

    def open_pedal(self):
        """A claimed Bulk-Only transport to the pedal, or None when it is not on the bus (yet)."""
        devs = ctypes.POINTER(ctypes.c_void_p)()
        n = self.lib.libusb_get_device_list(self.ctx, ctypes.byref(devs))
        try:
            for i in range(max(n, 0)):
                desc = DevDesc()
                if self.lib.libusb_get_device_descriptor(devs[i], ctypes.byref(desc)) == 0 and (desc.idVendor, desc.idProduct) == (VID, PID):
                    return self._claim(devs[i])
        finally:
            self.lib.libusb_free_device_list(devs, 1)
        return None

    def _claim(self, dev):
        handle = ctypes.c_void_p()
        code = self.lib.libusb_open(dev, ctypes.byref(handle))
        if code in (E_ACCESS, E_BUSY, E_NOT_SUPPORTED):
            raise RescueError(f"cannot open the pedal ({self.name(code)}). {ROOT_HINT if os.geteuid() == 0 else SUDO_HINT}")
        if code < 0:
            return None
        buf = ctypes.create_string_buffer(512)
        got = self.lib.libusb_control_transfer(handle, 0x80, 6, 2 << 8, 0, buf, 512, 1000)
        if got < 9:
            self.lib.libusb_close(handle)
            return None
        iface, ep_in, ep_out = mass_storage_endpoints(buf.raw[:got])
        self.lib.libusb_detach_kernel_driver(handle, iface)  # macOS: root (or an entitlement); other codes are fine here
        code = self.lib.libusb_claim_interface(handle, iface)
        if code in (E_ACCESS, E_BUSY, E_NOT_SUPPORTED):
            self.lib.libusb_close(handle)
            raise RescueError(f"cannot take the interface from macOS ({self.name(code)}). {ROOT_HINT if os.geteuid() == 0 else SUDO_HINT}")
        if code < 0:
            self.lib.libusb_close(handle)
            return None
        return LibusbMsc(self, handle, iface, ep_in, ep_out)


class LibusbMsc:
    def __init__(self, usb, handle, iface, ep_in, ep_out):
        self.usb, self.lib, self.handle, self.iface, self.ep_in, self.ep_out = usb, usb.lib, handle, iface, ep_in, ep_out
        self.tag = 0

    def close(self):
        self.lib.libusb_release_interface(self.handle, self.iface)
        self.lib.libusb_close(self.handle)

    def _bulk(self, ep, data, n, timeout):
        buf = ctypes.create_string_buffer(data, n) if data is not None else ctypes.create_string_buffer(n)
        done = ctypes.c_int()
        code = self.lib.libusb_bulk_transfer(self.handle, ep, buf, n, ctypes.byref(done), timeout)
        if code == E_NO_DEVICE:
            raise DeviceGone("the pedal left the bus")
        if code == E_PIPE:
            self.lib.libusb_clear_halt(self.handle, ep)
        if code < 0:
            raise ScsiError(f"USB transfer: {self.usb.name(code)}")
        return buf.raw[:done.value]

    def execute(self, cdb, out, in_len, timeout=2000):
        self.tag += 1
        data_len = len(out) if out is not None else in_len
        self._bulk(self.ep_out, cbw(self.tag, cdb, data_len, out is None and in_len > 0), 31, timeout)
        data = b""
        stalled = False
        try:
            if out is not None:
                self._bulk(self.ep_out, out, len(out), timeout)
            elif in_len:
                data = self._bulk(self.ep_in, None, in_len, timeout)
        except DeviceGone:
            raise
        except ScsiError:
            stalled = True
        try:
            _, status = parse_csw(self._bulk(self.ep_in, None, 13, timeout), self.tag)
        except ScsiError:
            raise ScsiError("no status after the command")
        if status or stalled:
            raise ScsiError("command failed")
        return data


def wait_for_pedal(usb, log):
    """Poll until 4C4A:8057 is on the bus; claim it at once (the UBOOT window is only about 44 s)."""
    announced = False
    while True:
        msc = usb.open_pedal()
        if msc:
            return msc
        if not announced:
            log("waiting for the pedal in its UBOOT state (USB 4C4A:8057; plug it straight into this Mac)")
            announced = True
        time.sleep(0.05)


def check(a):
    """Prerequisites, one line each, for `op.mjs doctor`; exit 1 only when the tool cannot run at all."""
    bad = 0
    try:
        Libusb()
        print("ok   libusb found")
    except RescueError as e:
        print(f"FAIL {e}")
        bad = 1
    package = Path(a.package or DEFAULT_PACKAGE).expanduser()
    print(f"ok   official package {package}" if a.image or package.exists() else
          f"note official package not found at {package}: pass --package FILE.fwsc (the BlackBox package from M-VAVE)")
    print("ok   wl82loader.bin cached" if LOADER_CACHE.exists() else "note wl82loader.bin is downloaded on the first run (SHA-256 checked)")
    if os.geteuid() != 0:
        print("note the rescue needs root to take the pedal from macOS's mass-storage driver: `make rescue` asks for your password (sudo)")
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description="Restore the official firmware of a crash-looping BlackBox from its UBOOT state.")
    ap.add_argument("--check", action="store_true", help="only check the prerequisites (libusb, package, loader, root) and exit")
    ap.add_argument("--write", action="store_true", help="really write the flash (default: read-only, steps 1 to 4)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--package", help="official .fwsc package (default ~/Documents/BlackBox (2).fwsc)")
    src.add_argument("--image", help="raw flash image (flash.bin) instead of a package")
    ap.add_argument("--loader", help="wl82loader.bin (default: download once into build/cache/)")
    ap.add_argument("--backup-dir", default=str(DEFAULT_BACKUPS), help="where the flash backup goes (default ~/OpenPocket-rescue)")
    ap.add_argument("--attempts", type=int, default=10, help="UBOOT windows to try before giving up (default 10)")
    a = ap.parse_args(argv)
    if a.check:
        return check(a)

    t0 = [time.monotonic()]
    log = lambda m: print(f"[{time.monotonic() - t0[0]:6.1f}s] {m}", flush=True)
    try:
        image = load_image(a.package, a.image)
        loader = get_loader(a.loader)
        log(f"image {len(image):#x} bytes, loader {LOADER_SHA256[:8]}... verified; "
            + ("WRITE mode" if a.write else "read-only mode: nothing will be written"))
        usb = Libusb()

        def open_device():
            msc = wait_for_pedal(usb, log)
            t0[0] = time.monotonic()
            log("pedal found")
            return msc

        n, saved = rescue(open_device, loader, image, a.write, Path(a.backup_dir).expanduser(), a.attempts, log)
    except GuardError as e:
        print(f"\nREFUSED: {e}", file=sys.stderr)
        return 2
    except RescueError as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nstopped; rerunning resumes where it stopped", file=sys.stderr)
        return 130
    if a.write:
        print(f"\nDone: {n} sectors written and verified. Power-cycle the pedal. Backup: {saved[0]}")
    else:
        print(f"\nRead-only run finished, nothing was written. {n} sectors differ from the image. Backup: {saved[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
