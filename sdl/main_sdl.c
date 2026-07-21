// SDL2 desktop frontend for the MSX core — for development on the Mac.
// Drives the same platform-agnostic machine.h interface as the Pico build.

#include <SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "machine.h"
#include "storage.h"
#include "zip.h"
#include "menu.h"
#include "tms9918.h"
#ifdef BAREMSX_MSX2
#include "v9938.h"
#endif

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

// Is dit de DISK.ROM in system/? (naam begint met "disk", hoofdletterongevoelig)
static bool is_disk_rom_name(const char *n)
{
    return (n[0] == 'd' || n[0] == 'D') && (n[1] == 'i' || n[1] == 'I') &&
           (n[2] == 's' || n[2] == 'S') && (n[3] == 'k' || n[3] == 'K');
}

// Sector-IO voor de WD2793: leest/schrijft het geselecteerde .dsk-image.
static char g_dsk_name[STORAGE_MAX_NAME];
static const char *g_dsk_dir = SD_DSK; // SD_CACHE voor uit een .zip gepakte .dsk
static int dsk_sector_io(void *ctx, uint32_t lba, uint8_t *buf, bool write)
{
    (void)ctx;
    long n = write
        ? storage_write_at(g_dsk_dir, g_dsk_name, lba * 512u, buf, 512)
        : storage_read_at(g_dsk_dir, g_dsk_name, lba * 512u, buf, 512);
    return n == 512 ? 0 : -1;
}

// --- diskwissel (F12): de .dsk-set uit de boot-zip ---
static char g_dsk_set[8][128];
static int g_dsk_set_n = 0, g_dsk_set_cur = 0;
static void disk_swap_next(void)
{
    if (g_dsk_set_n < 2) return;
    g_dsk_set_cur = (g_dsk_set_cur + 1) % g_dsk_set_n;
    snprintf(g_dsk_name, sizeof g_dsk_name, "%s", g_dsk_set[g_dsk_set_cur]);
    g_dsk_dir = SD_CACHE;
    long dsz = storage_size(SD_CACHE, g_dsk_name);
    if (dsz > 0)
        machine_disk_swap((dsz <= 80 * 9 * 512) ? 1 : 2, (uint32_t)dsz / 512u);
    printf("[disk] drive A <- cache/%s\n", g_dsk_name);
}

// --- .zip-transparantie: bij selectie uitpakken naar cache/ ---
static uint8_t zip_window[ZIP_WINDOW_SIZE];
static const char *ROM_EXTS[] = {".rom", ".mx1", ".mx2", ".bin", NULL};
static const char *DSK_EXTS[] = {".dsk", NULL};
// Is name_io een .zip, pak dan de eerste passende entry uit en vervang
// name_io door de cachenaam; retourneert de map om uit te laden.
static const char *maybe_unzip(const char *dir, char *name_io, const char *const *exts)
{
    if (!zip_is_zip(name_io)) return dir;
    char cached[STORAGE_MAX_NAME];
    if (!zip_extract_cached(dir, name_io, exts, zip_window, cached, sizeof cached)) {
        fprintf(stderr, "zip: geen bruikbare entry in %s/%s\n", dir, name_io);
        return dir; // gewone laadfout volgt
    }
    printf("[zip] %s -> cache/%s\n", name_io, cached);
    snprintf(name_io, STORAGE_MAX_NAME, "%s", cached);
    return SD_CACHE;
}

// Beam-sink: machine_do_cycles levert elke lijn (live VRAM, beam-positie);
// wij schrijven 'm in het actieve framebuffer.
static uint32_t *g_sink_fb = NULL;
static int g_sink_w = 256;
static void line_sink(int y, const uint32_t *px, int w)
{
    if (g_sink_fb && w == g_sink_w)
        memcpy(&g_sink_fb[(size_t)y * w], px, (size_t)w * sizeof(uint32_t));
}

// Dump een ARGB-framebuffer als PPM (headless test: kijken wat er op het
// scherm staat zonder venster-interactie).
static void dump_ppm(const char *path, const uint32_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t px[3] = { (fb[i] >> 16) & 0xFF, (fb[i] >> 8) & 0xFF, fb[i] & 0xFF };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    printf("[test] framebuffer -> %s\n", path);
}

