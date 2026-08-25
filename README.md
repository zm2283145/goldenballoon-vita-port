<p align="center">
  <img src="dist/web/assets/hero.jpg" alt="Golden Balloon" width="820">
</p>

<h1 align="center">Golden Balloon</h1>

<p align="center">
  A native and browser source port of the 1997 Nintendo 64 kart racer, built on
  the community <a href="https://github.com/DavidSM64/Diddy-Kong-Racing">decompilation</a>.<br>
  Not an emulator: the game is compiled for your machine and runs from a ROM you
  already own. No game assets are included.
</p>

<p align="center">
  <a href="https://akratch.github.io/golden-balloon/"><b>▶ Play in your browser</b></a>
  &nbsp;·&nbsp;
  <a href="https://github.com/akratch/goldenballoon/releases/latest"><b>⬇ Download the latest release</b></a>
</p>

## Quick start

1. **Get the game.** Download the build for your platform from the
   [latest release](https://github.com/akratch/goldenballoon/releases/latest),
   or use the [browser version](https://akratch.github.io/golden-balloon/)
   (needs a WebGPU browser — current Chrome or Edge).
2. **Provide your ROM.** You supply a legally acquired dump of the original
   game — US 1.1 or European 1.1, as `.z64`, `.v64`, or `.n64`. macOS and
   Windows open a file picker; on Linux, drag the ROM onto the window or paste
   its path. In the browser the ROM is read locally and never leaves your
   machine. Not sure which release you have? Drop it on the
   [ROM checker](https://akratch.github.io/golden-balloon/rom-check.html): it
   names the revision and tells you whether this port runs it.
3. **Play.** A gamepad is recommended and fully remappable, with rumble. On the
   keyboard, arrows/WASD steer, `X` accelerates, `Z` brakes, `Space`
   hops/power-slides, `Shift` uses items, and `Enter` is Start. `F1` pauses and
   opens settings mid-game.

First-launch notes:

- **macOS**: the app is unsigned. If macOS warns about an unidentified
  developer, use **System Settings → Privacy & Security → Open Anyway**
  ([Apple's guide](https://support.apple.com/102445)).
- **Windows**: the app is not code-signed. If SmartScreen warns, choose
  **More info → Run anyway**.
- **Windows portable data**: to keep settings, saves, and add-ons beside
  `GoldenBalloon.exe`, create an empty `portable.txt` next to it. Otherwise, if
  the user-data folder is not writable, the game stores data beside itself and
  reports where.

## Features

- **Widescreen** with the original field of view and art direction preserved,
  plus aspect, FOV, and internal-supersampling controls. The classic 4:3 look
  is one setting away (`--pure`).
- **Higher framerates without changing the game.** Frame limits run from the
  original cap up to uncapped, with a "just under display" option for
  variable-refresh screens, a battery-friendly 40 for handhelds, and optional
  motion smoothing. The simulation, physics, music, and lap times always run at
  their authored pace; smoothing only adds presentation-only in-between images.
- **Settings that apply while you play.** Frame limit, motion smoothing,
  presentation mode, and most video options take effect on the next frame.
- **An optional camera that stays clear of walls**, with a reduced-motion
  option. Off by default; the authored camera is the default everywhere.
- **Local multiplayer** for 2–4 players, split-screen, with per-controller
  bindings.
- **Three bonus racers.** Taj, Wizpig, and Terry are playable with their own
  vehicles. Unlock Wizpig by beating him a second time or with `WIZPIGPOWER`,
  and Terry via the Dino Domain rematch or `TERRYFLY`. Bonus-racer runs cannot
  overwrite the retail Time Trial records or ghosts.
- **Magic Codes that remember safe choices.** Reversible, unlocked codes come
  back after a restart; progression-changing, one-shot, credits, and lockout
  codes never reactivate on their own.
- **PAL support.** The European ROM keeps its original timing and pitch, with
  smooth presentation available on 60 Hz displays. German is in the language
  menu on every ROM revision.
- **Persistent saves.** EEPROM progress, ghosts, and settings persist,
  including in the browser, with export/import in the launcher.

## Platforms

| Platform | Status |
|---|---|
| Windows (x64) | Portable zip, no install |
| macOS (Apple silicon) | DMG. WebGPU with the Restored presentation is the qualified visual path; OpenGL is diagnostic-only |
| Linux (x86-64) | AppImage and tarball, SDL2 bundled. GPU and desktop support varies |
| Browser | Current Chrome/Edge with WebGPU. The same engine compiled to WebAssembly |

Every release file ships with a `.provenance.json` recording the exact source
commit and checksum it was built from.

## Accessibility

The desktop app gathers keyboard and gamepad menu navigation, UI scaling, and
reduced motion under **Settings → Accessibility**. It does not present itself to
a screen reader, so it is not advertised as screen-reader compatible, and it has
no contrast control.

## Known limitations

- The complete start-to-credits campaign is not automated or claimed complete.
- Linux does not yet have a native **Choose ROM File** dialog. Drag the ROM
  onto the window or paste its path instead.
- The Remastered presentation (lighting, shadows, SDF text) is opt-in and still
  in progress.
- Only US 1.1 and EU 1.1 are supported. Other revisions (US 1.0, EU 1.0, and
  the Japanese release) are recognized and refused by name; the
  [ROM checker](https://akratch.github.io/golden-balloon/rom-check.html) tells
  you which one you have and what is still missing for it.

## Custom content

You can replace the game's textures and music with your own. Put a folder or a
`.zip` in `mods/` — beside your save data on macOS and Windows, or beside the
game on a portable build — containing a small `pack.ini` that names the pack and
a `textures/` or `music/` folder. The game loads it at launch, and **Tab**
toggles replaced textures off and on while you play so you can compare. Settings
→ Content lists what loaded and names anything it skipped, with the reason.

Nothing is bundled or hosted here: a pack is files you or someone else made, on
your own machine. Replacement models and characters are not supported.

See [docs/MODDING.md](docs/MODDING.md) for details, including
`tools/mod_texture_dump.py`, which writes out every texture the game draws under
the exact filename a pack needs.

## No game data is included

No ROM, textures, audio, music, models, or level data are distributed here or in
any release. Golden Balloon reads everything at runtime from your own copy of
the game, and every release artifact is scanned to confirm it. We do not condone
piracy. See [DISCLAIMER.md](DISCLAIMER.md) and [NOTICE.md](NOTICE.md).

## Building from source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mdkr64 --rom /path/to/your.z64
```

Prerequisites: CMake 3.16+, a C11 toolchain, pkg-config, SDL2, python3, and
network access at configure time (the WebGPU runtime is fetched and
hash-verified). On macOS: `brew install cmake pkg-config sdl2`. Running
`./build/mdkr64` with no ROM opens the launcher.

Browser build (needs [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
**4.0.10**, the version CI pins — `./emsdk install 4.0.10 && ./emsdk activate 4.0.10`):

```bash
tools/web/build_web.sh
python3 -m http.server -d dist/web 8000
```

Useful flags: `--pure|--restored|--remastered`, `--video-set Key=Value`,
`--video-list`, `--aspect`, `--fov`, `--window-size`. `MDKR_RENDERER=webgpu|gl`
selects the backend.

## For developers

The game logic is the vendored decompilation; everything that runs it on a
modern machine (renderer, audio engine, input, pacing, saves, web build) is
first-party code under [`platform/`](platform/). Start here:

- [CONTRIBUTING.md](CONTRIBUTING.md) — build, rules, and what a fix ships with
- [docs/DEVELOPER_HANDBOOK.md](docs/DEVELOPER_HANDBOOK.md) — architecture and
  the 64-bit/endianness bug shapes behind most defects here
- [docs/README.md](docs/README.md) — full documentation index
- [tests/README.md](tests/README.md) — the regression suite
- [ROADMAP.md](ROADMAP.md) — deferred work and what would close it

The browser build publishes to
[`akratch/golden-balloon`](https://github.com/akratch/golden-balloon) (note the
hyphen); code changes belong in this repository.

## Credits

- [DavidSM64](https://github.com/DavidSM64/Diddy-Kong-Racing) and the decomp
  contributors, without whom none of this exists
- The sm64ex / fast3d lineage, for the renderer architecture this builds on
- [ares](https://ares-emu.net/), the reference emulator behind the visual
  comparisons
- [DKR-R](https://github.com/ThatGuyMcd/DKR-R) by
  [ThatGuyMcd](https://github.com/ThatGuyMcd), whose presentation-identity work
  informed several of Golden Balloon's interpolation safety rules
- The original developers at Rare, whose work this project exists to study and
  preserve

## Legal

[LICENSE](LICENSE) (MIT, first-party code only) ·
[NOTICE.md](NOTICE.md) (per-component provenance) ·
[DISCLAIMER.md](DISCLAIMER.md) (full position)

Unaffiliated with and unendorsed by any rights holder. All trademarks belong to
their respective owners. If you are a rights holder with a concern, open an issue
and it will be addressed promptly.
