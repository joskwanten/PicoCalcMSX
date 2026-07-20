#ifndef VIDEO_HSTX_H
#define VIDEO_HSTX_H

#include <stdint.h>

// HDMI/DVI display backend (RP2350 HSTX via de pico_hdmi-library) — als
// LIJN-PIPELINE ("race the beam"): er is géén framebuffer. Een lijnbron
// levert op aanvraag één MSX-lijn in RGB565 (256 of 512 breed); core 1
// rendert die 2-3 lijnen vóór de scanout in een kleine pingpong-ring en de
// HSTX-scanlinecallback expandeert 'm naar 640 pixels (256 -> verdubbeld,
// 512 -> 1:1, horizontaal gecentreerd; verticaal 2x met borders).
//
// Zo past straks óók de MSX2 (512x212, 128KB VRAM) in een kale Pico 2: de
// oude dubbele framebuffer (192KB) en VDP-snapshot vervallen.

#define VHSTX_MSX_W 256
#define VHSTX_MSX_H 192

// Lijnbron: vul dst met de RGB565-pixels van MSX-lijn `line`; schrijf de
// breedte (256 of 512) in *w. Draait op CORE 1, vlak vóór de scanout —
// leest dus de LIVE VDP-state (de beam-pacing van core 0 houdt dat klopend).
typedef void (*video_line_source_t)(uint16_t *dst, int line, int *w);

// pico_hdmi opzetten (video + audio-islands), callbacks registreren en
// core 1 starten. Systeemklok moet al goed staan (252 MHz / HSTX-div 2).
void video_hstx_init(void);

// Lijnbron (en het aantal zichtbare MSX-lijnen: 192 of 212) instellen.
// Mag on-the-fly (menu <-> machine, MSX1 <-> MSX2).
void video_hstx_set_line_source(video_line_source_t src, int lines);

// Borderkleur buiten het actieve beeld.
void video_hstx_set_border(uint16_t border565);

// Huidige beam-positie in MSX-lijnen (-1 = nog boven het actieve beeld).
// Core 0 paced hierop: emuleer lijn L pas als de scanout in de buurt komt.
int video_hstx_scan_msx_line(void);

// HDMI-frames tot nu toe (pico_hdmi's video_frame_count).
uint32_t video_hstx_frame_count(void);

#endif // VIDEO_HSTX_H
