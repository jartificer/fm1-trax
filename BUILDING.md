# Building FM1-DRIVE

## Host tests and the simulator

Any machine with a C compiler and Python 3 with Pillow (for the font header):

```
tests/run_tests.sh [WAVDIR]
```

This builds and runs:

- `tests/host/engine_test.c`: the YM2612, PSG and mixer. When it can be downloaded, the YM2612 is
  compared with Nuked-OPN2 at a fixed commit (`NO_NUKED=1` skips that).
- `tests/host/trk_test.c`: the song format, the player and its effects, the presets and VGM export.
- `tests/storage_test.c` and `tests/host/lz4_test.c`: the flash chunk store and the compressor.
- `tests/install_test.py`: the installer.
- The scenarios in `tests/scenarios/*.fd`, run against the whole app in the simulator
  (`build/host/drive_host`, built by `host/build_host.sh`). They run twice, the second time under
  AddressSanitizer and UBSan. Screenshots and audio go to `build/scenarios/`.

With `WAVDIR`, the engine and tracker tests also write WAV files there.

A scenario is a script of button presses, knob turns, key presses, waits, screenshots and
expectations. `linkcopy SRC DST` moves an object through the SysEx link the way the web editor does,
and `reboot` restarts the app with only the flash kept. See the top of `host/drive_host.c`.

## The device package

Needs Linux x86-64 (or Docker on macOS: set `JIELI_DOCKER=1`).

1. The JieLi toolchain (clang 4.0.1 for pi32v2):
   ```
   tools/get_toolchain.sh ~/.jieli
   export JIELI_TOOLCHAIN=~/.jieli/toolchain
   ```
2. The JieLi AC79 SDK, release AC79NN_SDK_V1.2.1_2023-12-13 (commit d179b448). The package builder
   takes the SPL and the config tool from it:
   ```
   git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/ac79-sdk
   export AC79_SDK=~/ac79-sdk
   ```
3. Build:
   ```
   ./build.sh                    # build/fm1drive.fwsc, identity FM-1_700
   ./build.sh --release 0.1      # build/fm1drive-0.1.fwsc, identity FM-1_7000100
   ```

Every package carries a revision number, shown on the boot screen: the number of commits in
`git rev-list --count HEAD`, with a "+" when `firmware/`, `assets/` or `tools/` has uncommitted changes.

The build checks the image before packaging: the entry point, no calls from RAM code into flash, no
soft-double routines (the FPU is single precision), the RAM and pool budgets, and that hardware
registers are touched only from `firmware/hal/`. There is no libgcc, so a 64-bit division does not
link: the engine divides with `q16_div` (`mdsnd.c`) instead.

`.github/workflows/build.yml` runs the host tests and the device build on every push and uploads the
package as an artifact.

## The web pages

`web/` is a static site: `index.html` and `app.js` (the emulator, with the FM-1's panel, and the
installer), `emu.js` (runs the emulator), `worklet.js` (the audio out), `fm1ota.js` (the installer, as
`tools/fm1_install.py`), `editor.html` and `editor.js` (the desktop tracker) and `fm1link.js` (the songs
and library over SysEx, to an FM-1 or to the emulator in another tab). Two built files are kept in the
repository, so the site needs no build step to serve:

- `web/fm1drive.wasm`: `sh host/build_web.sh`. Needs clang and wasm-ld (LLVM 13 or later,
  `--target=wasm32`); no Emscripten and no libc. The emulator and the editor both run it.
- `web/fm1drive.fwsc`: the device package, copied from `build/fm1drive.fwsc` after `./build.sh`. Update
  `web/build.txt` with the commit it was built from.

`tests/host/fuzz_test.c` fuzzes the sound path for loud-noise bugs: random songs with every effect and
instrument setting, live commands (play, stop, the keys, mute, reset, edits) between the blocks, and
damaged song files, checking that no block reaches the device's output range unclipped, that a stop goes
quiet, and that the safety net (below) works and never goes off on a real song. `tests/run_tests.sh` runs
300 seeds (`FUZZ_SEEDS`), and 60 more under ASan/UBSan. `tests/web_fuzz_test.mjs` does the same for the
editor's VGM import: damaged and random VGMs must be refused with a message or import into a song the
engine reads and plays within the clip.

The engine is optimised for the FM-1's time budget, and two tests keep it honest.
`tests/host/golden_test.c` hashes the engine's output sample by sample (random register logs for each
chip, the whole mixer, random songs through the player) and compares with `tests/host/golden.txt`, so
an optimisation must not change a single sample; `golden_test write FILE` records new hashes after an
intended change to the sound. `tests/cost_check.sh` counts the host instructions a 256-frame block
takes with valgrind (`tests/host/cost_test.c`: every FM operator sounding with the LFO and SSG-EG, and
the demo song), built with clang as the FM-1's compiler, and fails when the engine gets more than about
10% slower. Both run in `tests/run_tests.sh`. The device shows its own load in SETUP (AUDIO LOAD).

Hanging notes are fuzzed at two levels. `fuzz_test` plays the keys (notes on and off on any channel,
instruments changed meanwhile), lets every key go, and requires the chips to fall silent within 5 s
(`player_hanging()`: no FM operator keyed, every PSG channel at full attenuation, the DAC stopped).
`tests/ui_fuzz_test.mjs` drives the whole app in the simulator with random sessions (panel keys and MIDI
held and let go in any order, notes entered, changed with the knobs and deleted, play and stop, views
changed; 150 seeds, `UI_FUZZ_SEEDS`), then checks that no channel is left to the keys and nothing sounds.
`tests/scenarios/hang.fd` holds the cases found so far.

`tests/web_install_test.mjs` tests the installer against a simulated FM-1, and
`tests/web_editor_test.mjs` the editor's engine (songs edited in place, played, exported as VGM) and its
link against a second copy of the module running as the FM-1. `tests/run_tests.sh` runs both (Node 18 or
later).

The `gh-pages` branch holds `web/`'s files at its root, for GitHub Pages once the repository is public.

## Layout

- `firmware/hal/`, `firmware/loader/`, `firmware/crt0.S`, `firmware/app.ld`: the FM-1 platform and
  update loader from FM1-GM (Felucca's), unchanged.
- `firmware/src/main.c`: the unity build (everything that touches the HAL).
- `firmware/src/dsp/`: the sound chips. `opn2.c` the YM2612, `psg.c` the SN76489, `mdsnd.c` the two
  chips together, resampled to the output rate and filtered like a model 1 or 2 console.
- `firmware/src/trk/`: the tracker. `song.c` the song model and its byte format, `player.c` the
  playback and effects (run from the audio render, driven through a command ring), `presets*.c` the
  instruments, `vgm.c` the VGM export.
- `firmware/src/app/`: the app. `settings.c` the settings, the song slots and the library;
  `ui.c`, `ui_pat.c`, `ui_views.c` the screens and controls; `link.c` the songs and library over SysEx.
- `firmware/src/storage.c`: the flash. The A/B objects for the settings and panel calibration, and the
  chunk store for the songs and the library.
- `firmware/src/lz4.c`: LZ4 block compression for the songs.
- `host/drive_host.c`: the app on the host, driven by scenario scripts.
- `host/drive_web.c`: the app as WebAssembly for the web pages, driven by `web/emu.js` and `web/editor.js`.
- `tools/`: the build (`build.py`), the font and drum generators, the installer and rescue tools.
