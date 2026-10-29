#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make FM1-DRIVE's DAC kit: 48 drum and hit samples, synthesised here (no recordings), as 8-bit
unsigned PCM at the rates Mega Drive drivers play at, written as a C header.

  tools/gen_drums.py OUT.h

Python's standard library only; the same output on every run (fixed seeds)."""
import math
import random
import sys

RATE = 13300  # Hz, the rate most of the kit plays at (a common Z80 driver rate)


def env_exp(n, t60):
    """an exponential decay that falls 60 dB in t60 seconds"""
    k = math.log(1000.0) / (t60 * RATE)
    return [math.exp(-k * i) for i in range(n)]


def sine_sweep(n, f0, f1, curve):
    out, ph = [], 0.0
    for i in range(n):
        f = f1 + (f0 - f1) * math.exp(-curve * i / RATE)
        ph += 2 * math.pi * f / RATE
        out.append(math.sin(ph))
    return out


def noise(n, seed):
    r = random.Random(seed)
    return [r.uniform(-1, 1) for _ in range(n)]


def onepole_lp(x, fc):
    a = 1 - math.exp(-2 * math.pi * fc / RATE)
    y, out = 0.0, []
    for v in x:
        y += a * (v - y)
        out.append(y)
    return out


def onepole_hp(x, fc):
    lp = onepole_lp(x, fc)
    return [a - b for a, b in zip(x, lp)]


