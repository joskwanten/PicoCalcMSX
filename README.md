# BareMSX

An open-source **MSX1 emulator for the Raspberry Pi Pico 2 (RP2350)** with
**HDMI/DVI output over HSTX**, HDMI audio, a USB keyboard, and an SD card with
a classic MSX-styled boot menu.

Plug it into a monitor, put your ROMs on an SD card, and play — Konami SCC
games (Nemesis, Salamander, F1 Spirit, ...) run at a locked 60 Hz with sound.

There is also an SDL2 desktop build for development. (The project started as
an emulator for the [ClockworkPi PicoCalc](https://www.clockworkpi.com/picocalc) —
hence the git history — but is now HDMI-first.)

## Features

- **Emulation** — Z80 ([Zeta](https://github.com/redcode/Z80) core), TMS9918 VDP,
  AY-3-8910 PSG ([emu2149](https://github.com/digital-sound-antiques/emu2149)),
  Konami SCC, 8255 PPI keyboard.
- **Cartridge mappers** — plain 16/32K, Konami, Konami SCC, ASCII8, ASCII16,
  with automatic detection.
- **Video** — 640×480@60 DVI/HDMI straight from the RP2350's HSTX peripheral
  (via [pico_hdmi](https://github.com/fliperama86/pico_hdmi)), double-buffered,
  emulation paced to the scanout. No PIO tricks, no resistor DAC.
- **Audio** — 48 kHz stereo over HDMI data islands. No extra hardware.
- **Keyboard** — any USB keyboard on the Pico's own USB port (TinyUSB host).
- **SD card + boot menu** — FatFs over SPI; an MSX-styled menu (rendered with
  the character set from your BIOS ROM) to pick a cartridge.
- **Large ROMs** — up to 2 MB: ROMs that don't fit in RAM are staged to a
  reserved flash region and executed from XIP. Picking a big game reboots
  once (~2 s) to write flash safely; picking it again boots instantly.
- **Dual-core** — core 0 emulates, core 1 streams the HSTX video.

- **Floppy disks** — the Disk A menu entry boots .dsk images (360/720 KB)
  through an emulated WD2793 + your DISK.ROM; sector writes go back into
  the image, so games can save. Needs a real MSX1 BIOS — C-BIOS cannot
  boot disks.

Status: work in progress. The **MSX2 profile (V9938)** runs in the SDL
desktop build: SCREEN 0-8, sprite mode 2 with collision/status semantics,
and a command engine with a realistic busy-timing model (validated against
MAME's v99x8 — see `docs/V9938-MAME-DIFF.md` for the full comparison and
per-finding status). MSX2 on the Pico itself waits for PSRAM support
(128KB VRAM + mapper RAM don't fit next to the framebuffers in SRAM).

Still open on the V9938:

- **R#18 set-adjust** — screen positioning/shake effects are ignored.
- **G6/G7 VRAM interleave** — SCREEN 7/8 use a flat layout internally;
  correct within a mode, wrong for software that mixes mode families.
- **TEXT2 blink** (R#12/R#13) — 80-column cursor/highlight blinking
  (MSX-DOS2, word processors) doesn't blink yet.
- **Interlace / even-odd page flip** (R#9 IL/EO) — 512×424 images and
  flicker-transparency tricks show a single fixed page.
- **MC / SCREEN 3** — renders backdrop only.
- On the future Pico/MSX2 path: sprite status bits are computed in the
  line renderers, which is correct for the SDL beam loop but must move
  to the emulation side once core 1 does the rendering.

Other current limitations: Disk B is UI-only, and a slot 2 cartridge and
the disk drive can't be used at the same time (slot 2 is shared).

## Hardware

Everything hangs off a stock **Pico 2** (or another RP2350 board, e.g. the
WeAct RP2350B). Three connections:

### 1. DVI/HDMI — Pico DVI Sock (GPIO12–19)

The RP2350's HSTX peripheral is hard-wired to GPIO12–19, which is exactly
where the classic [Pico DVI Sock](https://github.com/Wren6991/Pico-DVI-Sock)
solders on. No resistors needed — HSTX drives the pairs directly.

| GPIO (pair)   | DVI signal |
|---------------|-----------|
| GP12 / GP13   | D0+ / D0− |
| GP14 / GP15   | CLK+ / CLK− |
| GP16 / GP17   | D2+ / D2− |
| GP18 / GP19   | D1+ / D1− |

(The `bit[]` routing in `third_party/pico_hdmi` is already remapped for this
sock layout — stock pico_hdmi expects CLK on GP12/13.)

### 2. SD card — SPI0 (GPIO2–5)

Any 3.3 V micro-SD breakout (the bare kind without a level shifter). FAT32
formatted; SDHC/SDXC cards work (tested with 64 GB).

| SD module | Pico pin |
|-----------|----------|
| SCK / CLK | GP2 (pin 4) |
| MOSI / DI | GP3 (pin 5) |
| MISO / DO | GP4 (pin 6) |
| CS        | GP5 (pin 7) |
| VCC       | 3V3 (pin 36) |
| GND       | GND |

> **Solder a decoupling capacitor (100 nF, plus a few µF bulk) directly
> across VCC–GND on the SD module.** Without it the card may answer commands
> but return all-zero data blocks — the current spike of a real NAND read
> browns out the card. Ask us how we know.

### 3. USB keyboard — the Pico's own USB port

The keyboard plugs into the Pico's micro-USB port through an OTG adapter.
Since that port is now a *host* port, the board must be powered externally
**with 5 V on VBUS** (pin 40) — the keyboard is powered from VBUS, so 3.3 V
on VSYS alone is not enough.

## SD card layout

```
/system/   MSX BIOS + DISK.ROM (see below)
/roms/     cartridge ROM images (.rom)
/dsk/      floppy images (.dsk, 360/720 KB raw sector dumps)
/hdd/      Nextor hard-disk images (future)
```

In `system/`, the first file whose name starts with `disk` is used as the
DISK.ROM; the first other file is the MSX BIOS.

> **How many files per folder?** The boot menu lists at most **128 files**
> per folder — any beyond that are simply not shown. On the **Pico** there's
> a stricter practical limit of **64 files** in `roms/` (and in `dsk/`):
> booting a ROM too big for RAM (>40 KB — most Konami SCC games) stages it to
> flash across a reboot, and that path only re-scans the first 64 directory
> entries, so a large ROM listed past #64 won't boot. The SDL desktop build
> isn't affected by the 64 limit (only the 128 display cap). Listings are in
> raw filesystem order, not sorted — use type-to-find in the menu to jump
> straight to a title. (The menu browses `roms/` and `dsk/` directly and
> doesn't descend into subfolders, so keep the files you want to boot in
> those folders and trim the rest.)

> **Recommended combo for disk support: a Philips VG-8020 BIOS + the
> VY-0010 disk ROM.** That pairing is a genuine 1985 Dutch MSX1 setup
> (the VY-0010 was *the* external drive for these machines), and both
> ROMs match the emulated hardware exactly: MSX1 BIOS, WD2793 FDC with
> the Philips register layout. An MSX2 DISK.ROM (e.g. the NMS-8245's)
> mostly works too, but it makes MSX2 assumptions — if a disk misbehaves,
> switch to the VY-0010 ROM first. No ROMs are distributed with this project.
For a cartridge-only setup the free [C-BIOS](https://cbios.sourceforge.net/)
(`cbios_main_msx1.rom`) works fine; for floppy disks you need the BIOS of
a real MSX1 (C-BIOS cannot boot disks) — dump one from a machine you own.

### Boot menu

| Key | Action |
|-----|--------|
| ↑ / ↓ | navigate |
| PgUp / PgDn | jump a page through the file list |
| A-Z / 0-9 | type-to-find: filter the file list on name prefix |
| Backspace | erase a search letter (or clear the field on the main screen) |
| Enter / Space | select / open browser |
| Esc | back |

Pick a ROM for Slot 1, select **Start**. A solid blue screen at boot means no
SD card (or no BIOS in `system/`) was found. **F11** returns from a game to
the menu (a soft reset on the Pico; the SDL build restarts fresh), and so
does the reset button.

## Building

Requires the [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x and the
ARM GNU toolchain.

```sh
# 1. Fetch the Zeta Z80 core (not bundled)
sh tools/setup-zeta.sh

# 2. Configure + build — Release is REQUIRED (Debug is too slow for the
#    HSTX scanline callback and gives a black screen)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Drop `build/BareMSX.uf2` onto the Pico in BOOTSEL mode, or flash
`build/BareMSX.elf` over SWD with a debug probe.

### Build options

| Option | Default | |
|--------|---------|---|
| `BAREMSX_USB_KEYBOARD` | ON | USB keyboard via TinyUSB host |
| `BAREMSX_SD` | ON | SD card + boot menu (FatFs) |
| `BAREMSX_BAKED_ROMS` | auto | Bake a fallback BIOS+game into the binary (only if you generated the headers yourself with `tools/rom_to_header.py`; OFF keeps the binary free of copyrighted material) |

### Desktop build (SDL2)

For development there is a desktop frontend that reads a `sdcard/` folder
(same layout as the SD card above) next to the executable, or from
`$MSX_SDCARD`. It needs CMake and the SDL2 development libraries
(`libsdl2-dev` on Debian/Ubuntu, `sdl2` in Homebrew) — the Pico SDK and ARM
toolchain are *not* required:

```sh
sh tools/setup-zeta.sh        # once, if you haven't already
cmake -S sdl -B sdl/build -DCMAKE_BUILD_TYPE=Release
cmake --build sdl/build
./sdl/build/msx_sdl
```

Handy dev flags: `--slot1 game.rom` / `--diska image.dsk` preselect the menu,
`--nomenu` skips it, and `--frames N --dump out.ppm` runs headless for N
frames and dumps the screen — useful for scripted VDP regression checks.
For hunting intermittent glitches: `--glitch prefix` dumps any frame that
differs >50% from the previous one (plus the VDP registers at that moment),
and `--trace F1 F2` logs per-line changes to the split-sensitive VDP
registers for frames F1..F2. Headless runs are deterministic, so a glitch
frame number reproduces exactly.

#### Windows (scoop)

```powershell
scoop install mingw cmake ninja extras/sdl2
sh tools/setup-zeta.sh        # from Git Bash
cmake -S sdl -B sdl/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build sdl/build
sdl\build\msx_sdl.exe
```

Open a *new* terminal after `scoop install` so the updated `PATH` and
`CMAKE_PREFIX_PATH` are picked up. The scoop `sdl2` package ships the VC
development libraries without their CMake config files;
`sdl/cmake/FindSDL2.cmake` compensates, and under MinGW it skips the
MSVC-built `SDL2main.lib` (the frontend calls `SDL_SetMainReady()` itself).
`SDL2.dll` is copied next to the exe automatically.

## Credits

- **Zeta Z80 CPU core** — Manuel Sainz de Baranda y Goñi ([redcode/Z80](https://github.com/redcode/Z80)), **LGPL-3.0-or-later**; fetched separately by `tools/setup-zeta.sh` (not bundled), full sources available for relinking as the LGPL requires.
- **emu2149** (AY-3-8910/YM2149) — Mitsutaka Okazaki (MIT).
- **pico_hdmi** — fliperama86 (Unlicense), vendored in `third_party/` with a
  DVI-Sock pin remap.
- **FatFs** — ChaN (BSD-style), vendored in `third_party/`.
- **TinyUSB** — via the Pico SDK (MIT).
- **C-BIOS** — the C-BIOS Association, the recommended free MSX BIOS.

Third-party components keep their own licenses. Everything else is
MIT © Jos Kwanten — see [LICENSE](LICENSE).
