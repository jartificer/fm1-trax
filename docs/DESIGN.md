# FM1-DRIVE design

A Mega Drive / Genesis sound tracker that runs on the M-VAVE FM-1. It plays and writes music for the
console's two sound chips, the Yamaha **YM2612** (OPN2) and the TI **SN76489** PSG built into the VDP,
with what they can do and nothing they can't. Songs export as `.vgm`, so what you write here plays in
any VGM player, and on real hardware through a VGM-playing cart or driver.

Status: stages 1–4 are written, tested in the host simulator, and tested on a real FM-1. Stages are listed at the end.

## 1. Platform

FM1-DRIVE is built on [FM1-GM](https://github.com/jartificer/fm1-gm)'s platform (itself Felucca, X0X and
FoMni's): the hardware layer, the update loader, the flash store, the USB stack, the installer, the
host simulator and the WebAssembly build of the whole app. Those parts are proven on the device and
are taken over unchanged where possible. Everything above them is new: the chip emulation, the
tracker, the song store, the UI and the web editor. The project is therefore GPL-3.0-only.

| | |
|---|---|
| CPU | JieLi AC791N, single-precision FPU (the engine is fixed-point integer) |
| Screen | 240 × 240 ST7789, RGB565, drawn in strips (no full frame buffer) |
| Audio | I2S at 44.1 kHz, 256-frame halves (5.8 ms) |
| RAM for big buffers | 344 KB (`.pool`) |
| App slot | 581,564 bytes (code + presets + DAC samples) |
| Package identity | `FM-1_700` (release `FM-1_7XXYYZZ`): FM1-GM is 6, FoMni 8, X0X 9 |

## 2. The sound engine (`firmware/src/dsp/`)

Both chips are emulated at register level: the tracker drives them only by writing registers, exactly
as a Mega Drive sound driver does. The same register writes are what the `.vgm` export records.

### YM2612 (`opn2.c`)

Sample-accurate (not cycle-accurate) emulation, run at the chip's own rate, 7.670454 MHz / 144 =
**53,267 Hz** (NTSC; PAL 52,781 Hz), resampled to 44.1 kHz. Implemented from the chip's documented and
measured behaviour (the same model as Nuked-OPN2 and ymfm):

- 6 channels × 4 operators, the 8 algorithms, operator 1 self-feedback (0–7).
- Phase: 11-bit F-number, 3-bit block, detune (DT1, from the key code) and multiple (MUL, 0 = ½);
  20-bit phase, 10-bit sine index.
- The real chip's log-sine (256 × 12-bit) and exponent (256 × 10-bit) ROM tables, so the 14-bit
  operator output, its quantisation and its rounding are the chip's own.
- Envelope generator: attack, decay 1, decay 2 (sustain rate), release, sustain level, total level,
  key scaling, 10-bit attenuation stepped every 3 samples with the chip's rate tables; attack rates 62–63
  are instant. **SSG-EG** (the 8 modes, with the chip's 4× decay and inversion quirks).
- **LFO**: one for the whole chip, 8 rates (3.98 … 72.2 Hz); AMS (0 / 1.4 / 5.9 / 11.8 dB) and PMS
  (8 depths) per channel, AM enable per operator.
- **Channel 3 special mode**: separate frequencies for operators 1–3 (the tracker's FM3 can be split
  into 4 one-operator channels, "FM3 extended", as Deflemask and Furnace do). **CSM** (timer A key-on)
  for the speech-like effects.
- **DAC**: channel 6 replaced by an 8-bit unsigned sample when the DAC is enabled (register 0x2B).
- Output: each channel's carriers summed and clipped to 14 bits, then the **9-bit DAC** (the low 5
  bits dropped), with the YM2612's **ladder effect** (the crossover distortion that makes quiet notes
  gritty on a Model 1). A setting picks **YM2612** (Model 1) or **YM3438** (Model 2, no ladder effect).
- Pan: hard left / centre / right per channel (L and R enable bits); there is no other panning.
- Timers A and B kept for CSM; the tracker's tempo does not use them.

### SN76489 (`psg.c`)

The Sega VDP variant: 3 square tone channels and 1 noise channel, clocked at 3.579545 MHz / 16.

- 10-bit tone periods (so the lowest note is about A2, 109 Hz; period 0 acts as 1, which is how
  drivers play samples on the PSG), 4-bit attenuation in 2 dB steps (15 = off).