def bandpass(x, f, q):
    """RBJ's band-pass biquad (constant 0 dB peak)"""
    f = min(f, RATE * 0.45)
    w = 2 * math.pi * f / RATE
    al = math.sin(w) / (2 * q)
    b0, b2 = al, -al
    a0, a1, a2 = 1 + al, -2 * math.cos(w), 1 - al
    x1 = x2 = y1 = y2 = 0.0
    out = []
    for v in x:
        y = (b0 * v + b2 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1 = x1, v
        y2, y1 = y1, y
        out.append(y)
    return out


def mix(*parts):
    n = max(len(p) for p, _ in parts)
    out = [0.0] * n
    for p, g in parts:
        for i, v in enumerate(p):
            out[i] += v * g
    return out


def mul(a, b):
    return [x * y for x, y in zip(a, b)]


def drive(x, k):
    return [math.tanh(v * k) / math.tanh(k) for v in x]


def secs(s):
    return int(s * RATE)


def kick(f0, f1, curve, t60, click=0.3, dist=1.0, seed=1):
    n = secs(t60)
    body = mul(sine_sweep(n, f0, f1, curve), env_exp(n, t60))
    clk = mul(onepole_hp(noise(secs(0.01), seed), 2000), env_exp(secs(0.01), 0.01))
    x = mix((body, 1.0), (clk, click))
    return drive(x, dist) if dist > 1 else x


def snare(tone, t_tone, t_noise, fc=3500, seed=2, snap=1.0):
    n = secs(max(t_tone, t_noise))
    body = mul(sine_sweep(n, tone * 1.6, tone, 30), env_exp(n, t_tone))
    nz = mul(bandpass(noise(n, seed), fc, 0.8), env_exp(n, t_noise))
    return mix((body, 0.6), (nz, 1.4 * snap))


def hat(t60, fc=7000, seed=3, metal=0.5):
    n = secs(t60)
    nz = onepole_hp(noise(n, seed), fc)
    # square partials at inharmonic ratios, the classic metallic part
    m = [0.0] * n
    for f in (205.3, 304.4, 369.6, 522.7, 540.0, 800.0):
        for i in range(n):
            m[i] += 1 if math.sin(2 * math.pi * f * 7.3 * i / RATE) > 0 else -1
    m = onepole_hp(m, fc)
    return mul(mix((nz, 1.0), (m, metal / 6)), env_exp(n, t60))


def tom(f, t60, seed=4):
    n = secs(t60)
    body = mul(sine_sweep(n, f * 1.8, f, 18), env_exp(n, t60))
    nz = mul(onepole_lp(noise(n, seed), 1500), env_exp(n, t60 / 5))
    return mix((body, 1.0), (nz, 0.4))


def clap(seed=5, spread=0.012, tail=0.25):
    n = secs(tail + 4 * spread)
    nz = bandpass(noise(n, seed), 1200, 1.2)
    e = [0.0] * n
    for k in range(4):
        st = secs(k * spread)
        dur = tail if k == 3 else spread
        for i in range(secs(dur)):
            if st + i < n:
                e[st + i] = max(e[st + i], math.exp(-i / (RATE * dur / 4)))
    return mul(nz, e)


def tonal(freqs, t60, attack=0.002, shape="sine", seed=6):
    n = secs(t60)
    out = [0.0] * n
    for f, g in freqs:
        for i in range(n):
            ph = 2 * math.pi * f * i / RATE
            v = math.sin(ph) if shape == "sine" else (1 if math.sin(ph) > 0 else -1)
            out[i] += g * v
    e = env_exp(n, t60)
    a = secs(attack) or 1
    return [o * ev * min(1, i / a) for i, (o, ev) in enumerate(zip(out, e))]


def cymbal(t60, seed, fc=5000, ring=0.6):
    n = secs(t60)
    nz = onepole_hp(noise(n, seed), fc)
    r = random.Random(seed)
    m = [0.0] * n
    for _ in range(8):
        f = r.uniform(3000, 6000)
        for i in range(n):
            m[i] += math.sin(2 * math.pi * f * i / RATE)
    return mul(mix((nz, 1.0), (m, ring / 8)), env_exp(n, t60))


def orchhit():
    n = secs(0.6)
    out = [0.0] * n
    for f in (130.8, 196.0, 261.6, 329.6, 392.0, 523.3):
        for d in (0.997, 1.003):
            ph = 0.0
            for i in range(n):
                ph += 2 * math.pi * f * d / RATE
                out[i] += (2 * ((ph / (2 * math.pi)) % 1) - 1) / 12
    return mul(onepole_lp(out, 3000), env_exp(n, 0.6))


def scratch(seed=9):
    n = secs(0.3)
    src = onepole_lp(noise(n * 2, seed), 2500)
    out, pos = [], 0.0
    for i in range(n):
        speed = 2.5 * math.sin(math.pi * i / n * 3)
        pos = (pos + abs(speed)) % (len(src) - 1)
        out.append(src[int(pos)])
    return mul(out, env_exp(n, 0.3))


def zap():
    n = secs(0.25)
    return mul(sine_sweep(n, 3000, 100, 25), env_exp(n, 0.25))


def synth_note(f, t60, harmonics):
    return tonal([(f * (k + 1), g) for k, g in enumerate(harmonics)], t60)


KIT = [
    ("KICK 808", lambda: kick(150, 48, 14, 0.7, 0.2)),
    ("KICK PUNCH", lambda: kick(220, 60, 30, 0.35, 0.6, 1.8)),
    ("KICK TIGHT", lambda: kick(180, 70, 40, 0.18, 0.5)),
    ("KICK BOOM", lambda: kick(120, 40, 8, 0.9, 0.1, 1.5)),
    ("KICK DIST", lambda: kick(200, 55, 22, 0.4, 0.4, 4.0)),
    ("KICK ELECTRO", lambda: kick(400, 52, 45, 0.45, 0.8, 2.0)),
    ("SNARE", lambda: snare(185, 0.12, 0.22)),
    ("SNARE TIGHT", lambda: snare(220, 0.06, 0.12, 4500)),
    ("SNARE 808", lambda: snare(170, 0.15, 0.25, 6000, 7)),
    ("SNARE FAT", lambda: drive(snare(150, 0.2, 0.3, 2500, 8), 2.0)),
    ("SNARE GATED", lambda: [v * (1 if i < secs(0.18) else 0) for i, v in enumerate(snare(180, 0.4, 0.6, 3000, 9))]),
    ("RIMSHOT", lambda: mix((tonal([(1700, 0.6), (820, 0.6)], 0.05), 1.0), (hat(0.04, 3000, 10), 0.6))),
    ("CLAP", lambda: clap()),
    ("CLAP 808", lambda: clap(11, 0.009, 0.35)),
    ("HAT CLOSED", lambda: hat(0.06)),
    ("HAT TIGHT", lambda: hat(0.03, 8000, 12)),
    ("HAT OPEN", lambda: hat(0.45, 6500, 13)),
    ("HAT PEDAL", lambda: hat(0.1, 5500, 14, 0.8)),
    ("RIDE", lambda: cymbal(1.0, 15, 4000, 1.2)),
    ("CRASH", lambda: cymbal(1.6, 16, 3500, 0.6)),
    ("CRASH 2", lambda: cymbal(1.2, 17, 4500, 0.4)),
    ("SPLASH", lambda: cymbal(0.6, 18, 5000, 0.5)),
    ("CHINA", lambda: drive(cymbal(1.2, 19, 2500, 1.0), 2.5)),
    ("TOM HIGH", lambda: tom(220, 0.4)),
    ("TOM MID", lambda: tom(165, 0.45)),
    ("TOM LOW", lambda: tom(120, 0.55)),
    ("TOM FLOOR", lambda: tom(90, 0.65)),
    ("SYNTOM HIGH", lambda: mul(sine_sweep(secs(0.5), 600, 150, 9), env_exp(secs(0.5), 0.5))),
    ("SYNTOM LOW", lambda: mul(sine_sweep(secs(0.6), 350, 80, 8), env_exp(secs(0.6), 0.6))),
    ("CONGA HIGH", lambda: tonal([(360, 1.0), (720, 0.2)], 0.25)),
    ("CONGA LOW", lambda: tonal([(250, 1.0), (500, 0.2)], 0.3)),
    ("BONGO", lambda: tonal([(480, 1.0), (1020, 0.3)], 0.15)),
    ("TIMBALE", lambda: mix((tonal([(520, 1.0), (1210, 0.5)], 0.35), 1.0), (hat(0.05, 2000, 20), 0.4))),
    ("COWBELL", lambda: onepole_hp(tonal([(587, 0.6), (845, 0.6)], 0.3, shape="square"), 400)),
    ("CLAVES", lambda: tonal([(2500, 1.0)], 0.08)),
    ("WOODBLOCK", lambda: tonal([(1000, 1.0), (1700, 0.4)], 0.07)),
    ("TAMBOURINE", lambda: mul(hat(0.3, 6000, 21, 1.5), [1 + 0.5 * math.sin(i / 30) for i in range(secs(0.3))])),
    ("SHAKER", lambda: [v * min(1, i / secs(0.03)) for i, v in enumerate(hat(0.12, 5000, 22, 0.1))]),
    ("TRIANGLE", lambda: tonal([(2900, 1.0), (4700, 0.3)], 1.0)),
    ("AGOGO", lambda: tonal([(880, 1.0), (2280, 0.3)], 0.3)),
    ("TIMPANI", lambda: tom(70, 1.2, 23)),
    ("ORCH HIT", orchhit),
    ("SCRATCH", scratch),
    ("ZAP", zap),
    ("NOISE BURST", lambda: mul(onepole_lp(noise(secs(0.3), 24), 3000), env_exp(secs(0.3), 0.3))),
    ("BASS PLUCK", lambda: synth_note(65.4, 0.6, [1.0, 0.6, 0.3, 0.2, 0.1])),
    ("SYNTH STAB", lambda: drive(synth_note(130.8, 0.4, [1.0, 0.5, 0.33, 0.25, 0.2, 0.16]), 1.5)),
    ("GONG", lambda: tonal([(110, 1.0), (163, 0.6), (251, 0.5), (340, 0.3), (517, 0.2)], 2.0, 0.01)),
]
assert len(KIT) == 48


def to_u8(x):
    peak = max(1e-9, max(abs(v) for v in x))
    out = []
    # normalised to 95 %, then the tail cut where it stays under one step
    for v in x:
        out.append(max(0, min(255, int(round(128 + 121 * v / peak)))))
    end = len(out)
    while end > 1 and abs(out[end - 1] - 128) <= 1:
        end -= 1
    return out[:end]


def main():
    out = sys.argv[1]
    lines = ["/* generated by tools/gen_drums.py: the DAC kit, 8-bit unsigned at %d Hz */" % RATE,
             "#pragma once", "#include <stdint.h>", ""]
    names = []
    total = 0
    for k, (name, fn) in enumerate(KIT):
        data = to_u8(fn())
        total += len(data)
        names.append((name, len(data)))
        lines.append("static const uint8_t DRUM_%d[%d] = {" % (k, len(data)))
        for i in range(0, len(data), 24):
            lines.append("    " + ",".join(str(v) for v in data[i:i + 24]) + ",")
        lines.append("};")
    lines.append("#define DRUM_RATE %du" % RATE)
    lines.append("#define NDRUM %d" % len(KIT))
    lines.append("static const char *const DRUM_NAME[NDRUM] = {%s};" % ", ".join('"%s"' % n for n, _ in names))
    lines.append("static const uint8_t *const DRUM_DATA[NDRUM] = {%s};" % ", ".join("DRUM_%d" % i for i in range(len(KIT))))
    lines.append("static const uint32_t DRUM_LEN[NDRUM] = {%s};" % ", ".join(str(n) for _, n in names))
    lines.append("/* %d bytes in all */" % total)
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d samples, %d bytes" % (out, len(KIT), total))


if __name__ == "__main__":
    main()
