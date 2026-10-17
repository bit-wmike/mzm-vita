# Metroid: Zero Mission – PS Vita port

A native PlayStation Vita port of *Metroid: Zero Mission*, built from the
[metroidret/mzm](https://github.com/metroidret/mzm) decompilation. The game
code runs natively; the GBA hardware (memory, DMA, interrupts, sound FIFO,
picture unit) is reproduced in C.

German setup guide: [`platform/vita/README.md`](platform/vita/README.md).
The original decompilation README is in [`docs/README-decomp.md`](docs/README-decomp.md).

**No game data is included.** The port needs your own dump of the European
ROM (`sha1: 0fd107445a42e6f3a3e5ce8c865f412583179903`, five languages). At
start-up it reads graphics, sounds and other data from that file.

## Features

- Wide view (282×160) and full view (368×208, default) with a fixed zoom;
  menus and close-ups stay at 240×160.
- Triangle fires a missile, quick save / quick load, cheats.
- In-game menu (START + SELECT): view, display style (smooth / sharp /
  pixel), 30 fps mode, controls, cheats. Settings in `settings.ini`.
- Frame pacing that skips drawing a frame instead of slowing the game down.
- Saves (`mzm_eu.sav`, 32 KiB) are compatible with GBA emulators.

## Building

Needs [VitaSDK](https://vitasdk.org) and `arm-none-eabi` binutils (for the
sound data and the assembler-to-C translation).

```sh
make -f Makefile.port TARGET=vita vpk     # ZeroMission_Vita_<version>.vpk
make -f Makefile.port TARGET=host         # 32-bit Linux test runner (headless)
```

The build itself does not need the ROM. The host test runner does
(`MZM_ROM=path`, see `platform/host/main_host.c` for its options).

## Layout

| Path | Contents |
|------|----------|
| `port/` | GBA hardware layer, view, settings, quick save, cheats |
| `port/ppu/` | Software GBA PPU (from VirtuaPPU, GPL-3.0) |
| `platform/vita/` | Vita front end (vita2d, audio, input, menu) |
| `platform/host/` | Headless Linux runner for automated tests |
| `tools/port/` | Build tools: asset loader generator, asm→C translator, sound data |

Game source changes for the port are marked with `#ifdef PORT`.

## License

- Decompilation and port code: MIT (see [`LICENSE`](LICENSE)).
- `port/ppu/`: GPL-3.0-or-later (see [`port/ppu/LICENSE`](port/ppu/LICENSE)).
  Because it is linked into the Vita build, the VPK as a whole is
  distributed under the GPL-3.0; the complete source is in this repository.
- Vita libraries: see [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md).

Metroid is a trademark of Nintendo. This project is not affiliated with or
endorsed by Nintendo.