int main(int argc, char **argv)
{
#ifdef SDL_MAIN_HANDLED
    SDL_SetMainReady(); // gelinkt zonder SDL2main (MinGW + VC-libs)
#endif
    setvbuf(stdout, NULL, _IONBF, 0); // direct loggen (dev)

    // Testvlaggen: --slot1/--diska prefillen de menukeuze en slaan het menu
    // over; --frames N + --dump pad = headless draaien en het scherm dumpen.
    const char *arg_slot1 = NULL, *arg_slot2 = NULL, *arg_diska = NULL, *arg_dump = NULL;
    const char *arg_glitch = NULL;
    int arg_frames = 0, arg_press = 0, arg_trace_from = 0, arg_trace_to = 0;
    bool arg_nomenu = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--slot1") && i + 1 < argc) arg_slot1 = argv[++i];
        else if (!strcmp(argv[i], "--slot2") && i + 1 < argc) arg_slot2 = argv[++i];
        else if (!strcmp(argv[i], "--diska") && i + 1 < argc) arg_diska = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) arg_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) arg_dump = argv[++i];
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) arg_press = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--glitch") && i + 1 < argc) arg_glitch = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 2 < argc) {
            arg_trace_from = atoi(argv[++i]);
            arg_trace_to = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--nomenu")) arg_nomenu = true;
    }
    if (arg_slot1 || arg_slot2 || arg_diska) arg_nomenu = true;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("BareMSX (SDL)",
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
    char diskrom_name[STORAGE_MAX_NAME] = "";
    char ext_name[STORAGE_MAX_NAME] = "";

    // system/: "disk*" = DISK.ROM; "msx2ext*" = sub-ROM en "msx2*" = de
    // MSX2-hoofd-BIOS (samen activeren ze het MSX2-profiel); het eerste
    // overige bestand is de MSX1-BIOS. Beide sets mogen naast elkaar staan.
    char msx2main_name[STORAGE_MAX_NAME] = "";
    int ns = storage_list(SD_SYSTEM, ent, 128);
    for (int i = 0; i < ns; i++) {
        if (ent[i].is_dir) continue;
        if (is_disk_rom_name(ent[i].name)) {
            if (!diskrom_name[0]) snprintf(diskrom_name, sizeof diskrom_name, "%s", ent[i].name);
        } else if (strncasecmp(ent[i].name, "msx2ext", 7) == 0) {
            if (!ext_name[0]) snprintf(ext_name, sizeof ext_name, "%s", ent[i].name);
        } else if (strncasecmp(ent[i].name, "msx2", 4) == 0) {
            if (!msx2main_name[0]) snprintf(msx2main_name, sizeof msx2main_name, "%s", ent[i].name);
        } else if (!bios_name[0]) {
            snprintf(bios_name, sizeof bios_name, "%s", ent[i].name);
        }
    }
    bool msx2 = ext_name[0] != 0 && msx2main_name[0] != 0;
    if (msx2) snprintf(bios_name, sizeof bios_name, "%s", msx2main_name);
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

    // DISK.ROM -> 32 KB buffer (meestal 16 KB groot).
    static uint8_t disk_rom[32768];
    long disk_rom_size = 0;
    if (diskrom_name[0]) {
        memset(disk_rom, 0xFF, sizeof disk_rom);
        disk_rom_size = storage_read(SD_SYSTEM, diskrom_name, disk_rom, sizeof disk_rom);
        if (disk_rom_size < 0) disk_rom_size = 0;
    }

    // MSX2 sub-ROM -> 64 KB gepadde buffer (rom_read maskeert op 16-bit).
    static uint8_t ext_rom[65536];
    if (msx2) {
        memset(ext_rom, 0xFF, sizeof ext_rom);
        if (storage_read(SD_SYSTEM, ext_name, ext_rom, sizeof ext_rom) < 0) {
            fprintf(stderr, "failed to read sub-ROM %s\n", ext_name);
            return 1;
        }
    }

    // --- Boot menu: pick cartridges / disks ---
    menu_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    menu_init(bios, &cfg);
    if (arg_slot1) snprintf(cfg.slot1, sizeof cfg.slot1, "%s", arg_slot1);
    if (arg_slot2) snprintf(cfg.slot2, sizeof cfg.slot2, "%s", arg_slot2);
    if (arg_diska) snprintf(cfg.diskA, sizeof cfg.diskA, "%s", arg_diska);
    while (!arg_nomenu && !menu_start_requested()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { SDL_Quit(); return 0; }
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_UP:     menu_input(MENU_UP); break;
                case SDL_SCANCODE_DOWN:   menu_input(MENU_DOWN); break;
                case SDL_SCANCODE_PAGEUP:   menu_input(MENU_PGUP); break;
                case SDL_SCANCODE_PAGEDOWN: menu_input(MENU_PGDN); break;
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

    // --- Load the chosen cartridges (slot 1 + 2) and boot ---
    uint8_t *game = NULL;
    uint32_t game_size = 0;
    if (cfg.slot1[0]) {
        const char *d1 = maybe_unzip(SD_ROMS, cfg.slot1, ROM_EXTS);
        game = storage_load(d1, cfg.slot1, &game_size);
        if (!game) {
            fprintf(stderr, "failed to load roms/%s\n", cfg.slot1);
            return 1;
        }
    }
    uint8_t *game2 = NULL;
    uint32_t game2_size = 0;
    if (cfg.slot2[0]) {
        const char *d2 = maybe_unzip(SD_ROMS, cfg.slot2, ROM_EXTS);
        game2 = storage_load(d2, cfg.slot2, &game2_size);
        if (!game2) {
            fprintf(stderr, "failed to load roms/%s\n", cfg.slot2);
            return 1;
        }
    }

    // --- Disk-interface (slot 2): DISK.ROM + WD2793, drive A = cfg.diskA ---
    // MSX2: alleen aankoppelen als er echt een disk gekozen is — met de
    // interface aanwezig maar zonder zinvolle taak hangt de boot van een
    // Konami-SCC-cart (KV2) in de disk-ROM-init (known issue, nog uitzoeken).
    if (disk_rom_size > 0 && (!msx2 || cfg.diskA[0])) {
        uint8_t sides = 0;
        uint32_t total_sectors = 0;
        if (cfg.diskA[0] && zip_is_zip(cfg.diskA)) {
            // Multi-disk-zip: alle .dsk-entries uitpakken; F12 wisselt erbinnen.
            g_dsk_set_n = zip_extract_all_cached(SD_DSK, cfg.diskA, DSK_EXTS,
                                                 zip_window, g_dsk_set, 8);
            if (g_dsk_set_n > 0) {
                snprintf(cfg.diskA, sizeof cfg.diskA, "%s", g_dsk_set[0]);
                g_dsk_dir = SD_CACHE;
                printf("[zip] disk-set: %d image(s), F12 wisselt\n", g_dsk_set_n);
            }
        }
        if (cfg.diskA[0]) {
            long dsz = storage_size(g_dsk_dir, cfg.diskA);
            if (dsz > 0) {
                snprintf(g_dsk_name, sizeof g_dsk_name, "%s", cfg.diskA);
                total_sectors = (uint32_t)dsz / 512u;
                sides = (dsz <= 80 * 9 * 512) ? 1 : 2; // 360KB enkel-, 720KB dubbelzijdig
            } else {
                fprintf(stderr, "dsk/%s not found\n", cfg.diskA);
            }
        }
        machine_attach_disk(disk_rom, (uint32_t)disk_rom_size, sides, total_sectors,
                            NULL, dsk_sector_io);
    }

    printf("BIOS: system/%s%s   diskrom: %s   slot1: %s (%u)   slot2: %s (%u)   diskA: %s\n",
           bios_name, msx2 ? " [MSX2]" : "", diskrom_name[0] ? diskrom_name : "(none)",
           cfg.slot1[0] ? cfg.slot1 : "(empty)", game_size,
           cfg.slot2[0] ? cfg.slot2 : "(empty)", game2_size,
           cfg.diskA[0] ? cfg.diskA : "(empty)");

    static uint8_t vram128k[V9938_VRAM_SIZE];
    static uint8_t sccplus_ram[65536];
    bool ok = msx2
        ? machine_init_msx2(bios, sizeof bios, ext_rom, sizeof ext_rom, game, game_size, vram128k, sccplus_ram)
        : machine_init(bios, sizeof bios, game, game_size, game2, game2_size);
    if (!ok) {
        fprintf(stderr, "machine_init failed\n");
        return 1;
    }

    // MSX2: 512x212-display -> texture + venster hierop aanpassen.
    int dw = machine_display_width(), dh = machine_display_height();
    if (dw != MSX_W || dh != MSX_H) {
        SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, dw, dh);
        SDL_RenderSetLogicalSize(ren, dw, dh * 2); // 512 breed = halve pixels
        SDL_SetWindowSize(win, dw * 3 / 2, dh * 3);
    }
    static uint32_t fb2[512 * 212];  // MSX2-framebuffer
    static uint32_t line2[512];
    (void)line2;

    // Beam-model: de machine levert lijnen tijdens machine_do_cycles.
    g_sink_fb = msx2 ? fb2 : fb;
    g_sink_w = dw;
    machine_set_line_sink(line_sink);

    const double freq = (double)SDL_GetPerformanceFrequency();
    uint64_t next = SDL_GetPerformanceCounter();
    bool running = true;
    int frame_no = 0;

    while (running) {
        // Headless test: na N frames het scherm dumpen en stoppen.
        if (arg_frames > 0 && frame_no >= arg_frames) {
            // Beam-model: framebuffers zijn al gevuld door de sink.
            if (arg_dump) dump_ppm(arg_dump, msx2 ? fb2 : fb, dw, dh);
            machine_dbg_dump();
#ifdef SCC_DEBUG
            {
                extern volatile uint32_t sccr_bf50_bank, sccr_bf50_val, sccr_bf50_hits;
                fprintf(stderr, "[bf50] bank=%u val=%02X hits=%u\n",
                        sccr_bf50_bank, sccr_bf50_val, sccr_bf50_hits);
                {
                    extern uint8_t ram[];
                    fprintf(stderr, "[bf50] E1DE=%02X pc=%04X\n", ram[0xE1DE], machine_dbg_pc());
                }
                {
                    extern tms9918_context_t tms9918;
                    extern uint8_t ram[];
                    fprintf(stderr, "[bf50] R1=%02X jiffy=%02X%02X\n",
                            tms9918.registers[1], ram[0xFC9F], ram[0xFC9E]);
                }
            }
#endif
#ifdef BAREMSX_MSX2
            if (msx2) {
                extern v9938_context_t v9938;
                fprintf(stderr, "[vdp] R0=%02X R1=%02X R7=%02X R9=%02X R15=%02X R19=%02X R23=%02X R2=%02X mode=%02X pc=%04X\n",
                        v9938.regs[0], v9938.regs[1], v9938.regs[7], v9938.regs[9],
                        v9938.regs[15], v9938.regs[19], v9938.regs[23], v9938.regs[2],
                        ((v9938.regs[1] >> 4) & 1) | ((v9938.regs[1] >> 2) & 2) | ((v9938.regs[0] & 0x0E) << 1),
                        machine_dbg_pc());
            }
#endif
            if (msx2)
                {
                    fprintf(stderr, "[ram] F7C0:");
                    for (int a = 0xF7C0; a < 0xF800; a++) fprintf(stderr, " %02X", machine_dbg_read((uint16_t)a));
                    fprintf(stderr, "\n");
                }
            if (0)
            {
                extern uint8_t ram[];
                fprintf(stderr, "[ram] 5650:");
                for (int i = 0x5650; i < 0x5680; i++) fprintf(stderr, " %02X", ram[i]);
                fprintf(stderr, "\n[ram] FC9E jiffy: %02X %02X\n", ram[0xFC9E], ram[0xFC9F]);
                fprintf(stderr, "[ram] RAMAD0-3: %02X %02X %02X %02X  EXPTBL: %02X %02X %02X %02X\n",
                        ram[0xF341], ram[0xF342], ram[0xF343], ram[0xF344],
                        ram[0xFCC1], ram[0xFCC2], ram[0xFCC3], ram[0xFCC4]);
            }
            break;
        }
        frame_no++;
        if (arg_frames > 0 && frame_no % 30 == 0)
            fprintf(stderr, "[trace] frame=%d pc=%04X\n", frame_no, machine_dbg_pc());
        // Testinjectie: spatie indrukken op frame N (10 frames lang).
        if (arg_press > 0) {
            if (frame_no == arg_press) machine_keydown(64);      // MSX-matrix: spatie
            if (frame_no == arg_press + 10) machine_keyup(64);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                if (e.key.keysym.scancode == SDL_SCANCODE_F12) { disk_swap_next(); continue; }
                uint8_t m = sc_to_msx[e.key.keysym.scancode];
                if (m) machine_keydown(m - 1);
            } else if (e.type == SDL_KEYUP) {
                uint8_t m = sc_to_msx[e.key.keysym.scancode];
                if (m) machine_keyup(m - 1);
            }
        }

        // One MSX frame.
        if (arg_trace_to > 0) {
            extern volatile int machine_trace;
            machine_trace = (frame_no >= arg_trace_from && frame_no <= arg_trace_to);
            if (machine_trace) fprintf(stderr, "[trc] ==== frame %d ====\n", frame_no);
        }
        machine_do_cycles();
        machine_generate_interrupt();

        // Glitch-detector (--glitch padprefix): vergelijk het frame met het
        // vorige; springt >50% van de (gesamplede) pixels om, dump dan
        // beide frames + de VDP-registers van dat moment. Bedoeld om de
        // sporadische "rommel-frames" in Quarth te vangen (headless is
        // deterministisch, dus het framenummer is reproduceerbaar).
        if (arg_glitch) {
            static uint32_t *gprev = NULL;
            static int gdumps = 0;
            const uint32_t *cur = msx2 ? fb2 : fb;
            size_t npx = (size_t)dw * (size_t)dh;
            if (!gprev) {
                gprev = malloc(npx * sizeof(uint32_t));
                memcpy(gprev, cur, npx * sizeof(uint32_t));
            } else {
                size_t samples = 0, diff = 0;
                for (size_t i = 0; i < npx; i += 4) {
                    samples++;
                    if (cur[i] != gprev[i]) diff++;
                }
                if (frame_no > 120 && diff * 2 > samples && gdumps < 20) {
                    char path[512];
                    snprintf(path, sizeof path, "%s_%05d_prev.ppm", arg_glitch, frame_no);
                    dump_ppm(path, gprev, dw, dh);
                    snprintf(path, sizeof path, "%s_%05d.ppm", arg_glitch, frame_no);
                    dump_ppm(path, cur, dw, dh);
                    gdumps++;
#ifdef BAREMSX_MSX2
                    if (msx2) {
                        extern v9938_context_t v9938;
                        fprintf(stderr, "[glitch] frame=%d diff=%d%% R0=%02X R1=%02X "
                                "R2=%02X R3=%02X R4=%02X R5=%02X R6=%02X R7=%02X R9=%02X "
                                "R14=%02X R19=%02X R23=%02X mode=%02X cm=%X pc=%04X\n",
                                frame_no, (int)(diff * 100 / samples),
                                v9938.regs[0], v9938.regs[1], v9938.regs[2], v9938.regs[3],
                                v9938.regs[4], v9938.regs[5], v9938.regs[6], v9938.regs[7],
                                v9938.regs[9], v9938.regs[14], v9938.regs[19], v9938.regs[23],
                                ((v9938.regs[1] >> 4) & 1) | ((v9938.regs[1] >> 2) & 2) |
                                    ((v9938.regs[0] & 0x0E) << 1),
                                machine_dbg_pc());
                    }
#endif
                }
                memcpy(gprev, cur, npx * sizeof(uint32_t));
            }
        }

        // Audio: queue one frame, but don't let the queue build unbounded latency.
        machine_get_audio(audio, AUDIO_SAMPLES_PER_FRAME * 2);
        if (SDL_GetQueuedAudioSize(adev) < AUDIO_SAMPLE_RATE) // < ~0.25 s buffered
            SDL_QueueAudio(adev, audio, sizeof(audio));

        // Beam-model: fb/fb2 is al tijdens machine_do_cycles per lijn gevuld.
        SDL_UpdateTexture(tex, NULL, msx2 ? (void *)fb2 : (void *)fb,
                          dw * (int)sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        // Pace to 60 Hz (headless test: zo snel mogelijk).
        if (arg_frames == 0) {
            next += (uint64_t)(freq / HZ);
            int64_t rem = (int64_t)(next - SDL_GetPerformanceCounter());
            if (rem > 0)
                SDL_Delay((uint32_t)(rem * 1000.0 / freq));
            else
                next = SDL_GetPerformanceCounter();
        }
    }

    SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
