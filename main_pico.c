#include <stdio.h>
#include "pico/stdlib.h" // trekt pico.h (__not_in_flash_func) mee
#include "hardware/clocks.h"
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
static int dsk_sector_io(void *ctx, uint32_t lba, uint8_t *buf, bool write)
{
    (void)ctx;
    long n = write
        ? storage_write_at(SD_DSK, g_dsk_name, lba * 512u, buf, 512)
        : storage_read_at(SD_DSK, g_dsk_name, lba * 512u, buf, 512);
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

// ARGB (0x00RRGGBB) -> RGB565.
static inline uint16_t argb_to_565(uint32_t px) {
    return (uint16_t)(((px >> 8) & 0xF800) | ((px >> 5) & 0x07E0) | ((px >> 3) & 0x001F));
}

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

int main(void)
{
    enable_fpu(); // core 0 FPU aan vóór iets FP-achtigs draait (o.a. sleep_us-IRQ)

    // 252 MHz sysclk + HSTX-deler 2 -> 25.2 MHz pixelklok (640x480@60), maar de
    // CPU draait 2x zo snel als bij 126 MHz -> genoeg headroom voor emulatie +
    // rendering op 60 fps. (Vereist MODE_HSTX_CLK_DIV=2 in de pico_hdmi-build.)
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

#ifdef BAREMSX_SD
    static uint8_t sd_bios[65536];
    static menu_config_t cfg;
    static storage_entry_t ent[64];
    bool sd_ok = storage_init();
    const uint8_t *staged_game = NULL;
    uint32_t staged_size = 0;

    // Reboot-staging: als het menu vóór de reboot een (grote) ROM koos, flash
    // die dan NU — de HSTX-video draait nog niet, dus dit is het enige veilige
    // moment voor flash_range_erase/program.
    if (sd_ok && watchdog_hw->scratch[0] == BOOT_STAGE_MAGIC) {
        uint32_t idx = watchdog_hw->scratch[1];
        uint32_t didx = watchdog_hw->scratch[2];
        watchdog_hw->scratch[0] = 0;
        watchdog_hw->scratch[2] = 0;
        // Disk A-keuze (index+1 in de dsk/-listing) mee over de reboot heen.
        if (didx) {
            int nd = storage_list(SD_DSK, ent, 64);
            if ((int)(didx - 1) < nd && !ent[didx - 1].is_dir)
                snprintf(g_dsk_name, sizeof g_dsk_name, "%s", ent[didx - 1].name);
        }
        int nr = storage_list(SD_ROMS, ent, 64);
        if ((int)idx < nr && !ent[idx].is_dir)
            staged_game = flash_stage_rom(SD_ROMS, ent[idx].name, &staged_size);
    }
#endif

    video_hstx_init();   // HDMI-output + core 1 starten (het menu rendert hierin)

#ifdef BAREMSX_SD
    static char diskrom_name[STORAGE_MAX_NAME];
    if (sd_ok) {
        // system/: eerste bestand dat NIET "disk..." heet = BIOS (gepad naar
        // 64KB); eerste dat wél zo heet = DISK.ROM (optioneel).
        char bios_name[STORAGE_MAX_NAME] = "";
        int ns = storage_list(SD_SYSTEM, ent, 64);
        for (int i = 0; i < ns; i++) {
            if (ent[i].is_dir) continue;
            if (is_disk_rom_name(ent[i].name)) {
                if (!diskrom_name[0]) snprintf(diskrom_name, sizeof diskrom_name, "%s", ent[i].name);
            } else if (!bios_name[0]) {
                snprintf(bios_name, sizeof bios_name, "%s", ent[i].name);
            }
        }
        if (bios_name[0]) {
            memset(sd_bios, 0xFF, sizeof sd_bios);
            storage_read(SD_SYSTEM, bios_name, sd_bios, sizeof sd_bios);
            use_bios = sd_bios;
            use_bios_size = sizeof sd_bios;

            if (staged_game) {
                // Zojuist gestaged (na de menu-reboot): direct booten, geen menu.
                use_game = staged_game;
                use_game_size = staged_size;
            } else {
                // Boot-menu (USB-keyboard bestuurt 'm; rendert in de HDMI-backbuffer).
                memset(&cfg, 0, sizeof cfg);
                menu_init(sd_bios, &cfg);
                usbkbd_menu_mode(true);
                while (!menu_start_requested()) {
                    usbkbd_task();
                    int ev;
                    while ((ev = usbkbd_menu_poll()) >= 0) menu_input((menu_input_t)ev);
                    uint16_t *bb = video_hstx_backbuffer();
                    if (bb) { menu_render(bb); video_hstx_present(0x52BD); } // 0x52BD = MSX-blauw (565)
                }
                usbkbd_menu_mode(false);

                if (cfg.diskA[0])
                    snprintf(g_dsk_name, sizeof g_dsk_name, "%s", cfg.diskA);

                use_game = NULL;
                use_game_size = 0;
                if (cfg.slot1[0]) {
                    // 1. Kleine ROM: in RAM laden (snel, geen flash-slijtage).
                    //    NB: eerst de grootte checken — een te grote malloc
                    //    PANICt op de Pico (PICO_MALLOC_PANIC) i.p.v. NULL.
                    long sz = storage_size(SD_ROMS, cfg.slot1);
                    if (sz > 0 && sz <= 48 * 1024)
                        use_game = storage_load(SD_ROMS, cfg.slot1, &use_game_size);
                    // 2. Groot: staat 'ie al in de flash-stage van een vorige keer?
                    if (!use_game)
                        use_game = flash_stage_get(cfg.slot1, &use_game_size);
                    // 3. Te groot voor RAM en nog niet gestaged: keuze in de
                    //    watchdog-scratch en zachte reboot; de staging draait
                    //    dan vóór de video-init (zie boven).
                    if (!use_game) {
                        // Disk A-keuze als dsk/-index+1 mee de reboot over.
                        uint32_t didx = 0;
                        if (cfg.diskA[0]) {
                            int nd = storage_list(SD_DSK, ent, 64);
                            for (int i = 0; i < nd; i++)
                                if (!ent[i].is_dir && strcmp(ent[i].name, cfg.diskA) == 0) { didx = (uint32_t)i + 1; break; }
                        }
                        int nr = storage_list(SD_ROMS, ent, 64);
                        for (int i = 0; i < nr; i++) {
                            if (!ent[i].is_dir && strcmp(ent[i].name, cfg.slot1) == 0) {
                                watchdog_hw->scratch[0] = BOOT_STAGE_MAGIC;
                                watchdog_hw->scratch[1] = (uint32_t)i;
                                watchdog_hw->scratch[2] = didx;
                                watchdog_reboot(0, 0, 0);
                                while (true) tight_loop_contents();
                            }
                        }
                    }
                }
            }
        }
    }

    // Disk-interface (slot 2): DISK.ROM uit system/ + gekozen .dsk als drive A.
    static uint8_t disk_rom[16384];
    if (sd_ok && diskrom_name[0]) {
        long drs = storage_read(SD_SYSTEM, diskrom_name, disk_rom, sizeof disk_rom);
        if (drs > 0) {
            uint8_t sides = 0;
            uint32_t total_sectors = 0;
            if (g_dsk_name[0]) {
                long dsz = storage_size(SD_DSK, g_dsk_name);
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
        while (true) {
            uint16_t *bb = video_hstx_backbuffer();
            if (bb) {
                for (int i = 0; i < MSX_W * MSX_H; i++) bb[i] = 0x52BD;
                video_hstx_present(0x52BD);
            }
            tight_loop_contents();
        }
    }

    if (!machine_init(use_bios, use_bios_size, use_game, use_game_size)) {
        while (true) tight_loop_contents();
    }

    uint32_t emu_frames = 0;
    uint64_t sec_t0 = time_us_64();

    while (true) {
#ifdef BAREMSX_USB_KEYBOARD
        usbkbd_task(); // USB-host pompen (HID-reports -> MSX-matrix)
#endif

        // Eén MSX-frame emuleren
        machine_do_cycles();
        machine_generate_interrupt();

        audio_hdmi_generate();   // emu-audio -> ring (core 1 pompt naar HDMI)
        render_frame_hdmi();     // render naar back buffer + present (swap op vsync)

        emu_frames++;

        // Pace op de HDMI-frame (zoals de bouncing_box-demo): wacht tot de HSTX-
        // scanout een frame verder is. Geen sleep_us -> geen SDK-timer-alarm-IRQ
        // met FP-instructies (die gaf een NOCP-HardFault), en de emulatie loopt
        // netjes in lockstep met de 60 Hz display-uitvoer.
        {
            uint32_t f = video_hstx_frame_count();
            while (video_hstx_frame_count() == f) {
#ifdef BAREMSX_USB_KEYBOARD
                usbkbd_task(); // USB vaak pompen tijdens de idle-wait -> snelle respons
#endif
                tight_loop_contents();
            }
        }

        // Eens per seconde: emulatie-fps + display-fps
        if (time_us_64() - sec_t0 >= 1000000) {
            static uint32_t last_disp = 0;
            uint32_t disp = video_hstx_frame_count();
            printf("[fps] emu=%lu display=%lu  pc=%04X\n",
                   (unsigned long)emu_frames, (unsigned long)(disp - last_disp),
                   machine_dbg_pc());
            last_disp = disp;
            emu_frames = 0;
            sec_t0 = time_us_64();
        }
    }
}
