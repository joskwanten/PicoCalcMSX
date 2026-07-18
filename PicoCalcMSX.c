#include <stdio.h>
#include "pico/stdlib.h" // trekt pico.h (__not_in_flash_func) mee
#include "pico/multicore.h"
#include "hardware/sync.h" // __dmb
#include "machine.h"
#include "i2ckbd.h"
#include "keymap.h"
#include "keycodes.h"

// PicoCalc MSX host.
//   Core 0: emulatie (Z80/VDP/keyboard), gepaced op 60 Hz.
//   Core 1: beelduitvoer.
//     - HDMI-build (PICOCALC_HDMI): pico_hdmi HSTX-outputloop, leest de
//       framebuffer die core 0 rendert; geluid via HDMI data islands.
//     - LCD-build (default fallback): blit van de VDP-snapshot naar de ILI9488.

#define MSX_W 256
#define MSX_H 192
#define FRAME_US 16667 // 60 Hz

// Zet de FPU (CP10/CP11 in CPACR) aan voor de huidige core. Nodig omdat de
// build met hardware-FP (-mfloat-abi=softfp +fp) code genereert die FPU-
// instructies gebruikt (o.a. de SDK-timer-IRQ die sleep_us triggert), terwijl
// de FPU hier niet door de runtime-init aan blijkt te staan -> anders NOCP-fault.
static inline void enable_fpu(void) {
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20); // CP10 + CP11 full access
    __asm volatile("dsb");
    __asm volatile("isb");
}

// ARGB (0x00RRGGBB) -> RGB565.
static inline uint16_t argb_to_565(uint32_t px) {
    return (uint16_t)(((px >> 8) & 0xF800) | ((px >> 5) & 0x07E0) | ((px >> 3) & 0x001F));
}

#ifdef PICOCALC_HDMI
// ===================== HDMI/DVI backend (RP2350 HSTX) =======================
#include "hardware/clocks.h"
#include "video_hstx.h"
#include "audio_hdmi.h"

static uint32_t line_argb[MSX_W]; // één gerenderde MSX-regel (ARGB)

// Render de huidige VDP-snapshot naar de RGB565 back buffer en toon 'm.
// Niet-blokkerend: als de display de vorige frame nog niet heeft geswapt,
// slaan we deze frame over (de emulatie loopt onafhankelijk door op 60 Hz).
static void render_frame_hdmi(void) {
    uint16_t *bb = video_hstx_backbuffer();
    if (!bb) return;
    machine_snapshot_vdp();
    for (int y = 0; y < MSX_H; y++) {
        machine_render_snapshot_line(line_argb, y);
        uint16_t *dst = &bb[y * MSX_W];
        for (int x = 0; x < MSX_W; x++) dst[x] = argb_to_565(line_argb[x]);
    }
    video_hstx_present(argb_to_565(machine_snapshot_background_color()));
}
#else
// ========================= LCD backend (ILI9488) ============================
#include "lcd.h"
#include "audio.h"

#define ORIG_X ((LCD_WIDTH - MSX_W) / 2)   // 32  (1:1 gecentreerd)
#define ORIG_Y ((LCD_HEIGHT - MSX_H) / 2)  // 64

// Opgeschaald beeld (aspectcorrect 4:3): 256x192 -> 320x240, gecentreerd.
#define SCALE_W 320
#define SCALE_H 240
#define SCALE_X ((LCD_WIDTH - SCALE_W) / 2)   // 0
#define SCALE_Y ((LCD_HEIGHT - SCALE_H) / 2)  // 40

static uint32_t line_argb[MSX_W];          // één gerenderde MSX-regel (ARGB) - core 1
static uint8_t  rgb_line[2][SCALE_W * 3];  // ping-pong RGB888 (max breedte = opgeschaald)
static uint16_t xmap[SCALE_W];             // bron-x per scherm-x (nearest-neighbor)

// Handshake tussen de cores
static volatile bool g_render_req = false;  // core0 -> core1: snapshot klaar, blit 'm
static volatile bool g_render_done = true;  // core1 -> core0: snapshot vrij
static volatile uint32_t g_blit_count = 0;  // display-fps teller
static volatile int g_scale_mode = 0;       // 0 = origineel 1:1, 1 = opgeschaald 320x240

static uint32_t last_border = 0;
static volatile bool border_valid = false;

