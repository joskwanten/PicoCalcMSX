// SDL2 desktop frontend for the MSX core — for development on the Mac.
// Drives the same platform-agnostic machine.h interface as the Pico build.

#include <SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "machine.h"
#include "storage.h"
#include "menu.h"

#include <stdlib.h>

#define MSX_W 256
#define MSX_H 192
#define SCALE 3 // window = 768 x 576

// SDL scancodes ARE the USB HID usage codes, so this matches the Pico USB map.
// value = (MSX matrix index + 1); 0 = unmapped.
static uint8_t sc_to_msx[SDL_NUM_SCANCODES];

static void build_map(void)
{
    for (int i = 0; i < 26; i++) sc_to_msx[SDL_SCANCODE_A + i] = (uint8_t)(22 + i) + 1; // a-z
    for (int i = 0; i < 9; i++)  sc_to_msx[SDL_SCANCODE_1 + i] = (uint8_t)(1 + i) + 1;  // 1-9
    sc_to_msx[SDL_SCANCODE_0] = 0 + 1;

    sc_to_msx[SDL_SCANCODE_RETURN] = 63 + 1;
    sc_to_msx[SDL_SCANCODE_ESCAPE] = 58 + 1;
    sc_to_msx[SDL_SCANCODE_BACKSPACE] = 61 + 1;
    sc_to_msx[SDL_SCANCODE_TAB] = 59 + 1;
    sc_to_msx[SDL_SCANCODE_SPACE] = 64 + 1;
    sc_to_msx[SDL_SCANCODE_DELETE] = 67 + 1;

    sc_to_msx[SDL_SCANCODE_MINUS] = 10 + 1;
    sc_to_msx[SDL_SCANCODE_SEMICOLON] = 15 + 1;
    sc_to_msx[SDL_SCANCODE_COMMA] = 18 + 1;
    sc_to_msx[SDL_SCANCODE_PERIOD] = 19 + 1;
    sc_to_msx[SDL_SCANCODE_SLASH] = 20 + 1;

    for (int i = 0; i < 5; i++) sc_to_msx[SDL_SCANCODE_F1 + i] = (uint8_t)(53 + i) + 1; // F1-F5

    sc_to_msx[SDL_SCANCODE_RIGHT] = 71 + 1;
    sc_to_msx[SDL_SCANCODE_LEFT] = 68 + 1;
    sc_to_msx[SDL_SCANCODE_DOWN] = 70 + 1;
    sc_to_msx[SDL_SCANCODE_UP] = 69 + 1;

    // Modifiers -> MSX Shift / Ctrl / Graph
    sc_to_msx[SDL_SCANCODE_LSHIFT] = 48 + 1;
    sc_to_msx[SDL_SCANCODE_RSHIFT] = 48 + 1;
    sc_to_msx[SDL_SCANCODE_LCTRL] = 49 + 1;
    sc_to_msx[SDL_SCANCODE_RCTRL] = 49 + 1;
    sc_to_msx[SDL_SCANCODE_LALT] = 50 + 1;
    sc_to_msx[SDL_SCANCODE_RALT] = 50 + 1;
}

