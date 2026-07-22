#include <stdio.h>
#include "pico/stdlib.h" // trekt pico.h (__not_in_flash_func) mee
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
#include "machine.h"
#include "video_hstx.h"
#include "audio_hdmi.h"
#ifdef BAREMSX_BAKED_ROMS
#include "bios_rom.h"  // ingebakken fallback-BIOS (zelf genereren, zie README)
#include "nemesis2.h"  // ingebakken fallback-cartridge
#endif
#ifdef BAREMSX_USB_KEYBOARD
#include "usbkbd.h"
#endif
#ifdef BAREMSX_SD
#include "storage.h"
#include "zip.h"
#include "menu.h"
#include "flash_stage.h"
#include "hardware/watchdog.h"
#include <string.h>

// Menu-keuze die een flash-staging vereist, doorgegeven over een zachte reboot
// heen (flashen kan alleen vóórdat de HSTX-video draait). scratch[0] = magic,
// scratch[1] = index in de roms/-listing, scratch[2] = index+1 in de dsk/-
// listing (0 = geen disk). (scratch[4..7] gebruikt de SDK zelf.)
#define BOOT_STAGE_MAGIC 0x53544147u // "STAG"

// Is dit de DISK.ROM in system/? (naam begint met "disk", hoofdletterongevoelig)
static bool is_disk_rom_name(const char *n)
{
    return (n[0] == 'd' || n[0] == 'D') && (n[1] == 'i' || n[1] == 'I') &&
           (n[2] == 's' || n[2] == 'S') && (n[3] == 'k' || n[3] == 'K');
}

// Sector-IO voor de WD2793: leest/schrijft het geselecteerde .dsk-image op SD.
static char g_dsk_name[STORAGE_MAX_NAME];
static const char *g_dsk_dir = SD_DSK; // SD_CACHE voor een uit .zip gepakte .dsk
static int dsk_sector_io(void *ctx, uint32_t lba, uint8_t *buf, bool write)
{
    (void)ctx;
    long n = write
        ? storage_write_at(g_dsk_dir, g_dsk_name, lba * 512u, buf, 512)
        : storage_read_at(g_dsk_dir, g_dsk_name, lba * 512u, buf, 512);
    return n == 512 ? 0 : -1;
}
#endif

// BareMSX host (RP2350):
//   Core 0: emulatie (Z80/VDP/keyboard), gepaced op de HSTX-frameteller.
//   Core 1: HDMI/DVI-beelduitvoer over HSTX (pico_hdmi); geluid via data
//           islands, gepompt als core 1-achtergrondtaak.

#define MSX_W 256
#define MSX_H 192

// Zet de FPU (CP10/CP11 in CPACR) aan voor de huidige core. Nodig omdat de
// build met hardware-FP (-mfloat-abi=softfp +fp) code genereert die FPU-
// instructies gebruikt (o.a. de SDK-timer-IRQ die sleep_us triggert), terwijl
// de FPU hier niet door de runtime-init aan blijkt te staan -> anders NOCP-fault.
static inline void enable_fpu(void) {
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20); // CP10 + CP11 full access
    __asm volatile("dsb");
    __asm volatile("isb");
}

// --- lijnbronnen voor de video-pipeline (core 1 trekt lijnen op aanvraag) ---

// VDP-arena (128KB), dubbel gebruikt: vóór de machine start is het eerste
// 96KB het menu-framebuffer; start het MSX2-profiel, dan wordt de héle arena
// het V9938-VRAM (machine_init_msx2 wist 'm — het menu is dan klaar).
static uint8_t vdp_arena[128 * 1024] __attribute__((aligned(4)));
#define menu_fb ((uint16_t *)vdp_arena)
#ifdef BAREMSX_SD
// .zip-transparantie: pak bij selectie de eerste passende entry uit naar
// cache/ en vervang de naam in-place; retourneert de map om uit te laden.
// Het 32KB-inflatevenster is de staart van de vdp_arena: vrij zolang alleen
// het menu (96KB) rendert, en helemaal vrij in het pre-video-stagingpad.
static const char *ZIPROM_EXTS[] = {".rom", ".mx1", ".mx2", ".bin", NULL};
static const char *ZIPDSK_EXTS[] = {".dsk", NULL};
// Diskwissel (F12): de .dsk-set uit de boot-zip; alleen dan is er iets te
// wisselen (losse .dsk's kunnen mid-game niet uit een zip worden gepakt).
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