// Rand met backdrop-kleur — layout hangt af van de mode.
static void fill_border(uint32_t argb) {
    uint8_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    if (g_scale_mode == 0) {
        // Origineel 256x192 gecentreerd -> rand aan alle vier de kanten
        lcd_fill_rect_rgb(0, 0, LCD_WIDTH, ORIG_Y, r, g, b);                       // boven
        lcd_fill_rect_rgb(0, ORIG_Y + MSX_H, LCD_WIDTH,
                          LCD_HEIGHT - (ORIG_Y + MSX_H), r, g, b);                 // onder
        lcd_fill_rect_rgb(0, ORIG_Y, ORIG_X, MSX_H, r, g, b);                      // links
        lcd_fill_rect_rgb(ORIG_X + MSX_W, ORIG_Y,
                          LCD_WIDTH - (ORIG_X + MSX_W), MSX_H, r, g, b);           // rechts
    } else {
        // Opgeschaald 320x240 (volle breedte) -> alleen boven/onder
        lcd_fill_rect_rgb(0, 0, LCD_WIDTH, SCALE_Y, r, g, b);
        lcd_fill_rect_rgb(0, SCALE_Y + SCALE_H, LCD_WIDTH,
                          LCD_HEIGHT - (SCALE_Y + SCALE_H), r, g, b);
    }
}

// Blit de VDP-snapshot naar de LCD (core 1, RAM). Twee modes.
static void __not_in_flash_func(draw_frame_snapshot)(void) {
    int mode = g_scale_mode;

    uint32_t border = machine_snapshot_background_color();
    if (!border_valid || border != last_border) {
        fill_border(border);
        last_border = border;
        border_valid = true;
    }

    int cur = 0;
    if (mode == 0) {
        // Origineel 1:1
        lcd_begin_blit(ORIG_X, ORIG_Y, MSX_W, MSX_H);
        for (int y = 0; y < MSX_H; y++) {
            machine_render_snapshot_line(line_argb, y);
            uint8_t *dst = rgb_line[cur];
            for (int i = 0; i < MSX_W; i++) {
                uint32_t px = line_argb[i];
                dst[i * 3 + 0] = (px >> 16) & 0xFF;
                dst[i * 3 + 1] = (px >> 8) & 0xFF;
                dst[i * 3 + 2] = px & 0xFF;
            }
            lcd_push_dma(dst, MSX_W * 3);
            cur ^= 1;
        }
    } else {
        // Opgeschaald 320x240 (nearest-neighbor)
        lcd_begin_blit(SCALE_X, SCALE_Y, SCALE_W, SCALE_H);
        int last_my = -1;
        for (int sy = 0; sy < SCALE_H; sy++) {
            int my = (sy * MSX_H) / SCALE_H;
            if (my != last_my) {
                machine_render_snapshot_line(line_argb, my);
                last_my = my;
            }
            uint8_t *dst = rgb_line[cur];
            for (int sx = 0; sx < SCALE_W; sx++) {
                uint32_t px = line_argb[xmap[sx]];
                dst[sx * 3 + 0] = (px >> 16) & 0xFF;
                dst[sx * 3 + 1] = (px >> 8) & 0xFF;
                dst[sx * 3 + 2] = px & 0xFF;
            }
            lcd_push_dma(dst, SCALE_W * 3);
            cur ^= 1;
        }
    }
    lcd_end_blit();
}

// Core 1: wacht op een snapshot en blit die.
static void __not_in_flash_func(core1_main)(void) {
    while (true) {
        while (!g_render_req) tight_loop_contents();
        g_render_req = false;
        __dmb(); // snapshot-writes van core 0 zichtbaar maken
        draw_frame_snapshot();
        __dmb();
        g_render_done = true;
        g_blit_count++;
    }
}
#endif // PICOCALC_HDMI

#ifdef PICOCALC_I2C_KEYBOARD
// Toetsenbord pollen; gedeeld tussen beide backends.
static void poll_keyboard(void) {
    static bool ctrl_down = false, alt_down = false;
    (void)ctrl_down; (void)alt_down;
    uint8_t kst, kcd;
    for (int n = 0; n < 8 && kbd_read(&kst, &kcd); n++) {
        bool down = (kst == KBD_STATE_PRESSED || kst == KBD_STATE_HOLD);
        (void)down;

#ifndef PICOCALC_HDMI
        // Host-hotkey: Ctrl + Alt samen = beeldschaal togglen (LCD-only)
        if (kcd == PC_KEY_CTRL) ctrl_down = down;
        else if (kcd == PC_KEY_ALT) alt_down = down;
        if (kst == KBD_STATE_PRESSED &&
            (kcd == PC_KEY_CTRL || kcd == PC_KEY_ALT) && ctrl_down && alt_down) {
            g_scale_mode ^= 1;
            border_valid = false; // rand opnieuw tekenen (layout wijzigt)
        }
#endif

        // Delete = debug-dump van de Z80-staat (tijdelijk, hang-diagnose)
        if (kcd == 0xD4) {
            if (kst == KBD_STATE_PRESSED) machine_dbg_dump();
            continue;
        }
        keymap_handle(kst, kcd);
    }
}
#endif // PICOCALC_I2C_KEYBOARD