// RGB565 -> ARGB8888 (the menu renders 565; the SDL texture is ARGB).
static inline uint32_t c565_to_argb(uint16_t c)
{
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0); // direct loggen (dev)

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("PicoCalcMSX (SDL)",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       MSX_W * SCALE, MSX_H * SCALE, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(ren, MSX_W, MSX_H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, MSX_W, MSX_H);

    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    SDL_PauseAudioDevice(adev, 0);

    build_map();

    // --- Load BIOS + game from the sdcard folder ---
    if (!storage_init())
        return 1;

    // Framebuffer + audio buffers.
    static uint32_t fb[MSX_W * MSX_H];    // ARGB, for the SDL texture
    static uint16_t menu565[MSX_W * MSX_H]; // RGB565, menu render target
    static uint32_t line[MSX_W];
    static int16_t audio[AUDIO_SAMPLES_PER_FRAME * 2];

    storage_entry_t ent[128];
    char bios_name[STORAGE_MAX_NAME] = "";

    int ns = storage_list(SD_SYSTEM, ent, 128);
    for (int i = 0; i < ns; i++)
        if (!ent[i].is_dir) { snprintf(bios_name, sizeof bios_name, "%s", ent[i].name); break; }
    if (!bios_name[0]) {
        fprintf(stderr, "no BIOS found in sdcard/%s/\n", SD_SYSTEM);
        return 1;
    }

    // BIOS -> 64 KB buffer (slot 0), padded with 0xFF for any read above the file.
    static uint8_t bios[65536];
    memset(bios, 0xFF, sizeof bios);
    if (storage_read(SD_SYSTEM, bios_name, bios, sizeof bios) < 0) {
        fprintf(stderr, "failed to read BIOS %s\n", bios_name);
        return 1;
    }

    // --- Boot menu: pick cartridges / disks ---
    menu_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    menu_init(bios, &cfg);
    while (!menu_start_requested()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { SDL_Quit(); return 0; }
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_UP:     menu_input(MENU_UP); break;
                case SDL_SCANCODE_DOWN:   menu_input(MENU_DOWN); break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                case SDL_SCANCODE_SPACE:  menu_input(MENU_ENTER); break;
                case SDL_SCANCODE_ESCAPE: menu_input(MENU_BACK); break;
                default: break;
                }
            }
        }
        menu_render(menu565);
        for (int i = 0; i < MSX_W * MSX_H; i++) fb[i] = c565_to_argb(menu565[i]);
        SDL_UpdateTexture(tex, NULL, fb, MSX_W * (int)sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    // --- Load the chosen cartridge (slot 1) and boot ---
    uint8_t *game = NULL;
    uint32_t game_size = 0;
    if (cfg.slot1[0]) {
        game = storage_load(SD_ROMS, cfg.slot1, &game_size);
        if (!game) {
            fprintf(stderr, "failed to load roms/%s\n", cfg.slot1);
            return 1;
        }
    }
    printf("BIOS: system/%s   slot1: %s (%u bytes)\n",
           bios_name, cfg.slot1[0] ? cfg.slot1 : "(empty)", game_size);

    if (!machine_init(bios, sizeof bios, game, game_size)) {
        fprintf(stderr, "machine_init failed\n");
        return 1;
    }

    const double freq = (double)SDL_GetPerformanceFrequency();
    uint64_t next = SDL_GetPerformanceCounter();
    bool running = true;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                uint8_t m = sc_to_msx[e.key.keysym.scancode];
                if (m) machine_keydown(m - 1);
            } else if (e.type == SDL_KEYUP) {
                uint8_t m = sc_to_msx[e.key.keysym.scancode];
                if (m) machine_keyup(m - 1);
            }
        }

        // One MSX frame.
        machine_do_cycles();
        machine_generate_interrupt();

        // Audio: queue one frame, but don't let the queue build unbounded latency.
        machine_get_audio(audio, AUDIO_SAMPLES_PER_FRAME * 2);
        if (SDL_GetQueuedAudioSize(adev) < AUDIO_SAMPLE_RATE) // < ~0.25 s buffered
            SDL_QueueAudio(adev, audio, sizeof(audio));

        // Render the VDP snapshot into the texture.
        machine_snapshot_vdp();
        for (int y = 0; y < MSX_H; y++) {
            machine_render_snapshot_line(line, y);
            memcpy(&fb[y * MSX_W], line, sizeof(line));
        }
        SDL_UpdateTexture(tex, NULL, fb, MSX_W * (int)sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        // Pace to 60 Hz.
        next += (uint64_t)(freq / HZ);
        int64_t rem = (int64_t)(next - SDL_GetPerformanceCounter());
        if (rem > 0)
            SDL_Delay((uint32_t)(rem * 1000.0 / freq));
        else
            next = SDL_GetPerformanceCounter();
    }

    SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
