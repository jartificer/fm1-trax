// SPDX-License-Identifier: GPL-3.0-only
// FM1-DRIVE: a Mega Drive VGM (YM2612 + SN76489 register log) turned back into a song for the
// tracker: the .fm1d byte stream song_read reads (firmware/src/trk/song.c). The editor imports VGMs
// with it, and tools/vgm2fm1d.mjs makes the demo song with it.
//
//   importVgm(bytes, {kit, title, author, name, dac, rows}) -> {song: Uint8Array, notes: [..], info: {..}}
//     bytes   the .vgm (a .vgz must be inflated first)
//     kit     the DAC kit, [{name, data: Uint8Array (8-bit unsigned), rate}], the sample bank
//     dac     {sample number: kit drum name}: the samples to map by hand
//     rows    rows a pattern (default 64)
//     name    the title when the VGM's tag has none (title: in any case)
//
// How: the log is replayed tick by tick (a frame: 1/60 s, or the VGM's rate). The row grid comes
// from the notes' start ticks: one speed, or two alternating (swing). Then each channel is rebuilt
// row by row by analysis through synthesis: the cells are chosen so that FM1-DRIVE's driver, modelled
// here tick for tick (player.c), plays the notes, pitches and levels of the log.
//   FM    key-ons are notes; the patch at a key-on is the instrument, its carriers' level taken out
//         as the volume column (one patch at several volumes is one instrument); key-offs are note
//         offs; the pitch in between becomes vibrato (04xy), slides (01xx/02xx) and tone
//         portamento (03xx) where they fit; panning 08xy.
//   PSG   a restart of the volume envelope is a note, the envelope its instrument's volume macro,
//         and where it falls silent a note off; a new pitch without a restart a legato note (03xx);
//         the noise mode 20xy.
//   DAC   each sample start is a note on FM6 of a DAC instrument, the sample the kit drum most like
//         it (FM1-DRIVE plays its own kit, not the VGM's samples).
// The patterns are cut from the rows with the repeats shared, and the VGM's loop point is the
// song's. Logs made by a tracker convert closely; a game's sound driver, with its own timing,
// approximately. FM3's special mode (four pitches) is played as one.

// ---------------------------------------------------------------- song.h ---
const NCH = 13, CH_FM6 = 5, CH_SQ1 = 6, CH_NOI = 9;
const NOTE_OFF = 0xFE, MACRO_LEN = 32, MACRO_NONE = 0xFF;
const INS_FM = 1, INS_PSG = 2, INS_DAC = 3;
const MAX_ROWS = 128, MAX_PAT = 256, MAX_ORD = 256, POOL_ROWS = 16384, MAX_INS = 256;
const DAC_ROOT = 49;                                  // C-4
// effects as the cells store them (the command + 1)
const FX = {PORTA_UP: 0x02, PORTA_DN: 0x03, TONE_PORTA: 0x04, VIBRATO: 0x05, PAN: 0x09, BREAK: 0x0E,
    NOISE: 0x21, CUT: 0xED, DELAY: 0xEE};
const CARRIERS = [8, 8, 8, 8, 10, 14, 14, 15];        // bit per operator (player.c)
const SINE = [0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127, 126, 125, 122, 117, 112,
    106, 98, 90, 81, 71, 60, 49, 37, 25, 12, 0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112,
    -117, -122, -125, -126, -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12];
const FM_CLK = 7670454, PSG_CLK = 3579545, C0_HZ = 16.351598;
const TOL = 4;                                        // 64ths of a semitone: as good as the same pitch
const LOOK = 3;                                       // rows looked ahead when choosing a row's effects

const pitchOfHz = f => 768 * Math.log2(f / C0_HZ);    // 64ths of a semitone above C-0
const cdiv = (a, b) => Math.trunc(a / b);             // C's integer division

