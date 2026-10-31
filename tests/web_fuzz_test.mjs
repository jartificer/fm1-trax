// SPDX-License-Identifier: GPL-3.0-only
// Fuzzing the editor's VGM import (web/vgmimport.js) and what it makes, in the editor's engine
// (web/fm1trax.wasm): damaged copies of a real VGM and random register logs for both chips. Each
// import must either refuse the file with an Error or make a song the engine reads; each song is
// played for a few seconds, and its output must stay inside the clip (|v| <= 1) with the safety net
// never going off. Run from the repo root:  node tests/web_fuzz_test.mjs [SEEDS]
import {readFileSync} from "node:fs";
import {importVgm} from "../web/vgmimport.js";

const seeds = +(process.argv[2] || 120);
const x = (await WebAssembly.instantiate(readFileSync("web/fm1trax.wasm"), {})).instance.exports;
x.web_ed_init();
const cs = p => { const m = new Uint8Array(x.memory.buffer); let s = ""; while (m[p]) s += String.fromCharCode(m[p++]); return s; };
const kit = Array.from({length: x.web_sample_count()}, (_, i) => ({name: cs(x.web_sample_name(i)),
    data: new Uint8Array(x.memory.buffer, x.web_sample_data(i), x.web_sample_len(i)).slice(), rate: x.web_sample_rate(i)}));
const base = new Uint8Array(readFileSync("assets/songs/SunsetBeach.vgm"));

let r = 1;
const rnd = () => { r ^= r << 13; r ^= r >>> 17; r ^= r << 5; return r >>> 0; };
const rn = n => n > 0 ? rnd() % n : 0;

// a VGM 1.60 header and a random log: YM2612 writes on both ports (key-ons included), PSG bytes, waits
function randomVgm() {
    const b = [];
    const n = 500 + rn(4000);
    for (let i = 0; i < n; i++) {
        const k = rn(10);
        if (k < 3) b.push(0x52, rn(4) ? 0x28 : rn(256), rn(256));
        else if (k < 5) b.push(0x52 + rn(2), 0x30 + rn(0x90), rn(256));
        else if (k < 7) b.push(0x50, rn(256));
        else if (k < 8) b.push(0x62);
        else if (k < 9) b.push(0x61, rn(256), rn(8));
        else b.push(0x70 + rn(16));
    }
    b.push(0x66);
    const h = new Uint8Array(0x100 + b.length), dv = new DataView(h.buffer);
    h.set([0x56, 0x67, 0x6D, 0x20]);
    dv.setUint32(4, h.length - 4, true);
    dv.setUint32(8, 0x160, true);
    dv.setUint32(0x0C, 3579545, true);
    dv.setUint32(0x24, rn(3) ? 60 : 50, true);
    dv.setUint32(0x2C, 7670453, true);
    dv.setUint32(0x34, 0x100 - 0x34, true);
    if (rn(2)) dv.setUint32(0x1C, 0x100 + rn(b.length) - 0x1C, true);
    h.set(b, 0x100);
    return h;
}

function damaged() {
    const b = base.slice();
    const n = 1 + rn(40);
    for (let i = 0; i < n; i++) {
        const at = rn(b.length), how = rn(4);
        if (how === 0) b[at] ^= 1 << rn(8);
        else if (how === 1) b[at] = rn(256);
        else if (how === 2 && at > 0x40) return b.subarray(0, at);
        else b[0x100 + rn(b.length - 0x100)] = rn(256);
    }
    return b;
}

let refused = 0, made = 0, worst = 0, fails = 0, secs = 0;
const trips0 = x.web_guard_trips();
for (let s = 1; s <= seeds; s++) {
    r = (0x9E3779B9 * s | 1) >>> 0;
    const vgm = s % 3 ? damaged() : randomVgm();
    let out;
    const t0 = Date.now();
    try {
        out = importVgm(vgm, {kit});
    } catch (err) {
        if (!(err instanceof Error)) { fails++; console.log(`FAIL seed ${s}: threw ${err}`); }
        refused++;
        continue;
    }
    if (Date.now() - t0 > 20000) { fails++; console.log(`FAIL seed ${s}: the import took ${Date.now() - t0} ms`); }
    new Uint8Array(x.memory.buffer, x.web_io(), out.song.length).set(out.song);
    if (x.web_song_read(out.song.length) !== 0) { fails++; console.log(`FAIL seed ${s}: the engine does not read the import`); continue; }
    made++;
    x.web_post(0, 0, 0, 0);
    for (let k = 0; k < 40; k++) {                       // ~3.7 s
        const m = x.web_render(4096), a = new Float32Array(x.memory.buffer, x.web_audio_buf(), m * 2);
        for (const v of a) {
            const av = Math.abs(v);
            if (!(av <= 1)) { fails++; console.log(`FAIL seed ${s}: sample ${v}`); break; }
            if (av > worst) worst = av;
        }
        secs += m / 44100;
    }
    x.web_post(1, 0, 0, 0);
}
const trips = x.web_guard_trips() - trips0;
if (trips) { fails++; console.log(`FAIL the safety net went off ${trips} times`); }
console.log(`${seeds} VGMs: ${refused} refused, ${made} imported and played (${secs.toFixed(0)} s), loudest ${(20 * Math.log10(worst + 1e-9)).toFixed(1)} dBFS`);
if (fails) { console.log(`${fails} failures`); process.exit(1); }
