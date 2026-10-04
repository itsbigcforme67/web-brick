# Web Brick

Brick games, keychain games, virtual pets (Tamagotchi, Digimon) and other LCD handhelds that run in a phone or desktop browser, from their original chip programs. It installs as an app, works offline, and shows each handheld with its real faceplate: you tap the handheld's own buttons.

The chip emulation, device definitions and faceplate drawings come from **[BrickEmuPy](https://github.com/azya52/BrickEmuPy)** by azya52 (a Python/Qt desktop emulator, public domain). This project ports its cores to portable C++, compiles them to WebAssembly for the website, and keeps the same source ready for an ESP32 handheld. It is a sibling of Web Pokémon mini and Web PocketStation.

**No ROMs are included.** See *ROMs* below.

## What works now

Two chip families so far, 28 handhelds. The Holtek HT943 / HTG12N0 4-bit microcontrollers:

| Handheld | Chip | ROM |
|---|---|---|
| Brick Game E-23 Plus Mark II (96 in 1) | HT943 | published by BrickEmuPy |
| Brick Game E-88 (8 in 1) | HT943 | published by BrickEmuPy |
| Block Game & Echo Key GA888 | HT943 | published by BrickEmuPy |
| Keychain 55 in 1 | HT943 | published by BrickEmuPy |
| Keychain Pin Ball | HT943 | published by BrickEmuPy |
| Puyolin | HT943 | published by BrickEmuPy |
| Space Intruder TK-150I | HT943 | published by BrickEmuPy |
| Mickey Deluxe Virtual Game | HTG12N0 | published by BrickEmuPy |
| Mame Galaxian | HT943 | bring your own dump (untested here) |
| Mame Game Tamagotch | HT943 | bring your own dump (untested here) |

and the Seiko Epson E0C6200, the chip inside the original Bandai virtual pets:

| Handheld | ROM |
|---|---|
| Tamagotchi (P1), Tamagotchi (Japan), Tamagotchi P2 | bring your own dump |
| Angel, Mothra, Morino, Umino, Yasashii Tamagotchi, Genjintchi | bring your own dump |
| Digimon Ver. 1 (English), Digital Monster Ver. 1 (two revisions), Ver. 2, Ver. 3, Ver. 4 | bring your own dump |
| Alien Fever | published by BrickEmuPy |
| Stack Challenge (Radio Shack) | published by BrickEmuPy |
| Nikko | published by BrickEmuPy |

The first-generation Tamagotchi has been run with a real dump (`tama.b`, 12,288 bytes, works under any name when chosen from its card): it matches BrickEmuPy instruction for instruction, hatches about five minutes after a daytime clock is set, and catches up 6 hours in about 25 s. The other pets have not been run with their real programs (nobody publishes those dumps); the chip itself is checked instruction for instruction against BrickEmuPy with the three published games and with 400 random programs that hammer every I/O register (see *Tested*). The Digimon battle link is not connected yet, so battles against another device do not work.

BrickEmuPy emulates 17 more chips; they are added here one core at a time (see *Roadmap*).

## Running it

* **As a website:** `bash publish.sh` puts it on GitHub Pages (see *Publishing*).
* **From this computer:** `bash run.sh`, then open `http://localhost:8767`. It also prints an address for phones on the same Wi-Fi. (Installing as an offline app needs https or localhost, so that part only works from the published site.)

## ROMs

Each handheld needs its chip's program ROM, and the HT943 ones a small sound ROM as well. They are stored in the browser on that device and never uploaded. Three ways to add them:

* **Download from BrickEmuPy.** One button. The browser fetches the ROM files that BrickEmuPy publishes straight from its GitHub repository (pinned to the exact version this was tested against, and checked by size and SHA-1). This site hosts none of them.
* **Import files…** Pick `.bin` / `.srom` files, a zip of them, or the whole BrickEmuPy zip from GitHub ("Code → Download ZIP"); only the ROMs are read from it. You can also drop files on the page.
* **Per handheld:** tap one marked *needs ROM* to choose its files one by one. This is the route for the handhelds BrickEmuPy has no ROM for, such as the Tamagotchis and Digimon. Any file name works here; when importing in bulk, name the file as the card says (for example `TamagotchiP1.bin`). The E0C6200 ROMs are two bytes per 12-bit instruction, high byte first, as BrickEmuPy uses them.

Whether the manufacturers' ROM programs may be redistributed is not something BrickEmuPy's public-domain licence can settle, which is why this repository does not contain them and `publish.sh` refuses to upload any. If you would rather not have the download button on a public site, remove `romBase` from `web/devices/index.json`; importing still works.

## Using it

| | |
|---|---|
| Tap a handheld | Starts it, or continues where you left off. |
| The handheld's buttons | Tap them. Small ones have a larger invisible touch area. |
| ‹ back | Saves the whole machine (score, game in progress), so it resumes exactly there. |
| ⋯ in a game | Keyboard and gamepad controls for that handheld, screen effects, and "take the batteries out" (full reset). |
| ⋯ on a card | Start fresh, or remove its ROM. |
| Keyboard | BrickEmuPy's shortcuts: usually arrows or A/S/D, Space/Enter, and 1/2/3 for the small buttons. Esc goes back. |
| Gamepad | D-pad, A, Start and Select are mapped per handheld (`pad` in `web/devices/index.json`). |
| Sideways | Wide handhelds fill the screen when the phone is turned. |

The screen imitates real liquid crystal the way BrickEmuPy does: segments fade in and out, unlit segments stay faintly visible, and each casts a slight shadow. All three can be switched off.

**Virtual pets keep living while you are away.** When a pet is opened again, or its tab comes back to the front, the emulator runs through the time that passed (up to a week) as fast as the device allows, with a progress note at the bottom and a *Skip* button; a day takes roughly half a minute to a minute on a phone. Skipped time simply did not happen for the pet. This can be switched on or off per handheld under ⋯ (it is off for games). The pet's own clock still has to be set on the device the first time, as on the real thing. On the first-generation Tamagotchi: the middle button leaves the start screen, left sets the hours, middle the minutes, right confirms. Set a daytime hour: a pet set to midnight is asleep and its egg waits for the morning (that is the real game, not a fault).

## Tested

`tests/compare.py` runs the original BrickEmuPy Python core and this C++ core side by side with the same random button presses and compares, every 1,000 instructions, the program flow, every register, all RAM and the LCD, plus every sound event. All 11 handhelds with a ROM match over 2 emulated minutes each (and over 4 seconds with every single instruction compared). The same run is repeated with a save/load round trip at every checkpoint, and with the built `brick.wasm` (in Node, or with Python's `wasmtime` module where there is no Node); both stay identical.

The E0C6200 was also checked with 400 random programs (biased towards the timers, stopwatch, programmable timer, buzzer envelope, ports and clock switching), every instruction compared, all identical. BrickEmuPy keeps the E0C6200 timing counters in floating point; all of them turn out to be exact binary fractions, so the C++ keeps them as integers and still matches bit for bit.

`tests/e2e.py` drives the real app in a phone-sized Chromium: import (files and zip), download, boot of every handheld, touch, keyboard, sound, saved games, sideways layout.

## Developer notes

```
core/        the emulator: portable C++11, no heap, no libc, no floating point
  brick.h/.cpp     Machine (chip + buttons + timekeeping), Audio (tone events -> samples), save states
  ht4bit.h/.cpp    Holtek HT943 / HTG12N0 (port of BrickEmuPy's HT4BIT.py, HT943.py, HTG12N0.py, HT4BITsound.py)
  e0c6200.h/.cpp   Seiko Epson E0C6200: Tamagotchi, Digimon (port of E0C6200.py, E0C6200sound.py)
  wasm_api.cpp     the WebAssembly interface (only part that is web-specific)
build.sh     core/*.cpp -> web/core/brick.wasm (plain clang, no Emscripten). The built file is committed.
             Without clang it uses Zig's bundled clang if that is installed (pip install ziglang).
web/         the site (this folder is what gets published)
  core/brick.js    small wrapper class around brick.wasm; no DOM, also runs in Node
  app.js           library, ROM storage, the SVG faceplate, input, sound, saved games
  devices/         per handheld: BrickEmuPy's .brick definition (as .json), its faceplate .svg, a thumbnail; index.json
tools/       make_devices.py builds web/devices from a BrickEmuPy checkout (--thumbs renders the pictures)
tests/       compare.py (reference comparison), e2e.py (browser), embed_example.cpp (firmware-style use),
             wasm_trace.mjs / wasm_trace.py (the built brick.wasm, in Node or wasmtime)
```

Everything that needs BrickEmuPy takes the path of a checkout: `git clone https://github.com/azya52/BrickEmuPy ../BrickEmuPy`, then for example

```bash
python3 tests/compare.py ../BrickEmuPy --seconds 30            # needs g++, node
python3 tools/make_devices.py ../BrickEmuPy --thumbs           # needs playwright + pillow for --thumbs
bash run.sh & python3 tests/e2e.py ../BrickEmuPy               # needs playwright
```

How the faceplate works: in each SVG, the element with id `<n>_<bit>` is the LCD segment for bit `<bit>` of byte `<n>` of the chip's display memory, and elements named like the buttons (`btnLeft`, `btnStart`…) are the buttons. The page sets each segment's opacity sixty times a second; nothing is drawn by hand.

### Adding the next chip

1. Port `cores/<CHIP>.py` (and its `...sound.py`) to `core/<chip>.cpp` as a `brick::Core`, following `ht4bit.cpp`. `debugRegs()` must list the registers in the order of the Python `examine()`.
2. Register it in `coreSize()` / `createCore()` in `core/brick.cpp`, and add it to `SUPPORTED` in `tests/compare.py` and `SUPPORTED_CORES` in `tools/make_devices.py`.
3. `python3 tests/compare.py ../BrickEmuPy` until every device of that chip says OK, then `bash build.sh` and `python3 tools/make_devices.py ../BrickEmuPy --thumbs`.

Handhelds with a key matrix, EEPROM or link connector need those peripherals ported too (`peripherals/*.py`, about 400 lines in all); `make_devices.py` skips such devices until then.

### The core in firmware (ESP32, Meshtastic)

`tests/embed_example.cpp` is the pattern, and it is built and run by `compare.py`. The core was written for this:

* No `malloc`, no global constructors, no floating point, no library calls. A brick game needs about 1.5 KB of RAM (272 bytes for the machine, 1,176 for the chip) plus the ROM, which can stay in flash.
* You call `machine.run(cycles)` for however much time has passed, from any task at any priority; it never blocks and never touches hardware. A brick game needs roughly 170,000 simple instructions per second.
* Sound is optional: without `audio.init()` no samples are produced.
* `saveState()` is about 540 bytes, small enough to write to flash whenever a game is left.

What the firmware has to add: drawing the segments (masks baked from the SVG on a PC, see the notes file), mapping its buttons to `press()`, and playing the samples.

## Roadmap

1. ~~E0C6200: Tamagotchi, Digimon and friends~~ done (the 15 pets need your own ROM dump).
2. T7741 and T6770S: 13 Game & Watch-style games from Bandai, Gakken and others. Needs the key-matrix peripheral.
3. SPLB20, then the rest: SPL02/03, KS56/KS57, EM73000, MSM50XX, LC5732, D750X, and the 6502-family chips.
4. ESP32 build: a PC-side tool that bakes each faceplate's LCD into 320x240 segment masks, and a renderer for them.
5. Link play between two handhelds (Digimon battles, Tamagotchi infrared).
6. ~~Virtual pets that keep living while the app is closed~~ done (they catch up on return). A faster idle path in the core would make long absences quicker.

## Publishing

`bash publish.sh` creates a public GitHub repo from this folder, pushes the code to `main`, pushes the `web/` folder to a `gh-pages` branch and switches on GitHub Pages for it (it needs `git` and the GitHub CLI `gh`, logged in). To publish a change later: `git add -A && git commit -m "what changed" && bash publish.sh`.

Pocket Arcade links to `../web-brick/`, so publish under that name (the default) or change the link there.

## Licence and credits

Web Brick is free software under the **GNU General Public License, version 3 or later** (see `LICENSE`). It comes with no warranty.

* **BrickEmuPy** by azya52, <https://github.com/azya52/BrickEmuPy>, released into the public domain (CC0 1.0): the chip emulation this core is ported from, the device definitions in `web/devices/*.json` and the faceplate drawings in `web/devices/*.svg` (copied unchanged).
* `web/zip.js` comes from Web Pokémon mini.

This is an unofficial fan project, not affiliated with or endorsed by the makers of any of these handhelds. Product names are trademarks of their owners and are used only to say what is emulated.