// ------------------------------------------------------------- the log ---
function readVgm(b) {
    if (b[0] === 0x1f && b[1] === 0x8b) throw new Error("a .vgz: inflate it first");
    if (String.fromCharCode(b[0], b[1], b[2], b[3]) !== "Vgm ") throw new Error("not a VGM file");
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const u32 = o => o + 4 <= b.length ? dv.getUint32(o, true) : 0;
    const ver = u32(8);
    const vgm = {rate: (ver >= 0x101 ? u32(0x24) : 0) || 60, loop: null, ev: [], blocks: [], dacData: null,
        fmClock: ver >= 0x110 ? u32(0x2C) & 0x3FFFFFFF : 0, psgClock: u32(0x0C) & 0x3FFFFFFF, title: "", author: ""};
    if (!vgm.fmClock) throw new Error("no YM2612 in this VGM: not a Mega Drive log");
    if (u32(0x14)) {
        const g = u32(0x14) + 0x14, n = u32(g + 8), s = [];
        for (let i = g + 12; i + 1 < g + 12 + n && i + 1 < b.length; i += 2) s.push(dv.getUint16(i, true));
        const tags = String.fromCharCode(...s).split("\0");
        vgm.title = tags[0] || "";
        vgm.author = tags[6] || "";
    }
    const loopAt = u32(0x1C) ? u32(0x1C) + 0x1C : -1;
    let p = ver >= 0x150 && u32(0x34) ? u32(0x34) + 0x34 : 0x40, t = 0, seek = 0;
    const streamRate = {};
    const skip = {0x4F: 2, 0x90: 5, 0x91: 5, 0x93: 11, 0xE1: 5};
    while (p < b.length) {
        if (p === loopAt) vgm.loop = t;
        const c = b[p];
        if (c === 0x66) break;
        if (c === 0x67) {
            const n = u32(p + 3) & 0x7FFFFFFF;
            if (b[p + 2] === 0) {
                vgm.blocks.push(b.subarray(p + 7, p + 7 + n));
                if (!vgm.dacData) vgm.dacData = b.subarray(p + 7, p + 7 + n);
            }
            p += 7 + n;
        } else if (c === 0x50) { vgm.ev.push([t, 1, 0, b[p + 1]]); p += 2; }
        else if (c === 0x52 || c === 0x53) { vgm.ev.push([t, 0, (c - 0x52) << 8 | b[p + 1], b[p + 2]]); p += 3; }
        else if (c === 0x61) { t += dv.getUint16(p + 1, true); p += 3; }
        else if (c === 0x62) { t += 735; p += 1; }
        else if (c === 0x63) { t += 882; p += 1; }
        else if (c >= 0x70 && c <= 0x7F) { t += (c & 15) + 1; p += 1; }
        else if (c >= 0x80 && c <= 0x8F) {           // a DAC byte from the data block: a sample plays from a seek
            t += c & 15;
            p += 1;
        } else if (c === 0xE0) {
            seek = u32(p + 1);
            vgm.ev.push([t, 3, seek, 0]);            // a sample from the data block at this offset
            p += 5;
        } else if (c === 0x92) { streamRate[b[p + 1]] = u32(p + 2); p += 6; }
        else if (c === 0x95) { vgm.ev.push([t, 2, b[p + 2] | b[p + 3] << 8, streamRate[b[p + 1]] || 0]); p += 5; }
        else if (c === 0x94) { p += 2; }
        else if (skip[c]) p += skip[c];
        else if (c >= 0x30 && c <= 0x3F) p += 2;     // other chips' writes
        else if ((c >= 0x40 && c <= 0x4E) || (c >= 0x54 && c <= 0x5F) || (c >= 0xA0 && c <= 0xBF)) p += 3;
        else if (c >= 0xC0 && c <= 0xDF) p += 4;
        else if (c >= 0xE2 && c <= 0xFF) p += 5;
        else throw new Error(`VGM command ${c.toString(16)} at ${p.toString(16)}`);
    }
    vgm.end = t;
    return vgm;
}

// the samples the log plays: [{data, rate}] and, per event, which
function samples(vgm) {
    const list = [], byKey = new Map();
    const get = (key, make) => {
        if (!byKey.has(key)) { byKey.set(key, list.length); list.push(make()); }
        return byKey.get(key);
    };
    // the data blocks keep their numbers (a stream's rate, from its first start)
    vgm.blocks.forEach((d, k) => get(`b${k}`, () => ({data: d, rate: 0})));
    for (const e of vgm.ev) {
        if (e[1] === 2 && e[2] < vgm.blocks.length && !list[e[2]].rate) list[e[2]].rate = e[3] || 8000;
        if (e[1] === 2 && e[2] < vgm.blocks.length)
            e[4] = get(`b${e[2]}`, () => ({data: vgm.blocks[e[2]], rate: e[3] || 8000}));
        else if (e[1] === 3 && vgm.dacData && e[2] < vgm.dacData.length)
            e[4] = get(`s${e[2]}`, () => ({data: vgm.dacData.subarray(e[2], Math.min(vgm.dacData.length, e[2] + 16384)), rate: 8000}));
    }
    for (const x of list) x.rate ||= 8000;
    // a seek's sample runs to the next one's start
    const starts = [...byKey.keys()].filter(k => k[0] === "s").map(k => +k.slice(1)).sort((a, b) => a - b);
    for (let i = 0; i < starts.length; i++) {
        const s = list[byKey.get(`s${starts[i]}`)];
        const end = i + 1 < starts.length ? starts[i + 1] : vgm.dacData.length;
        s.data = vgm.dacData.subarray(starts[i], Math.min(end, starts[i] + 16384));
    }
    return list;
}

// the tick's real length, in samples, and its phase: the header's rate is nominal (a 60 Hz log can tick
// at 59.92 Hz, or every 735.13 samples), and over minutes the difference moves notes off the rows. The
// length whose multiples the note starts keep to best (their vector strength), near the nominal one.
function measureTick(vgm) {
    const t = [];
    for (const e of vgm.ev)
        if ((e[1] === 0 && e[2] === 0x28 && e[3] >> 4) || (e[1] === 1 && (e[3] & 0x90) === 0x90) || e[1] >= 2) t.push(e[0]);
    const nominal = 44100 / vgm.rate;
    const strength = T => {
        let c = 0, s = 0;
        for (const x of t) { const a = 2 * Math.PI * x / T; c += Math.cos(a); s += Math.sin(a); }
        return [Math.hypot(c, s) / Math.max(1, t.length), Math.atan2(s, c)];
    };
    let best = [nominal, ...strength(nominal)];
    if (t.length < 8 || best[1] > 0.99) return {tick: nominal, t0: 0};
    const ticks = vgm.end / nominal, step = nominal / (8 * ticks);  // a step drifts an eighth of a tick over the log
    for (let pass = 0, lo = nominal * 0.985, hi = nominal * 1.015, d = Math.max(step, (hi - lo) / 4000); pass < 3; pass++) {
        for (let T = lo; T <= hi; T += d) {
            const [v, ph] = strength(T);
            if (v > best[1] + 1e-6) best = [T, v, ph];
        }
        lo = best[0] - 2 * d; hi = best[0] + 2 * d; d = Math.max(step / 4, d / 20);
    }
    const [tick, , ph] = best;
    return {tick, t0: ((ph / (2 * Math.PI)) * tick + tick) % tick};
}

