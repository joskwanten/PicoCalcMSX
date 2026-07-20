#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "wd2793.h" // wd_sector_io_t (disk-interface)

#define HZ 60
#define MHZ 3.56
#define CYC_PER_INT (uint32_t)((MHZ * 1000000) / HZ)
#define MACHINE_LOOP_TIME (1000 / HZ)
#define AUDIO_SAMPLE_RATE 48000 // direct 48 kHz -> HDMI-native, geen resampling nodig
#define AUDIO_SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / HZ) // 800

// Initialiseer de machine met een BIOS (slot 0) en een cartridge-ROM (slot 1).
// De ROM-data leeft buiten de core (baked-in flash op de Pico, of geladen van
// de sdcard) — de aanroeper levert de pointers + groottes.
// game = cartridge in slot 1, game2 = cartridge in slot 2 (beide optioneel,
// NULL/0 = leeg). Een SCC-game in slot 2 bankt wel maar heeft geen SCC-geluid.
bool machine_init(const uint8_t *bios, uint32_t bios_size,
                  const uint8_t *game, uint32_t game_size,
                  const uint8_t *game2, uint32_t game2_size);

// Optioneel, aanroepen VÓÓR machine_init: hang een disk-interface in slot 2
// (DISK.ROM op 0x4000-0x7FFF + WD2793 memory-mapped op 0x7FF8-0x7FFF).
// `sides` 0 = lege drive; sector-IO loopt via de callback (512B-sectors).
void machine_attach_disk(const uint8_t *disk_rom, uint32_t disk_rom_size,
                         uint8_t sides, uint32_t total_sectors,
                         void *io_ctx, wd_sector_io_t io);

// Diskwissel in drive A tijdens het draaien (F12): nieuwe geometrie, zelfde
// sector-IO-callback. No-op zonder aangesloten disk-interface.
void machine_disk_swap(uint8_t sides, uint32_t total_sectors);

// MSX2-profiel (alleen beschikbaar in builds met BAREMSX_MSX2; de Pico kan
// dit kan sinds de lijn-pipeline óók op een kale Pico 2). bios = 32KB main-
// BIOS, ext = sub-ROM (begrensd op ext_size), vram128k = door de host
// geleverd 128KB-VRAM (op de Pico: de menu-arena — het menu is dan klaar).
// sccplus_ram64k: optioneel 64KB voor een Konami Sound Cartridge (SCC-I) in
// slot 2 — vereist door Snatcher/SD Snatcher. NULL = geen Sound Cartridge
// (op de kale Pico is er geen 64KB vrij; wacht op PSRAM).
bool machine_init_msx2(const uint8_t *bios, uint32_t bios_size,
                       const uint8_t *ext, uint32_t ext_size,
                       const uint8_t *game, uint32_t game_size,
                       uint8_t *vram128k, uint8_t *sccplus_ram64k);
bool machine_is_msx2(void);
int machine_display_width(void);  // 256 (MSX1) of 512 (MSX2)
int machine_display_height(void); // 192 (MSX1) of 212 (MSX2)
void machine_do_cycles();
void machine_generate_interrupt();
void machine_get_audio(int16_t* chunk, uint32_t len);
// Beam-gebaseerd renderen ("race the beam", zonder snapshot/framebuffer in
// de core): registreer een sink en machine_do_cycles levert elke zichtbare
// displaylijn aan op het moment dat de geëmuleerde beam 'm passeert,
// gerenderd uit LIVE VRAM. w = 256 (MSX1) of 512 (MSX2). Zonder sink geldt
// het oude snapshot-model (Pico-HDMI-pad, tot de video_hstx-refactor).
typedef void (*machine_line_sink_t)(int y, const uint32_t *px, int w);
void machine_set_line_sink(machine_line_sink_t sink);

// Pico-varianten van het beam-model:
//  - machine_do_line(line): emuleer precies één beam-lijn (0..261) — 228
//    T-states Z80 + de VDP-events van die lijn. De host paced dit op de
//    echte HSTX-scanout.
//  - machine_render_line_565(dst, y): render displaylijn y uit de LIVE
//    VDP-state naar RGB565 (aangeroepen op core 1, vlak vóór de scanout).
//    Retourneert de breedte (256 of 512).
void machine_do_line(int line);
int machine_render_line_565(uint16_t *dst, int y);
uint16_t machine_border_565(void);

void machine_keydown(uint32_t index);
void machine_keyup(uint32_t index);
uint32_t machine_get_background_color();
uint32_t machine_dbg_a9_reads(void);
uint16_t machine_dbg_pc(void);
void machine_dbg_dump(void);
uint8_t machine_dbg_read(uint16_t addr);