static const char *maybe_unzip(const char *dir, char *name_io, const char *const *exts)
{
    if (!zip_is_zip(name_io)) return dir;
    char cached[STORAGE_MAX_NAME];
    if (!zip_extract_cached(dir, name_io, exts, vdp_arena + 96 * 1024, cached, sizeof cached)) {
        printf("[zip] geen bruikbare entry in %s/%s\n", dir, name_io);
        return dir; // gewone laadfout volgt
    }
    printf("[zip] %s -> cache/%s\n", name_io, cached);
    snprintf(name_io, STORAGE_MAX_NAME, "%s", cached);
    return SD_CACHE;
}
#endif

static void menu_line_source(uint16_t *dst, int line, int *w)
{
    memcpy(dst, &menu_fb[line * MSX_W], MSX_W * sizeof(uint16_t));
    *w = MSX_W;
}

volatile uint32_t dbg_core0_frames = 0;
volatile uint32_t dbg_overruns = 0; // >1 display-flip per emulatieframe (te traag)
// XIP-integriteit: som van de eerste 4KB van het (gestagede) game-ROM,
// per frame herberekend; mismatches tellen = runtime-XIP-corruptie.
volatile uint32_t dbg_xip_ref = 0, dbg_xip_bad = 0, dbg_xip_checks = 0;
static const uint8_t *dbg_xip_ptr = NULL;
static uint32_t xip_sum(const uint8_t *p) {
    uint32_t s = 0;
    for (int i = 0; i < 4096; i++) s = (s * 33) ^ p[i];
    return s;
}