// the chips' state at each tick
function replay(vgm) {
    const {tick, t0} = vgm;
    const n = Math.floor((vgm.end - t0) / tick + 0.5) + 1;
    const reg = [new Uint8Array(256), new Uint8Array(256)];
    const fmHz = vgm.fmClock / 144;
    const psgClk = vgm.psgClock || PSG_CLK;
    const key = [0, 0, 0, 0, 0, 0], per = [0, 0, 0, 0], att = [15, 15, 15, 15];
    let latch = 0, i = 0;
    const frames = [];
    const fmState = ch => {
        const r = reg[ch / 3 | 0], k = ch % 3;
        const hi = r[0xA4 + k], fnum = (hi & 7) << 8 | r[0xA0 + k], blk = hi >> 3 & 7;
        const ops = [];
        for (const slot of [0, 8, 4, 12]) {            // OP1..OP4: the registers hold 1, 3, 2, 4
            const a = k + slot;
            ops.push([r[0x50 + a] & 31, r[0x60 + a] & 31, r[0x70 + a] & 31, r[0x80 + a] & 15, r[0x80 + a] >> 4,
                r[0x40 + a] & 127, r[0x50 + a] >> 6, r[0x30 + a] & 15, r[0x30 + a] >> 4 & 7, r[0x60 + a] >> 7, r[0x90 + a] & 15]);
        }
        return {pitch: fnum ? pitchOfHz(fnum * fmHz * (1 << blk) / (1 << 21)) : null,
            alg: r[0xB0 + k] & 7, fb: r[0xB0 + k] >> 3 & 7, ams: r[0xB4 + k] >> 4 & 3, pms: r[0xB4 + k] & 7,
            pan: r[0xB4 + k] >> 6, ops};
    };
    for (let f = 0; f < n; f++) {
        const fr = {on: [0, 0, 0, 0, 0, 0], off: [0, 0, 0, 0, 0, 0], dac: null, restart: [0, 0, 0, 0]};
        while (i < vgm.ev.length && vgm.ev[i][0] < t0 + (f + 0.5) * tick) {
            const [, kind, a, v] = vgm.ev[i++];
            if (kind === 0) {
                reg[a >> 8][a & 255] = v;
                if (a === 0x28 && (v & 3) !== 3) {
                    const ch = (v & 3) + (v & 4 ? 3 : 0);
                    if (v >> 4) fr.on[ch] = 1; else fr.off[ch] = 1;
                    key[ch] = v >> 4 ? 1 : 0;
                }
            } else if (kind === 1) {
                if (v & 0x80) {
                    latch = v >> 5 & 3;
                    if (v & 0x10) {
                        if ((v & 15) < att[latch] || att[latch] === 15) fr.restart[latch] = 1;
                        att[latch] = v & 15;
                    } else if (latch === 3) per[3] = v & 7;
                    else per[latch] = per[latch] & 0x3F0 | v & 15;
                } else if (latch < 3) per[latch] = per[latch] & 15 | (v & 0x3F) << 4;
            } else if (vgm.ev[i - 1][4] !== undefined) fr.dac = vgm.ev[i - 1][4];
        }
        fr.key = key.slice();
        fr.fm = [0, 1, 2, 3, 4, 5].map(fmState);
        fr.dacOn = reg[0][0x2B] >> 7;
        fr.lfo = reg[0][0x22];
        fr.ext3 = reg[0][0x27] >> 6 & 1;
        fr.per = per.slice();
        fr.att = att.slice();
        frames.push(fr);
    }
    const psgPitch = (c, f) => {
        const p = frames[f].per[c];
        return p ? pitchOfHz(psgClk / (32 * p)) : null;
    };
    return {n, frames, psgPitch};
}

// the row grid: (speed of the even rows, of the odd ones, first tick), the coarsest that nearly every
// note start falls on
function findGrid(fr) {
    const starts = [];
    fr.frames.forEach((x, f) => {
        if (x.on.some(Boolean) || x.dac !== null || x.restart.some(Boolean)) starts.push(f);
    });
    let best = null;
    for (let s0 = 1; s0 <= 31; s0++)
        for (let s1 = 1; s1 <= 31; s1++) {
            const per = s0 + s1;
            const hist = new Uint32Array(per);
            for (const f of starts) hist[f % per]++;
            for (let off = 0; off < per; off++) {
                const hit = (hist[off] + (s0 === s1 ? 0 : hist[(off + s0) % per])) / Math.max(1, starts.length);
                const h2 = s0 === s1 ? (hist[off] + hist[(off + s0) % per]) / Math.max(1, starts.length) : hit;
                const ok = h2 >= 0.98;
                const key = [ok ? 1 : 0, ok ? per : h2, s0 === s1 ? 1 : 0, -off];
                if (!best || key.some((v, k) => v !== best.key[k]) && cmp(key, best.key) > 0)
                    best = {key, s0, s1, off: off % (s0 === s1 ? s0 : per), hit: h2};
            }
        }
    return best;
}
const cmp = (a, b) => { for (let k = 0; k < a.length; k++) if (a[k] !== b[k]) return a[k] > b[k] ? 1 : -1; return 0; };

