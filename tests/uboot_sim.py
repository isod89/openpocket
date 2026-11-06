# SPDX-License-Identifier: GPL-3.0-only
"""A simulated pedal in the JieLi UBOOT state, for tests/test_uboot_rescue.py (no hardware).

It answers the mass-storage commands of the mask ROM and of the RAM loader the way the open tools document them, with a
NOR flash (a write can only clear bits, an erase sets a 4 KiB sector to 0xFF), the loader's 3 s watchdog on a fake clock,
and a switch that makes the pedal drop off the bus after N commands (to test resume). It keeps every command it saw.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import uboot_rescue as ur
from uboot_rescue import DeviceGone, ScsiError, crc16

WATCHDOG = 3.0


class FakeClock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


class SimPedal:
    def __init__(self, flash, loader, clock=None, key=0x980F, flash_id=0xEB6014, vendor="WL82", buffer=0x8000):
        self.flash = bytearray(flash)
        self.loader, self.key, self.flash_id, self.vendor, self.buffer = loader, key, flash_id, vendor, buffer
        self.clock = clock or FakeClock()
        self.seen = []            # (opcode, addr) of every command that reached the device
        self.ram = bytearray(ur.LOADER_SIZE)
        self.running = False      # the RAM loader is up
        self.last = None
        self.drop_at = None       # drop off the bus when this many commands have been seen in all
        self.present = True
        self.on_first_write = None
        self._wrote = False

    def reenumerate(self):
        """The pedal rebooted and is in UBOOT again: the ROM only, the loader gone, the flash as it was left."""
        self.running, self.ram, self.last, self.present, self.drop_at = False, bytearray(ur.LOADER_SIZE), None, True, None
        return self

    def close(self):
        pass

    def writes(self):
        return [a for op, a in self.seen if op == ur.WRITE_FLASH]

    def erases(self):
        return [a for op, a in self.seen if op == ur.ERASE_SECTOR]

    def execute(self, cdb, out, in_len, timeout=2000):
        if not self.present:
            raise DeviceGone("gone")
        if self.drop_at is not None and len(self.seen) >= self.drop_at:
            self.present = False
            raise DeviceGone("gone")
        self.clock.t += 0.002
        if self.running and self.last is not None and self.clock.t - self.last > WATCHDOG:
            self.running = False  # the loader reset the chip
            self.present = False
            raise DeviceGone("watchdog")
        self.last = self.clock.t
        if cdb[0] == 0x12:
            return (b"\0\0\0\0\x1f\0\0\0" + self.vendor.encode().ljust(8) + b"UBOOT1.00".ljust(16) + b"1.00")[:in_len]
        op = int.from_bytes(cdb[:2], "big")
        self.seen.append((op, int.from_bytes(cdb[2:6], "big")))
        a = cdb[2:]
        reply = lambda body=b"": (cdb[:2] + body).ljust(16, b"\0")
        if op == ur.WRITE_MEMORY:
            addr, n = struct.unpack(">IH", a[:6])
            off = addr - ur.LOADER_AT
            if len(out) != n or crc16(out) != int.from_bytes(a[7:9], "little") or not 0 <= off <= len(self.ram) - n:
                raise ScsiError("bad data")
            self.ram[off:off + n] = out
            return b""
        if op == ur.JUMP:
            addr = struct.unpack(">I", a[:4])[0]
            self.running = addr == ur.LOADER_AT and bytes(self.ram[:len(self.loader)]) == self.loader
            return reply()
        if not self.running:
            raise ScsiError("the loader is not running")
        if op == ur.READ_KEY:
            ks = ur.cipher(b"\0\0")
            return reply(bytes([0, 0, 0, 0, (self.key >> 8) ^ ks[1], (self.key & 0xFF) ^ ks[0]]))
        if op == ur.ONLINE_DEVICE:
            return reply(bytes([3, 0]) + self.flash_id.to_bytes(4, "little"))
        if op == ur.USB_BUFFER:
            return reply(self.buffer.to_bytes(4, "big"))
        if op == ur.READ_FLASH:
            addr, n = struct.unpack(">IH", a[:6])
            if n > self.buffer or addr + n > len(self.flash):
                raise ScsiError("bad read")
            return bytes(self.flash[addr:addr + n])
        if op == ur.ERASE_SECTOR:
            addr = struct.unpack(">I", a[:4])[0]
            assert addr % ur.SECTOR == 0
            self.flash[addr:addr + ur.SECTOR] = b"\xff" * ur.SECTOR
            return reply()
        if op == ur.WRITE_FLASH:
            if not self._wrote and self.on_first_write:
                self.on_first_write()
            self._wrote = True
            addr, n = struct.unpack(">IH", a[:6])
            if len(out) != n or crc16(out) != int.from_bytes(a[7:9], "little"):
                raise ScsiError("bad data")
            for i, b in enumerate(out):
                self.flash[addr + i] &= b
            return b""
        if op == ur.RUN_APP:
            self.running, self.present = False, False
            return reply()
        raise ScsiError(f"unknown command {op:04X}")
