#include "video_hstx.h"
#include "audio_hdmi.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"

#include "pico_hdmi/video_output.h"
#include "pico_hdmi/hstx_data_island_queue.h"

// --- Output geometry: 256x192 -> 2x -> 512x384, centered in 640x480 ---
#define OUT_W 640
#define OUT_H 480
#define X_OFF ((OUT_W - VHSTX_MSX_W * 2) / 2) // 64
#define Y_OFF ((OUT_H - VHSTX_MSX_H * 2) / 2) // 48

// Word (2-pixel) indices into the 320-word scanline buffer.
#define WORD_LEFT  (X_OFF / 2)                    // 32  first active word
#define WORD_RIGHT ((X_OFF + VHSTX_MSX_W * 2) / 2) // 288 first border word on the right

// Double-buffered RGB565 framebuffers. front/back are only ever swapped by the
// vsync callback (core 1); core 0 writes fb[back] and reads/writes nothing that
// core 1 relies on except via swap_pending / border565.
static uint16_t fb[2][VHSTX_MSX_W * VHSTX_MSX_H];
static volatile int fb_front = 0;
static volatile int fb_back = 1;
static volatile bool swap_pending = false;
static volatile uint16_t border565 = 0;

// Fill one HDMI scanline (320 uint32_t words = 640 RGB565 pixels). Runs on
// core 1 during h-blank, so it must stay cheap: no rendering, just copy+double.
static void __not_in_flash_func(scanline_cb)(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    const uint16_t b = border565;
    const uint32_t bw = (uint32_t)b | ((uint32_t)b << 16);

    if (active_line < Y_OFF || active_line >= Y_OFF + VHSTX_MSX_H * 2) {
        for (int w = 0; w < OUT_W / 2; w++) dst[w] = bw;
        return;
    }

    const int my = (int)(active_line - Y_OFF) >> 1; // vertical 2x
    const uint16_t *row = &fb[fb_front][my * VHSTX_MSX_W];

    int w = 0;
    for (; w < WORD_LEFT; w++) dst[w] = bw;             // left border
    for (; w < WORD_RIGHT; w++) {                       // active: 1 MSX px -> 1 word (2x horiz)
        uint16_t px = row[w - WORD_LEFT];
        dst[w] = (uint32_t)px | ((uint32_t)px << 16);
    }
    for (; w < OUT_W / 2; w++) dst[w] = bw;             // right border
}

// Swap buffers at the top of each frame if core 0 has a new one ready.
static void __not_in_flash_func(vsync_cb)(void)
{
    if (swap_pending) {
        int t = fb_front;
        fb_front = fb_back;
        fb_back = t;
        __dmb();
        swap_pending = false;
    }
}

// Core 1 entry: zet eerst de FPU aan (CP10/CP11), draai dan de HSTX-outputloop.
// De pico_hdmi-loop/IRQ's kunnen FPU-instructies bevatten; zonder dit -> NOCP.
static void core1_entry(void)
{
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
    __asm volatile("dsb");
    __asm volatile("isb");
    video_output_core1_run();
}

void video_hstx_init(void)
{
    hstx_di_queue_init();
    audio_hdmi_init();

    video_output_init(OUT_W, OUT_H);
    video_output_set_scanline_callback(scanline_cb);
    video_output_set_vsync_callback(vsync_cb);
    // Audio is drained into the Data Island queue from core 1's idle loop.
    video_output_set_background_task(audio_hdmi_pump);

    multicore_launch_core1(core1_entry);
}

uint16_t *video_hstx_backbuffer(void)
{
    // Non-blocking: if the last presented frame hasn't been swapped in yet,
    // return NULL so the caller skips rendering this frame. This decouples the
    // emulation (paced at 60 Hz) from the HDMI scanout rate — blocking here
    // would slave the emulator to the display and run the MSX at the wrong speed.
    if (swap_pending) return NULL;
    return fb[fb_back];
}

void video_hstx_present(uint16_t border)
{
    border565 = border;
    __dmb(); // back-buffer pixel writes visible before the swap is requested
    swap_pending = true;
}

uint32_t video_hstx_frame_count(void)
{
    return video_frame_count;
}