// ------------------------------------------- FM1-DRIVE's driver, modelled ---
// one channel of player.c: the pitch (and, on the PSG, the attenuation) each tick
class Chan {
    constructor(psg) {
        Object.assign(this, {psg, note: 0, pitch: 0, target: 0, slide: 0, porta: 0, vibSpeed: 0, vibDepth: 0,
            vibPos: 0, fine: 0, vol: psg ? 15 : 127, ins: 0, key: 0, mpos: 0, cut: 0, delayed: null});
    }
    copy() { return Object.assign(new Chan(this.psg), this); }
    apply(cell) {
        this.cut = 0;
        this.delayed = null;
        for (const [fx, v] of cell.fx || []) if (fx === FX.DELAY && v) { this.delayed = [v, cell]; return; }
        this.applyNow(cell);
    }
    applyNow(cell) {
        const fxs = cell.fx || [];
        const porta = fxs.some(([fx]) => fx === FX.TONE_PORTA);
        if (cell.ins !== undefined) this.ins = cell.ins;
        if (cell.vol !== undefined) this.vol = cell.vol;
        for (const [fx, v] of fxs) {
            if (fx === FX.PORTA_UP) this.slide = v;
            else if (fx === FX.PORTA_DN) this.slide = -v;
            else if (fx === FX.TONE_PORTA) { if (v) this.porta = v; }
            else if (fx === FX.VIBRATO) { this.vibSpeed = v >> 4; this.vibDepth = v & 15; }
            else if (fx === FX.CUT) this.cut = v;
        }
        const n = cell.note || 0;
        if (n === NOTE_OFF) this.key = 0;
        else if (n >= 1 && n <= 96) {
            if (porta && this.key && this.note) { this.target = (n - 1) * 64; this.note = n; }
            else Object.assign(this, {note: n, pitch: (n - 1) * 64, target: (n - 1) * 64, vibPos: 0, mpos: 0, key: 1});
        }
    }
    tick(t, macros) {
        if (this.delayed && t === this.delayed[0]) { const c = this.delayed[1]; this.delayed = null; this.applyNow(c); }
        if (this.cut && t === this.cut) { this.cut = 0; this.key = 0; this.note = 0; }
        if (this.slide) this.pitch += this.slide;
        if (this.porta && this.target !== this.pitch && this.note)
            this.pitch = this.pitch < this.target ? Math.min(this.pitch + this.porta, this.target) : Math.max(this.pitch - this.porta, this.target);
        this.pitch = Math.max(0, Math.min(96 * 64, this.pitch));
        let p = this.pitch + this.fine;
        if (this.vibDepth) p += cdiv(SINE[this.vibPos & 63] * this.vibDepth, 16);
        this.vibPos = this.vibPos + this.vibSpeed & 255;
        let att = 15;
        if (this.psg) {
            const m = macros.get(this.ins);
            if (this.note && this.key) att = 15 - cdiv(this.vol * (m ? m[Math.min(this.mpos, m.length - 1)] : 15), 15);
            if (this.note && m) this.mpos = Math.min(this.mpos + 1, m.length - 1);
        }
        return [p, att];
    }
}

function simulate(state, cells, ticks, macros) {
    const s = state.copy(), out = [];
    cells.forEach((cell, k) => {
        s.apply(cell);
        for (let t = 0; t < ticks[k]; t++) out.push(s.tick(t, macros));
    });
    return [s, out];
}

function pitchCost(got, want) {
    let c = 0;
    for (let i = 0; i < want.length; i++) {
        if (want[i] === null) continue;
        const d = Math.abs(got[i] - want[i]);
        c += d <= 96 ? d : 96 + (d - 96) * 0.1;
    }
    return c;
}

// the row's cell with each pitch treatment worth trying: row is what the row should play, window
// the same over the lookahead
function candidates(cell, state, row, window, psg) {
    const fx0 = cell.fx || [];
    const out = [{...cell, fx: fx0.slice()}];
    const known = row.filter(w => w !== null), wk = window.filter(w => w !== null);
    if (!known.length || fx0.length >= 2) return out;
    const held = state.note && (state.key || psg);
    const adds = [];
    if (state.vibDepth) adds.push([[FX.VIBRATO, 0]]);
    if (state.slide) adds.push([[FX.PORTA_UP, 0]]);
    // vibrato: the depth from the swing, the speed from its period (over the window)
    const lo = Math.min(...wk), hi = Math.max(...wk);
    if (hi - lo > TOL) {
        const mid = (hi + lo) / 2, cross = [];
        for (let i = 1; i < window.length; i++)
            if (window[i] !== null && window[i - 1] !== null && (window[i - 1] - mid) * (window[i] - mid) < 0) cross.push(i);
        if (cross.length >= 2) {
            const period = 2 * (cross[cross.length - 1] - cross[0]) / Math.max(1, cross.length - 1);
            const sp = Math.max(1, Math.min(15, Math.round(64 / Math.max(1, period))));
            const dp = Math.max(1, Math.min(15, Math.round((hi - lo) / 2 * 16 / 127)));
            for (const s of [sp - 1, sp, sp + 1])
                for (const d of [dp - 1, dp, dp + 1])
                    if (s >= 1 && s <= 15 && d >= 1 && d <= 15) adds.push([[FX.VIBRATO, s << 4 | d]]);
        }
    }
    // slides: the mean step a tick, over the row and over the window's start
    const rates = new Map();
    for (const seq of [known, wk.slice(0, 8)]) {
        if (seq.length < 2) continue;
        const m = (seq[seq.length - 1] - seq[0]) / (seq.length - 1);
        if (Math.abs(m) < 1) continue;
        for (const k of [0.8, 1, 1.25]) {
            const v = Math.round(Math.abs(m) * k);
            if (v >= 1 && v <= 255) rates.set(`${m > 0}${v}`, [m > 0 ? FX.PORTA_UP : FX.PORTA_DN, v]);
        }
    }
    for (const fx of rates.values()) {
        adds.push([fx]);
        if (state.vibDepth) adds.push([fx, [FX.VIBRATO, 0]]);
    }
    // a new pitch without a new note: tone portamento, at once or as fast as the log moves
    if (held && !cell.note) {
        const targets = new Set([known[known.length - 1]]);
        for (let i = 2; i < wk.length; i++)
            if (Math.abs(wk[i] - wk[i - 1]) < 1 && Math.abs(wk[i - 1] - wk[i - 2]) < 1) { targets.add(wk[i]); break; }
        for (const end of targets) {
            const n = Math.round((end - state.fine) / 64) + 1;
            if (n < 1 || n > 96 || n === state.note) continue;
            const speeds = new Set([255]);
            if (wk.length > 1) {
                let step = 0;
                for (let i = 0; i + 1 < wk.length; i++) step = Math.max(step, Math.abs(wk[i + 1] - wk[i]));
                if (step >= 1 && step < 255) for (const k of [1, 0.8, 1.25]) speeds.add(Math.max(1, Math.min(255, Math.round(step * k))));
            }
            const first = Math.abs(known[0] - state.pitch - state.fine);
            if (first >= 1 && first < 255) speeds.add(Math.round(first));
            for (const v of [...speeds].sort((a, b) => b - a))
                out.push({...cell, note: n, fx: [...fx0, [FX.TONE_PORTA, v]]});
        }
    }
    for (const a of adds) if (fx0.length + a.length <= 2) out.push({...cell, fx: [...fx0, ...a]});
    return out;
}