int main(void)
{
    enable_fpu(); // core 0 FPU aan vóór iets FP-achtigs draait (o.a. sleep_us-IRQ)

    // 252 MHz sysclk + HSTX-deler 2 -> 25.2 MHz pixelklok (640x480@60), maar de
    // CPU draait 2x zo snel als bij 126 MHz -> genoeg headroom voor emulatie +
    // rendering op 60 fps. (Vereist MODE_HSTX_CLK_DIV=2 in de pico_hdmi-build.)
    // NB: 378 MHz (HSTX-div 3, vreg 1.30V, QMI-div 3) is geprobeerd voor 50%
    // meer Z80-headroom maar bleek instabiel — waarschijnlijk omdat alle
    // peripheral-klokken (SD-SPI!) meeschalen; apart experiment voor later.
    set_sys_clock_khz(252000, true);

    stdio_init_all();
    sleep_ms(750);

#ifdef BAREMSX_USB_KEYBOARD
    usbkbd_init(); // TinyUSB host op de native USB-poort
#endif

    // BIOS + game bepalen: van SD (met boot-menu) of ingebakken als fallback.
#ifdef BAREMSX_BAKED_ROMS
    const uint8_t *use_bios = bios_rom;
    uint32_t use_bios_size = BIOS_ROM_SIZE;
    const uint8_t *use_game = game_rom;
    uint32_t use_game_size = GAME_ROM_SIZE;
#else
    // Publiceerbare build: geen copyrighted materiaal in de binary; de BIOS
    // móet dan van de SD-kaart komen (system/).
    const uint8_t *use_bios = NULL;
    uint32_t use_bios_size = 0;
    const uint8_t *use_game = NULL;
    uint32_t use_game_size = 0;
#endif
    const uint8_t *use_game2 = NULL;
    uint32_t use_game2_size = 0;

#ifdef BAREMSX_SD
    static uint8_t sd_bios[65536];
    bool boot_msx2 = false;      // beide msx2*-bestanden in system/ aanwezig
    uint32_t msx2ext_size = 0;   // sub-ROM-grootte (staat op sd_bios+32K)
    static menu_config_t cfg;
    cfg.mapper1 = -1; // auto-detect (menu kan overrulen; staged-boot houdt auto)
    static storage_entry_t ent[64];
    bool sd_ok = storage_init();
    const uint8_t *staged_game = NULL;
    uint32_t staged_size = 0;

    // Reboot-staging: als het menu vóór de reboot een (grote) ROM koos, flash
    // die dan NU — de HSTX-video draait nog niet, dus dit is het enige veilige
    // moment voor flash_range_erase/program.
    uint32_t s2idx = 0; // slot 2-keuze (roms-index+1) over de reboot heen
    if (sd_ok && watchdog_hw->scratch[0] == BOOT_STAGE_MAGIC) {
        uint32_t idx = watchdog_hw->scratch[1];
        uint32_t didx = watchdog_hw->scratch[2];
        s2idx = watchdog_hw->scratch[3];
        watchdog_hw->scratch[0] = 0;
        watchdog_hw->scratch[2] = 0;
        watchdog_hw->scratch[3] = 0;
        // Disk A-keuze (index+1 in de dsk/-listing) mee over de reboot heen.
        if (didx) {
            int nd = storage_list(SD_DSK, ent, 64);
            if ((int)(didx - 1) < nd && !ent[didx - 1].is_dir)
                snprintf(g_dsk_name, sizeof g_dsk_name, "%s", ent[didx - 1].name);
        }
        if (didx && g_dsk_name[0] && zip_is_zip(g_dsk_name)) {
            g_dsk_set_n = zip_extract_all_cached(SD_DSK, g_dsk_name, ZIPDSK_EXTS,
                                                 vdp_arena + 96 * 1024, g_dsk_set, 8);
            if (g_dsk_set_n > 0) {
                snprintf(g_dsk_name, sizeof g_dsk_name, "%s", g_dsk_set[0]);
                g_dsk_dir = SD_CACHE;
            }
        }
        int nr = storage_list(SD_ROMS, ent, 64);
        if ((int)idx < nr && !ent[idx].is_dir) {
            char nm[STORAGE_MAX_NAME];
            snprintf(nm, sizeof nm, "%s", ent[idx].name);
            const char *d = maybe_unzip(SD_ROMS, nm, ZIPROM_EXTS);
            staged_game = flash_stage_rom(d, nm, &staged_size);
        }
    }
#endif

    video_hstx_init();   // HDMI-output + core 1 starten (het menu rendert hierin)

#ifdef BAREMSX_SD
    static char diskrom_name[STORAGE_MAX_NAME];
    if (sd_ok) {
        // system/: "disk*" = DISK.ROM, "msx2ext*" = MSX2-sub-ROM, "msx2*" =
        // MSX2-hoofd-BIOS, eerste overige bestand = de MSX1-BIOS. Zijn de
        // beide msx2-bestanden aanwezig, dan boot de Pico het MSX2-profiel;
        // anders MSX1.
        char bios_name[STORAGE_MAX_NAME] = "";
        char msx2_name[STORAGE_MAX_NAME] = "", msx2ext_name[STORAGE_MAX_NAME] = "";
        int ns = storage_list(SD_SYSTEM, ent, 64);
        for (int i = 0; i < ns; i++) {
            if (ent[i].is_dir) continue;
            if (is_disk_rom_name(ent[i].name)) {
                if (!diskrom_name[0]) snprintf(diskrom_name, sizeof diskrom_name, "%s", ent[i].name);
            } else if (strncasecmp(ent[i].name, "msx2ext", 7) == 0) {
                if (!msx2ext_name[0]) snprintf(msx2ext_name, sizeof msx2ext_name, "%s", ent[i].name);
            } else if (strncasecmp(ent[i].name, "msx2", 4) == 0) {
                if (!msx2_name[0]) snprintf(msx2_name, sizeof msx2_name, "%s", ent[i].name);
            } else if (!bios_name[0]) {
                snprintf(bios_name, sizeof bios_name, "%s", ent[i].name);
            }
        }
#ifdef BAREMSX_MSX2
        boot_msx2 = msx2_name[0] && msx2ext_name[0];
#else
        if (msx2_name[0])
            printf("[boot] msx2* genegeerd (build zonder BAREMSX_MSX2)\n");
#endif
        if (boot_msx2) {
            // MSX2-set in sd_bios pakken: hoofd-BIOS (32KB) op [0..32K),
            // sub-ROM op [32K..48K). Het menu-font komt uit de hoofd-BIOS
            // (CGTABL wijst gewoon binnen de eerste 32KB).
            memset(sd_bios, 0xFF, sizeof sd_bios);
            storage_read(SD_SYSTEM, msx2_name, sd_bios, 32768);
            long es = storage_read(SD_SYSTEM, msx2ext_name, sd_bios + 32768, 16384);
            msx2ext_size = es > 0 ? (uint32_t)es : 0;
            use_bios = sd_bios;
            use_bios_size = 32768;
            printf("[boot] MSX2-profiel: %s + %s\n", msx2_name, msx2ext_name);
        } else if (bios_name[0]) {
            memset(sd_bios, 0xFF, sizeof sd_bios);
            storage_read(SD_SYSTEM, bios_name, sd_bios, sizeof sd_bios);
            use_bios = sd_bios;
            use_bios_size = sizeof sd_bios;
        }
        if (use_bios) {

            if (staged_game) {
                // Zojuist gestaged (na de menu-reboot): direct booten, geen menu.
                use_game = staged_game;
                use_game_size = staged_size;
                if (s2idx) { // slot 2-keuze van vóór de reboot herstellen
                    int nr = storage_list(SD_ROMS, ent, 64);
                    if ((int)(s2idx - 1) < nr && !ent[s2idx - 1].is_dir)
                        snprintf(cfg.slot2, sizeof cfg.slot2, "%s", ent[s2idx - 1].name);
                }
            } else {
                // Boot-menu: rendert in menu_fb; core 1 scant het uit via
                // de menu-lijnbron. 0x52BD = MSX-blauw (565).
                memset(&cfg, 0, sizeof cfg);
                menu_init(sd_bios, &cfg);
                video_hstx_set_border(0x52BD);
                video_hstx_set_line_source(menu_line_source, MSX_H);
                usbkbd_menu_mode(true);
                menu_render(menu_fb); // eenmalig; daarna alleen bij input
                while (!menu_start_requested()) {
                    usbkbd_task();
                    int ev;
                    bool dirty = false;
                    while ((ev = usbkbd_menu_poll()) >= 0) {
                        if (ev >= USBKBD_MENU_CHAR_BASE) menu_char((char)(ev & 0xFF));
                        else menu_input((menu_input_t)ev);
                        dirty = true;
                    }
                    if (dirty) menu_render(menu_fb); // geen continue herteken-tearing
                }
                usbkbd_menu_mode(false);

                if (cfg.diskA[0]) {
                    // cfg.diskA blijft de menunaam (nodig voor de reboot-
                    // index); g_dsk_name wordt de echte (evt. cache-)naam.
                    snprintf(g_dsk_name, sizeof g_dsk_name, "%s", cfg.diskA);
                    if (zip_is_zip(g_dsk_name)) {
                        // Multi-disk-zip: hele set uitpakken; F12 wisselt.
                        g_dsk_set_n = zip_extract_all_cached(SD_DSK, g_dsk_name, ZIPDSK_EXTS,
                                                             vdp_arena + 96 * 1024, g_dsk_set, 8);
                        if (g_dsk_set_n > 0) {
                            snprintf(g_dsk_name, sizeof g_dsk_name, "%s", g_dsk_set[0]);
                            g_dsk_dir = SD_CACHE;
                            printf("[zip] disk-set: %d image(s), F12 wisselt\n", g_dsk_set_n);
                        }
                    }
                }

                use_game = NULL;
                use_game_size = 0;
                if (cfg.slot1[0]) {
                    // .zip? Eerst uitpakken; de menunaam blijft bewaard voor
                    // de reboot-staging-index in roms/.
                    char slot1_menu[STORAGE_MAX_NAME];
                    snprintf(slot1_menu, sizeof slot1_menu, "%s", cfg.slot1);
                    const char *slot1_dir = maybe_unzip(SD_ROMS, cfg.slot1, ZIPROM_EXTS);
                    // 1. Kleine ROM: in RAM laden (snel, geen flash-slijtage).
                    //    NB: eerst de grootte checken — een te grote malloc
                    //    PANICt op de Pico (PICO_MALLOC_PANIC) i.p.v. NULL.
                    long sz = storage_size(slot1_dir, cfg.slot1);
                    if (sz > 0 && sz <= 40 * 1024)
                        use_game = storage_load(slot1_dir, cfg.slot1, &use_game_size);
                    // 2. Groot: staat 'ie al in de flash-stage van een vorige keer?
                    if (!use_game)
                        use_game = flash_stage_get(cfg.slot1, &use_game_size);
                    // 3. Te groot voor RAM en nog niet gestaged: keuze in de
                    //    watchdog-scratch en zachte reboot; de staging draait
                    //    dan vóór de video-init (zie boven).
                    if (!use_game) {
                        // Disk A- en slot 2-keuzes als listing-index+1 mee de reboot over.
                        uint32_t didx = 0, sidx2 = 0;
                        if (cfg.diskA[0]) {
                            int nd = storage_list(SD_DSK, ent, 64);
                            for (int i = 0; i < nd; i++)
                                if (!ent[i].is_dir && strcmp(ent[i].name, cfg.diskA) == 0) { didx = (uint32_t)i + 1; break; }
                        }
                        int nr = storage_list(SD_ROMS, ent, 64);
                        if (cfg.slot2[0]) {
                            for (int i = 0; i < nr; i++)
                                if (!ent[i].is_dir && strcmp(ent[i].name, cfg.slot2) == 0) { sidx2 = (uint32_t)i + 1; break; }
                        }
                        for (int i = 0; i < nr; i++) {
                            if (!ent[i].is_dir && strcmp(ent[i].name, slot1_menu) == 0) {
                                watchdog_hw->scratch[0] = BOOT_STAGE_MAGIC;
                                watchdog_hw->scratch[1] = (uint32_t)i;
                                watchdog_hw->scratch[2] = didx;
                                watchdog_hw->scratch[3] = sidx2;
                                watchdog_reboot(0, 0, 0);
                                while (true) tight_loop_contents();
                            }
                        }
                    }
                }
            }
        }
    }

    // Slot 2-cartridge: klein en via RAM (geen flash-staging voor slot 2).
    // (Nog niet bedraad in het MSX2-profiel: daar is slot 2 niet aangesloten.)
    if (sd_ok && cfg.slot2[0] && boot_msx2)
        printf("[boot] slot 2 genegeerd (MSX2-profiel heeft nog geen slot 2)\n");
    if (sd_ok && cfg.slot2[0] && !boot_msx2) {
        const char *slot2_dir = maybe_unzip(SD_ROMS, cfg.slot2, ZIPROM_EXTS);
        long sz2 = storage_size(SD_ROMS == slot2_dir ? SD_ROMS : SD_CACHE, cfg.slot2);
        if (sz2 > 0 && sz2 <= 40 * 1024)
            use_game2 = storage_load(slot2_dir, cfg.slot2, &use_game2_size);
        if (!use_game2) {
            printf("[boot] slot 2: %s laadt niet (max 48KB via RAM) -> leeg\n", cfg.slot2);
            use_game2_size = 0;
        }
    }

    // Disk-interface (slot 2): DISK.ROM uit system/ + gekozen .dsk als
    // drive A. In het MSX2-profiel alleen aankoppelen als er echt een disk
    // gekozen is (interface + Konami-SCC-cart botsen anders in de boot —
    // known issue, nog uitzoeken).
    static uint8_t disk_rom[16384];
    if (sd_ok && diskrom_name[0] && (!boot_msx2 || g_dsk_name[0])) {
        long drs = storage_read(SD_SYSTEM, diskrom_name, disk_rom, sizeof disk_rom);
        if (drs > 0) {
            uint8_t sides = 0;
            uint32_t total_sectors = 0;
            if (g_dsk_name[0]) {
                long dsz = storage_size(g_dsk_dir, g_dsk_name);
                if (dsz > 0) {
                    total_sectors = (uint32_t)dsz / 512u;
                    sides = (dsz <= 80 * 9 * 512) ? 1 : 2; // 360KB enkel-, 720KB dubbelzijdig
                } else {
                    g_dsk_name[0] = 0; // dsk verdwenen -> lege drive
                }
            }
            machine_attach_disk(disk_rom, (uint32_t)drs, sides, total_sectors,
                                NULL, dsk_sector_io);
            printf("[boot] disk: %s, drive A: %s\n", diskrom_name,
                   g_dsk_name[0] ? g_dsk_name : "(leeg)");
        }
    }
#endif

    if (!use_bios) {
        // Geen BIOS beschikbaar: geen (leesbare) SD-kaart en geen ingebakken
        // fallback. Effen MSX-blauw scherm als "plaats een SD-kaart"-signaal.
        printf("[boot] no BIOS: insert an SD card with system/<bios>.rom\n");
        video_hstx_set_border(0x52BD);
        video_hstx_set_line_source(NULL, MSX_H); // alleen border = effen blauw
        while (true) tight_loop_contents();
    }

    bool init_ok;
#ifdef BAREMSX_SD
    machine_set_mapper_override(cfg.mapper1); // menu: slot 1-mapper (of -1 auto)
#endif
#if defined(BAREMSX_SD) && defined(BAREMSX_MSX2)
    if (boot_msx2)
        init_ok = machine_init_msx2(use_bios, use_bios_size,
                                    sd_bios + 32768, msx2ext_size,
                                    use_game, use_game_size, vdp_arena,
                                    NULL /* geen 64KB vrij voor SCC-I; PSRAM later */);
    else
#endif
        init_ok = machine_init(use_bios, use_bios_size, use_game, use_game_size,
                               use_game2, use_game2_size);
    if (!init_ok) {
        while (true) tight_loop_contents();
    }

    // Render-op-core-0-model: core 0 emuleert ELKE lijn en rendert 'm meteen
    // in de ring (correcte registerstand, precies zoals SDL). Core 1 doet
    // alleen nog scanout + audio. Geen live-render-mismatch meer -> split-
    // screen (R23/R2 mid-frame) e.d. kloppen vanzelf, en alle offload-/lead-/
    // priming-/fallback-trucs vervallen. Lijnbron blijft NULL (producer uit).
    const int vis_h = machine_display_height();
    video_hstx_set_line_source(NULL, vis_h);

    if (use_game && use_game_size >= 4096) {
        dbg_xip_ptr = use_game;
        dbg_xip_ref = xip_sum(use_game);
    }

    uint32_t emu_frames = 0;
    uint64_t sec_t0 = time_us_64();

    while (true) {
#ifdef BAREMSX_USB_KEYBOARD
        usbkbd_task(); // USB-host pompen (HID-reports -> MSX-matrix)
#ifdef BAREMSX_SD
        if (usbkbd_swap_requested()) disk_swap_next();
#endif
        // F11 = terug naar het bootmenu via een zachte reset (zelfde pad als
        // de resetknop; de watchdog-scratch bevat geen staging-magic, dus de
        // herstart boot gewoon het menu).
        if (usbkbd_reset_requested()) watchdog_reboot(0, 0, 0);
#endif

        uint32_t f_start = video_hstx_frame_count();

        // Eén MSX-frame: elke zichtbare lijn wordt geëmuleerd en meteen in de
        // ring gerenderd, gepaced op de scanout (max ~20 lijnen vooruit, past in
        // de 24-slots ring). Audio-synthese draait NIET meer hier: die zit op
        // core 1 (pipeline_task), zodat core 0 puur emuleert+rendert. Anders
        // werd bij zware games (Aleste/Zanac) de ~735 samples/frame synthese de
        // vblank in geperst en overrunde core 0 de frame-naad -> stale toplijnen.
        // Emuleer het VOLLEDIGE frame (262 NTSC / 313 PAL): een PAL-game (Zanac,
        // Aleste) zet z'n split-ISR in de vblank op — kap je die af op 262, dan
        // mist de line-interrupt-bewapening en breekt de score-split intermittent.
        int frame_lines = machine_frame_lines();
        video_hstx_set_border(machine_border_565());
        for (int ln = 0; ln < frame_lines; ln++) {
            if (ln < vis_h) {
                // Cross-frame-gate: een emulatieframe begint terwijl de display
                // nog de ONDERKANT van het vorige frame scant (meting: scan
                // ~205 bij frame-start). De in-frame-pacing hieronder helpt dan
                // niet — de scanpositie clampt op vis_h in de onderborder,
                // waardoor elke wachtvoorwaarde slaagt en lijn 24+ de ring-
                // slots van net gerenderde lijn 0..7 overschreef vóór de
                // display ze scande (= de jitterende toplijnen in Quarth).
                // Regel: zolang de display-frameteller nog niet geflipt is,
                // mag lijn ln pas de ring in als de OUDE bewoner van z'n slot
                // (grootste oude lijn == ln mod RING_N) al gescand is; lijnen
                // >= RING_N wachten sowieso op de flip.
                int old_line = ln + VHSTX_RING_N * ((vis_h - 1 - ln) / VHSTX_RING_N);
                while (video_hstx_frame_count() == f_start &&
                       (ln >= VHSTX_RING_N ||
                        video_hstx_scan_msx_line() <= old_line)) {
#ifdef BAREMSX_USB_KEYBOARD
                    usbkbd_task();
#endif
                    tight_loop_contents();
                }
                // In-frame-pacing (na de flip): max ~20 lijnen voor de beam uit.
                while (video_hstx_frame_count() != f_start &&
                       video_hstx_scan_msx_line() < ln - 20) {
#ifdef BAREMSX_USB_KEYBOARD
                    usbkbd_task();
#endif
                    tight_loop_contents();
                }
            }
            // Renderen VÓÓR de Z80-slice van deze lijn — met de registerstand
            // van de lijnSTART, zoals de SDL-sink en echte hardware (die latcht
            // per lijn). Renderen ná de slice liet een mid-lijn R23/R2-write
            // (Zanac's veld-switch op lijn 14) één lijn te vroeg doorwerken:
            // de onderste scorelijn toonde al scrollend veld.
            if (ln < vis_h) {
                int w = machine_render_line_565(video_hstx_claim_line(ln), ln);
                video_hstx_publish_line(ln, w);
            }
            machine_do_line(ln);
        }

        dbg_core0_frames++;
        emu_frames++;
        if (dbg_xip_ptr) {
            dbg_xip_checks++;
            if (xip_sum(dbg_xip_ptr) != dbg_xip_ref) dbg_xip_bad++;
        }

        // De display-flip valt bewust MIDDEN in het emulatieframe (lijn >=
        // RING_N wacht erop), dus één flip per frame is de norm. Meer dan één
        // flip = de emulatie hield het tempo niet bij (echte overrun).
        if (video_hstx_frame_count() - f_start > 1) dbg_overruns++;

        // Geen aparte flank-wait meer nodig: de cross-frame-gate bovenin het
        // volgende frame (lijn 0 wacht tot z'n slot vrij is, lijn >= RING_N op
        // de flip) verzorgt de pacing volledig.

        // Eens per seconde: emulatie-fps + display-fps
        // NB: geen per-seconde fps-printf meer — stdio (UART/USB) stalt core 0
        // ~ms op de frame-rand, waardoor het volgende frame te laat start en de
        // top mist (zichtbaar als een ~1s-flikker). Tellers blijven via SWD.
        (void)sec_t0; (void)emu_frames;
    }
}
