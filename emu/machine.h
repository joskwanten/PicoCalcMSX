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
void machine_do_cycles();
void machine_generate_interrupt();
void machine_get_audio(int16_t* chunk, uint32_t len);
void machine_get_rendered_image_rgba(uint32_t* image);
void machine_get_rendered_line(uint32_t* line, int y);
void machine_snapshot_vdp(void);                       // core 0: kopieer VDP-state
void machine_render_snapshot_line(uint32_t* line, int y); // core 1: render uit snapshot
uint32_t machine_snapshot_background_color(void);
void machine_keydown(uint32_t index);
void machine_keyup(uint32_t index);
uint32_t machine_get_background_color();
uint32_t machine_dbg_a9_reads(void);
uint16_t machine_dbg_pc(void);
void machine_dbg_dump(void);