// each row's pitch effects (and legato notes), front to back; returns the state at the end
function fitPitch(cells, ticks, want, state, macros, psg) {
    for (let r = 0; r < cells.length; r++) {
        const look = [];
        for (let k = r; k < Math.min(cells.length, r + 1 + LOOK); k++) look.push(k);
        const window = look.flatMap(k => want[k]);
        const n0 = ticks[r];
        let best = null;
        for (const cand of candidates(cells[r], state, want[r], window, psg)) {
            const [, out] = simulate(state, [cand, ...look.slice(1).map(k => cells[k])], look.map(k => ticks[k]), macros);
            const got = out.map(o => o[0]);
            let cost = pitchCost(got.slice(0, n0), window.slice(0, n0)) + 0.5 * pitchCost(got.slice(n0), window.slice(n0));
            cost += 6 * ((cand.fx || []).length - (cells[r].fx || []).length);
            if (cand.note && !cells[r].note) cost += 8;
            if (!best || cost < best.cost - 0.01) best = {cost, cand};
        }
        cells[r] = best.cand;
        [state] = simulate(state, [best.cand], [n0], macros);
    }
    return state;
}

// how closely a channel's cells play the log: the share of its sounding ticks within a quarter tone
function matchShare(cells, ticks, want, wantAtt, macros, psg) {
    const s = new Chan(psg);
    let ok = 0, all = 0;
    cells.forEach((cell, r) => {
        s.apply(cell);
        for (let t = 0; t < ticks[r]; t++) {
            const [p, att] = s.tick(t, macros);
            const w = want[r][t], a = wantAtt ? wantAtt[r][t] : null;
            if (w === null && (a === null || a === 15)) continue;
            all++;
            if ((w === null || Math.abs(p - w) <= 16) && (a === null || Math.abs(a - att) <= 1)) ok++;
        }
    });
    return all ? ok / all : 1;
}

// --------------------------------------------------------------- the kit ---
// a drum's sound in three numbers: the centre of its spectrum and the spread around it (octaves, over the
// first 64 ms at 8 kHz) and how long it lasts (log2 s, to 20 dB down)
function drumFeatures(data, rate) {
    const R = 8000, n = Math.floor(data.length * R / rate), y = new Float64Array(n);
    let mean = 0;
    for (const v of data) mean += v;
    mean /= data.length || 1;
    for (let i = 0; i < n; i++) y[i] = data[Math.min(data.length - 1, Math.floor(i * rate / R))] - mean;
    let pk = 1e-9, last = 0;
    const fr = [];
    for (let f = 0; f * 64 < n; f++) {
        let e = 0;
        for (let i = f * 64; i < Math.min(n, f * 64 + 64); i++) e += y[i] * y[i];
        fr.push(e);
        pk = Math.max(pk, e);
    }
    fr.forEach((e, f) => { if (e > pk / 100) last = f; });
    const N = 512, m = Math.min(N, n), w = Float64Array.from({length: m}, (_, i) => y[i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / N)));
    let sw = 1e-9, s1 = 0, s2 = 0;
    for (let k = 2; k < N / 2; k++) {
        let re = 0, im = 0;
        for (let i = 0; i < m; i++) { const a = 2 * Math.PI * k * i / N; re += w[i] * Math.cos(a); im -= w[i] * Math.sin(a); }
        const p = re * re + im * im, l = Math.log2(k * R / N);
        sw += p; s1 += p * l; s2 += p * l * l;
    }
    const c = s1 / sw;
    return [c, Math.sqrt(Math.max(0, s2 / sw - c * c)), Math.log2((last + 1) * 0.008)];
}

// the kit's sounds that are not drums are never picked, and a drum kit's own are preferred
const NOT_DRUMS = /SCRATCH|ZAP|NOISE BURST|BASS PLUCK|SYNTH STAB|ORCH HIT|GONG|TIMPANI/;
const LATIN = /CONGA|BONGO|TIMBALE|COWBELL|CLAVES|WOODBLOCK|TAMBOURINE|SHAKER|TRIANGLE|AGOGO/;