- Noise: 16-bit LFSR, white (taps bits 0 and 3) or periodic, at clock/512, /1024, /2048 or at tone
  channel 3's period. Writing the noise control resets the LFSR, as on the chip.
- Downsampled with an exact box filter (each output sample is the average of the 223.7 kHz steps it
  covers), so high notes alias as little as the real PSG through its output stage.

### Mixing

YM2612 and PSG are mixed at the console's measured relative levels, then a choice of output filter:
**Model 1** (VA0–VA6, the first-order low-pass around 3.4 kHz that gives the classic dull sound),
**Model 2**, or **none** (a clean emulator-like sound). The FM-1's MASTER pot sets the level.

Accuracy is checked in the host tests by rendering the same register writes through this engine and
through Nuked-OPN2 (the cycle-accurate reference, fetched only for the tests, not built into the
firmware) and comparing.

### The safety net

As in FM1-GM (whose 0.9.8 made a sudden loud noise), the sound has a safety net, in `player_render`
so that the device, the simulator and the web pages all have it. Every sample is clipped to 16 bits:
the device scales the mix by the master volume (up to x256) into its 24-bit output, and six FM
channels and the PSG at full level can reach about 46000, which would otherwise wrap round to full
scale of the other sign. The device clamps the 24-bit value once more. A block whose level no real
mix reaches (`PLAYER_GUARD`, 18 dB over the clip, the loudest fuzzed mix being 2 dB under it) means
the engine's state is broken: the block is dropped, the player stops and the chips start over, and
the screen says SOUND RESET. `tests/host/fuzz_test.c` fuzzes the whole path (BUILDING.md).

### The time budget

The audio interrupt renders 256 frames every 5.8 ms; a block that comes late is heard as a stuck
buffer, and the screen gets only the time the sound leaves. The engine is built for it:

- The sound code and its tables run from RAM (`DSP_HOT`, `dsp/dsp_hot.h`, placed in `.ram_text`
  after the flash driver and copied there at boot), not from flash, whose cache misses cost more
  than the work. Calls from flash into it go through a pointer (`md_render_far`).
- The YM2612 is rendered a chunk at a time (`opn2_render`): the per-sample globals (LFO, envelope
  clock) first, then each channel and operator over the whole chunk, with operators at rest skipped
  and the attenuation worked out again only when the envelope moves. Register writes, key events
  and CSM fall back to the sample-by-sample path, which computes the same samples.
- The PSG works out each output sample's weights from its edges instead of stepping the chip.

SETUP shows AUDIO LOAD, the average and the slowest block of the last 2 s as a share of the 5.8 ms,
and the screen says AUDIO TOO SLOW when a block comes late.

### Limits kept on purpose

6 FM voices and 4 PSG voices, no more: each tracker channel *is* one chip channel. One LFO for the
whole chip. FM6 plays either FM or the DAC, not both. The DAC plays one 8-bit sample at a time. PSG
notes cannot go below A2, and the noise channel is either its own three rates or tone 3's pitch. Pan is
left / centre / right. These are what make Mega Drive music sound the way it does.

## 3. The tracker

### Channels

| Channel | Chip | |
|---|---|---|
| FM1, FM2, FM4, FM5 | YM2612 ch 1, 2, 4, 5 | FM |
| FM3 | YM2612 ch 3 | FM, or (song setting) **extended**: four channels OP1–OP4, each its own pitch |
| FM6 | YM2612 ch 6 | FM, or **DAC** samples when a sample instrument plays on it |
| SQ1, SQ2, SQ3 | PSG tone 1–3 | square |
| NOI | PSG noise | noise (its mode from the instrument or effect `20xy`) |

10 channels (13 with FM3 extended).

### Song structure

- **Order list**: up to 256 positions; each position names a pattern for each channel (as in
  Deflemask), so a bass line can repeat while the drums change. A loop point picks where playback goes
  after the last position (it is also the `.vgm` loop).
- **Patterns**: per channel, 0–255 each; length 1–128 rows (song-wide); up to 256 patterns of 64 rows in
  RAM per song (the pool is 16,384 rows, so 128 of 128 rows).
- **Cell**: note (C-0 … B-7, key-off `===`), instrument (00–FF), volume (FM 00–7F, PSG 0–F), two effect
  columns (command + value each).