int main(void)
{
    enable_fpu(); // core 0 FPU aan vóór iets FP-achtigs draait (o.a. sleep_us-IRQ)

#ifdef PICOCALC_HDMI
    // 252 MHz sysclk + HSTX-deler 2 -> 25.2 MHz pixelklok (640x480@60), maar de
    // CPU draait 2x zo snel als bij 126 MHz -> genoeg headroom voor emulatie +
    // rendering op 60 fps. (Vereist MODE_HSTX_CLK_DIV=2 in de pico_hdmi-build.)
    set_sys_clock_khz(252000, true);
#endif

    stdio_init_all();
    sleep_ms(750);

#ifndef PICOCALC_HDMI
    lcd_init();
    lcd_fill_screen(LCD_BLACK);
#endif
#ifdef PICOCALC_I2C_KEYBOARD
    kbd_init();
#endif

    if (!machine_init()) {
#ifndef PICOCALC_HDMI
        lcd_fill_screen(LCD_RED);
#endif
        while (true) tight_loop_contents();
    }

#ifdef PICOCALC_HDMI
    video_hstx_init();   // pico_hdmi + audio-island queue; start core 1
#else
    audio_init();
    // Nearest-neighbor x-mapping: scherm-x -> bron-x (256 breed -> 320 breed)
    for (int sx = 0; sx < SCALE_W; sx++)
        xmap[sx] = (uint16_t)((sx * MSX_W) / SCALE_W);
    multicore_launch_core1(core1_main); // core 0 emuleert, core 1 blit
#endif

    uint32_t frame = 0;
    uint32_t emu_frames = 0;
    uint64_t sec_t0 = time_us_64();

    while (true) {
#ifndef PICOCALC_HDMI
        uint64_t t0 = time_us_64();
#endif

#ifdef PICOCALC_I2C_KEYBOARD
        // Toetsenbord pollen (I2C @10kHz is duur): elke 4e frame
        if (frame % 4 == 0) poll_keyboard();
#endif

        // Eén MSX-frame emuleren
        machine_do_cycles();
        machine_generate_interrupt();

#ifdef PICOCALC_HDMI
        audio_hdmi_generate();   // resample emu-audio -> ring (core 1 pompt naar HDMI)
        render_frame_hdmi();     // render naar back buffer + present (swap op vsync)
#else
        // Audio-samples voor dit frame genereren en naar de PWM-DMA voeden
        audio_service();

        // Snapshot doorgeven aan core 1 als die klaar is met de vorige
        if (g_render_done) {
            machine_snapshot_vdp();
            __dmb();
            g_render_done = false;
            g_render_req = true;
        }
#endif

        frame++;
        emu_frames++;

#ifdef PICOCALC_HDMI
        // Pace op de HDMI-frame (zoals de bouncing_box-demo): wacht tot de HSTX-
        // scanout een frame verder is. Geen sleep_us -> geen SDK-timer-alarm-IRQ
        // met FP-instructies (die gaf een NOCP-HardFault), en de emulatie loopt
        // netjes in lockstep met de 60 Hz display-uitvoer.
        {
            uint32_t f = video_hstx_frame_count();
            while (video_hstx_frame_count() == f) tight_loop_contents();
        }
#else
        // LCD: 60 Hz via sleep_us.
        uint64_t elapsed = time_us_64() - t0;
        if (elapsed < FRAME_US) sleep_us(FRAME_US - elapsed);
#endif

        // Eens per seconde: emulatie-fps + display-fps
        if (time_us_64() - sec_t0 >= 1000000) {
#ifdef PICOCALC_HDMI
            static uint32_t last_disp = 0;
            uint32_t disp = video_hstx_frame_count();
            printf("[fps] emu=%lu display=%lu  pc=%04X\n",
                   (unsigned long)emu_frames, (unsigned long)(disp - last_disp),
                   machine_dbg_pc());
            last_disp = disp;
#else
            printf("[fps] emu=%lu display=%lu  pc=%04X\n",
                   (unsigned long)emu_frames, (unsigned long)g_blit_count,
                   machine_dbg_pc());
            g_blit_count = 0;
#endif
            emu_frames = 0;
            sec_t0 = time_us_64();
        }
    }
}
