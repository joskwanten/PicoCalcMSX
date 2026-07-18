#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HZ 60
#define MHZ 3.56
#define CYC_PER_INT (uint32_t)((MHZ * 1000000) / HZ)
#define MACHINE_LOOP_TIME (1000 / HZ)
#define AUDIO_SAMPLE_RATE 48000 // direct 48 kHz -> HDMI-native, geen resampling nodig
#define AUDIO_SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / HZ) // 800

bool machine_init();
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