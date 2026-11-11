#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Restore the AM4 sound bank after installing official BlackBox V21."""
import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile

import pocketx_fw
import uboot_rescue as usb

V21_SHA = "1dc7bc50db2b8ac3468509a8c43eb871fead262ebe7727e28e4df882fad5bb87"
AMP_CAB_SHA = "7e77ce724e53b8880415b5f3c40e60b5760a49ea110012931beeab6b93e7ec0d"
PRESET_SHA = "a76c5a3b710b38ee0d3859977987ebb29d50dee3e10527ef73c09accc9194467"
BANK = usb.Window(0xBF000, 0xFD000)
V21_SIZE = 0x9F000
EDITOR_ASSETS = Path("/Applications/M-EFCS.app/Contents/Frameworks/App.framework/Versions/A/"
                     "Resources/flutter_assets/lib/resources/bin/blackbox")


def require_hash(data, expected, label):
    if hashlib.sha256(data).hexdigest() != expected:
        raise usb.GuardError(f"{label} does not match the verified vendor file")
    return data


def load_inputs(package, assets):
    raw = require_hash(package.read_bytes(), V21_SHA, "Firmware package")
    logical, _ = pocketx_fw.fwsc_to_logical(raw)
    entry = next(e for e in pocketx_fw.ufw_parse(logical)["ents"] if e["name"] == "flash.bin")
    firmware = logical[entry["off"]:entry["off"] + entry["len"]]
    amps = require_hash((assets / "blackbox_am4_ampCab.bin").read_bytes(), AMP_CAB_SHA, "Amp/cab bank")
    presets = require_hash((assets / "blackbox_am4_preset.bin").read_bytes(), PRESET_SHA, "Preset bank")
    return firmware, amps + presets


def restore(msc, loader, firmware, bank, write, save, log):
    if len(firmware) != V21_SIZE or len(bank) != 0x3C000 + 7370:
        raise usb.GuardError("Unexpected V21 firmware or sound bank size")
    dev = usb.Uboot(msc, BANK if write else None)
    chip = usb.attach(dev, loader, log)
    try:
        before = dev.read_range(0, chip.size, chip.io)
        if before[:V21_SIZE] != firmware:
            raise usb.GuardError("Flash does not contain the complete original BlackBox V21 image. Install official V21 first")
        save(before)
        dev.keepalive()
        end = BANK.lo + len(bank)
        target = before[:BANK.lo] + bank + before[end:]
        sectors = [a for a in range(BANK.lo, BANK.hi, usb.SECTOR)
                   if before[a:a + usb.SECTOR] != target[a:a + usb.SECTOR]]
        log(f"{len(sectors)} factory bank sectors differ")
        if write:
            for i, addr in enumerate(sectors, 1):
                dev.write_sector(addr, target[addr:addr + usb.SECTOR])
                if i % 8 == 0 or i == len(sectors):
                    log(f"Factory bank sectors written and checked: {i}/{len(sectors)}")
            if dev.read_range(0, chip.size, chip.io) != target:
                raise usb.GuardError("Full flash readback differs from the planned restore")
        return len(sectors)
    finally:
        dev.run_app()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path, help="original M-VAVE V21 .fwsc")
    parser.add_argument("--assets", type=Path, default=EDITOR_ASSETS, help="directory with the two M-EFCS AM4 files")
    parser.add_argument("--backup-dir", type=Path, default=usb.DEFAULT_BACKUPS)
    parser.add_argument("--loader", type=Path, help="local copy of the pinned JieLi RAM loader")
    parser.add_argument("--write", action="store_true", help="restore factory sounds (default: read and back up)")
    args = parser.parse_args(argv)
    log = lambda message: print(message, flush=True)

    def save(flash):
        directory = args.backup_dir.expanduser()
        usb.make_dir(directory)
        with tempfile.NamedTemporaryFile(dir=directory, prefix="blackbox-v21-", suffix=".dump.bin", delete=False) as f:
            f.write(flash)
            f.flush()
            os.fsync(f.fileno())
        path = Path(f.name)
        if path.stat().st_size != len(flash):
            raise usb.RescueError(f"The backup {path} is incomplete")
        usb.hand_back(path)
        log(f"Flash backup saved: {path}")

    try:
        firmware, bank = load_inputs(args.package.expanduser(), args.assets.expanduser())
        loader = usb.get_loader(args.loader)
        msc = usb.wait_for_pedal(usb.Libusb(), log)
        try:
            count = restore(msc, loader, firmware, bank, args.write, save, log)
        finally:
            msc.close()
    except (OSError, usb.RescueError, usb.GuardError, usb.ScsiError) as e:
        print(f"Restore stopped: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Stopped. Enter USB recovery again to rerun.", file=sys.stderr)
        return 130
    if args.write:
        log(f"Factory bank verified ({count} sectors updated). After reboot, verify the sounds in the web installer.")
    else:
        log(f"Read-only check complete ({count} sectors differ). Add --write to restore the factory bank.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
