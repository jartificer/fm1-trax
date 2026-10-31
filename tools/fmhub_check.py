#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check fmhub.json (The Firmware Hub's card for this firmware) against the hub's rules
(https://thefirmwarehub.com/kb/#fmhub) before pushing it. The hub's own "Check your file" stays the
final word.   tools/fmhub_check.py [fmhub.json]"""
import json, re, struct, sys
from pathlib import Path

FIELDS = {"what", "author", "kinds", "play", "editor", "cheat", "image"}
KINDS = {"synth", "groove", "drums", "chords", "tracker", "other"}
URLISH = re.compile(r"https?://|www\.|\b[\w-]+\.(com|net|org|io|de|app|dev|me)\b", re.I)


def image_size(b):
    """(kind, width, height) from the file's bytes: PNG, JPEG or WebP, else None."""
    if b[:8] == b"\x89PNG\r\n\x1a\n" and b[12:16] == b"IHDR":
        return ("png",) + struct.unpack(">II", b[16:24])
    if b[:4] == b"RIFF" and b[8:12] == b"WEBP":
        c = b[12:16]
        if c == b"VP8X":
            return ("webp", 1 + int.from_bytes(b[24:27], "little"), 1 + int.from_bytes(b[27:30], "little"))
        if c == b"VP8 ":
            w, h = struct.unpack("<HH", b[26:30])
            return ("webp", w & 0x3FFF, h & 0x3FFF)
        if c == b"VP8L":
            v = int.from_bytes(b[21:25], "little")
            return ("webp", (v & 0x3FFF) + 1, ((v >> 14) & 0x3FFF) + 1)
    if b[:2] == b"\xff\xd8":
        i = 2
        while i + 9 < len(b):
            if b[i] != 0xFF:
                i += 1
                continue
            m, n = b[i + 1], struct.unpack(">H", b[i + 2:i + 4])[0]
            if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
                h, w = struct.unpack(">HH", b[i + 5:i + 9])
                return ("jpeg", w, h)
            i += 2 + n
    return None


def check(path):
    root, errs = path.parent, []
    try:
        d = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return [f"not valid JSON: {e}"]
    if not isinstance(d, dict):
        return ["the file must be one JSON object"]
    if set(d) - FIELDS:
        errs.append(f"fields the hub does not allow (the whole file would be unused): {sorted(set(d) - FIELDS)}")
    w = d.get("what")
    if w is not None:
        if not isinstance(w, str) or len(w) < 20:
            errs.append("what: at least 20 characters")
        else:
            if len(w) > 300:
                errs.append(f"what: {len(w)} characters, the hub cuts at 300")
            if re.search(r"[<>{}`]", w):
                errs.append("what: no < > { } or backticks")
            if URLISH.search(w):
                errs.append("what: no links or web addresses")
            if not w.isascii():
                print(f"note: what has non-ASCII characters; make sure it is plain English")
    a = d.get("author")
    if a is not None and (not isinstance(a, str) or not 2 <= len(a) <= 60 or URLISH.search(a)):
        errs.append("author: plain text, 2-60 characters, no links")
    k = d.get("kinds")
    if k is not None and (not isinstance(k, list) or not k or k[0] not in KINDS):
        errs.append(f"kinds: a list whose first entry is one of {sorted(KINDS)}")
    for f in ("play", "editor", "cheat"):
        if f in d and (not isinstance(d[f], str) or not d[f].startswith("https://")):
            errs.append(f"{f}: an https:// address")
    im = d.get("image")
    if im is not None:
        p = root / im if isinstance(im, str) and "://" not in im else None
        if p is None or not p.is_file():
            errs.append("image: a path to a file inside the repository")
        else:
            b = p.read_bytes()
            s = image_size(b)
            if s is None:
                errs.append("image: PNG, WebP or JPEG (by its bytes)")
            elif s[1] != s[2] or not 240 <= s[1] <= 1024:
                errs.append(f"image: square, 240-1024 px (it is {s[1]}x{s[2]})")
            if len(b) > 300 * 1024:
                errs.append(f"image: 300 KB at most (it is {len(b) // 1024} KB)")
            if s:
                print(f"image: {im}, {s[0].upper()} {s[1]}x{s[2]}, {len(b)} bytes")
    if w:
        print(f"what: {len(w)} characters")
    return errs


if __name__ == "__main__":
    errs = check(Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "fmhub.json"))
    for e in errs:
        print("FAIL", e)
    print("fmhub.json: ok" if not errs else "fmhub.json: not usable by the hub")
    sys.exit(1 if errs else 0)