function mapDrums(list, kit, forced, note) {
    const feats = kit.map(k => drumFeatures(k.data, k.rate));
    return list.map((s, k) => {
        let best = 0;
        if (forced[k] !== undefined) {
            best = kit.findIndex(d => d.name === forced[k]);
            if (best < 0) throw new Error(`no kit drum named ${forced[k]}`);
        } else {
            const f = drumFeatures(s.data, s.rate);
            let bc = Infinity;
            feats.forEach((g, i) => {
                if (NOT_DRUMS.test(kit[i].name)) return;
                const c = (g[0] - f[0]) ** 2 + (g[1] - f[1]) ** 2 + 0.5 * (g[2] - f[2]) ** 2 + (LATIN.test(kit[i].name) ? 0.5 : 0);
                if (c < bc) { bc = c; best = i; }
            });
        }
        note(`sample ${k} (${s.data.length} bytes at ${s.rate} Hz) plays as the kit's ${kit[best].name}`);
        return best;
    });
}

// ------------------------------------------------------------- converting ---
export function importVgm(bytes, opt = {}) {
    const notes = [];
    const note = s => notes.push(s);
    const vgm = readVgm(bytes);
    const kit = opt.kit || [];
    const smp = samples(vgm);                          // before the replay, which reads which sample each event plays
    Object.assign(vgm, measureTick(vgm));
    const hz = 44100 / vgm.tick;
    const fr = replay(vgm), F = fr.frames;
    const g = findGrid(fr);
    note(`rows: speed ${g.s0}${g.s1 !== g.s0 ? "/" + g.s1 : ""} at ${+hz.toFixed(2)} Hz, ${Math.round(g.hit * 100)}% of the notes on the grid`);
    if (g.hit < 0.97) note("the notes do not keep to one grid (not a tracker's log?): they are moved to the nearest row");
    if (F.some(x => x.ext3)) note("FM3 special mode: its four pitches are played as one");
    // the rows: their first tick, their length (a whole number of row pairs)
    const rowTick = [];
    for (let t = g.off, r = 0; t < fr.n; t += r % 2 ? g.s1 : g.s0, r++) rowTick.push(t);
    if (rowTick.length % 2) rowTick.pop();
    let nrows = Math.min(rowTick.length, MAX_ORD * MAX_ROWS);
    const ticks = rowTick.slice(0, nrows).map((t, k) => k % 2 ? g.s1 : g.s0);
    const endTick = rowTick[nrows - 1] + ticks[nrows - 1];
    const rowOf = f => {
        let lo = 0, hi = nrows - 1;
        while (lo < hi) { const m = lo + hi + 1 >> 1; if (rowTick[m] <= f) lo = m; else hi = m - 1; }
        return [lo, f - rowTick[lo]];
    };
    let loopRow = 0;
    if (vgm.loop !== null) {
        const lt = Math.round((vgm.loop - vgm.t0) / vgm.tick);
        let bd = Infinity;
        for (let k = 0; k < nrows; k += 2) if (Math.abs(rowTick[k] - lt) < bd) { bd = Math.abs(rowTick[k] - lt); loopRow = k; }
    }
    const song = {title: (opt.title || vgm.title || opt.name || "Untitled").slice(0, 23), author: (opt.author ?? vgm.author).slice(0, 23),
        tickHz: Math.round(hz * 10), speed: [g.s0, g.s1], lfo: (F[0] ? F[0].lfo : 0) & 15, ins: []};
    const insKey = new Map();
    const instrument = (key, make) => {
        if (!insKey.has(key)) {
            if (song.ins.length >= MAX_INS) throw new Error("more than 256 instruments");
            insKey.set(key, song.ins.length);
            song.ins.push(make(song.ins.length));
        }
        return insKey.get(key);
    };
    const cells = Array.from({length: 10}, () => Array.from({length: nrows}, () => ({})));
    const blank = () => Array.from({length: nrows}, (_, r) => new Array(ticks[r]).fill(null));
    const match = {};

    // ---- FM
    const fmStart = [];
    for (let ch = 0; ch < 6; ch++) {
        const col = cells[ch], want = blank();
        let pan = 3, sounding = false;
        for (let f = rowTick[0]; f < endTick; f++) {
            const x = F[f];
            if (ch === CH_FM6 && x.dacOn) continue;
            const st = x.fm[ch], [r, t] = rowOf(f), cell = col[r];
            if (x.on[ch] && st.pitch !== null) {
                if (cell.note && cell.note !== NOTE_OFF) continue;      // two notes in a row: the first stands
                const car = [0, 1, 2, 3].filter(i => CARRIERS[st.alg] >> i & 1);
                const off = Math.min(...car.map(i => st.ops[i][5]));
                const ops = st.ops.map((o, i) => car.includes(i) ? [...o.slice(0, 5), o[5] - off, ...o.slice(6)] : o);
                const patch = [st.alg, st.fb, st.ams, st.pms, ops];
                cell.note = Math.max(1, Math.min(96, Math.round(st.pitch / 64) + 1));
                cell.ins = instrument("fm" + JSON.stringify(patch), k => ({type: INS_FM, name: `FM${ch + 1} ${String.fromCharCode(65 + k % 26)}`, fm: patch}));
                cell.vol = 127 - off;                 // on every note, so that repeats are alike
                cell.fx = [];
                if (t) cell.fx.push([FX.DELAY, t]);
                if (st.pan !== pan) { cell.fx.push([FX.PAN, (st.pan & 2 ? 0x10 : 0) | (st.pan & 1 ? 1 : 0)]); pan = st.pan; }
                sounding = true;
            } else if (x.off[ch] && sounding && !x.key[ch]) {
                if (!cell.note) {
                    if (t) (cell.fx = cell.fx || []).push([FX.CUT, t]);
                    else cell.note = NOTE_OFF;
                }
                sounding = false;
            }
            if (x.key[ch] && st.pitch !== null) want[r][t] = st.pitch;
        }
        fmStart[ch] = want;
        fitPitch(col, ticks, want, new Chan(false), new Map(), false);
        if (want.some(w => w.some(v => v !== null))) match[`FM${ch + 1}`] = matchShare(col, ticks, want, null, new Map(), false);
    }

    // ---- DAC
    const drum = kit.length ? mapDrums(smp, kit, opt.dac || {}, note) : [];
    if (smp.length && !kit.length) note("samples: no kit given, left out");
    for (let f = rowTick[0]; f < endTick; f++) {
        const x = F[f];
        if (x.dac === null || !kit.length) continue;
        const [r, t] = rowOf(f), cell = cells[CH_FM6][r];
        if (cell.note && cell.note !== NOTE_OFF) continue;
        const d = drum[x.dac];
        cell.note = DAC_ROOT;
        cell.ins = instrument(`dac${d}`, () => ({type: INS_DAC, name: kit[d].name, dac: d}));
        cell.fx = t ? [[FX.DELAY, t]] : [];
    }

    // ---- PSG: each restart of the envelope a note
    const macros = new Map();
    for (let c = 0; c < 4; c++) {
        const col = cells[CH_SQ1 + c];
        const starts = [];
        for (let f = rowTick[0]; f < endTick; f++) if (F[f].restart[c] && F[f].att[c] < 15) starts.push(f);
        starts.forEach((f, i) => {
            const end = Math.min(i + 1 < starts.length ? starts[i + 1] : endTick, f + 255);
            const e = [];
            for (let h = f; h < end; h++) e.push(15 - F[h].att[c]);
            // the macro: the levels while it sounds; a note off where it falls silent, unless the
            // macro ends there by itself
            let k = e.length;
            while (k > 0 && e[k - 1] === 0) k--;
            const core = k ? e.slice(0, k) : [0];
            let hold = core.length;
            while (hold > 1 && core[hold - 1] === core[hold - 2]) hold--;
            let mac = core.slice(0, hold), offAt = null;
            if (k < e.length) {
                if (hold === k && hold < MACRO_LEN) mac.push(0);
                else offAt = f + k;
            }
            if (mac.length > MACRO_LEN) {
                note(`${c < 3 ? "SQ" + (c + 1) : "NOI"}: a volume envelope of ${mac.length} ticks, its first ${MACRO_LEN} kept`);
                mac = mac.slice(0, MACRO_LEN);
            }
            const idx = instrument(`psg${c === 3}${mac}`, k2 => ({type: INS_PSG, name: `${c === 3 ? "NOISE" : "PSG"} ${String.fromCharCode(65 + k2 % 26)}`, mac}));
            macros.set(idx, mac);
            const [r, t] = rowOf(f), cell = col[r];
            if (cell.note && cell.note !== NOTE_OFF) return;
            let p;
            if (c < 3) p = fr.psgPitch(c, f);
            else p = (F[f].per[3] & 3) === 3 ? fr.psgPitch(2, f) : 48 * 64;
            cell.note = p === null ? DAC_ROOT : Math.max(1, Math.min(96, Math.round(p / 64) + 1));
            cell.ins = idx;
            cell.fx = t ? [[FX.DELAY, t]] : [];
            if (offAt !== null && offAt < endTick) {
                const [r2, t2] = rowOf(offAt), c2 = col[r2];
                if (r2 === r) { if (cell.fx.length < 2) cell.fx.push([FX.CUT, t2]); }
                else if (!c2.note) {
                    if (t2) (c2.fx = c2.fx || []).push([FX.CUT, t2]);
                    else c2.note = NOTE_OFF;
                }
            }
        });
        if (c === 3) {                                 // the noise mode where it changes (the player starts white, rate 0)
            let mode = 4;
            col.forEach((cell, r) => {
                const m = F[rowTick[r]].per[3];
                if (cell.note && cell.note !== NOTE_OFF && m !== mode) {
                    cell.fx = [[FX.NOISE, (m & 4 ? 0x10 : 0) | m & 3], ...(cell.fx || [])].slice(0, 2);
                    mode = m;
                }
            });
        } else {
            const want = blank(), wantAtt = blank();
            for (let r = 0; r < nrows; r++)
                for (let t = 0; t < ticks[r]; t++) {
                    const f = rowTick[r] + t;
                    wantAtt[r][t] = F[f].att[c];
                    if (F[f].att[c] < 15) want[r][t] = fr.psgPitch(c, f);
                }
            fitPitch(col, ticks, want, new Chan(true), macros, true);
            if (starts.length) match[`SQ${c + 1}`] = matchShare(col, ticks, want, wantAtt, macros, true);
        }
    }

    // ---- the loop: what the effects leave on at the end must not run on into the loop
    if (loopRow) {
        for (let ch = 0; ch < 10; ch++) {
            const psg = ch >= CH_SQ1, m = psg ? macros : new Map();
            const [atLoop] = simulate(new Chan(psg), cells[ch].slice(0, loopRow), ticks.slice(0, loopRow), m);
            const [atEnd] = simulate(new Chan(psg), cells[ch], ticks, m);
            const cell = cells[ch][loopRow];
            cell.fx = cell.fx || [];
            if (atEnd.vibDepth && !atLoop.vibDepth && cell.fx.length < 2 && !cell.fx.some(([f]) => f === FX.VIBRATO)) cell.fx.push([FX.VIBRATO, 0]);
            if (atEnd.slide && !atLoop.slide && cell.fx.length < 2 && !cell.fx.some(([f]) => f === FX.PORTA_UP || f === FX.PORTA_DN)) cell.fx.push([FX.PORTA_UP, 0]);
        }
    }

    // ---- patterns: the rows cut, the repeats shared
    let rows = opt.rows || 64, pat = null;
    for (const r0 of [rows, 32, 16, 8]) {
        if (r0 > rows) continue;
        if (Math.ceil(nrows / r0) > MAX_ORD) continue;
        pat = patterns(cells, nrows, r0, loopRow);
        if (pat) { if (r0 !== rows) note(`patterns of ${r0} rows (of ${rows} they did not fit)`); rows = r0; break; }
    }
    if (!pat) throw Object.assign(new Error("the song is too long or varied for the tracker's pattern memory"), {notes});
    if (loopRow % rows) note(`the loop point (row ${loopRow}) is inside a pattern: the song loops to its start`);
    Object.assign(song, pat, {rows, loop: Math.floor(loopRow / rows)});
    const out = writeSong(song);
    return {song: out, notes, info: {title: song.title, author: song.author, orders: song.nord, rows, instruments: song.ins.length,
        patternRows: pat.pool, seconds: endTick / hz, loop: vgm.loop !== null, match}};
}

