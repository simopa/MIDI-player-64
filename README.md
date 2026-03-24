# MIDI PLAYER 64

A Commodore 64 player that reads a MIDI file from disk and plays it on the SID chip.

MIDI and the C64 never really met, even though they are more or less from the same era.
There are interfaces and cartridges that let a C64 send MIDI data to external synths,
but a direct MIDI file player on C64 is uncommon.
MIDI PLAYER 64 is built to close that gap.

![MIDI PLAYER 64 screen](artifacts/midiplayer64.png)

## Features

- Supports MIDI Type 0 and Type 1.
- Implemented events: Note On, Note Off, Meta Tempo (`FF 51`).
- Playback on 3 SID voices with automatic note allocation.
- Disk MIDI filename: primary fallback is `MIDI`.

## Repository Layout

- `src/main.c` C source for the player.
- `artifacts/player.prg` precompiled standalone player included in the repository.
- `scripts/build.sh` build + D64 creation + MIDI selection menu.
- `scripts/c64disk` alternative utility to create a D64 using `c1541`.
- `c64build` quick command (wrapper for `scripts/build.sh`).
- `c64disk` quick command (wrapper for `scripts/c64disk`).

## Requirements

- [Oscar64](https://github.com/drmortalwombat/oscar64) in your `PATH` (`oscar64`).
- Optional: `c1541` (VICE), if you want to use `c64disk`.

## Bundled PRG Artifact

The repository includes a tracked prebuilt PRG at `artifacts/player.prg`.

`./c64disk` uses `build/player.prg` if available, otherwise it automatically falls back to `artifacts/player.prg`.
This lets you test disk creation with `c64disk` even before running a local compile.

Important: `player.prg` contains only the player code, not MIDI song data.
At runtime it reads MIDI from device 8.
It first tries fixed names (`MIDI`, `MJ`, `SONG`) and then automatically scans
the disk directory for PRG/SEQ candidates.

## Build

Put one or more `.mid` / `.midi` files in the project root, then run:

```bash
./c64build
```

The script shows a menu (`1`, `2`, `3`, ...) and creates:

- `build/player.prg`
- `build/midi-player-64.d64`
- `build/midi-player-64_fresh_YYYYMMDD_HHMMSS.d64`

Useful commands:

```bash
./c64build --list
./c64build 2
./c64build file_name.mid
```

## Run In VICE

1. `File -> Attach disk image -> Drive 8` and select `build/midi-player-64.d64`.
2. On C64:

```basic
LOAD"*",8,1
RUN
```

## Troubleshooting

- Run `LOAD"$",8` then `LIST`: you should see at least `PLAYER` and `MIDI`.
- If `LIST` is empty, the wrong disk image or drive is attached in VICE.
- If you only hear an initial click, check SID volume and VICE audio settings.
- Press `STOP` during playback to interrupt.

## Practical Notes

- MIDI files are not versioned: this repository only contains source and scripts.
- Build always copies the selected MIDI into the disk image as C64 filename `MIDI`.
- `./c64build` also refreshes `artifacts/player.prg` from the latest compiled player.

## License

BSD 3-Clause. See [LICENSE](LICENSE).
