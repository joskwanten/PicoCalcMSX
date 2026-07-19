#ifndef VIDEO_HSTX_H
#define VIDEO_HSTX_H

#include <stdint.h>

// HDMI/DVI display backend (RP2350 HSTX via the pico_hdmi library).
// Replaces the SPI-LCD path when the project is built with -DBAREMSX_HDMI.
//
// The MSX image is 256x192. It is rendered by core 0 into an RGB565 back
// buffer, integer-scaled 2x to 512x384 and centered in a 640x480 @ 60 Hz
// HDMI frame (border filled with the VDP backdrop colour). Core 1 runs the
// pico_hdmi HSTX output loop and pulls scanlines from the front buffer.

#define VHSTX_MSX_W 256
#define VHSTX_MSX_H 192

// Set up pico_hdmi (video + audio-island queue), register callbacks and the
// audio background task, then launch core 1 with the HSTX output loop.
// The caller MUST have set the system clock to 126 MHz first.
void video_hstx_init(void);

// Non-blocking: returns the off-screen back buffer to render into, or NULL if
// the previously presented frame hasn't been swapped to the display yet (in
// which case the caller should skip rendering this frame). Core 0 fills it with
// VHSTX_MSX_W * VHSTX_MSX_H RGB565 pixels (row-major, 256 wide).
uint16_t *video_hstx_backbuffer(void);

// Publish the freshly rendered back buffer: it is swapped to the front at the
// next vsync. `border565` is the backdrop colour used outside the 512x384 area.
void video_hstx_present(uint16_t border565);

// HDMI frames displayed so far (pico_hdmi's video_frame_count).
uint32_t video_hstx_frame_count(void);

#endif // VIDEO_HSTX_H