- **Timing**: ticks at 60 Hz (NTSC) or 50 Hz (PAL), or a custom tick rate; speed (ticks per row), with
  an optional swing (alternating speeds), as Mega Drive drivers do.
- **Instruments**: each song carries the instruments it uses (up to 256), so a song is complete in
  itself and plays the same anywhere. The instrument library (below) is where they come from.

### Effects

Deflemask's Genesis set, so the format is familiar and `.dmf` songs can be imported later:

| | |
|---|---|
| `00xy` arpeggio | `01xx` / `02xx` portamento up / down, `03xx` tone portamento |
| `04xy` vibrato | `0Axy` volume slide |
| `08xy` pan (x left, y right: on/off) | `0Bxx` jump to order, `0Dxx` pattern break |
| `0Cxx` retrigger | `0Fxx` speed |
| `10xy` LFO (x on/off, y rate) | `11xx` feedback |
| `12xx`–`15xx` total level of OP1–OP4 | `16xy` multiple (x operator, y value) |
| `17xx` DAC on/off | `18xx` FM3 extended on/off |
| `19xx` attack rate, all ops; `1Axx`–`1Dxx` OP1–OP4 | `20xy` PSG noise (x periodic/white, y rate or tone 3) |
| `E1xy` / `E2xy` note slide up / down | `E5xx` fine tune |
| `ECxx` note cut | `EDxx` note delay |

