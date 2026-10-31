// SPDX-License-Identifier: GPL-3.0-only
// The FM1-TRAX editor: a desktop tracker on the firmware's own engine. web/fm1trax.wasm (host/drive_web.c)
// holds the song as the FM-1 does (song_t); this page reads and writes it in place, through the
// struct's layout the module reports (web_layout), and plays it with the firmware's player and chips.
// Files go through the module too: .fm1d songs (song_write / song_read), .fm1i instruments, VGM.
// The FM-1 (or the emulator in another tab) is reached through fm1link.js.
import {SongLink, findDevice, findEmulator, MEMORY, BANK, BANKS, SLOTS, LinkError} from "./fm1link.js";
import {importVgm} from "./vgmimport.js";

const $ = id => document.getElementById(id);
const RATE = 44100, AHEAD = 3072;
const PC = {PLAY: 0, STOP: 1, JAM_ON: 2, JAM_OFF: 3, MUTE: 4, RESET: 5, INS: 6};
const NOTE_OFF = 0xFE, NOTE_CUT = 0xFD, NOTE_MAX = 96, NONE = 0xFF;
const T = {EMPTY: 0, FM: 1, PSG: 2, DAC: 3};
const MAC = ["VOL", "ARP", "PITCH", "NOISE"], MAC_LO = [0, -48, -64, 0], MAC_HI = [15, 48, 63, 7];
const hex2 = v => v.toString(16).toUpperCase().padStart(2, "0");
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

// ------------------------------------------------------------ the engine ---
let x, L;                                          // the module's exports, the struct layout
const mem = () => new Uint8Array(x.memory.buffer);
const cstr = p => { const m = mem(); let s = ""; while (m[p]) s += String.fromCharCode(m[p++]); return s; };

// the song, read and written where the module keeps it
const S = {
    get base() { return x.web_song(); },
    u8(k, i = 0) { return mem()[this.base + L[k] + i]; },
    set8(k, v, i = 0) { mem()[this.base + L[k] + i] = v; },
    u16(k) { const m = mem(), p = this.base + L[k]; return m[p] | m[p + 1] << 8; },
    set16(k, v) { const m = mem(), p = this.base + L[k]; m[p] = v & 255; m[p + 1] = v >> 8; },
    str(k, n) { const m = mem(), p = this.base + L[k]; let s = ""; for (let i = 0; i < n && m[p + i]; i++) s += String.fromCharCode(m[p + i]); return s; },
    setStr(k, n, s) { const m = mem(), p = this.base + L[k]; for (let i = 0; i < n; i++) m[p + i] = i < n - 1 && i < s.length ? s.charCodeAt(i) & 127 : 0; },
    get rows() { return this.u8(8); },
    get nord() { return this.u16(11); },
    set nord(v) { this.set16(11, v); },
    get loop() { return this.u16(12); },
    set loop(v) { this.set16(12, v); },
    get ext3() { return this.u8(5); },
    order(o, ch) { return mem()[this.base + L[13] + o * L[23] + ch]; },
    setOrder(o, ch, v) { mem()[this.base + L[13] + o * L[23] + ch] = v; },
    ins(i) { return this.base + L[14] + i * L[15]; },
};
// a cell: {note, ins, vol, fx[2], fxv[2]} at p (cell_t: note, ins, vol, fx0, fx1, fxv0, fxv1)
const C = {NOTE: 0, INS: 1, VOL: 2, FX0: 3, FX1: 4, FV0: 5, FV1: 6};
const cellAt = (ch, pat, row, alloc = false) => x.web_cell(ch, pat, row, alloc ? 1 : 0);

