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

Status: work in progress. MSX1 only (MSX2/V9938 is on the roadmap); the
Disk A/B menu entries are UI-only for now.

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
/system/   system ROMs — the first file found is used as the MSX BIOS
/roms/     cartridge ROM images (.rom)
/dsk/      disk images (future)
/hdd/      Nextor hard-disk images (future)
```

No ROMs are distributed with this project. For a fully free setup, use the
open-source [C-BIOS](https://cbios.sourceforge.net/) (`cbios_main_msx1.rom`)
as the BIOS; otherwise dump the BIOS of a machine you own.

### Boot menu

| Key | Action |
|-----|--------|
| ↑ / ↓ | navigate |
| Enter / Space | select / open browser |
| Esc / Backspace | back |

Pick a ROM for Slot 1, select **Start**. A solid blue screen at boot means no
SD card (or no BIOS in `system/`) was found. The reset button returns from a
game to the menu.

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
(same layout) next to the executable, or from `$MSX_SDCARD`:

```sh
cmake -S sdl -B sdl/build -DCMAKE_BUILD_TYPE=Release
cmake --build sdl/build
./sdl/build/msx_sdl
```

## Credits

- **Zeta Z80 CPU core** — Manuel Sainz de Baranda y Goñi ([redcode/Z80](https://github.com/redcode/Z80)); fetched separately by `tools/setup-zeta.sh`.
- **emu2149** (AY-3-8910/YM2149) — Mitsutaka Okazaki (MIT).
- **pico_hdmi** — fliperama86 (Unlicense), vendored in `third_party/` with a
  DVI-Sock pin remap.
- **FatFs** — ChaN (BSD-style), vendored in `third_party/`.
- **TinyUSB** — via the Pico SDK (MIT).
- **C-BIOS** — the C-BIOS Association, the recommended free MSX BIOS.

Third-party components keep their own licenses. Everything else is
MIT © Jos Kwanten — see [LICENSE](LICENSE).
