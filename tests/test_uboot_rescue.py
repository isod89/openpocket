#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/uboot_rescue.py against a simulated UBOOT pedal (tests/uboot_sim.py): protocol layer, guards, resume. No hardware.
  python3 tests/test_uboot_rescue.py [official-BlackBox.fwsc]   (the package tests are skipped without the official file)
"""
import contextlib, io, os, random, subprocess, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uboot_sim
import uboot_rescue as ur
import blackbox_v21_factory as factory
from uboot_rescue import DeviceGone, GuardError, RescueError, ScsiError, Uboot, Window

PACKAGE = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and sys.argv[1].endswith(".fwsc") else ur.DEFAULT_PACKAGE
SECTOR, HI = ur.SECTOR, 0xA3000
FORBIDDEN = (0xFC12, 0xFB02, 0xFB00, 0xFB03)


def noise(seed, n):
    return random.Random(seed).randbytes(n)


LOADER = noise(1, ur.LOADER_SIZE)
IMAGE = noise(2, HI)


def crashed_flash(same_sectors=(0x10, 0x11, 0x50)):
    """What the looping pedal holds: the official head, other app bytes (a few sectors already right), other data above."""
    flash = bytearray(noise(3, ur.FLASH_SIZE))
    flash[:0x4000] = IMAGE[:0x4000]
    for s in same_sectors:
        flash[s * SECTOR:(s + 1) * SECTOR] = IMAGE[s * SECTOR:(s + 1) * SECTOR]
    return flash


def differing(flash):
    return sum(flash[a:a + SECTOR] != IMAGE[a:a + SECTOR] for a in range(0x4000, HI, SECTOR))


class Rescue(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.backups = Path(self.tmp.name)
        self.logs = []

    def pedal(self, flash=None, **kw):
        sim = uboot_sim.SimPedal(flash if flash is not None else crashed_flash(), LOADER, **kw)
        self.opened = 0

        def open_device():
            self.opened += 1
            return sim if self.opened == 1 else sim.reenumerate()
        return sim, open_device

    def run_rescue(self, open_device, sim, write, attempts=10, image=IMAGE):
        return ur.rescue(open_device, LOADER, image, write, self.backups, attempts, self.logs.append, sim.clock)

    def test_v21_factory_restore_backs_up_and_changes_only_the_sound_bank(self):
        before = bytes(crashed_flash())
        firmware = before[:factory.V21_SIZE]
        bank = noise(4, 0x3C000 + 7370)
        lo, hi = factory.BANK.lo, factory.BANK.hi
        target = before[:lo] + bank + before[lo + len(bank):]
        for write in (False, True):
            with self.subTest(write=write):
                sim, _ = self.pedal(before)
                backups = []
                sim.on_first_write = lambda: self.assertEqual(backups, [before])
                count = factory.restore(sim, LOADER, firmware, bank, write, backups.append, self.logs.append)
                self.assertEqual(backups, [before])
                self.assertEqual(count, (hi - lo) // SECTOR)
                self.assertEqual(bytes(sim.flash), target if write else before)
                self.assertEqual(sim.erases(), list(range(lo, hi, SECTOR)) if write else [])
                self.assertEqual(sim.seen[-1][0], ur.RUN_APP)

    def test_v21_factory_restore_refuses_nonstock_app(self):
        flash = crashed_flash()
        firmware = bytes(flash[:factory.V21_SIZE])
        flash[0x9E273] ^= 1
        sim, _ = self.pedal(flash)
        with self.assertRaisesRegex(GuardError, "complete original BlackBox V21"):
            factory.restore(sim, LOADER, firmware, noise(4, 0x3C000 + 7370), True,
                            self.fail, self.logs.append)
        self.assertFalse(sim.writes() or sim.erases())

    # ---- the read-only mode
    def test_read_only_never_writes_and_backs_up(self):
        flash = crashed_flash()
        before = bytes(flash)
        sim, open_device = self.pedal(flash)
        n, saved = self.run_rescue(open_device, sim, write=False)
        self.assertEqual(n, differing(before))
        self.assertEqual(bytes(sim.flash), before)
        self.assertFalse(sim.writes() or sim.erases())
        self.assertEqual(sim.seen[-1][0], ur.RUN_APP)
        self.assertEqual(saved[0].read_bytes(), before)
        self.assertTrue(saved[0].name.endswith(".dump.bin"))

    def test_read_only_uboot_cannot_erase_or_write(self):
        sim, _ = self.pedal()
        dev = Uboot(sim, None, sim.clock)
        for call in (lambda: dev.erase_sector(0x5000), lambda: dev.flash_write(0x5000, b"x" * 512)):
            self.assertRaises(GuardError, call)
        self.assertEqual(sim.seen, [])

    # ---- the rescue
    def test_write_restores_only_the_window(self):
        flash = crashed_flash()
        before = bytes(flash)
        sim, open_device = self.pedal(flash)
        sim.on_first_write = lambda: self.assertTrue(list(self.backups.glob("*.dump.bin")), "backup before the first write")
        n, saved = self.run_rescue(open_device, sim, write=True)
        self.assertEqual(n, differing(before))
        self.assertEqual(bytes(sim.flash[:HI]), IMAGE)
        self.assertEqual(bytes(sim.flash[HI:]), before[HI:], "everything above the image is untouched")
        self.assertEqual(sim.erases(), sorted(sim.erases()))
        self.assertEqual(len(sim.erases()), n, "sectors that already match are not erased")
        self.assertTrue(all(0x4000 <= a < HI for a in sim.erases() + sim.writes()))
        self.assertFalse(any(op in FORBIDDEN for op, _ in sim.seen))
        self.assertEqual(sim.seen[-1][0], ur.RUN_APP)
        self.assertEqual(saved[0].read_bytes(), before)
        self.assertEqual(self.opened, 1, "the loader's watchdog never fired: a command is always sent in time")

    def test_rerun_after_success_writes_nothing(self):
        sim, open_device = self.pedal()
        self.run_rescue(open_device, sim, write=True)
        sim.seen.clear()
        n, _ = self.run_rescue(open_device, sim, write=True)
        self.assertEqual(n, 0)
        self.assertFalse(sim.writes() or sim.erases())

    def test_resume_after_the_pedal_drops_off(self):
        original = bytes(crashed_flash())
        for drop in (3, 60, 200, 700, 1500, 2600):
            with self.subTest(drop_after_commands=drop):
                self.backups = Path(tempfile.mkdtemp(dir=self.tmp.name))
                sim, open_device = self.pedal()
                sim.drop_at = drop
                n, saved = self.run_rescue(open_device, sim, write=True)
                self.assertGreater(self.opened, 1)
                self.assertEqual(bytes(sim.flash[:HI]), IMAGE)
                self.assertEqual(bytes(sim.flash[HI:]), original[HI:])
                self.assertLessEqual(len(sim.erases()), differing(original) + 1, "only the interrupted sector is done twice")
                self.assertEqual(len(list(self.backups.glob("*.dump.bin"))), 1)
                self.assertEqual(saved[0].read_bytes(), original, "the backup is the flash before anything was written")

    def test_the_loader_watchdog_is_simulated(self):
        sim, open_device = self.pedal()
        dev = Uboot(sim, None, sim.clock)
        ur.attach(dev, LOADER, self.logs.append)
        sim.clock.t += 4
        self.assertRaises(DeviceGone, dev.keepalive)

    def test_gives_up_after_the_attempts(self):
        sim, open_device = self.pedal()
        flaky = lambda: (sim.reenumerate(), setattr(sim, "drop_at", 5))[0]
        with self.assertRaises(RescueError):
            self.run_rescue(flaky, sim, write=True, attempts=3)

    # ---- the refusals
    def refused(self, sim, open_device, write=True):
        with self.assertRaises(GuardError) as cm:
            self.run_rescue(open_device, sim, write=write)
        self.assertFalse(sim.writes() or sim.erases(), "nothing was erased or written")
        return str(cm.exception)

    def test_refuses_another_device(self):
        sim, open_device = self.pedal(vendor="UDISK")
        self.assertIn("not the WL82 UBOOT1.00", self.refused(sim, open_device))
        self.assertEqual(sim.seen, [], "not even the loader was sent")

    def test_refuses_another_chip_key(self):
        sim, open_device = self.pedal(key=0x1234)
        self.assertIn("chip key 0x1234", self.refused(sim, open_device))

    def test_refuses_another_flash_size(self):
        sim, open_device = self.pedal(flash_id=0xEB6015)
        self.assertIn("1 MiB", self.refused(sim, open_device))

    def test_refuses_a_package_with_another_head(self):
        flash = crashed_flash()
        flash[0x100] ^= 0xFF
        sim, open_device = self.pedal(flash)
        self.assertIn("head", self.refused(sim, open_device))
        self.assertEqual(len(list(self.backups.glob("*.dump.bin"))), 1, "the backup was made before the head check")

    def test_loader_commands_are_a_whitelist(self):
        sim, _ = self.pedal()
        dev = Uboot(sim, Window.for_image(HI), sim.clock)
        for op in FORBIDDEN + (0xFC13, 0xFB31):
            with self.subTest(op=hex(op)):
                self.assertRaises(GuardError, dev._command, op, b"")
        self.assertEqual(sim.seen, [])

    def test_write_window(self):
        sim, _ = self.pedal()
        dev = Uboot(sim, Window.for_image(HI), sim.clock)
        for addr in (0, 0x3000, 0x3FFF, HI, HI + SECTOR, 0xFF000, 0x5001):
            with self.subTest(erase=hex(addr)):
                self.assertRaises(GuardError, dev.erase_sector, addr)
        for addr, n in ((0, 512), (0x3E00, 512), (HI, 512), (HI - 256, 512), (0x4F00, 512), (0x4000, 0)):
            with self.subTest(write=(hex(addr), n)):
                self.assertRaises(GuardError, dev.flash_write, addr, b"x" * n)
        self.assertRaises(GuardError, dev.mem_write, 0x1C00000, b"x" * 512)
        self.assertRaises(GuardError, dev.mem_write, ur.LOADER_AT + ur.LOADER_SIZE - 100, b"x" * 512)
        self.assertEqual(sim.seen, [])

    def test_window_comes_from_the_image_and_never_exceeds_0xff000(self):
        self.assertEqual(Window.for_image(HI), Window(0x4000, HI))
        self.assertEqual(Window.for_image(0xFF000), Window(0x4000, 0xFF000))
        for size in (0x4000, 0x100000, 0xFF800, HI + 1, 0):
            with self.subTest(size=hex(size)):
                self.assertRaises(GuardError, Window.for_image, size)

    # ---- pieces
    def test_cipher_and_magic(self):
        self.assertEqual(ur.MAGIC, "孟黎我爱你，玉林".encode("gb2312"))

        def bit_crc(data, crc):
            for b in data:
                crc ^= b << 8
                for _ in range(8):
                    crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
            return crc
        crc, ks = bit_crc(b"\xff\xff", 0xFFFF), []
        for i in range(40):
            crc = bit_crc(ur.MAGIC[i % 16:i % 16 + 1], crc)
            ks.append(crc & 0xFF)
        self.assertEqual(ur.cipher(bytes(40)), bytes(ks))
        self.assertEqual(ur.cipher(ur.cipher(b"abc")), b"abc")

    def test_bulk_only_framing(self):
        w = ur.cbw(7, b"\x12\0\0\0\x24\0", 36, True)
        self.assertEqual((len(w), w[:4], w[12], w[14]), (31, b"USBC", 0x80, 6))
        self.assertEqual(ur.parse_csw(b"USBS" + (7).to_bytes(4, "little") + bytes(5), 7), (0, 0))
        self.assertRaises(ScsiError, ur.parse_csw, b"USBS" + (8).to_bytes(4, "little") + bytes(5), 7)
        self.assertRaises(ScsiError, ur.parse_csw, b"XXXX" + (7).to_bytes(4, "little") + bytes(5), 7)

    def test_finds_the_bulk_only_interface(self):
        cfg = bytes.fromhex("0902200001010080c80904000002080650000705810240000107050102400001")  # read from the real pedal
        self.assertEqual(ur.mass_storage_endpoints(cfg), (0, 0x81, 0x01))
        hid = bytes.fromhex("0902190001010080320904000001030100000705810340000a")
        self.assertRaises(RescueError, ur.mass_storage_endpoints, hid)

    def test_loader_must_match_the_pin(self):
        bad = Path(self.tmp.name) / "wl82loader.bin"
        bad.write_bytes(LOADER)
        self.assertRaises(RescueError, ur.get_loader, str(bad))
        sha, ur.LOADER_SHA256 = ur.LOADER_SHA256, __import__("hashlib").sha256(LOADER).hexdigest()
        self.addCleanup(setattr, ur, "LOADER_SHA256", sha)
        self.assertEqual(ur.get_loader(str(bad)), LOADER)

    def test_image_checks_and_exit_codes(self):
        small = Path(self.tmp.name) / "short.bin"
        small.write_bytes(b"x" * 1000)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(ur.main(["--image", str(small)]), 2)
            self.assertEqual(ur.main(["--image", str(small) + ".missing"]), 1)
            self.assertEqual(ur.main(["--package", str(small) + ".missing"]), 1)
        self.assertIn("REFUSED", err.getvalue())

    def test_check_reports_the_prerequisites(self):
        r = subprocess.run([sys.executable, str(Path(ur.__file__)), "--check", "--package", str(Path(self.tmp.name) / "none.fwsc")],
                           capture_output=True, text=True)
        lines = r.stdout.splitlines()
        self.assertTrue(any("libusb" in l for l in lines), r.stdout)
        self.assertTrue(any(l.startswith("note official package not found") for l in lines), r.stdout)
        self.assertEqual(r.returncode, 1 if any(l.startswith("FAIL") for l in lines) else 0)

    # ---- the official package
    @unittest.skipUnless(PACKAGE.exists(), "official BlackBox package not found")
    def test_official_package(self):
        image = ur.load_image(package=PACKAGE)
        self.assertEqual(len(image), HI)
        self.assertEqual(Window.for_image(len(image)), Window(0x4000, HI))
        flash = bytearray(noise(4, ur.FLASH_SIZE))
        flash[:0x4000] = image[:0x4000]
        sim = uboot_sim.SimPedal(flash, LOADER)
        n, _ = ur.rescue(lambda: sim, LOADER, image, True, self.backups, 1, self.logs.append, sim.clock)
        self.assertEqual(bytes(sim.flash[:HI]), image)
        broken = Path(self.tmp.name) / "broken.fwsc"
        raw = bytearray(PACKAGE.read_bytes())
        raw[len(raw) // 2] ^= 1
        broken.write_bytes(raw)
        self.assertRaises(RescueError, ur.load_image, package=broken)


if __name__ == "__main__":
    unittest.main(verbosity=1)