// the channels, as columns: FM3's operators after it when it is extended
const NAMES = ["FM1", "FM2", "FM3", "FM4", "FM5", "FM6", "SQ1", "SQ2", "SQ3", "NOI", "OP2", "OP3", "OP4"];
const columns = () => S.ext3 ? [0, 1, 2, 10, 11, 12, 3, 4, 5, 6, 7, 8, 9] : [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
const isPsg = ch => ch >= 6 && ch <= 9;
const chName = ch => ch === 2 && S.ext3 ? "OP1" : NAMES[ch];
const chColor = ch => ch >= 10 || (ch === 2 && S.ext3) ? "#ffcf4a" : ch === 9 ? "#7dff9a" : ch >= 6 ? "#4fd8ff" : "#ff6ac1";

// ----------------------------------------------------------------- state ---
const ui = {
    ord: 0, row: 0, col: 0, sub: 0,                // cursor: position, row, column (channel), field
    ins: 0, oct: 4, step: 1, edit: false, follow: true,
    sel: null,                                     // {c0, r0, c1, r1}: a block of columns and rows
    anchor: null,
    mute: 0,
    ordSel: 0,                                     // the order grid's column
    ordNib: 0, nib: 0,
    mac: 0,
    dirty: false,
};
let clip = null, insClip = null;

// ------------------------------------------------------------------ undo ---
const undo = [], redo = [];
function snapshot() {
    const n = x.web_song_write();
    return new Uint8Array(x.memory.buffer, x.web_io(), n).slice();
}
function restore(b) {
    const playing = x.web_playing();
    new Uint8Array(x.memory.buffer, x.web_io(), b.length).set(b);
    x.web_song_read(b.length);
    if (playing) play(false);
}
function change() {                                // call before an edit
    undo.push(snapshot());
    if (undo.length > 200) undo.shift();
    redo.length = 0;
    ui.dirty = true;
    autosave();
}
function doUndo(r) {
    const from = r ? redo : undo, to = r ? undo : redo;
    if (!from.length) return say("nothing to " + (r ? "redo" : "undo"));
    to.push(snapshot());
    restore(from.pop());
    fixCursor();
    refreshAll();
    autosave();
}
let saveT = 0;
function autosave() {
    clearTimeout(saveT);
    saveT = setTimeout(() => {
        try {
            const b = snapshot();
            let s = "";
            for (let i = 0; i < b.length; i += 8192) s += String.fromCharCode(...b.subarray(i, i + 8192));
            localStorage.setItem("fm1drive-song", btoa(s));   // the old name kept, so saved songs survive the rename
        } catch { /* private window, or full: the song stays in the page */ }
    }, 600);
}
function loadBytes(b) {
    new Uint8Array(x.memory.buffer, x.web_io(), b.length).set(b);
    return x.web_song_read(b.length);
}

// ----------------------------------------------------------------- audio ---
let actx = null, node = null, sent = 0, played = 0;
async function startAudio() {
    if (actx) { if (actx.state !== "running") await actx.resume(); return; }
    actx = new AudioContext({sampleRate: RATE, latencyHint: "interactive"});
    await actx.audioWorklet.addModule(new URL("worklet.js", import.meta.url));
    node = new AudioWorkletNode(actx, "fm1-out", {outputChannelCount: [2]});
    node.connect(actx.destination);
    node.port.onmessage = e => { played = e.data; pump(); };
    await actx.resume();
    pump();
}
function pump() {
    if (!node) return;
    let need = AHEAD - (sent - played);
    while (need > 0) {
        const n = x.web_render(Math.max(256, Math.ceil(need / 256) * 256));
        if (!n) break;
        const blk = new Float32Array(x.memory.buffer, x.web_audio_buf(), n * 2).slice();
        node.port.postMessage(blk, [blk.buffer]);
        sent += n;
        need -= n;
    }
}
addEventListener("pointerdown", () => startAudio().catch(() => {}), {capture: true});
addEventListener("keydown", () => startAudio().catch(() => {}), {capture: true});

// ------------------------------------------------------------- transport ---
function play(loop, fromRow = 0) {
    startAudio().catch(() => {});
    x.web_post(PC.PLAY, ui.ord, fromRow, loop ? 1 : 0);
    $("copper").classList.add("playing");
}
function stop() {
    x.web_post(PC.STOP, 0, 0, 0);
    $("copper").classList.remove("playing");
}
const jams = new Map();                            // key code -> channel playing
function jamOn(code, note) {
    const ch = jamChannel();
    const t = mem()[S.ins(ui.ins) + L[16]];
    if (t === T.EMPTY) return;
    x.web_post(PC.JAM_ON, ch, note, ui.ins);
    jams.set(code, ch);
}
function jamOff(code) {
    if (!jams.has(code)) return;
    const ch = jams.get(code);
    jams.delete(code);
    if (![...jams.values()].includes(ch)) x.web_post(PC.JAM_OFF, ch, 0, 0);
}
// the channel an instrument plays on, as the firmware's ins_ch_for (ui_pat.c): drums on FM6, a PSG
// instrument with a noise macro on NOI, other PSG ones on the cursor's SQ1-SQ3 (else SQ1), FM patches
// on the cursor's FM channel (else FM1)
function jamChannel() {
    const ch = columns()[ui.col], t = mem()[S.ins(ui.ins) + L[16]];
    if (t === T.DAC) return 5;
    if (t === T.PSG) return mem()[macP(ui.ins, 3)] ? 9 : ch >= 6 && ch <= 8 ? ch : 6;
    if (t === T.FM) return isPsg(ch) ? 0 : ch;
    return ch;
}
const JAM_WHERE = {[T.DAC]: "drums play on FM6", [T.FM]: "FM instruments play on FM1-FM6"};
function jamWhere() {                              // why a note would not sound in the cursor's column
    const t = mem()[S.ins(ui.ins) + L[16]];
    return t === T.PSG ? (mem()[macP(ui.ins, 3)] ? "noise instruments play on NOI" : "tone instruments play on SQ1-SQ3")
                       : JAM_WHERE[t];
}

// ------------------------------------------------------------- the cursor ---
// fields of a column: note | ins hi lo | vol hi lo | fx1 hi lo, value hi lo | fx2 hi lo, value hi lo
const NSUB = 13;
const SUB_X = [0, 4, 5, 7, 8, 10, 11, 12, 13, 15, 16, 17, 18];   // in characters
const COLW = 20;                                   // characters a column takes
function curPat(col = ui.col) { return S.order(ui.ord, columns()[col]); }
function fixCursor() {
    ui.ord = clamp(ui.ord, 0, S.nord - 1);
    ui.row = clamp(ui.row, 0, S.rows - 1);
    ui.col = clamp(ui.col, 0, columns().length - 1);
    ui.ins = clamp(ui.ins, 0, 255);
}
function moveRow(d) {
    let r = ui.row + d;
    if (r < 0) {
        if (ui.ord > 0) { ui.ord--; r += S.rows; } else r = 0;
    } else if (r >= S.rows) {
        if (ui.ord < S.nord - 1) { ui.ord++; r -= S.rows; } else r = S.rows - 1;
    }
    ui.row = clamp(r, 0, S.rows - 1);
    ui.nib = 0;
}
function moveSub(d) {
    let s = ui.sub + d, c = ui.col;
    if (s < 0) { if (c > 0) { c--; s = NSUB - 1; } else s = 0; }
    if (s >= NSUB) { if (c < columns().length - 1) { c++; s = 0; } else s = NSUB - 1; }
    ui.col = c;
    ui.sub = s;
}
const selNorm = () => ui.sel && {
    c0: Math.min(ui.sel.c0, ui.sel.c1), c1: Math.max(ui.sel.c0, ui.sel.c1),
    r0: Math.min(ui.sel.r0, ui.sel.r1), r1: Math.max(ui.sel.r0, ui.sel.r1)};

// --------------------------------------------------------------- editing ---
function writeCell(col, row, f) {                  // f(p, ch): edit the cell at p; allocates the pattern
    const ch = columns()[col], p = cellAt(ch, S.order(ui.ord, ch), row, true);
    if (!p) { say("the song is full: no room for more pattern rows", true); return false; }
    f(p, ch);
    return true;
}
function setNote(note) {
    if (note && note <= NOTE_MAX && mem()[S.ins(ui.ins) + L[16]] !== T.EMPTY && jamChannel() !== columns()[ui.col])
        return say(`not written: ${jamWhere()}`, true);
    change();
    writeCell(ui.col, ui.row, (p, ch) => {
        const m = mem();
        m[p + C.NOTE] = note;
        if (note && note <= NOTE_MAX) {
            const t = m[S.ins(ui.ins) + L[16]];
            if (t !== T.EMPTY) m[p + C.INS] = ui.ins;
        }
    });
    moveRow(ui.step);
}
function typeHex(d) {
    const s = ui.sub;
    if (s === 0) return;
    const field = s <= 2 ? C.INS : s <= 4 ? C.VOL : s <= 6 ? C.FX0 : s <= 8 ? C.FV0 : s <= 10 ? C.FX1 : C.FV1;
    const hi = [1, 3, 5, 7, 9, 11].includes(s);
    change();
    writeCell(ui.col, ui.row, (p, ch) => {
        const m = mem(), isFx = field === C.FX0 || field === C.FX1;
        let v = m[p + field];
        if (isFx) v = v ? v - 1 : 0;                  // stored + 1
        else if (v === NONE) v = 0;
        v = hi ? (d << 4) | (v & 15) : (v & 0xF0) | d;
        if (field === C.VOL) v = Math.min(v, isPsg(ch) ? 15 : 127);
        if (isFx) v = (v + 1) & 255;
        m[p + field] = v;
        if (field === C.INS) ui.ins = v;
    });
    if (hi) ui.sub++;
    else { ui.sub--; moveRow(ui.step); }
}
function clearField() {
    const sn = selNorm();
    change();
    if (sn) { forBlock(sn, p => clearCell(p)); return; }
    const s = ui.sub;
    writeCell(ui.col, ui.row, p => {
        const m = mem();
        if (s === 0) { m[p + C.NOTE] = 0; m[p + C.INS] = NONE; m[p + C.VOL] = NONE; }
        else if (s <= 2) m[p + C.INS] = NONE;
        else if (s <= 4) m[p + C.VOL] = NONE;
        else if (s <= 8) { m[p + C.FX0] = 0; m[p + C.FV0] = 0; }
        else { m[p + C.FX1] = 0; m[p + C.FV1] = 0; }
    });
    moveRow(ui.step);
}
function clearCell(p) {
    const m = mem();
    m[p] = 0; m[p + 1] = NONE; m[p + 2] = NONE; m[p + 3] = 0; m[p + 4] = 0; m[p + 5] = 0; m[p + 6] = 0;
}
function forBlock(sn, f, alloc = false) {
    const cols = columns();
    for (let c = sn.c0; c <= sn.c1; c++) {
        const ch = cols[c], pat = S.order(ui.ord, ch);
        if (!alloc && x.web_pat_empty(ch, pat)) continue;
        for (let r = sn.r0; r <= sn.r1; r++) {
            const p = cellAt(ch, pat, r, alloc);
            if (p) f(p, ch, c - sn.c0, r - sn.r0);
        }
    }
}
const curBlock = () => selNorm() || {c0: ui.col, c1: ui.col, r0: ui.row, r1: ui.row};
function copy(cut) {
    const sn = curBlock(), m = mem();
    clip = {w: sn.c1 - sn.c0 + 1, h: sn.r1 - sn.r0 + 1, cells: []};
    for (let c = 0; c < clip.w; c++) {
        clip.cells.push([]);
        const ch = columns()[sn.c0 + c], pat = S.order(ui.ord, ch);
        for (let r = 0; r < clip.h; r++) {
            const p = cellAt(ch, pat, sn.r0 + r);
            clip.cells[c].push(p ? m.slice(p, p + 7) : Uint8Array.of(0, NONE, NONE, 0, 0, 0, 0));
        }
    }
    if (cut) { change(); forBlock(sn, p => clearCell(p)); }
    say(`${cut ? "cut" : "copied"} ${clip.w} × ${clip.h}`);
}
function paste() {
    if (!clip) return say("nothing copied");
    change();
    const sn = {c0: ui.col, c1: Math.min(columns().length - 1, ui.col + clip.w - 1), r0: ui.row, r1: Math.min(S.rows - 1, ui.row + clip.h - 1)};
    forBlock(sn, (p, ch, c, r) => mem().set(clip.cells[c][r], p), true);
    say(`pasted ${clip.w} × ${clip.h}`);
}
function transpose(d) {
    change();
    forBlock(curBlock(), p => {
        const m = mem(), n = m[p];
        if (n && n <= NOTE_MAX) m[p] = clamp(n + d, 1, NOTE_MAX);
    });
}
function shiftRows(down) {
    change();
    const ch = columns()[ui.col], pat = S.order(ui.ord, ch);
    if (x.web_pat_empty(ch, pat)) return;
    const m = mem(), n = S.rows;
    const cells = [];
    for (let r = 0; r < n; r++) cells.push(m.slice(cellAt(ch, pat, r), cellAt(ch, pat, r) + 7));
    const blank = Uint8Array.of(0, NONE, NONE, 0, 0, 0, 0);
    for (let r = ui.row; r < n; r++) {
        const src = down ? (r === ui.row ? blank : cells[r - 1]) : (r + 1 < n ? cells[r + 1] : blank);
        m.set(src, cellAt(ch, pat, r));
    }
    if (!down) moveRow(-1);
}

// ----------------------------------------------------------- the orders ---
function unusedPattern(ch) {
    const used = new Set();
    for (let o = 0; o < S.nord; o++) used.add(S.order(o, ch));
    for (let p = 0; p < 256; p++) if (!used.has(p) && x.web_pat_empty(ch, p)) return p;
    return -1;
}
function ordInsert(dup) {
    if (S.nord >= L[24]) return say("the order list is full", true);
    change();
    const n = S.nord, o = ui.ord;
    for (let k = n; k > o + 1; k--) for (let ch = 0; ch < L[23]; ch++) S.setOrder(k, ch, S.order(k - 1, ch));
    for (let ch = 0; ch < L[23]; ch++) S.setOrder(o + 1, ch, S.order(o, ch));
    S.nord = n + 1;
    if (S.loop > o) S.loop = S.loop + 1;
    if (dup) {
        for (let ch = 0; ch < L[23]; ch++) {
            const src = S.order(o, ch);
            if (x.web_pat_empty(ch, src)) continue;
            const dst = unusedPattern(ch);
            if (dst < 0) continue;
            for (let r = 0; r < S.rows; r++) {
                const ps = cellAt(ch, src, r), pd = cellAt(ch, dst, r, true);
                if (!pd) { say("the song is full", true); break; }
                mem().copyWithin(pd, ps, ps + 7);
            }
            S.setOrder(o + 1, ch, dst);
        }
    }
    ui.ord = o + 1;
    refreshAll();
}
function ordDelete() {
    if (S.nord <= 1) return;
    change();
    const n = S.nord, o = ui.ord;
    for (let k = o; k < n - 1; k++) for (let ch = 0; ch < L[23]; ch++) S.setOrder(k, ch, S.order(k + 1, ch));
    S.nord = n - 1;
    if (S.loop > o || S.loop >= S.nord) S.loop = Math.max(0, S.loop - 1);
    x.web_compact();
    fixCursor();
    refreshAll();
}
function ordMove(d) {
    const o = ui.ord, t = o + d;
    if (t < 0 || t >= S.nord) return;
    change();
    for (let ch = 0; ch < L[23]; ch++) { const a = S.order(o, ch); S.setOrder(o, ch, S.order(t, ch)); S.setOrder(t, ch, a); }
    ui.ord = t;
    refreshAll();
}

// -------------------------------------------------------------- the song ---
function songToForm() {
    $("s-title").value = S.str(0, 24);
    $("s-author").value = S.str(1, 24);
    $("s-region").value = S.u8(2);
    $("s-chip").value = S.u8(3);
    $("s-filter").value = S.u8(4);
    $("s-ext3").value = S.u8(5);
    $("s-tick").value = S.u16(6) / 10;
    $("s-sp1").value = S.u8(7, 0);
    $("s-sp2").value = S.u8(7, 1);
    $("s-rows").value = S.rows;
    $("s-hl1").value = S.u8(9, 0);
    $("s-hl2").value = S.u8(9, 1);
    $("s-lfo").value = S.u8(10) & 8 ? S.u8(10) & 15 : 0;
}
function formHook(id, apply, reset = false) {
    $(id).addEventListener("change", e => {
        change();
        apply(e.target.value);
        if (reset) x.web_reset();
        songToForm();
        fixCursor();
        refreshAll();
    });
}
formHook("s-title", v => S.setStr(0, 24, v));
formHook("s-author", v => S.setStr(1, 24, v));
formHook("s-region", v => S.set8(2, +v), true);
formHook("s-chip", v => S.set8(3, +v), true);
formHook("s-filter", v => S.set8(4, +v), true);
formHook("s-ext3", v => S.set8(5, +v), true);
formHook("s-tick", v => S.set16(6, clamp(Math.round(+v * 10), 0, 3000)), true);
formHook("s-sp1", v => S.set8(7, clamp(+v, 1, 31), 0));
formHook("s-sp2", v => S.set8(7, clamp(+v, 1, 31), 1));
formHook("s-rows", v => { if (x.web_set_rows(clamp(+v, 1, 128))) say("no room for longer patterns", true); });
formHook("s-hl1", v => S.set8(9, clamp(+v, 0, 64), 0));
formHook("s-hl2", v => S.set8(9, clamp(+v, 0, 128), 1));
formHook("s-lfo", v => S.set8(10, +v), true);

// ---------------------------------------------------------- instruments ---
const insType = i => mem()[S.ins(i) + L[16]];
const insName = i => { const m = mem(), p = S.ins(i) + L[17]; let s = ""; for (let k = 0; k < 15 && m[p + k]; k++) s += String.fromCharCode(m[p + k]); return s; };
const fmP = i => S.ins(i) + L[18];                 // alg, fb, ams, pms, then the operators
const opP = (i, op) => fmP(i) + L[20] + op * L[21];
const macP = (i, k) => S.ins(i) + L[19] + k * L[22];
const OPF = [                                      // fm_op_t: ar d1r d2r rr sl tl ks mul dt am ssg
    ["AR", 0, 31], ["D1R", 1, 31], ["D1L", 4, 15], ["D2R", 2, 31], ["RR", 3, 15], ["TL", 5, 127],
    ["MUL", 7, 15], ["DT", 8, 7], ["RS", 6, 3], ["AM", 9, 1], ["SSG", 10, 8]];
const ALG_POS = [[[0, 1], [1, 1], [2, 1], [3, 1]], [[0, 0], [0, 2], [1, 1], [2, 1]], [[1, 0], [0, 2], [1, 2], [2, 1]],
    [[0, 0], [1, 0], [1, 2], [2, 1]], [[0, 0], [1, 0], [0, 2], [1, 2]], [[0, 1], [1, 0], [1, 1], [1, 2]],
    [[0, 0], [1, 0], [1, 1], [1, 2]], [[0, 1], [1, 1], [2, 1], [3, 1]]];
const ALG_EDGE = [[[1, 2], [2, 3], [3, 4]], [[1, 3], [2, 3], [3, 4]], [[2, 3], [1, 4], [3, 4]], [[1, 2], [2, 4], [3, 4]],
    [[1, 2], [3, 4]], [[1, 2], [1, 3], [1, 4]], [[1, 2]], []];
function algSvg(a) {
    const P = ALG_POS[a].map(([cx, cy]) => [6 + cx * 13, 6 + cy * 10]);
    let s = `<svg viewBox="0 0 60 30" aria-hidden="true">`;
    for (const [p, q] of ALG_EDGE[a]) s += `<line x1="${P[p - 1][0] + 4}" y1="${P[p - 1][1]}" x2="${P[q - 1][0] - 4}" y2="${P[q - 1][1]}" stroke="#a3a6c8" stroke-width="1"/>`;
    for (let k = 0; k < 4; k++) {
        const car = x.web_is_carrier(a, k);
        if (car) s += `<line x1="${P[k][0] + 4}" y1="${P[k][1]}" x2="58" y2="${P[k][1]}" stroke="#ff6ac1" stroke-width="1"/>`;
        s += `<rect x="${P[k][0] - 4}" y="${P[k][1] - 3}" width="8" height="6" rx="1" fill="${car ? "#ff6ac1" : "#4fd8ff"}"/>`;
    }
    return s + `<line x1="58" y1="2" x2="58" y2="28" stroke="#ff6ac1" stroke-width="1.5"/></svg>`;
}
function buildFm() {
    $("algs").innerHTML = "";
    for (let a = 0; a < 8; a++) {
        const b = document.createElement("button");
        b.type = "button";
        b.title = `Algorithm ${a}`;
        b.innerHTML = algSvg(a);
        b.onclick = () => { change(); mem()[fmP(ui.ins)] = a; x.web_ins_changed(ui.ins); refreshIns(); };
        $("algs").append(b);
    }
    for (const [id, off] of [["fm-fb", 1], ["fm-ams", 2], ["fm-pms", 3]]) {
        const el = $(id);
        el.oninput = () => { if (!el.dataset.busy) { el.dataset.busy = 1; change(); } mem()[fmP(ui.ins) + off] = +el.value; el.nextElementSibling.value = el.value; x.web_ins_changed(ui.ins); };
        el.onchange = () => { delete el.dataset.busy; };
    }
    let h = `<tr><th></th>${[1, 2, 3, 4].map(k => `<th id="op-h${k - 1}">OP${k}</th>`).join("")}</tr>`;
    for (const [name, off, max] of OPF)
        h += `<tr><td class="n">${name}</td>${[0, 1, 2, 3].map(op => `<td><div class="knob"><input type="range" min="0" max="${max}" data-op="${op}" data-off="${off}"><output></output></div></td>`).join("")}</tr>`;
    h += `<tr><td></td>${[0, 1, 2, 3].map(op => `<td><canvas class="env" data-env="${op}"></canvas></td>`).join("")}</tr>`;
    $("ops").innerHTML = h;
    for (const el of $("ops").querySelectorAll("input")) {
        el.oninput = () => {
            if (!el.dataset.busy) { el.dataset.busy = 1; change(); }
            const op = +el.dataset.op, off = +el.dataset.off;
            let v = +el.value;
            if (off === 10) v = v ? 7 + v : 0;               // SSG-EG: off, or 8 + mode
            mem()[opP(ui.ins, op) + off] = v;
            el.nextElementSibling.value = opText(off, v);
            x.web_ins_changed(ui.ins);
            drawEnvs();
        };
        el.onchange = () => { delete el.dataset.busy; };
    }
}
function opText(off, v) {
    if (off === 8) return ["0", "+1", "+2", "+3", "0", "-1", "-2", "-3"][v & 7];
    if (off === 10) return v ? String(v - 8) : "off";
    if (off === 9) return v ? "on" : "off";
    return String(v);
}
function drawEnvs() {
    for (const cv of $("ops").querySelectorAll("canvas.env")) {
        const op = +cv.dataset.env, p = opP(ui.ins, op), m = mem();
        const ar = m[p], d1r = m[p + 1], d2r = m[p + 2], rr = m[p + 3], sl = m[p + 4], tl = m[p + 5];
        const w = cv.width = cv.clientWidth * devicePixelRatio, h = cv.height = cv.clientHeight * devicePixelRatio;
        const g = cv.getContext("2d"), car = x.web_is_carrier(mem()[fmP(ui.ins)], op);
        const top = h * (0.08 + 0.85 * tl / 127), floor = h - 2, slv = top + (floor - top) * (sl / 15);
        const ta = ar ? 0.25 * (31 - ar) / 31 + 0.02 : 1, td = d1r ? 0.25 * (31 - d1r) / 31 + 0.03 : 1;
        const xa = w * Math.min(ta, .3), xd = xa + w * Math.min(td, .3), xr = w * 0.75;
        const yr = d2r ? slv + (floor - slv) * Math.min(1, (xr - xd) / w * d2r / 8) : slv;
        g.strokeStyle = car ? "#ff6ac1" : "#4fd8ff";
        g.lineWidth = 1.5 * devicePixelRatio;
        g.shadowColor = g.strokeStyle;
        g.shadowBlur = 6;
        g.beginPath();
        g.moveTo(0, floor);
        g.lineTo(xa, ar ? top : floor);
        g.lineTo(xd, ar ? slv : floor);
        g.lineTo(xr, ar ? yr : floor);
        g.lineTo(xr + w * 0.25 * (rr ? (16 - rr) / 15 : 1), floor);
        g.stroke();
    }
}
function refreshIns() {
    const i = ui.ins, t = insType(i);
    $("ins-name").value = insName(i);
    $("ins-type").value = t;
    $("p-fm").hidden = t !== T.FM;
    $("p-dac").hidden = t !== T.DAC;
    $("p-mac").hidden = t !== T.FM && t !== T.PSG;
    if (t === T.FM) {
        const m = mem(), alg = m[fmP(i)];
        [...$("algs").children].forEach((b, a) => b.classList.toggle("on", a === alg));
        for (const [id, off] of [["fm-fb", 1], ["fm-ams", 2], ["fm-pms", 3]]) { $(id).value = m[fmP(i) + off]; $(id).nextElementSibling.value = m[fmP(i) + off]; }
        for (const el of $("ops").querySelectorAll("input")) {
            const v = m[opP(i, +el.dataset.op) + +el.dataset.off], off = +el.dataset.off;
            el.value = off === 10 ? (v ? v - 7 : 0) : v;
            el.nextElementSibling.value = opText(off, v);
        }
        for (let k = 0; k < 4; k++) $(`op-h${k}`).classList.toggle("car", !!x.web_is_carrier(alg, k));
        drawEnvs();
    }
    if (t === T.DAC) {
        const p = fmP(i), m = mem();
        $("dac-s").value = m[p];
        $("dac-root").value = m[p + 1];
        $("dac-loop").value = m[p + 2];
        $("dac-pitched").value = m[p + 3];
    }
    if (t === T.FM || t === T.PSG) {
        const avail = t === T.FM ? [1, 2] : [0, 1, 2, 3];
        if (!avail.includes(ui.mac)) ui.mac = avail[0];
        $("mac-tabs").innerHTML = "";
        for (const k of avail) {
            const b = document.createElement("button");
            b.type = "button";
            b.textContent = MAC[k] + (mem()[macP(i, k)] ? " ●" : "");
            b.className = k === ui.mac ? "on" : "";
            b.onclick = () => { ui.mac = k; refreshIns(); };
            $("mac-tabs").append(b);
        }
        const mp = macP(i, ui.mac), m = mem();
        $("mac-len").value = m[mp];
        $("mac-loop").value = m[mp + 1] === 255 ? -1 : m[mp + 1];
        $("mac-rel").value = m[mp + 2] === 255 ? -1 : m[mp + 2];
        drawMacro();
    }
    refreshInsList();
}
function refreshInsList() {
    const box = $("inslist"), m = mem();
    let h = "", n = 0;
    for (let i = 0; i < 256; i++) {
        const t = m[S.ins(i) + L[16]];
        if (t) n++;
        if (!t && i !== ui.ins && i > lastUsedIns() + 1) continue;
        h += `<div data-i="${i}" class="${i === ui.ins ? "cur" : ""}"><i>${hex2(i)}</i><span class="t-${t}">${t ? ["", "FM ", "PSG", "DAC"][t] : "   "}</span> ${esc(insName(i)) || (t ? "" : "<span class=tiny>empty</span>")}</div>`;
    }
    box.innerHTML = h;
    $("ins-n").textContent = `${n} in use`;
    const cur = box.querySelector(".cur");
    if (cur) cur.scrollIntoView({block: "nearest"});
}
const lastUsedIns = () => { const m = mem(); for (let i = 255; i >= 0; i--) if (m[S.ins(i) + L[16]]) return i; return -1; };
$("inslist").addEventListener("click", e => {
    const d = e.target.closest("[data-i]");
    if (d) { ui.ins = +d.dataset.i; refreshIns(); }
});
$("ins-name").addEventListener("change", e => {
    change();
    const p = S.ins(ui.ins) + L[17], s = e.target.value, m = mem();
    for (let k = 0; k < 15; k++) m[p + k] = k < 14 && k < s.length ? s.charCodeAt(k) & 127 : 0;
    refreshInsList();
});
$("ins-type").addEventListener("change", e => {
    change();
    x.web_ins_init(ui.ins, +e.target.value);
    ui.mac = +e.target.value === T.PSG ? 0 : 1;
    refreshIns();
});
let presets = [];
function buildPresets() {
    const keep = snapshotIns(255);
    presets = [];
    const groups = [["FM presets", 0], ["PSG presets", 1], ["DAC kit", 2]];
    let h = `<option value="">Load a preset…</option>`;
    for (const [label, kind] of groups) {
        h += `<optgroup label="${label}">`;
        for (let n = 0; n < x.web_preset_count(kind); n++) {
            x.web_preset(255, kind, n);
            h += `<option value="${kind}:${n}">${esc(insName(255))}</option>`;
        }
        h += `</optgroup>`;
    }
    restoreIns(255, keep);
    $("ins-preset").innerHTML = h;
    let s = "";
    for (let k = 0; k < x.web_sample_count(); k++) s += `<option value="${k}">${esc(cstr(x.web_sample_name(k)))}</option>`;
    $("dac-s").innerHTML = s;
}
function snapshotIns(i) { const n = x.web_ins_write(i); return new Uint8Array(x.memory.buffer, x.web_io(), n).slice(); }
function restoreIns(i, b) { new Uint8Array(x.memory.buffer, x.web_io(), b.length).set(b); return x.web_ins_read(i, b.length); }
$("ins-preset").addEventListener("change", e => {
    if (!e.target.value) return;
    const [kind, n] = e.target.value.split(":").map(Number);
    change();
    x.web_preset(ui.ins, kind, n);
    e.target.value = "";
    refreshIns();
});
for (const [id, off] of [["dac-s", 0], ["dac-root", 1], ["dac-loop", 2], ["dac-pitched", 3]])
    $(id).addEventListener("change", e => {
        change();
        mem()[fmP(ui.ins) + off] = off === 1 ? clamp(+e.target.value, 1, 96) : +e.target.value;
        x.web_ins_changed(ui.ins);
        refreshIns();
    });
$("ins-copy").onclick = () => { insClip = snapshotIns(ui.ins); say("instrument copied"); };
$("ins-paste").onclick = () => { if (!insClip) return; change(); restoreIns(ui.ins, insClip); refreshIns(); };
$("ins-save").onclick = () => {
    const b = snapshotIns(ui.ins);
    download(new Blob([new TextEncoder().encode("FDI1"), b]), `${insName(ui.ins) || "instrument"}.fm1i`);
};
$("ins-load").onclick = () => $("f-ins").click();
$("f-ins").addEventListener("change", async e => {
    const f = e.target.files[0];
    e.target.value = "";
    if (!f) return;
    const b = new Uint8Array(await f.arrayBuffer());
    if (String.fromCharCode(...b.subarray(0, 4)) !== "FDI1") return say(`${f.name}: not an FM1-TRAX instrument`, true);
    change();
    if (restoreIns(ui.ins, b.subarray(4))) say(`${f.name}: damaged`, true);
    refreshIns();
});

// macros: drawn with the mouse, a bar a step
function drawMacro() {
    const cv = $("mac"), w = cv.width = cv.clientWidth * devicePixelRatio, h = cv.height = cv.clientHeight * devicePixelRatio;
    const g = cv.getContext("2d"), m = mem(), p = macP(ui.ins, ui.mac), len = m[p], loop = m[p + 1], rel = m[p + 2];
    const lo = MAC_LO[ui.mac], hi = MAC_HI[ui.mac], bw = w / 32, y0 = h - (0 - lo) / (hi - lo) * h;
    g.fillStyle = "#04030a";
    g.fillRect(0, 0, w, h);
    g.strokeStyle = "#2c2a52";
    g.beginPath();
    g.moveTo(0, y0);
    g.lineTo(w, y0);
    g.stroke();
    for (let s = 0; s < 32; s++) {
        const v = (m[p + 3 + s] << 24 >> 24), y = h - (v - lo) / (hi - lo) * h;
        const on = s < len, grad = g.createLinearGradient(0, 0, 0, h);
        grad.addColorStop(0, on ? "#ff6ac1" : "#2c2a52");
        grad.addColorStop(1, on ? "#8a5cff" : "#1c1940");
        g.fillStyle = grad;
        g.fillRect(s * bw + 1, Math.min(y, y0), bw - 2, Math.max(2, Math.abs(y0 - y)));
    }
    g.fillStyle = "#ffb43c";
    if (loop !== 255 && loop < 32) g.fillRect(loop * bw, 0, 2, h);
    g.fillStyle = "#54e08a";
    if (rel !== 255 && rel < 32) g.fillRect(rel * bw, 0, 2, h);
    g.fillStyle = "#a3a6c8";
    g.font = `${11 * devicePixelRatio}px IBM Plex Mono, monospace`;
    g.fillText(`${MAC[ui.mac]} ${lo}..${hi}${len ? "" : "  (off: draw to start it)"}`, 6, 14 * devicePixelRatio);
}
{
    const cv = $("mac");
    let drawing = false;
    const at = e => {
        const r = cv.getBoundingClientRect(), s = clamp(Math.floor((e.clientX - r.left) / r.width * 32), 0, 31);
        const lo = MAC_LO[ui.mac], hi = MAC_HI[ui.mac], v = clamp(Math.round(lo + (1 - (e.clientY - r.top) / r.height) * (hi - lo)), lo, hi);
        const m = mem(), p = macP(ui.ins, ui.mac);
        if (m[p] <= s) {                            // drawing past the end makes it longer
            for (let k = m[p]; k < s; k++) m[p + 3 + k] = (k ? m[p + 2 + k] : (ui.mac === 0 ? 15 : 0)) & 255;
            m[p] = s + 1;
        }
        m[p + 3 + s] = v & 255;
        x.web_ins_changed(ui.ins);
        drawMacro();
    };
    cv.addEventListener("pointerdown", e => { drawing = true; cv.setPointerCapture(e.pointerId); change(); at(e); });
    cv.addEventListener("pointermove", e => { if (drawing) at(e); });
    cv.addEventListener("pointerup", () => { drawing = false; refreshIns(); });
}
for (const [id, off] of [["mac-len", 0], ["mac-loop", 1], ["mac-rel", 2]])
    $(id).addEventListener("change", e => {
        change();
        let v = Math.round(+e.target.value);
        v = off === 0 ? clamp(v, 0, 32) : v < 0 ? 255 : clamp(v, 0, 31);
        mem()[macP(ui.ins, ui.mac) + off] = v;
        x.web_ins_changed(ui.ins);
        refreshIns();
    });

// -------------------------------------------------------------- drawing ---
const cv = $("pat"), g = cv.getContext("2d");
let geo = null;                                    // the last frame's layout, for the mouse
const FONT = "IBM Plex Mono";
function drawPattern() {
    const dpr = devicePixelRatio || 1, W = cv.clientWidth, H = cv.clientHeight;
    if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.font = `500 12px ${FONT}, monospace`;
    const cw = Math.ceil(g.measureText("0").width * 100) / 100, rh = 15, head = 46, numW = cw * 3 + 8;
    const colW = cw * COLW, cols = columns(), nvis = Math.max(1, Math.floor((W - numW) / colW));
    let first = geo ? geo.first : 0;
    if (ui.col < first) first = ui.col;
    if (ui.col >= first + nvis) first = ui.col - nvis + 1;
    first = clamp(first, 0, Math.max(0, cols.length - nvis));
    const nrows = Math.floor((H - head) / rh), mid = Math.floor(nrows / 2), rows = S.rows, m = mem();
    const pos = x.web_pos(), pOrd = pos >>> 16, pRow = pos & 0xFFFF, playing = !!x.web_playing();
    const hl1 = S.u8(9, 0), hl2 = S.u8(9, 1), sn = selNorm();
    geo = {first, cw, rh, head, numW, colW, mid, nvis};
    g.fillStyle = "#0b0a18";
    g.fillRect(0, 0, W, H);
    // the headers: name, level, scope
    const so = x.web_scope_pos();
    for (let i = 0; i < nvis && first + i < cols.length; i++) {
        const ch = cols[first + i], x0 = numW + i * colW, col = chColor(ch), muted = ui.mute >> ch & 1;
        const lv = x.web_ch_level(ch);
        g.fillStyle = "#141230";
        g.fillRect(x0 + 1, 2, colW - 3, head - 6);
        g.fillStyle = muted ? "#2c2a52" : col;
        g.globalAlpha = muted ? 1 : 0.25 + lv / 20;
        g.fillRect(x0 + 1, 2, colW - 3, 2);
        g.globalAlpha = 1;
        g.fillStyle = muted ? "#6b6e94" : col;
        g.fillText(chName(ch) + (muted ? "  MUTE" : ""), x0 + 6, 17);
        for (let k = 0; k < 8; k++) {                 // the level, as LEDs
            g.fillStyle = lv > k * 2 ? (k > 5 ? "#ff5d73" : col) : "#2c2a52";
            g.fillRect(x0 + colW - 10 - (7 - k) * 7, 9, 5, 7);
        }
        const sc = ch >= 10 ? 2 : ch, sp = x.web_scope(sc);
        if (sp) {
            const s = new Int8Array(x.memory.buffer, sp, 128), y0 = 32, sw = colW - 12;
            g.strokeStyle = muted ? "#2c2a52" : col;
            g.lineWidth = 1.2;
            g.shadowColor = col;
            g.shadowBlur = muted ? 0 : 6;
            g.beginPath();
            for (let k = 0; k < sw; k++) {
                const v = s[(so + Math.floor(k * 128 / sw)) & 127];
                const y = y0 + clamp(v * 10 / 64, -9, 9);
                if (k) g.lineTo(x0 + 6 + k, y); else g.moveTo(x0 + 6, y);
            }
            g.stroke();
            g.shadowBlur = 0;
        }
    }
    // the rows: the cursor's row in the middle, the positions before and after above and below
    for (let i = 0; i < nrows; i++) {
        let o = ui.ord, r = ui.row + i - mid;
        while (r < 0 && o > 0) { o--; r += rows; }
        while (r >= rows && o < S.nord - 1) { o++; r -= rows; }
        if (r < 0 || r >= rows) continue;
        const y = head + i * rh, other = o !== ui.ord;
        let bg = null;
        if (hl2 && r % hl2 === 0) bg = "#1c1940";
        else if (hl1 && r % hl1 === 0) bg = "#141230";
        if (i === mid) bg = ui.edit ? "#3a1430" : "#2a2260";
        if (playing && o === pOrd && r === pRow) bg = "#1d4a3a";
        if (bg) { g.fillStyle = bg; g.fillRect(0, y, W, rh); }
        g.globalAlpha = other ? 0.4 : 1;
        g.fillStyle = hl1 && r % hl1 === 0 ? "#eef0ff" : "#6b6e94";
        g.fillText(hex2(r), 4, y + 12);
        for (let j = 0; j < nvis && first + j < cols.length; j++) {
            const c = first + j, ch = cols[c], x0 = numW + j * colW, p = cellAt(ch, S.order(o, ch), r);
            if (sn && !other && c >= sn.c0 && c <= sn.c1 && r >= sn.r0 && r <= sn.r1) {
                g.fillStyle = "#5a2a7a";
                g.fillRect(x0, y, colW - 2, rh);
            }
            drawCell(p ? m.subarray(p, p + 7) : null, x0 + 6, y + 12, ch, cw, playing && o === pOrd && r === pRow);
        }
        g.globalAlpha = 1;
    }
    // the cursor
    const cx = numW + (ui.col - first) * colW + 6 + SUB_X[ui.sub] * cw, cy = head + mid * rh;
    const t = performance.now() / 1000, hue = (t * 120) % 360;
    g.strokeStyle = `hsl(${hue} 100% 65%)`;
    g.lineWidth = 1.5;
    g.shadowColor = g.strokeStyle;
    g.shadowBlur = 8;
    g.strokeRect(cx - 2, cy + 0.5, (ui.sub ? cw : cw * 3) + 4, rh - 1);
    g.shadowBlur = 0;
    if (ui.edit) { g.strokeStyle = "#ff3da5"; g.lineWidth = 2; g.strokeRect(1, 1, W - 2, H - 2); }
    // the scroll hints
    g.fillStyle = "#6b6e94";
    if (first > 0) g.fillText("◀", numW - 14, 17);
    if (first + nvis < cols.length) g.fillText("▶", W - 14, 17);
}
const FXC = "#ffcf4a";
function drawCell(c, x0, y, ch, cw, lit) {
    const empty = "#3a3866";
    let s, col;
    const note = c ? c[0] : 0;
    if (note === NOTE_OFF) { s = "==="; col = "#eef0ff"; }
    else if (note === NOTE_CUT) { s = "^^^"; col = "#eef0ff"; }
    else if (note && note <= NOTE_MAX) { s = cstr(x.web_note_name(note)); col = lit ? "#ffffff" : chColor(ch); }
    else { s = "..."; col = empty; }
    if (lit && note && note <= NOTE_MAX) { g.shadowColor = col; g.shadowBlur = 10; }
    g.fillStyle = col;
    g.fillText(s, x0, y);
    g.shadowBlur = 0;
    const ins = c ? c[1] : NONE, vol = c ? c[2] : NONE;
    g.fillStyle = ins !== NONE ? "#ffb43c" : empty;
    g.fillText(ins !== NONE ? hex2(ins) : "..", x0 + 4 * cw, y);
    g.fillStyle = vol !== NONE ? "#54e08a" : empty;
    g.fillText(vol !== NONE ? hex2(vol) : "..", x0 + 7 * cw, y);
    for (let k = 0; k < 2; k++) {
        const fx = c ? c[3 + k] : 0, fv = c ? c[5 + k] : 0, xo = x0 + (10 + k * 5) * cw;
        if (fx) {
            g.fillStyle = cstr(x.web_fx_name(fx)) ? FXC : "#ff5d73";
            g.fillText(hex2(fx - 1), xo, y);
            g.fillStyle = "#eef0ff";
            g.fillText(hex2(fv), xo + 2 * cw, y);
        } else {
            g.fillStyle = empty;
            g.fillText("....", xo, y);
        }
    }
}
cv.addEventListener("mousedown", e => {
    if (!geo) return;
    cv.focus();
    const r = cv.getBoundingClientRect(), mx = e.clientX - r.left, my = e.clientY - r.top;
    const c = geo.first + Math.floor((mx - geo.numW) / geo.colW);
    if (c < 0 || c >= columns().length) return;
    if (my < geo.head) {                            // a header: mute; Shift: solo
        const ch = columns()[c], all = (1 << 13) - 1;
        ui.mute = e.shiftKey ? (ui.mute === (all & ~(1 << ch)) ? 0 : all & ~(1 << ch)) : ui.mute ^ (1 << ch);
        x.web_set_mute(ui.mute);
        return;
    }
    const cx = mx - geo.numW - (c - geo.first) * geo.colW - 6, ch8 = cx / geo.cw;
    let sub = 0;
    for (let k = 0; k < NSUB; k++) if (ch8 >= SUB_X[k] - 0.5) sub = k;
    const dr = Math.floor((my - geo.head) / geo.rh) - geo.mid;
    if (e.shiftKey) {
        if (!ui.sel) ui.sel = {c0: ui.col, r0: ui.row, c1: ui.col, r1: ui.row};
        ui.sel.c1 = c;
        ui.sel.r1 = clamp(ui.row + dr, 0, S.rows - 1);
        return;
    }
    ui.sel = null;
    ui.col = c;
    ui.sub = sub;
    moveRow(dr);
    ui.nib = 0;
});
cv.addEventListener("wheel", e => {
    e.preventDefault();
    if (e.shiftKey) { ui.col = clamp(ui.col + Math.sign(e.deltaY), 0, columns().length - 1); return; }
    moveRow(Math.sign(e.deltaY) * (Math.abs(e.deltaY) > 50 ? 4 : 1));
}, {passive: false});

// the orders, as a table
function drawOrders() {
    const cols = columns(), m = mem(), pos = x.web_pos() >>> 16, playing = x.web_playing();
    let h = `<tr><th></th>${cols.map(ch => `<th style="color:${chColor(ch)}">${chName(ch)}</th>`).join("")}</tr>`;
    for (let o = 0; o < S.nord; o++) {
        const cls = [o === ui.ord ? "cur" : "", playing && o === pos ? "play" : "", o === S.loop ? "loop" : ""].join(" ");
        h += `<tr class="${cls}" data-o="${o}"><td class="pos">${hex2(o)}</td>`;
        for (let c = 0; c < cols.length; c++) {
            const p = S.order(o, cols[c]);
            h += `<td data-c="${c}" class="${x.web_pat_empty(cols[c], p) ? "empty" : ""}${o === ui.ord && c === ui.ordSel ? " sel" : ""}">${hex2(p)}</td>`;
        }
        h += "</tr>";
    }
    $("ord").innerHTML = h;
    $("ord-n").textContent = `${S.nord} positions, loop ${hex2(S.loop)}`;
    const cur = $("ord").querySelector("tr.cur");
    if (cur) cur.scrollIntoView({block: "nearest"});
    void m;
}
$("ord").addEventListener("click", e => {
    const tr = e.target.closest("tr[data-o]");
    if (!tr) return;
    ui.ord = +tr.dataset.o;
    const td = e.target.closest("td[data-c]");
    if (td) { ui.ordSel = +td.dataset.c; ui.col = ui.ordSel; }
    ui.ordNib = 0;
    ordFocus = true;
    drawOrders();
});
let ordFocus = false;
cv.addEventListener("focus", () => { ordFocus = false; });
$("o-ins").onclick = () => ordInsert(false);
$("o-dup").onclick = () => ordInsert(true);
$("o-del").onclick = () => ordDelete();
$("o-up").onclick = () => ordMove(-1);
$("o-down").onclick = () => ordMove(1);
$("o-loop").onclick = () => { change(); S.loop = ui.ord; drawOrders(); };

// ------------------------------------------------------------- keyboard ---
const LOW = ["KeyZ", "KeyS", "KeyX", "KeyD", "KeyC", "KeyV", "KeyG", "KeyB", "KeyH", "KeyN", "KeyJ", "KeyM",
    "Comma", "KeyL", "Period", "Semicolon", "Slash"];
const HIGH = ["KeyQ", "Digit2", "KeyW", "Digit3", "KeyE", "KeyR", "Digit5", "KeyT", "Digit6", "KeyY", "Digit7", "KeyU",
    "KeyI", "Digit9", "KeyO", "Digit0", "KeyP"];
const noteOf = code => {
    let k = LOW.indexOf(code), o = ui.oct;
    if (k < 0) { k = HIGH.indexOf(code); o++; }
    if (k < 0) return 0;
    const n = o * 12 + k + 1;
    return n >= 1 && n <= NOTE_MAX ? n : 0;
};
const typing = e => /^(INPUT|SELECT|TEXTAREA|BUTTON)$/.test(e.target.tagName) && e.target.type !== "range";
addEventListener("keydown", e => {
    if ($("help").open) return;
    if (typing(e)) return;
    const ctrl = e.ctrlKey || e.metaKey, k = e.key;
    if (ctrl && (k === "z" || k === "Z")) { e.preventDefault(); doUndo(e.shiftKey); return; }
    if (ctrl && (k === "y" || k === "Y")) { e.preventDefault(); doUndo(true); return; }
    if (k === "Enter") { e.preventDefault(); if (x.web_playing()) stop(); else play(e.shiftKey); return; }
    if (k === "Escape") { stop(); ui.sel = null; return; }
    if (e.code === "Space") { e.preventDefault(); setEdit(!ui.edit); return; }
    if (k === "-" || e.code === "NumpadSubtract") { setOct(ui.oct - 1); e.preventDefault(); return; }
    if (k === "=" || e.code === "NumpadAdd") { setOct(ui.oct + 1); e.preventDefault(); return; }
    if (k === "[" || k === "]") { ui.ins = clamp(ui.ins + (k === "]" ? 1 : -1), 0, 255); refreshIns(); return; }
    if (ordFocus && !ctrl && /^[0-9a-fA-F]$/.test(k)) {           // a pattern number in the orders
        change();
        const ch = columns()[ui.ordSel], v = S.order(ui.ord, ch), d = parseInt(k, 16);
        S.setOrder(ui.ord, ch, ui.ordNib ? (v & 0xF0) | d : (d << 4) | (v & 15));
        ui.ordNib ^= 1;
        drawOrders();
        return;
    }
    const nav = {ArrowUp: [-1, 0], ArrowDown: [1, 0], ArrowLeft: [0, -1], ArrowRight: [0, 1], PageUp: [-16, 0], PageDown: [16, 0]}[k];
    if (nav && ctrl && (k === "ArrowUp" || k === "ArrowDown")) { e.preventDefault(); transpose((k === "ArrowUp" ? 1 : -1) * (e.shiftKey ? 12 : 1)); return; }
    if (nav && ctrl && (k === "ArrowLeft" || k === "ArrowRight")) { e.preventDefault(); ui.ord = clamp(ui.ord + nav[1], 0, S.nord - 1); return; }
    if (nav) {
        e.preventDefault();
        ordFocus = false;
        if (e.shiftKey) {
            if (!ui.sel) ui.sel = {c0: ui.col, r0: ui.row, c1: ui.col, r1: ui.row};
            ui.sel.r1 = clamp(ui.sel.r1 + nav[0], 0, S.rows - 1);
            ui.sel.c1 = clamp(ui.sel.c1 + nav[1], 0, columns().length - 1);
            return;
        }
        ui.sel = null;
        if (nav[0]) moveRow(nav[0]); else moveSub(nav[1]);
        return;
    }
    if (k === "Tab") { e.preventDefault(); ui.col = clamp(ui.col + (e.shiftKey ? -1 : 1), 0, columns().length - 1); ui.sub = 0; return; }
    if (k === "Home") { e.preventDefault(); ui.row = 0; return; }
    if (k === "End") { e.preventDefault(); ui.row = S.rows - 1; return; }
    if (ctrl && (k === "a" || k === "A")) {
        e.preventDefault();
        const whole = ui.sel && ui.sel.r0 === 0 && ui.sel.r1 === S.rows - 1 && ui.sel.c0 === ui.col && ui.sel.c1 === ui.col;
        ui.sel = whole ? {c0: 0, c1: columns().length - 1, r0: 0, r1: S.rows - 1} : {c0: ui.col, c1: ui.col, r0: 0, r1: S.rows - 1};
        return;
    }
    if (ctrl && (k === "c" || k === "C")) { e.preventDefault(); copy(false); return; }
    if (ctrl && (k === "x" || k === "X")) { e.preventDefault(); if (ui.edit) copy(true); return; }
    if (ctrl && (k === "v" || k === "V")) { e.preventDefault(); if (ui.edit) paste(); else say("edit mode is off (Space)"); return; }
    if (ctrl) return;
    if (e.repeat) { if (ui.sub === 0 && noteOf(e.code)) e.preventDefault(); return; }
    if (ui.sub === 0 || !ui.edit) {
        const n = noteOf(e.code);
        if (n) {
            e.preventDefault();
            jamOn(e.code, n);
            if (ui.edit && ui.sub === 0) setNote(n);
            return;
        }
    }
    if (!ui.edit) return;
    if (k === "Delete") { e.preventDefault(); clearField(); return; }
    if (k === "Insert") { e.preventDefault(); shiftRows(true); return; }
    if (k === "Backspace") { e.preventDefault(); shiftRows(false); return; }
    if (ui.sub === 0 && e.code === "Digit1") { e.preventDefault(); setNote(NOTE_OFF); return; }
    if (ui.sub === 0 && e.code === "Backquote") { e.preventDefault(); setNote(NOTE_CUT); return; }
    if (ui.sub > 0 && /^[0-9a-fA-F]$/.test(k)) { e.preventDefault(); typeHex(parseInt(k, 16)); }
});
addEventListener("keyup", e => jamOff(e.code));
addEventListener("blur", () => { for (const c of [...jams.keys()]) jamOff(c); });
function setEdit(on) { ui.edit = on; $("b-edit").classList.toggle("on", on); }
function setOct(o) { ui.oct = clamp(o, 0, 7); $("i-oct").value = ui.oct; }
$("i-oct").addEventListener("change", e => setOct(+e.target.value));
$("i-step").addEventListener("change", e => { ui.step = clamp(+e.target.value, 0, 16); e.target.value = ui.step; });
$("b-edit").onclick = () => setEdit(!ui.edit);
$("b-follow").onclick = () => { ui.follow = !ui.follow; $("b-follow").classList.toggle("on", ui.follow); };
$("b-play").onclick = () => play(false);
$("b-loop").onclick = () => play(true);
$("b-stop").onclick = () => stop();
$("b-help").onclick = () => $("help").showModal();

// --------------------------------------------------------------- files ---
function download(blob, name) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name.replace(/[\\/:*?"<>|]+/g, "_");
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
}
const songName = () => (S.str(0, 24).trim() || "untitled");
$("b-new").onclick = () => { change(); stop(); x.web_song_new(); afterLoad("a new song"); };
$("b-demo").onclick = () => { change(); stop(); x.web_song_demo(); afterLoad("the demo song"); };
$("b-open").onclick = () => $("f-song").click();
$("f-song").addEventListener("change", async e => {
    const f = e.target.files[0];
    e.target.value = "";
    if (!f) return;
    const b = new Uint8Array(await f.arrayBuffer());
    if (isVgm(b)) return importFile(f.name, b);
    change();
    stop();
    const rc = loadBytes(b);
    afterLoad(rc ? `${f.name}: not an FM1-TRAX song (a new one instead)` : f.name, !!rc);
});
$("b-import").onclick = () => $("f-vgm").click();
$("f-vgm").addEventListener("change", async e => {
    const f = e.target.files[0];
    e.target.value = "";
    if (f) importFile(f.name, new Uint8Array(await f.arrayBuffer()));
});

// a Mega Drive VGM as a song (web/vgmimport.js), the DAC kit from the engine
const isVgm = b => (b[0] === 0x1F && b[1] === 0x8B) || String.fromCharCode(...b.subarray(0, 4)) === "Vgm ";
let kit = null;
const theKit = () => kit ||= Array.from({length: x.web_sample_count()}, (_, i) => ({
    name: cstr(x.web_sample_name(i)),
    data: new Uint8Array(x.memory.buffer, x.web_sample_data(i), x.web_sample_len(i)).slice(),
    rate: x.web_sample_rate(i),
}));
async function importFile(name, b) {
    stop();
    say(`importing ${name}…`);
    await new Promise(r => setTimeout(r, 30));
    let r;
    try {
        if (b[0] === 0x1F && b[1] === 0x8B)
            b = new Uint8Array(await new Response(new Blob([b]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
        r = importVgm(b, {kit: theKit(), name: name.replace(/\.vg[mz]$/i, "")});
    } catch (err) {
        return say(`${name}: ${err.message}`, true);
    }
    change();
    if (loadBytes(r.song)) {                       // the engine refused it: a converter bug
        x.web_song_new();
        return afterLoad(`${name}: the import did not read back (a new song instead)`, true);
    }
    const i = r.info;
    afterLoad(`${name}: imported, ${i.orders} positions, ${i.instruments} instruments`);
    $("imp-sum").textContent = `"${i.title}"${i.author ? ` by ${i.author}` : ""}: ${Math.floor(i.seconds / 60)}:${String(Math.round(i.seconds % 60)).padStart(2, "0")}` +
        `${i.loop ? ", looping" : ""}, ${i.orders} order positions of ${i.rows} rows, ${i.instruments} instruments.`;
    const m = Object.entries(i.match);
    $("imp-match").innerHTML = m.length ? "<tr><td>Pitch match</td><td>" +
        m.map(([c, v]) => `${esc(c)} ${Math.round(v * 100)}%`).join(" · ") + "</td></tr>" : "";
    $("imp-notes").innerHTML = r.notes.map(s => `<li>${esc(s)}</li>`).join("");
    $("imp").showModal();
}
$("b-save").onclick = () => { download(new Blob([snapshot()]), `${songName()}.fm1d`); ui.dirty = false; };
$("b-vgm").onclick = () => {
    stop();
    say("exporting VGM…");
    setTimeout(() => {
        const n = x.web_vgm(900);
        if (!n) return say("the VGM did not fit (a song longer than 15 minutes?)", true);
        download(new Blob([new Uint8Array(x.memory.buffer, x.web_io(), n).slice()], {type: "audio/x-vgm"}), `${songName()}.vgm`);
        x.web_reset();
        say(`VGM: ${(n / 1024).toFixed(1)} KB`, false, true);
    }, 30);
};
$("b-wav").onclick = async () => {
    stop();
    say("rendering WAV…");
    await new Promise(r => setTimeout(r, 30));
    const chunks = [];
    let frames = 0;
    x.web_stop_at_end(1);
    x.web_post(PC.PLAY, 0, 0, 0);
    const max = RATE * 600;
    while (frames < max) {
        const n = x.web_render(4096);
        const f = new Float32Array(x.memory.buffer, x.web_audio_buf(), n * 2);
        const s = new Int16Array(n * 2);
        for (let i = 0; i < n * 2; i++) s[i] = clamp(Math.round(f[i] * 32767), -32768, 32767);
        chunks.push(s);
        frames += n;
        if (!x.web_playing()) break;
        if (frames % (RATE * 20) < 4096) await new Promise(r => setTimeout(r, 0));
    }
    for (let k = 0; k < 2; k++) {                  // the tail: the release of the last notes
        const n = x.web_render(RATE / 2 & ~255), f = new Float32Array(x.memory.buffer, x.web_audio_buf(), n * 2), s = new Int16Array(n * 2);
        for (let i = 0; i < n * 2; i++) s[i] = clamp(Math.round(f[i] * 32767), -32768, 32767);
        chunks.push(s);
        frames += n;
    }
    x.web_stop_at_end(0);
    const head = new DataView(new ArrayBuffer(44)), len = frames * 4;
    const w = (o, s) => [...s].forEach((c, i) => head.setUint8(o + i, c.charCodeAt(0)));
    w(0, "RIFF"); head.setUint32(4, 36 + len, true); w(8, "WAVEfmt "); head.setUint32(16, 16, true);
    head.setUint16(20, 1, true); head.setUint16(22, 2, true); head.setUint32(24, RATE, true); head.setUint32(28, RATE * 4, true);
    head.setUint16(32, 4, true); head.setUint16(34, 16, true); w(36, "data"); head.setUint32(40, len, true);
    download(new Blob([head, ...chunks], {type: "audio/wav"}), `${songName()}.wav`);
    say(`WAV: ${(frames / RATE).toFixed(1)} s`, false, true);
};
function afterLoad(what, bad = false) {
    ui.ord = ui.row = 0;
    ui.sel = null;
    ui.mute = 0;
    x.web_set_mute(0);
    fixCursor();
    songToForm();
    refreshAll();
    say(what, bad, !bad);
}

// ------------------------------------------------------------- the FM-1 ---
let link = null, slots = [], slotSel = -1, bankSel = 0, access = null;
const prog = f => { $("d-prog").style.width = `${Math.round(f * 100)}%`; };
async function connectWith(fn, label) {
    try {
        if (link) { link.close(); link = null; }
        $("dev-state").textContent = "looking…";
        const r = await fn();
        if (!r || !r.link) {
            $("dev-state").textContent = r && r.found ? `found ${r.found}, not FM1-TRAX` : `no ${label} found`;
            return;
        }
        link = r.link;
        const i = r.info;
        $("dev-state").textContent = `${label}: FM1-TRAX ${i.version}, ${i.free} of ${i.kib} KB free`;
        await refreshSlots();
    } catch (e) {
        $("dev-state").textContent = e.message;
    }
    devButtons();
}
async function refreshSlots() {
    if (!link) return;
    slots = await link.list(prog);
    const info = await link.hello();
    let h = "";
    for (let k = 0; k < SLOTS; k++) {
        const s = slots[k], cls = [s.used ? "" : "empty", k === slotSel ? "cur" : "", k === info.slot ? "inmem" : ""].join(" ");
        h += `<div data-s="${k}" class="${cls}"><span>${k.toString().padStart(3, "0")}</span><span>${s.used ? esc(s.title || "(untitled)") : "empty"}${k === info.slot ? " ◆" : ""}</span><span>${s.used ? (s.bytes / 1024).toFixed(1) + " KB" : ""}</span></div>`;
    }
    $("slots").innerHTML = h;
    $("dev-state").textContent = `FM1-TRAX ${info.version}, ${info.free} of ${info.kib} KB free, ◆ in memory${info.dirty ? " (unsaved)" : ""}`;
    prog(0);
}
function devButtons() {
    const on = !!link, s = slots[slotSel];
    $("d-open").disabled = !on || !s || !s.used;
    $("d-mem").disabled = !on;
    $("d-write").disabled = !on || slotSel < 0;
    $("d-del").disabled = !on || !s || !s.used;
    $("lib-read").disabled = $("lib-write").disabled = !on;
}
$("slots").addEventListener("click", e => {
    const d = e.target.closest("[data-s]");
    if (!d) return;
    slotSel = +d.dataset.s;
    for (const el of $("slots").children) el.classList.toggle("cur", el === d);
    devButtons();
});
async function guarded(fn) {
    try { await fn(); } catch (e) { say(e instanceof LinkError ? e.message : `error: ${e.message}`, true); }
    prog(0);
    devButtons();
}
$("d-usb").onclick = async () => {
    if (!navigator.requestMIDIAccess) return say("this browser has no Web MIDI: use Chrome, Edge or Opera", true);
    try { access = access || await navigator.requestMIDIAccess({sysex: true}); }
    catch (e) { return say(`MIDI was refused (${e.name})`, true); }
    connectWith(() => findDevice(access), "FM-1");
};
$("d-emu").onclick = () => connectWith(() => findEmulator(), "the emulator");
$("d-open").onclick = () => guarded(async () => {
    const b = await link.read(slotSel, prog);
    change();
    stop();
    const rc = loadBytes(b);
    afterLoad(rc ? "that is not a song this editor reads" : `slot ${slotSel} opened`, !!rc);
});
$("d-mem").onclick = () => guarded(async () => {
    const b = await link.read(MEMORY, prog);
    change();
    stop();
    const rc = loadBytes(b);
    afterLoad(rc ? "that is not a song this editor reads" : "the FM-1's song opened", !!rc);
});
let writeArmed = 0;
$("d-write").onclick = () => guarded(async () => {
    if (slots[slotSel] && slots[slotSel].used && Date.now() - writeArmed > 4000) {
        writeArmed = Date.now();
        return say(`slot ${slotSel} holds "${slots[slotSel].title}": click again to replace it`);
    }
    writeArmed = 0;
    await link.write(slotSel, snapshot(), prog);
    say(`written to slot ${slotSel}: the FM-1 plays it now`, false, true);
    await refreshSlots();
});
let delArmed = 0;
$("d-del").onclick = () => guarded(async () => {
    if (Date.now() - delArmed > 4000) { delArmed = Date.now(); return say(`click again to delete slot ${slotSel}`); }
    delArmed = 0;
    await link.remove(slotSel);
    say(`slot ${slotSel} deleted`);
    await refreshSlots();
});

// the library: a bank of 16 read, edited here, written back
{
    let h = "";
    for (let b = 0; b < BANKS; b++) h += `<option value="${b}">${b < 16 ? `FM bank ${b + 1} (USER FM ${b * 16}-${b * 16 + 15})` : `PSG bank ${b - 15} (USER PSG ${(b - 16) * 16}-${(b - 16) * 16 + 15})`}</option>`;
    $("lib-bank").innerHTML = h;
}
function drawBank() {
    let h = "";
    for (let k = 0; k < 16; k++) {
        const p = x.web_bank_ins(k), m = mem(), t = m[p + L[16]];
        let name = "";
        for (let i = 0; i < 15 && m[p + L[17] + i]; i++) name += String.fromCharCode(m[p + L[17] + i]);
        h += `<div data-k="${k}" class="${t ? "" : "empty"}"><span>${(bankSel * 16 + k - (bankSel >= 16 ? 256 : 0)).toString().padStart(3, "0")}</span><span>${t ? ["", "FM ", "PSG ", "DAC "][t] + esc(name) : "empty"}</span><span></span></div>`;
    }
    $("lib").innerHTML = h;
}
$("lib-bank").addEventListener("change", e => { bankSel = +e.target.value; x.web_bank_clear(); drawBank(); $("lib-state").textContent = "not read yet"; });
$("lib-read").onclick = () => guarded(async () => {
    bankSel = +$("lib-bank").value;
    let b;
    try { b = await link.read(BANK + bankSel, prog); }
    catch (e) { if (e.status === 1) b = null; else throw e; }
    if (!b) { x.web_bank_clear(); $("lib-state").textContent = "empty on the FM-1"; }
    else {
        new Uint8Array(x.memory.buffer, x.web_io(), b.length).set(b);
        $("lib-state").textContent = x.web_bank_read(b.length) ? "damaged" : "read";
    }
    drawBank();
});
$("lib-write").onclick = () => guarded(async () => {
    const n = x.web_bank_write(), b = new Uint8Array(x.memory.buffer, x.web_io(), n).slice();
    await link.write(BANK + +$("lib-bank").value, b, prog);
    $("lib-state").textContent = "written";
    say("bank written to the FM-1", false, true);
});
$("lib").addEventListener("click", e => {
    const d = e.target.closest("[data-k]");
    if (!d) return;
    const k = +d.dataset.k;
    if (e.shiftKey) { x.web_bank_put(k, ui.ins); drawBank(); $("lib-state").textContent = "changed: write the bank to keep it"; return; }
    if (!mem()[x.web_bank_ins(k) + L[16]]) return;
    change();
    x.web_bank_get(k, ui.ins);
    refreshIns();
    say(`library entry loaded into instrument ${hex2(ui.ins)}`);
});

// ------------------------------------------------------------- the rest ---
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));
let sayT = 0;
function say(s, bad = false, ok = false) {
    const el = $("msg");
    if (!el) return;
    el.textContent = s;
    el.className = bad ? "bad" : ok ? "ok" : "";
    clearTimeout(sayT);
    sayT = setTimeout(() => { el.textContent = ""; }, 6000);
}
function refreshAll() {
    drawOrders();
    refreshIns();
    $("pool").textContent = `room for ${x.web_pool_free()} more patterns`;
}
function lcd() {
    const pos = x.web_pos(), playing = x.web_playing(), ch = columns()[ui.col];
    const p = cellAt(ch, curPat(), ui.row), m = mem();
    let fx = "";
    if (p && ui.sub >= 5) { const f = m[p + (ui.sub <= 8 ? 3 : 4)]; fx = f ? cstr(x.web_fx_name(f)) || "not an effect" : ""; }
    $("lcd").innerHTML = `${playing ? "▶" : "■"} <b>${hex2(pos >>> 16)}:${hex2(pos & 0xFFFF)}</b> · ${chName(ch)} pat <b>${hex2(curPat())}</b> · oct <b>${ui.oct}</b> · ins <b>${hex2(ui.ins)}</b>${fx ? ` · <b>${esc(fx)}</b>` : ""}`;
}
let lastPlay = 0, trips = 0;
function frame() {
    if (x) {
        if (x.web_guard_trips() !== trips) {          // the sound's safety net went off (player_render)
            trips = x.web_guard_trips();
            say("the sound was reset by its safety net (a level no song reaches)", true);
        }
        const playing = x.web_playing();
        if (playing && ui.follow) {
            const pos = x.web_pos();
            ui.ord = pos >>> 16;
            ui.row = pos & 0xFFFF;
        }
        if (!playing && lastPlay) { $("copper").classList.remove("playing"); drawOrders(); }
        if (playing && (x.web_pos() >>> 16) !== lastPlay - 1) drawOrders();
        lastPlay = playing ? (x.web_pos() >>> 16) + 1 : 0;
        drawPattern();
        lcd();
    }
    requestAnimationFrame(frame);
}

async function boot() {
    const {instance} = await WebAssembly.instantiate(await (await fetch("fm1trax.wasm")).arrayBuffer(), {});
    x = instance.exports;
    L = Array.from({length: 31}, (_, k) => x.web_layout(k));
    x.web_ed_init();
    buildFm();
    buildPresets();
    let restored = false;
    try {
        const s = localStorage.getItem("fm1drive-song");
        if (s) restored = loadBytes(Uint8Array.from(atob(s), c => c.charCodeAt(0))) === 0;
    } catch { /* nothing kept */ }
    if (!restored) x.web_song_demo();
    x.web_bank_clear();
    drawBank();
    songToForm();
    refreshAll();
    devButtons();
    say(restored ? "your last song, kept by this browser" : "the demo song: press Enter to play it", false, true);
    requestAnimationFrame(frame);
    cv.focus();
}
window.fm1ed = {get x() { return x; }, ui, S, cellAt};    // for the console
boot().catch(e => { $("lcd").textContent = `the engine did not load: ${e.message}`; });