As in Deflemask, `01xx` / `02xx` slide on every tick, the row's first included, and a new note
without `03xx` ends a tone portamento there (so an imported VGM's pitch follows the original).

## 4. Instruments

### Kinds

- **FM**: the chip's patch, exactly the registers: algorithm, feedback, AMS, PMS, and for each operator
  AR, D1R, D2R, RR, D1L (SL), TL, RS (key scale), MUL, DT, AM on/off, SSG-EG. 32 bytes packed. Optional
  arpeggio and pitch macros.
- **PSG**: macros (sequences of up to 64 steps with a loop and a release point): volume (0–15),
  arpeggio (semitones, or fixed notes), pitch (fine), noise mode (NOI only). PSG has no other timbre:
  only volume shapes the sound. A note let go goes silent at once, or plays the volume macro's
  release part once (its last step is not held), or, with a loop after the release point, for at most
  `PSG_REL_MAX` ticks (4 s at 60 Hz): no note sounds for ever after its key is up.
- **DAC**: an 8-bit sample on FM6: the sample, its root note, its playback rate (drivers play samples
  at fixed rates; notes pick one), loop on/off.

### Presets and user slots

| Bank | Slots | Where |
|---|---|---|
| FM presets | 128, in 16 groups (bass, brass, keys, leads, bells, organ, strings, guitar, drums, FX …) | firmware (read-only) |
| PSG presets | 32 (plucks, pads, hats, snares, arps …) | firmware |
| DAC kit | 48 drum and hit samples, synthesised by a script in this repo (no ripped sounds) | firmware |
| **User FM** | **256** | flash, library |
| **User PSG** | **128** | flash, library |
| User DAC | samples sent from the web editor, as room allows | flash, library |

Any preset or user instrument can be loaded into a song, tweaked there, and saved back to any user
slot. The library is backed up and restored, and instruments imported (`.tfi`, `.dmp`, `.y12`,
`.opni`), from the web editor.

## 5. Controls

The FM-1's panel maps onto the tracker by what the controls are printed with, and the four knobs
always edit four things side by side: in the pattern, the cell's four fields; in the FM editor, the
**same parameter on the four operators**, so an envelope is shaped like four faders on a mixer.

```
 [SELECT] [ALGORITHM] [PRESETS] (MASTER)      [KNOB1] [KNOB2] [KNOB3] [KNOB4]
                                              [FX ] [SEL] [ENV] [LFO] [EDIT] [GLO]
  [OCT-] [OCT+]                               [HOME][SAVE][ARP] [SEQ] [PLAY] [REC]
 [ 27 keys, F3 .. G5 ]
```

### Everywhere

| Control | |
|---|---|
| HOME | **Pattern** view. Again: **Stage** view (the visualiser, below) |
| SEQ | **Order** view (the song's arrangement). Again: song settings |
| EDIT | **Instrument** editor (the instrument under the cursor) |
| GLO | **Songs** (load, save as, new, delete). Again: **Setup** |
| PLAY | Play / stop. In the pattern view from the cursor's pattern; held 1 s: the song from the top |
| REC | Edit mode on / off (lit red): the keys write notes. Playing with REC on records live, quantised to rows |
| SAVE | Save the song to its slot. In Songs: to the highlighted slot (a used slot asks twice) |
| ARP | Undo (again: redo) |
| OCT- / OCT+ | The keys' octave (0–7). Both held 5 s: update mode (as FM1-GM) |
| PRESETS | The instrument the keys play and enter (the song's instruments; library instruments are loaded into one from the Library) |
| keys | Play the instrument on the cursor's channel; in edit mode, also write the note and step down |
| MASTER | Volume |

The header on every screen shows the battery as a small icon left of the song position: three bars,
one in yellow when low, and a blinking red outline when empty. Nothing is shown on USB power (the
FM-1 has no charger status line, so it cannot tell charging from full). When the battery reads empty
for 5 s the header says BATTERY LOW (SAVE SONG when there are unsaved changes), again every 10
minutes. Setup's BATTERY row shows the bars and the divider's ADC reading. The reading is taken by
the main loop 20 times a second, never by the audio ISR; Felucca's thresholds (531, 561, 591) with
4 counts of hysteresis (`app/battery.h`).

### Pattern view

| Control | |
|---|---|
| SELECT | Row (fast turns jump by the row highlight, 4 / 16) |
| ALGORITHM | Channel |
| KNOB 1–4 | NOTE, INSTRUMENT, VOLUME, EFFECT 1 value of the cell; FX flips them to FX1 command, FX1 value, FX2 command, FX2 value (the command knob runs through the effects by name) |
| ENV | Key-off `===` at the cursor (the envelope's release); on a key-off, clears the cell |
| LFO | Mute the channel; held: solo |
| SEL held + SELECT / ALGORITHM | Mark a block (rows, channels). SEL: copy (with nothing marked: the cursor's whole pattern); SEL held 1 s: paste at the cursor; ENV: clear the block; KNOB 1 with a block: transpose it |

### Order view

SELECT the position, ALGORITHM the channel, KNOB 1 the pattern number in the cell, KNOB 2 all of the
row's patterns at once, KNOB 3 the song's length, KNOB 4 the loop point. ENV inserts a position, LFO
duplicates it (as new copies of its patterns), held: deletes it.

### Instrument editor (FM)

Its own pages under the buttons with their names:

| Page | KNOB 1–4: OP1–OP4's … (SELECT picks the row) |
|---|---|
| **EDIT**: tone | MUL, DT, TL, RS, AM on/off |
| **ENV**: envelope | AR, D1R, D1L (SL), D2R, RR, SSG-EG, drawn as the four envelopes |
| **LFO**: modulation | (song) LFO rate, AMS, PMS, feedback, on KNOB 1–4 |

ALGORITHM sets the algorithm, its diagram drawn large, the operators lighting up as they sound.
PRESETS steps through instruments. Keys audition. FX opens the macro page (arpeggio, pitch).
SEL opens the **Library** (presets and user slots by group: SELECT scrolls, KNOB 1 the group; SEL
loads into the song's instrument; SAVE stores it to the highlighted user slot, twice over a used one).

### Instrument editor (PSG, DAC)

PSG: a macro editor, a bar per step. ENV the volume macro, LFO arpeggio, FX pitch, EDIT noise mode.
SELECT the step, KNOB 1 its value, KNOB 2 the length, KNOB 3 the loop, KNOB 4 the release.
DAC: KNOB 1 the sample, 2 root note, 3 rate, 4 loop.

### Stage view

The eye-candy page, for playing a song to someone: a scope per channel, the YM2612's register map
lighting up as it is written, the PSG's square waves, a 10-channel piano roll scrolling past and the
song title in raster-bar chrome. LFO mutes the channel ALGORITHM points at; KNOB 1–4 nothing.

### Look and feel

16-bit console era, and fun: a boot intro (FM1·DRIVE in chrome letters over a parallax star field,
with a little FM chime), CRT scanlines (Setup: on / off), raster-bar headers, channel colours, notes
flashing as they play, scopes in the pattern view's header, a palette-cycling cursor, screen changes as
horizontal wipes. Colour schemes in Setup, including high-contrast and colour-blind-safe ones as in
FM1-GM. All of it within the frame budget: the screen is drawn in strips and only what changed is sent.

## 6. Storage

### On the device

The data area FM1-GM gives its songs (0x97000–0xDFFFF less the two panel sectors at 0xDC000 and
0xDD000: 70 sectors) holds a **chunk store** (`storage.c`): each 4 KiB sector is 16 chunks of 256
bytes, a 20-byte header (object, version, part, parts in the version, length, CRC) and 236 bytes of
data. A save writes a new version of the object into erased chunks; its last chunk carries the number
of parts, so at boot each object is its newest complete version, and a save cut off by power leaves
the old one. Space is reclaimed by erasing the sector with the most dead chunks, its live chunks
copied out first, 16 chunks kept free for that; the choice starts from a moving cursor, which spreads
the erases evenly over the sectors (the test measures it). About **258 KB** hold data.

- **256 song slots** (objects 0–255), sharing the store. A song is stored as `FDS1`, its title (for the
  song list, read without unpacking), then its `.fm1d` stream in LZ4 blocks of up to 4 KiB: empty
  cells cost nothing, repeated patterns are stored once, and the blocks compress further. The demo
  song (2 minutes, 35 orders) takes 4.6 KB; a dense 3-minute song a few. That is well over a hundred songs at once.
- The **library**: 24 banks of 16 instruments (objects 256–279; banks 0–15 user FM, 16–23 user PSG),
  each `FDL1` and its 16 instruments. A bank is rewritten when one of its slots is stored.
- The settings and the panel calibration keep FM1-GM's own A/B objects.
- The web editor backs songs and the library up to the computer, so the device's room is a working
  set, not a limit.

The stock firmware's patch area (0xEA000–0xFBFFF) is not used: the flash driver only lets the app write
its own data area (`FL_STORE_OK`), which keeps the loader and the stock firmware safe.

### Song file (`.fm1d`)

What a song slot holds, unpacked: a header (magic `FM1D`, format version, title, author, timing, chip and
filter settings), the instruments, the order list, the patterns (each row as a bitmask of the fields
present, then those fields), then the song's own samples if any. The web editor reads and writes it,
and moves it to and from the device over SysEx (`F0 7D 46 44 …`, the protocol at the top of
`link.c`).

## 7. Web (branch `gh-pages`)

A static site, kept private until the repository is public (Pages is not enabled):

- **Emulator**: the firmware's app compiled to WebAssembly (as FM1-GM's), with the FM-1's panel; songs
  saved in the browser.
- **Installer**: installs the firmware over Web MIDI (FM1-GM's installer, with FM1-DRIVE's package).
- **Editor**: a full desktop tracker on the same engine (the same `.wasm`), with a computer-keyboard
  layout like Deflemask / Furnace: patterns, orders, instruments, the library; open and save `.fm1d`;
  read and write the FM-1's songs and library over USB; **export `.vgm`** (VGM 1.71, YM2612 + SN76489,
  DAC data block, loop) and `.wav`; import instruments, later Deflemask `.dmf` songs.
- **VGM import** (`web/vgmimport.js`, also `tools/vgm2fm1d.mjs` on the command line, which made the
  demo song): a Mega Drive VGM becomes a song by replaying its register log and fitting the tracker
  to it. The row grid (speed and swing) is the coarsest that the note starts keep to; each key-on is
  a note with its patch as an instrument (alike patches shared) and its carriers' level as the
  volume; each PSG envelope is a note with a volume macro; slides and vibrato are chosen row by row
  by running the player's own pitch model against the log's pitches; repeated patterns are stored
  once; the loop point becomes the song's loop. The VGM's DAC samples are played by the kit drum
  most like each (the song has no room for samples), and other chips' commands are skipped.

## 8. Stages

1. **Engine**: platform taken over from FM1-GM, YM2612 + SN76489 + mixer, host tests against
   Nuked-OPN2, a WAV renderer.
2. **Tracker core**: song model, playback with the effects, instruments and presets, the song format,
   VGM export.
3. **Device UI**: the views and controls above, the chunk store, the library, the eye-candy; scenario
   tests in the host simulator; the device package from CI.
4. **Web**: emulator, installer and editor on `gh-pages`.