function patterns(cells, nrows, rows, loopRow) {
    const nord = Math.ceil(nrows / rows);
    const col = cells.map(c => c.map(x => ({...x, fx: (x.fx || []).slice()})));
    if (nrows % rows) {                                // the last pattern breaks early
        const r = nrows - 1;
        const free = col.find(c => c[r].fx.length < 2);
        if (!free) return null;
        free[r].fx.push([FX.BREAK, 0]);
    }
    const order = Array.from({length: nord}, () => new Array(NCH).fill(0));
    const pats = Array.from({length: NCH}, () => []);
    let pool = 0;
    for (let ch = 0; ch < col.length; ch++) {
        const seen = new Map();
        for (let o = 0; o < nord; o++) {
            const part = col[ch].slice(o * rows, (o + 1) * rows);
            while (part.length < rows) part.push({fx: []});
            const key = JSON.stringify(part);
            if (!seen.has(key)) {
                seen.set(key, pats[ch].length);
                pats[ch].push(part);
                if (part.some(c => c.note || c.ins !== undefined || c.vol !== undefined || c.fx.length)) pool += rows;
            }
            order[o][ch] = seen.get(key);
        }
        if (seen.size > MAX_PAT) return null;
    }
    if (pool > POOL_ROWS) return null;
    return {nord, order, pats, pool};
}

