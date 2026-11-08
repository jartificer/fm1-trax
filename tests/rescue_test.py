#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1_rescue.rescue() against a fake UBOOT device: both FM-1 flash chips are taken, another refused; of V15 and
V16 the one whose bootloader the FM-1 has is written, and only up to the end of its app area."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import fm1_rescue as R  # noqa: E402


class FakeUboot:
    def __init__(self, fid, flash):
        self.fid, self.flash, self.writes, self.ran = fid, bytearray(flash), 0, False

    def inquiry(self): return "WL82", "UBOOT"
    def start_loader(self, loader): pass
    def chip(self): return 0x980F, 3, self.fid
    def read(self, a, n): return bytes(self.flash[a:a + n])

    def write_sector(self, a, data, end):
        assert R.WRITE_MIN <= a < end and len(data) == R.SECTOR
        self.flash[a:a + len(data)] = data
        self.writes += 1

    def run_app(self): self.ran = True


def main():
    bad = 0

    def ok(cond, name):
        nonlocal bad
        print(("ok   " if cond else "FAIL ") + name)
        bad += not cond

    v15 = bytes((i * 7 + 3) & 0xFF for i in range(0x93000))
    v16 = bytes((i * 5 + 1) & 0xFF for i in range(0xA9000))
    pkgs = [("V15", v15), ("V16", v16)]
    data = bytes((i * 3) & 0xFF for i in range(R.FLASH_SIZE))     # songs, settings, calibration
    quiet = lambda *a, **k: None

    def unit(head):                                                # a broken app on a bootloader
        return head[:R.WRITE_MIN] + data[R.WRITE_MIN:]

    with tempfile.TemporaryDirectory() as tmp:
        for fid, name in ((0x856014, "Puya P25Q80H"), (0xB36014, "UCUN UC25HQ80")):
            for vname, im in pkgs:
                flash = unit(im)
                dev = FakeUboot(fid, flash)
                done = R.rescue(dev, pkgs, b"", True, tmp, say=quiet)
                n = len(im)
                ok(done and dev.ran and dev.flash[:n] == im and dev.flash[n:] == flash[n:],
                   f"{fid:06X} ({name}), {vname}'s bootloader: {vname} written, the data above it kept")
        dev = FakeUboot(0xEF4014, unit(v15))
        try:
            R.rescue(dev, pkgs, b"", True, tmp, say=quiet)
            ok(False, "EF4014 (another flash): refused")
        except R.Fail:
            ok(dev.writes == 0, "EF4014 (another flash): refused, nothing written")
        dev = FakeUboot(0x856014, unit(v16))
        try:
            R.rescue(dev, [("V15", v15)], b"", True, tmp, say=quiet)
            ok(False, "V16's bootloader, only V15 given: refused")
        except R.Fail:
            ok(dev.writes == 0, "V16's bootloader, only V15 given: refused, nothing written")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
