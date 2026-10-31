// SPDX-License-Identifier: GPL-3.0-only
// Fuzzing the whole firmware app (build/host/drive_host) for hanging notes and loud noise: random
// sessions on the panel and over MIDI (keys held and let go in any order, notes entered, changed with
// the knobs and deleted, the song started and stopped, views changed), then every key let go. After
// that no channel may be left to the keys, nothing may sound once the song is stopped and the
// releases have run out, and the output never goes past the clip. Run from the repo root after
// host/build_host.sh:  node tests/ui_fuzz_test.mjs [SEEDS]
import {execFileSync} from "node:child_process";
import {mkdirSync, writeFileSync} from "node:fs";

const seeds = +(process.argv[2] || 40), dir = "build/ui_fuzz";
mkdirSync(dir, {recursive: true});
let r = 1;
const rnd = () => { r ^= r << 13; r ^= r >>> 17; r ^= r << 5; return r >>> 0; };
const rn = n => rnd() % n;
const pick = a => a[rn(a.length)];
const BTN = ["FX", "SEL", "ENV", "ENV", "ENV", "LFO", "EDIT", "HOME", "HOME", "SEQ", "ARP", "REC", "REC", "OCT-", "OCT+"];
const ENC = ["SELECT", "SELECT", "ALGO", "PRESET", "K1", "K1", "K1", "K2", "K3", "K4"];

function session() {
    const out = ["wait 300", "tap HOME"], keys = new Set(), midi = new Set();
    if (rn(2)) out.push("tap GLO", "turn SELECT 1", "tap ENV");         // a new, empty song
    const n = 40 + rn(160);
    for (let i = 0; i < n; i++) {
        const k = rn(20);
        if (k < 4) {
            const key = rn(16);
            if (keys.has(key)) { out.push(`key ${key} up`); keys.delete(key); }
            else { out.push(`key ${key} down`); keys.add(key); }
        } else if (k < 6) out.push(`tapkey ${rn(16)}`);
        else if (k < 8) {
            const note = 36 + rn(36), hex = note.toString(16).toUpperCase();
            if (midi.has(note)) { out.push(rn(2) ? `midi 80 ${hex} 00` : `midi 90 ${hex} 00`); midi.delete(note); }
            else { out.push(`midi 90 ${hex} ${(1 + rn(126)).toString(16)}`); midi.add(note); }
        } else if (k < 13) out.push(`turn ${pick(ENC)} ${rn(2) ? 1 + rn(3) : -1 - rn(3)} ${10 + rn(80)}`);
        else if (k < 17) out.push(`tap ${pick(BTN)}`);
        else if (k < 18 && rn(3) === 0) out.push("tap PLAY");
        else out.push(`wait ${rn(400)}`);
    }
    for (const key of keys) out.push(`key ${key} up`);
    for (const note of midi) out.push(`midi 80 ${note.toString(16).toUpperCase()} 00`);
    out.push("wait 600", "expect jams 0", "wait 5000", "expect hanging 0", "expect peak_db_max 0");
    return out.join("\n") + "\n";
}

let fails = 0;
for (let s = 1; s <= seeds; s++) {
    r = (0x9E3779B9 * s | 1) >>> 0;
    const f = `${dir}/s${s}.fd`;
    writeFileSync(f, session());
    try {
        execFileSync("build/host/drive_host", [f, dir], {encoding: "utf8"});
    } catch (e) {
        fails++;
        console.log(`FAIL seed ${s} (${f}):\n` + (e.stdout || "").split("\n").filter(l => /FAIL|line/.test(l)).join("\n"));
    }
}
console.log(`${seeds} sessions: ${seeds - fails} with no hanging note${fails ? `, ${fails} failures` : ""}`);
process.exit(fails ? 1 : 0);