// ------------------------------------------------------------ the stream ---
function writeSong(s) {
    const o = [];
    const str = (t, n) => { for (let i = 0; i < n; i++) o.push(i < n - 1 && i < t.length ? t.charCodeAt(i) & 127 : 0); };
    const u16 = v => o.push(v & 255, v >> 8 & 255);
    o.push(...[..."FM1D"].map(c => c.charCodeAt(0)), 1);
    str(s.title, 24);
    str(s.author, 24);
    o.push(0, 0, 0, 0);                                // NTSC, YM2612, no filter, FM3 normal
    u16(s.tickHz);
    o.push(s.speed[0], s.speed[1], s.rows, 4, 16, s.lfo);
    u16(s.nord);
    u16(s.loop);
    o.push(NCH);
    u16(s.ins.length);
    s.ins.forEach((ins, i) => {
        o.push(i, ins.type);
        str(ins.name, 15);
        let vol = [];
        if (ins.type === INS_FM) {
            const [alg, fb, ams, pms, ops] = ins.fm;
            o.push(alg, fb, ams, pms);
            for (const [ar, d1r, d2r, rr, sl, tl, ks, mul, dt, am, ssg] of ops)
                o.push(ar, d1r, d2r, rr, sl, tl, ks, mul, dt, am, ssg & 8 ? ssg : 0);
        } else if (ins.type === INS_DAC) o.push(ins.dac, DAC_ROOT, 0, 0);
        else vol = ins.mac;
        o.push(vol.length);                            // the volume macro, then the arpeggio, pitch, noise ones (none)
        if (vol.length) o.push(MACRO_NONE, MACRO_NONE, ...vol);
        o.push(0, 0, 0);
    });
    for (const row of s.order) o.push(...row);
    for (let ch = 0; ch < NCH; ch++) {
        const list = (s.pats[ch] || []).map((cells, p) => [p, cells]).filter(([, cells]) =>
            cells.some(c => c.note || c.ins !== undefined || c.vol !== undefined || c.fx.length));
        u16(list.length);
        for (const [p, cells] of list) {
            o.push(p);
            let skip = 0;
            for (const c of cells) {
                const fx = [...c.fx, [0, 0], [0, 0]];
                const m = (c.note ? 1 : 0) | (c.ins !== undefined ? 2 : 0) | (c.vol !== undefined ? 4 : 0) | (fx[0][0] ? 8 : 0) | (fx[1][0] ? 16 : 0);
                if (!m) { skip++; continue; }
                while (skip) { const k = Math.min(skip, 128); o.push(0x80 | k - 1); skip -= k; }
                o.push(m);
                if (m & 1) o.push(c.note);
                if (m & 2) o.push(c.ins);
                if (m & 4) o.push(c.vol);
                if (m & 8) o.push(fx[0][0], fx[0][1] & 255);
                if (m & 16) o.push(fx[1][0], fx[1][1] & 255);
            }
            o.push(0x40);
        }
    }
    o.push(0xEE);
    return Uint8Array.from(o);
}
