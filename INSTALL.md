# Installing FM1-TRAX on an M-VAVE FM-1

The install bundle (made by `tools/make_bundle.sh`) holds these files. From the repository instead,
the installers are in `tools/` and the package is `web/fm1trax.fwsc`:
`python3 tools/fm1_install.py web/fm1trax.fwsc`.

The bundle holds:

- `fm1trax.fwsc`: the firmware package
- `fm1_install.py`: the installer, which sends the package to the FM-1 over USB MIDI
- `fm1_rescue.py`: the last-resort recovery tool (macOS or Linux; see "If something goes wrong")
- `LICENSE`: the GNU GPL-3.0, which FM1-TRAX is under

**No warranty.** FM1-TRAX comes "as is", without warranty of any kind. Installing custom firmware can
leave a device unable to start and may void its warranty; you install it at your own risk. Read "If
something goes wrong" below before you start.

**The easiest way** needs no Python: open https://jartificer.github.io/fm1-trax/ in Chrome, Edge or
Opera on a computer, connect the FM-1, press Connect MIDI and then Install. The steps below are for installing from this
bundle instead.

**Mind the volume** the first time you play it: start with the FM-1's volume low.

## What you need

- The FM-1 and a USB **data** cable (some cables only charge).
- A computer with **Python 3.8 to 3.12**. The installer's MIDI library (python-rtmidi) comes
  ready-built for those versions. With Python 3.13 or newer, pip has to compile it, which needs a
  C++ compiler; installing Python 3.12 next to it is simpler.
- Close any app that uses the FM-1's MIDI port (a DAW, the M-VAVE app, a web page with Web MIDI).

FM1-TRAX knows both flash chips the FM-1 has shipped with: the Puya P25Q80H of the first units
and the UCUN UC25HQ80 of FM-1s sold since autumn 2026 (logo on the front). On a unit with any other
flash it still plays, but nothing is saved and it cannot be updated from FM1-TRAX itself.

## Windows

1. Install Python 3.12 from https://www.python.org/downloads/ (tick "Add python.exe to PATH").
2. Open a Command Prompt in this folder (in Explorer: type `cmd` in the address bar and press Enter).
3. Install the MIDI library:
   ```
   py -3.12 -m pip install mido python-rtmidi
   ```
4. Connect the FM-1, switch it on, and run:
   ```
   py -3.12 fm1_install.py fm1trax.fwsc
   ```

## macOS

1. Install Python 3.12 from https://www.python.org/downloads/ (or `brew install python@3.12`).
2. Open Terminal in this folder (right-click the folder in Finder: Services, New Terminal at Folder).
3. Install the MIDI library in a virtual environment:
   ```
   python3.12 -m venv ~/fm1-venv
   ~/fm1-venv/bin/pip install mido python-rtmidi
   ```
4. Connect the FM-1, switch it on, and run:
   ```
   ~/fm1-venv/bin/python fm1_install.py fm1trax.fwsc
   ```

## Linux

1. Python 3.12 or older and pip (`sudo apt install python3 python3-venv` on Debian and Ubuntu). With a
   newer Python, also `sudo apt install build-essential libasound2-dev` so that pip can build the
   MIDI library.
2. In a terminal, in this folder:
   ```
   python3 -m venv ~/fm1-venv
   ~/fm1-venv/bin/pip install mido python-rtmidi
   ~/fm1-venv/bin/python fm1_install.py fm1trax.fwsc
   ```
   If it cannot open the MIDI system, your user may need to be in the `audio` group.

## What you will see

The installer finds the FM-1, shows the package and the firmware it is running, and asks
`Install? [y/N]`. Type `y`. It then:

1. sends the first part of the package; the FM-1 restarts into its update loader;
2. writes the whole package (about a minute, with a progress bar);
3. waits for the FM-1 to restart and checks that FM1-TRAX is running.

**Do not unplug the FM-1 or switch it off while it writes.** If the install is cut off anyway, run the
same command again: it finds the FM-1 in update mode and finishes the write.

To see what an FM-1 is running without installing anything: `fm1_install.py --info`.
If several MIDI devices are connected and it picks the wrong one: `--port NAME` (part of its name).

## In the browser instead

The FM1-TRAX web page has the same installer, over Web MIDI, in Chrome, Edge or Opera on a computer
(not Safari or Firefox). It asks for permission to use MIDI devices with SysEx: allow it.

## If something goes wrong

- **The installer does not find the FM-1**: check the cable (a data cable), close other MIDI apps, and
  try another USB port. On the FM-1, holding **OCT-** and **OCT+** together for 5 seconds puts
  FM1-TRAX into update mode, for an installer that cannot reach it otherwise.
- **The FM-1 does not start properly**: after two failed starts in a row FM1-TRAX comes up in a
  **safe mode** (no sound, USB on), so the installer can reach it: install again, or install M-VAVE's
  stock firmware. PLAY tries FM1-TRAX again.
- **Last resort**: after four failed starts the FM-1 enters the chip's own update mode.
  `fm1_rescue.py` writes the stock firmware back through it, from a Mac or Linux; its first lines explain
  how. Give it M-VAVE's FM-1 packages (V16 from m-vave.com/download, and V15 if you have it): it writes
  the one whose bootloader the FM-1 already has. `fm1_rescue.sh` fetches them for you.

## Going back to the stock firmware

Install M-VAVE's stock package the same way (`fm1_install.py FM-1.fwsc`), or use M-VAVE's own updater.
Both V15 and V16 are taken, but each brings its own bootloader, and an FM-1 takes only the version whose
bootloader it already has: one that has ever run V16 goes back to V16, one that never has to V15. Install
the other one and nothing is written; the installer says which one to use.
The installer works from the stock firmware and from any Felucca-based firmware (FM1-GM, X0X, FoMni).
