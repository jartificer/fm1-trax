// SPDX-License-Identifier: GPL-3.0-only
// The web pages' engine and link, without a browser: web/fm1drive.wasm as the editor uses it (the
// song edited in place through web_layout, played, exported as VGM), and web/fm1link.js (the editor's
// side of the SysEx link) against a second instance of the module running as the FM-1, as the
// emulator does. Run from the repo root:  node tests/web_editor_test.mjs  (after host/build_web.sh)
import {readFileSync} from "node:fs";
import {SongLink, MEMORY, BANK} from "../web/fm1link.js";

let failed = 0;
const ok = (cond, what) => {
    console.log(`${what.padEnd(64)} ${cond ? "ok" : "FAIL"}`);
    failed += !cond;
};
const wasm = readFileSync("web/fm1drive.wasm");
const load = async () => (await WebAssembly.instantiate(wasm, {})).instance.exports;
const bytes = (x, p, n) => new Uint8Array(x.memory.buffer, p, n);
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// ------------------------------------------------------------ the editor ---
const e = await load();
const L = Array.from({length: 31}, (_, k) => e.web_layout(k));
e.web_ed_init();
ok(L[23] === 13 && L[28] === 7 && L[29] === 32, `layout: ${L[23]} channels, 7-byte cells, 32-step macros`);
e.web_song_demo();
const S = () => e.web_song();
const nord = () => bytes(e, S() + L[11], 2)[0] | bytes(e, S() + L[11], 2)[1] << 8;
ok(nord() === 4 && bytes(e, S() + L[8], 1)[0] === 64, "the demo song: 4 positions of 64 rows");
// a cell written in place, as the page does
const p = e.web_cell(9, 7, 3, 1);
bytes(e, p, 7).set([49, 8, 12, 0x0B, 0, 0x22, 0]);
let n = e.web_song_write();
const song = bytes(e, e.web_io(), n).slice();
e.web_song_new();
ok(e.web_cell(9, 7, 3, 0) === 0, "a new song: the cell is gone");
bytes(e, e.web_io(), n).set(song);
ok(e.web_song_read(n) === 0, `the .fm1d read back (${n} bytes)`);
const q = e.web_cell(9, 7, 3, 0);
ok(q && eq(bytes(e, q, 7), [49, 8, 12, 0x0B, 0, 0x22, 0]), "the cell came back");
// the player through web_render
e.web_post(0, 0, 0, 0);
let peak = 0;
for (let k = 0; k < 40; k++) {
    const m = e.web_render(4096), a = new Float32Array(e.memory.buffer, e.web_audio_buf(), m * 2);
    for (const v of a) peak = Math.max(peak, Math.abs(v));
}
ok(e.web_playing() && peak > 0.05 && peak < 1, `it plays (peak ${peak.toFixed(2)})`);
e.web_post(1, 0, 0, 0);
const v = e.web_vgm(900), vgm = bytes(e, e.web_io(), v);
ok(v > 1000 && String.fromCharCode(...vgm.subarray(0, 4)) === "Vgm ", `VGM export: ${v} bytes`);
// an instrument file round trip, and a preset
e.web_preset(20, 0, 5);
const ip = e.web_song() + L[14] + 20 * L[15];
ok(bytes(e, ip, 1)[0] === 1, "an FM preset loaded into instrument 20");
n = e.web_ins_write(20);
const ins = bytes(e, e.web_io(), n).slice();
e.web_ins_init(21, 0);
bytes(e, e.web_io(), n).set(ins);
ok(e.web_ins_read(21, n) === 0 && eq(bytes(e, e.web_song() + L[14] + 21 * L[15], L[15]), bytes(e, ip, L[15])), "an instrument written and read back");

// -------------------------------------------- the FM-1, as the emulator ---
const d = await load();
d.web_boot(1);
d.web_run(200);
class DeviceTransport {                            // fm1link's transport, straight into the module
    constructor() { this.q = []; }
    send(m) {
        const body = m.subarray(1, -1);
        bytes(d, d.web_sx_in_buf(), body.length).set(body);
        d.web_sx_in(body.length);
        d.web_run(5);
        this.take();
        return true;
    }
    take() {
        const n = d.web_sx_out_len(), out = bytes(d, d.web_sx_out(), n).slice();
        d.web_sx_out_clear();
        for (let i = 0; i < n;) { const j = out.indexOf(0xF7, i); this.q.push(out.subarray(i, j + 1)); i = j + 1; }
    }
    async read() { if (!this.q.length) { d.web_run(20); this.take(); } return this.q.shift() || null; }
    drain() { this.q.length = 0; }
    close() {}
}
const link = new SongLink(new DeviceTransport(), "test");
const hi = await link.hello();
ok(hi.proto === 1 && hi.slots === 256 && hi.kib > 200 && hi.free > 200, `HELLO: ${hi.slots} slots, ${hi.free} of ${hi.kib} KB free, ${hi.version}`);
const list = await link.list();
ok(list.length === 256 && list.every(s => !s.used), "LIST: 256 slots, all empty on a new FM-1");
const mem = await link.read(MEMORY);
ok(mem.length > 100 && String.fromCharCode(...mem.subarray(0, 4)) === "FM1D", `READ the song in memory: ${mem.length} bytes`);
await link.write(7, song);
const back = await link.read(7);
ok(eq(back, song), "WRITE the editor's song to slot 7, READ it back the same");
const l2 = await link.list();
ok(l2[7].used && l2[7].title === "DRIVE ON" && l2[7].bytes > 0 && l2[7].bytes < song.length, `LIST: slot 7 "${l2[7].title}", ${l2[7].bytes} bytes in flash (of ${song.length})`);
ok((await link.hello()).slot === 7, "the FM-1 switched to slot 7");
// a bank: the editor's instrument 20 into entry 3 of bank 2
e.web_bank_clear();
e.web_bank_put(3, 20);
n = e.web_bank_write();
const bank = bytes(e, e.web_io(), n).slice();
await link.write(BANK + 2, bank);
const bank2 = await link.read(BANK + 2);
ok(eq(bank, bank2), `a library bank written and read back (${n} bytes)`);
bytes(e, e.web_io(), bank2.length).set(bank2);
ok(e.web_bank_read(bank2.length) === 0 && bytes(e, e.web_bank_ins(3), 1)[0] === 1 && bytes(e, e.web_bank_ins(4), 1)[0] === 0, "the bank parsed: entry 3 FM, entry 4 empty");
await link.remove(7);
ok(!(await link.list())[7].used, "DELETE slot 7");
let refused = null;
try { await link.read(7); } catch (err) { refused = err; }
ok(refused && refused.status === 1, "READ an empty slot: refused");

console.log(failed ? `${failed} web editor tests FAILED` : "web editor tests passed");
process.exit(failed ? 1 : 0);
