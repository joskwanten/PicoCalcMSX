#include "video_hstx.h"
#include "audio_hdmi.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"

#include "pico_hdmi/video_output.h"
#include "pico_hdmi/hstx_data_island_queue.h"

// --- uitvoergeometrie: actief gebied is altijd 512 schermpixels breed ---
#define OUT_W 640
#define OUT_H 480
#define X_OFF ((OUT_W - 512) / 2)  // 64 px border links/rechts
#define WORD_LEFT (X_OFF / 2)      // 32 woorden border
#define WORD_ACTIVE (512 / 2)      // 256 woorden actief

// --- lijn-pipeline ---
// Slot = lijnnummer % RING_N. Producer (core 1-idle) rendert vooruit; de
// scanlinecallback consumeert. ring_line[i] = welk MSX-lijnnummer er in slot
// i klaarstaat (-1 = leeg); pas gezet NA het vullen (dmb) zodat de consumer
// nooit een half gevulde lijn ziet.
#define RING_N 4
static uint16_t ring[RING_N][512];
static volatile int16_t ring_line[RING_N] = {-1, -1, -1, -1};
static volatile uint16_t ring_w[RING_N];

// Diagnose (uitleesbaar via SWD): callback-aanroepen, pipeline-misses,
// geproduceerde lijnen.
volatile uint32_t vhstx_cb_calls, vhstx_miss, vhstx_produced;

static video_line_source_t line_source = NULL;
static volatile int src_lines = VHSTX_MSX_H; // 192 of 212
static volatile uint16_t border565 = 0;
static volatile uint32_t scan_active_line = 0; // laatst gescande HDMI-lijn

static inline int y_off(void) { return (OUT_H - 2 * src_lines) / 2; }

// HDMI-scanline vullen (320 woorden = 640 px). Draait op core 1 in de
// h-blank: alleen kopiëren/expanderen, nooit renderen.
static void __not_in_flash_func(scanline_cb)(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    scan_active_line = active_line;
    vhstx_cb_calls++;

    const uint16_t b = border565;
    const uint32_t bw = (uint32_t)b | ((uint32_t)b << 16);
    const int yo = y_off();

    if ((int)active_line < yo || (int)active_line >= yo + 2 * src_lines) {
        for (int w = 0; w < OUT_W / 2; w++) dst[w] = bw;
        return;
    }

    const int my = ((int)active_line - yo) >> 1; // verticale 2x
    const int slot = my % RING_N;

    int w = 0;
    for (; w < WORD_LEFT; w++) dst[w] = bw;

    if (ring_line[slot] == my) {
        const uint16_t *row = ring[slot];
        if (ring_w[slot] == 512) {
            const uint32_t *src32 = (const uint32_t *)row; // 2 px per woord, 1:1
            for (int i = 0; i < WORD_ACTIVE; i++) dst[w + i] = src32[i];
        } else {
            for (int i = 0; i < 256; i++) {                // 256 px -> 2x breed
                uint16_t px = row[i];
                dst[w + i] = (uint32_t)px | ((uint32_t)px << 16);
            }
        }
        w += WORD_ACTIVE;
    } else {
        // Pipeline-miss (hoort niet te gebeuren): border i.p.v. rommel.
        vhstx_miss++;
        for (int i = 0; i < WORD_ACTIVE; i++) dst[w + i] = bw;
        w += WORD_ACTIVE;
    }

    for (; w < OUT_W / 2; w++) dst[w] = bw;
}

// Producer: render de eerstvolgende lijnen (venster van 3) vooruit op de
// scanout. Draait als core 1-achtergrondtaak, samen met de audio-pomp.
static void __not_in_flash_func(pipeline_task)(void)
{
    // Audio eerst: de data-island-queue is latency-kritischer dan de
    // lijnproducer (die 2-3 lijnen speling heeft).
    audio_hdmi_pump();

    video_line_source_t src = line_source;
    if (src) {
        int n = src_lines;
        int yo = y_off();
        int sl = (int)scan_active_line;
        // Beam-positie in MSX-lijnen; vóór/onder het beeld -> volgende frame
        // voorbereiden vanaf lijn 0.
        int base = (sl < yo) ? -1
                 : (sl >= yo + 2 * n) ? -1
                 : ((sl - yo) >> 1);
        for (int k = 1; k <= 3; k++) {
            int L = (base + k) % n;
            if (L < 0) L += n;
            int slot = L % RING_N;
            if (ring_line[slot] != L) {
                int w = 256;
                src(ring[slot], L, &w);
                ring_w[slot] = (uint16_t)w;
                __dmb(); // lijndata zichtbaar vóór het label
                ring_line[slot] = (int16_t)L;
                vhstx_produced++;
            }
        }
    }
}

// Vsync: beam-teller resetten zodat core 0's pacing de nieuwe frame ziet.
static void __not_in_flash_func(vsync_cb)(void)
{
    scan_active_line = 0;
}

// Core 1: FPU aan (CP10/CP11), dan de HSTX-outputloop draaien.
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
    video_output_set_background_task(pipeline_task);

    multicore_launch_core1(core1_entry);
}

void video_hstx_set_line_source(video_line_source_t src, int lines)
{
    line_source = NULL;
    __dmb();
    src_lines = (lines == 212) ? 212 : 192;
    for (int i = 0; i < RING_N; i++) ring_line[i] = -1; // ring invalideren
    __dmb();
    line_source = src;
}

void video_hstx_set_border(uint16_t border)
{
    border565 = border;
}

int video_hstx_scan_msx_line(void)
{
    int yo = y_off();
    int sl = (int)scan_active_line;
    if (sl < yo) return -1;
    int my = (sl - yo) >> 1;
    return my >= src_lines ? src_lines : my;
}

uint32_t video_hstx_frame_count(void)
{
    return video_frame_count;
}
