#include <stdio.h>
#include "machine.h"
#include "rom.h"
#include "ram.h"
#include "empty.h"
#include <Z80.h>
#include <Z/constants/pointer.h>
#include <Z/constants/boolean.h>
#include "slots.h"
#include "subslots.h"
#include "tms9918.h"
#include "PPI.h"
#include <string.h>
#include "konami-mega-rom-scc.h"
#include "mapper.h"
#include "emu2149.h"
#include "diskrom.h"
#ifdef BAREMSX_MSX2
#include "v9938.h"
#include "rtc.h"
#include "mappedram.h"
#include "sccplus.h"
#endif
#include "pico.h"

// Platform-agnostische machine: BIOS/cartridge komen via machine_init binnen.
// Verschillen met de SDL-versie:
//  - BIOS komt uit flash (bios_rom[] in bios_rom.h) i.p.v. van disk
//  - geen cartridge/SCC en geen audio (komt later)

Z80 cpu;

slots_context_t slots;
static subslot_context_t subslots3; // slot 3: RAM op 3-2 (NMS-8245-stijl)

// Machine-arena: het MSX1-profiel (64KB RAM + TMS9918) en het MSX2-profiel
// (128KB mapper-pool) delen dezelfde 128KB — er draait er maar één tegelijk.
// De namen `ram` en `tms9918` blijven via macro's gewoon werken.
typedef struct { uint8_t ram[0x10000]; tms9918_context_t vdp; } msx1_state_t;
static union {
    msx1_state_t m1;
#ifdef BAREMSX_MSX2
    uint8_t mapram[MAPPEDRAM_SIZE];
#endif
} machine_arena;
#define ram     (machine_arena.m1.ram)
#define tms9918 (machine_arena.m1.vdp)

ppi_context_t ppi;
konami_scc_t konami_scc;
mapper_t cart;  // slot 1-cartridge-mapper (plain/konami/ascii8/16)
mapper_t cart2; // slot 2-cartridge-mapper (paging only — SCC-geluid is slot 1)
PSG *psg; // AY-3-8910 (emu2149, integer)

// Disk-interface (slot 2), optioneel: gezet via machine_attach_disk vóór
// machine_init. Zonder attach blijft slot 2 leeg.
static diskrom_t diskrom;
static bool disk_attached = false;

// MSX2-profiel (alleen in builds met BAREMSX_MSX2 — de Pico-SRAM kan de
// 2x128KB V9938-context + 128KB mapper-pool pas aan als PSRAM er is).
static bool g_msx2 = false;
bool machine_is_msx2(void) { return g_msx2; }
int machine_display_width(void)  { return g_msx2 ? 512 : 256; }
int machine_display_height(void) { return g_msx2 ? 212 : 192; }
#ifdef BAREMSX_MSX2
// De V9938-context is klein geworden (VRAM zit achter een pointer): de
// host levert het 128KB-VRAM (op de Pico: de menu-arena, zie main_pico).
v9938_context_t v9938;
static rtc_t rtc;
static mappedram_t mapram;
static sccplus_t sccplus; // Sound Cartridge in slot 2 (alleen met host-RAM)
#endif

// Totaal aantal scanlijnen per frame: 262 (NTSC/60Hz) of 313 (PAL/50Hz, R9
// bit1). De Pico-beam-lus moet het HELE frame emuleren, anders wordt de vblank
// van een PAL-game afgekapt en breekt de split-ISR-timing (zie main_pico.c).
int machine_frame_lines(void)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) return (v9938.regs[9] & 0x02) ? 313 : 262;
#endif
    return 262;
}

void machine_attach_disk(const uint8_t *disk_rom, uint32_t disk_rom_size,
                         uint8_t sides, uint32_t total_sectors,
                         void *io_ctx, wd_sector_io_t io)
{
    diskrom.rom = disk_rom;
    diskrom.rom_size = disk_rom_size;
    wd2793_init(&diskrom.fdc, sides, total_sectors, io_ctx, io);
    disk_attached = true;
}

void machine_disk_swap(uint8_t sides, uint32_t total_sectors)
{
    if (disk_attached)
        wd2793_set_disk(&diskrom.fdc, sides, total_sectors);
}

// Slot 1-mapper-override uit het menu: -1 = auto-detect, anders forceer een
// MAPPER_*-type. Eenmalig (per machine_init) — geldt alleen voor slot 1.
static int g_mapper_ovr = -1;
void machine_set_mapper_override(int type) { g_mapper_ovr = type; }

uint8_t psg_register = 0;

// Debug (SWD): geluidskanalen muten — bit0/1/2 = PSG A/B/C, bit3 = SCC.
volatile uint32_t dbg_snd_mask = 0;
// Debug (SWD): low-pass op de mix (0=uit, 1/2 = aantal 7,6kHz-polen).
volatile uint32_t dbg_snd_lpf = 0;

// Deterministische emu2149-selftest (audio_hdmi toon-modus 7): eigen
// PSG-instantie, vaste registers, exact dezelfde configuratie als de echte
// init hierboven — output moet byte-voor-byte gelijk zijn aan de host-run.
int16_t emu2149_selftest_sample(void)
{
    static PSG *tp = NULL;
    if (!tp) {
        tp = PSG_new(3579545, AUDIO_SAMPLE_RATE);
        if (!tp) return 0;
        PSG_setVolumeMode(tp, EMU2149_VOL_AY_3_8910);
        PSG_set_quality(tp, 0);
        PSG_reset(tp);
        PSG_writeReg(tp, 0, 0xF7); // toonperiode A = 0x1F7 (~444Hz)
        PSG_writeReg(tp, 1, 0x01);
        PSG_writeReg(tp, 7, 0x3E); // mixer: alleen toon A
        PSG_writeReg(tp, 8, 0x0F); // volume A = 15
    }
    return PSG_calc(tp);
}

#ifdef SCC_DEBUG
volatile uint16_t scc_log_a[64], scc_log_pc[64];
volatile uint8_t scc_log_v[64];
volatile int scc_log_n = 0;
volatile uint32_t sccr_bf50_bank, sccr_bf50_val, sccr_bf50_hits;
volatile uint8_t r1_log_v[32];
volatile uint16_t r1_log_pc[32];
volatile int r1_log_n = 0;
#endif

// Beam-render-sink (zie machine.h). NULL = legacy snapshot-model.
static machine_line_sink_t g_line_sink = NULL;
void machine_set_line_sink(machine_line_sink_t sink) { g_line_sink = sink; }

volatile uint32_t g_a9_reads = 0; // debug: hoe vaak leest de BIOS de keyboard-rij
uint32_t machine_dbg_a9_reads(void) { return g_a9_reads; }
uint16_t machine_dbg_pc(void) { return Z80_PC(cpu); } // debug: Z80 PC (voor hang-diagnose)

static void gen_interrupt(void)
{
    z80_int(&cpu, Z_TRUE); // INT-lijn asserteren (level-getriggerd, zoals de MSX-VDP)
}

static uint8_t __not_in_flash_func(read_port_impl)(uint8_t port)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) {
        switch (port) {
        case 0x98:
            return v9938_read_data(&v9938);
        case 0x99: {
            uint8_t v = v9938_read_status(&v9938);
            // Na elke statusread de INT-lijn herberekenen: S0-read ackt de
            // frame-IRQ, S1-read de lijn-IRQ; de lijn blijft hoog zolang de
            // andere bron nog hangt (level-triggered).
            z80_int(&cpu, v9938_irq_asserted(&v9938) ? Z_TRUE : Z_FALSE);
            return v;
        }
        case 0xb5:
            return rtc_read(&rtc);
        case 0xfc: case 0xfd: case 0xfe: case 0xff:
            return mappedram_get_page(&mapram, port - 0xfc);
        case 0xa8:
            return slots_get_slot_register(&slots);
        case 0xa9:
            return ppi_read_a9(&ppi);
        default:
            return 0xff;
        }
    }
#endif
    switch (port)
    {
    case 0x98:
        return tms9918_read(&tms9918, false);
    case 0x99: {
        uint8_t v = tms9918_read(&tms9918, true);
        z80_int(&cpu, Z_FALSE); // VDP-status gelezen -> INT-lijn intrekken
        return v;
    }
    case 0xa8:
        return slots_get_slot_register(&slots);
    case 0xa9:
        g_a9_reads++;
        return ppi_read_a9(&ppi); // keyboard row read
    default:
        return 0xff;
    }
}

static void __not_in_flash_func(write_port_impl)(uint8_t port, uint8_t value)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) {
        switch (port) {
        case 0x98: v9938_write_data(&v9938, value); return;
        case 0x99:
            v9938_write_ctrl(&v9938, value);
            // R0/R1-writes (IE1/IE0) kunnen de INT-lijn beide kanten op
            // veranderen: IE aan met hangende flag -> meteen asserteren,
            // IE uit -> intrekken (anders hertriggert de ISR eindeloos).
            z80_int(&cpu, v9938_irq_asserted(&v9938) ? Z_TRUE : Z_FALSE);
            return;
        case 0x9a: v9938_write_palette(&v9938, value); return;
        case 0x9b:
            v9938_write_indirect(&v9938, value); // kan ook R0/R1 raken
            z80_int(&cpu, v9938_irq_asserted(&v9938) ? Z_TRUE : Z_FALSE);
            return;
        case 0xb4: rtc_select(&rtc, value); return;
        case 0xb5: rtc_write(&rtc, value); return;
        case 0xfc: case 0xfd: case 0xfe: case 0xff:
            mappedram_set_page(&mapram, port - 0xfc, value); return;
        case 0xa0: psg_register = value; return;
        case 0xa1: PSG_writeReg(psg, psg_register, value); return;
        case 0xa8: slots_set_slot_register(&slots, value); return;
        case 0xaa: ppi_write_aa(&ppi, value); return;
        default: return;
        }
    }
#endif
    switch (port)
    {
    case 0x98:
        tms9918_write(&tms9918, false, value);
        break;
    case 0x99:
        tms9918_write(&tms9918, true, value);
        break;
    case 0xa0:
        psg_register = value; // register-latch
        break;
    case 0xa1:
        PSG_writeReg(psg, psg_register, value);
        break;
    case 0xa8:
        slots_set_slot_register(&slots, value);
        break;
    case 0xaa:
        ppi_write_aa(&ppi, value); // keyboard row select
        break;
    default:
        break;
    }
}

#ifdef SCCPLUS_DEBUG
// Tijdelijk: zie welke slotconfiguratie BFFE/9000-writes ontvangt.
static void dbg_slots_write(void *ctx, uint16_t a, uint8_t v)
{
    if (a == 0xBFFE || a == 0x9000 || a == 0x9002 || a == 0xFFFF) {
        extern uint16_t machine_dbg_pc(void);
        fprintf(stderr, "[mw] %04X=%02X slotreg=%02X pc=%04X\n",
                a, v, slots_get_slot_register(&slots), machine_dbg_pc());
    }
    slots_write((slots_context_t *)ctx, a, v);
}
#endif

// Zeta-Z80: 16-bit poort-adressen + void*-context; MSX decodeert alleen de lage 8 bits.
static zuint8 zeta_in(void *ctx, zuint16 port) { (void)ctx; return read_port_impl((uint8_t)port); }
static void zeta_out(void *ctx, zuint16 port, zuint8 value) { (void)ctx; write_port_impl((uint8_t)port, value); }

bool machine_init(const uint8_t *bios, uint32_t bios_size,
                  const uint8_t *game, uint32_t game_size,
                  const uint8_t *game2, uint32_t game2_size)
{
    (void)bios_size; // rom_read maskeert op 16-bit; de BIOS beslaat slot 0 (0x0000-0x7FFF)

    // SCC altijd initialiseren zodat scc_process (audio) veilig/stil is,
    // ook als de game geen SCC gebruikt.
    scc_init(&konami_scc);

    // Primaire slots
    slots_add_slot(&slots, 0, (void *)bios, rom_read, rom_write);       // BIOS

    mapper_type_t mt = (g_mapper_ovr >= 0) ? (mapper_type_t)g_mapper_ovr
                                           : mapper_detect(game, game_size);
    g_mapper_ovr = -1; // eenmalig; slot 2 detecteert altijd automatisch
    printf("[machine] cartridge mapper: %s (%u bytes)\n", mapper_name(mt), (unsigned)game_size);
    if (mt == MAPPER_KONAMI_SCC) {
        scc_set_rom(&konami_scc, (uint8_t *)game, game_size);
        slots_add_slot(&slots, 1, &konami_scc, scc_read, scc_write);
    } else if (mt != MAPPER_NONE) {
        mapper_init(&cart, game, game_size, mt);
        slots_add_slot(&slots, 1, &cart, mapper_read, mapper_write);
    } else {
        slots_add_slot(&slots, 1, NULL, empty_read, empty_write);       // leeg -> BIOS-only
    }
    // Slot 2: tweede cartridge, of anders de disk-interface (DISK.ROM +
    // WD2793 op 0x7FF8-0x7FFF). Een SCC-game hier bankt via de generieke
    // mapper maar klinkt niet — SCC-geluid zit op slot 1.
    // KNOWN ISSUE: de disk-interface in een GEËXPANDEERD slot (2-1 of 3-3)
    // ontspoort nog (inter-slot-calls van de MSX1-BIOS naar de DISK.ROM);
    // daarom is "cartridge 2 + disk tegelijk" nu niet mogelijk. Uitzoeken
    // vóór MSX2/Nextor.
    mapper_type_t mt2 = mapper_detect(game2, game2_size);
    if (mt2 != MAPPER_NONE) {
        printf("[machine] slot 2 mapper: %s (%u bytes)\n", mapper_name(mt2), (unsigned)game2_size);
        if (disk_attached)
            printf("[machine] disk uitgeschakeld: slot 2 is bezet door cartridge 2\n");
        mapper_init(&cart2, game2, game2_size, mt2);
        slots_add_slot(&slots, 2, &cart2, mapper_read, mapper_write);
    } else if (disk_attached) {
#ifdef TEST_DISK_21
        // Diagnose: disk in geëxpandeerd subslot 2-1 (faalde met de MSX2-rom).
        printf("[machine] TEST: disk interface in subslot 2-1\n");
        static subslot_context_t subslots2t;
        subslots2t.subslot_register = 0;
        subslots_add_subslot(&subslots2t, 0, NULL, empty_read, empty_write);
        subslots_add_subslot(&subslots2t, 1, &diskrom, diskrom_read, diskrom_write);
        subslots_add_subslot(&subslots2t, 2, NULL, empty_read, empty_write);
        subslots_add_subslot(&subslots2t, 3, NULL, empty_read, empty_write);
        slots_add_slot(&slots, 2, &subslots2t, subslots_read, subslots_write);
#else
        printf("[machine] disk interface in slot 2 (%u KB DISK.ROM, %u sides)\n",
               (unsigned)(diskrom.rom_size / 1024), diskrom.fdc.sides);
        slots_add_slot(&slots, 2, &diskrom, diskrom_read, diskrom_write);
#endif
    } else {
        slots_add_slot(&slots, 2, NULL, empty_read, empty_write);
    }

    // Slot 3: 64KB RAM op subslot 3-2 (geëxpandeerd, NMS-8245-stijl).
    // NB: met plat 64KB-RAM in slot 3 activeert de DISK.ROM zijn MSX-DOS-
    // bootfase en die ontspoort nu nog in de emulatie (banner-corruptie,
    // EXPTBL stuk) — uitzoeken vóór Nextor-support. Expanded RAM neemt het
    // Disk BASIC-pad en dat werkt.
#ifdef TEST_FLAT_RAM
    // Diagnose: plat 64KB-RAM (activeert de MSX-DOS-bootfase van de DISK.ROM).
    printf("[machine] TEST: plat RAM in slot 3\n");
    slots_add_slot(&slots, 3, ram, ram_read, ram_write);
#else
    subslots3.subslot_register = 0;
    subslots_add_subslot(&subslots3, 0, NULL, empty_read, empty_write);
    subslots_add_subslot(&subslots3, 1, NULL, empty_read, empty_write);
    subslots_add_subslot(&subslots3, 2, ram, ram_read, ram_write);
    subslots_add_subslot(&subslots3, 3, NULL, empty_read, empty_write);
    slots_add_slot(&slots, 3, &subslots3, subslots_read, subslots_write);
#endif

    cpu.context = &slots;
    cpu.fetch_opcode = cpu.fetch = cpu.nop = cpu.read = (Z80Read)slots_read;
    cpu.write = (Z80Write)slots_write;
    cpu.in = zeta_in;
    cpu.out = zeta_out;
    cpu.halt = Z_NULL;
    cpu.nmia = Z_NULL;
    cpu.inta = Z_NULL;
    cpu.int_fetch = Z_NULL;
    cpu.ld_i_a = Z_NULL;
    cpu.ld_r_a = Z_NULL;
    cpu.reti = Z_NULL;
    cpu.retn = Z_NULL;
    cpu.hook = Z_NULL;
    cpu.illegal = Z_NULL;
    cpu.options = Z80_MODEL_ZILOG_NMOS;
    z80_power(&cpu, Z_TRUE);
    z80_instant_reset(&cpu);

    tms9918_register_interrupt_func(&tms9918, gen_interrupt);

    // AY-3-8910 (PSG): emu2149 verwacht de master-klok 3.579545MHz (niet /2),
    // anders klinkt alles een octaaf te laag.
    psg = PSG_new(3579545, AUDIO_SAMPLE_RATE);
    if (!psg) return false;
    PSG_setVolumeMode(psg, EMU2149_VOL_AY_3_8910);
    PSG_set_quality(psg, 0); // snelste modus (49716 Hz ~ native, geen oversampling nodig)
    PSG_reset(psg);

    return true;
}

// Eén beam-lijn emuleren: (optionele SDL-sink-render) + 228 T-states Z80 +
// de VDP-events van deze lijn. De vblank-ISR draait daardoor per definitie
// ná het zichtbare veld — sprite-multiplexers kloppen zonder snapshot.
// Dev-trace (SDL --trace F1 F2): logt per lijn de wijzigingen in de
// splitgevoelige VDP-registers + IRQ-flanken, om raster-splitbugs te
// ontleden. 0 = uit.
volatile int machine_trace = 0;

void __not_in_flash_func(machine_do_line)(int line)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) {
        int active_h = (v9938.regs[9] & 0x80) ? 212 : 192;
        if (g_line_sink && line < active_h) {
            static uint32_t linebuf2[512];
            v9938_render_line(&v9938, linebuf2, line);
            g_line_sink(line, linebuf2, 512);
        }
        uint8_t t0 = 0, t2 = 0, t19 = 0, t23 = 0, ts1 = 0;
        bool tirq = false;
        if (machine_trace) {
            t0 = v9938.regs[0]; t2 = v9938.regs[2];
            t19 = v9938.regs[19]; t23 = v9938.regs[23];
            ts1 = v9938.status[1]; tirq = v9938_irq_asserted(&v9938);
        }
        v9938_line_start(&v9938); // R19/R23-latch voor de coïncidentie-check
        z80_run(&cpu, 228);
        v9938_scanline(&v9938, line);
        if (machine_trace) {
            if (t0 != v9938.regs[0] || t2 != v9938.regs[2] ||
                t19 != v9938.regs[19] || t23 != v9938.regs[23] ||
                (ts1 ^ v9938.status[1]) & 1 || tirq != v9938_irq_asserted(&v9938))
                fprintf(stderr, "[trc] ln=%3d R0=%02X R2=%02X R19=%02X R23=%02X "
                        "FH=%d INT=%d pc=%04X\n",
                        line, v9938.regs[0], v9938.regs[2], v9938.regs[19],
                        v9938.regs[23], v9938.status[1] & 1,
                        v9938_irq_asserted(&v9938), Z80_PC(cpu));
        }
        return;
    }
#endif
    if (g_line_sink && line < 192) {
        static uint32_t linebuf1[256];
        tms9918_render_line(&tms9918, linebuf1, line);
        g_line_sink(line, linebuf1, 256);
    }
    z80_run(&cpu, 228);
    if (line == 192)
        check_and_generate_interrupt(&tms9918);
}

void __not_in_flash_func(machine_do_cycles)(void)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) {
        // R9 bit 1 = PAL: 313 lijnen/50Hz i.p.v. 262/60 — anders draait
        // Europese software ~20% te snel.
        int nlines = (v9938.regs[9] & 0x02) ? 313 : 262;
        for (int line = 0; line < nlines; line++) machine_do_line(line);
        return;
    }
#endif
    if (g_line_sink) {
        for (int line = 0; line < 262; line++) machine_do_line(line);
        return;
    }
    z80_run(&cpu, CYC_PER_INT); // legacy frame-lus (Pico tot de refactor)
}

// ARGB -> RGB565 (zelfde formule als de frontends).
static inline uint16_t m_argb565(uint32_t px)
{
    return (uint16_t)(((px >> 8) & 0xF800) | ((px >> 5) & 0x07E0) | ((px >> 3) & 0x001F));
}

// Live lijnrender voor core 1 (Pico "race the beam"): geen snapshot, de
// beam-pacing van de host houdt de cores in de pas.
int __not_in_flash_func(machine_render_line_565)(uint16_t *dst, int y)
{
    static uint32_t tmp[512]; // alleen core 1 gebruikt dit
#ifdef BAREMSX_MSX2
    if (g_msx2)
        return v9938_render_line_565(&v9938, dst, y); // 256-modes: HSTX verdubbelt
#endif
    tms9918_render_line(&tms9918, tmp, y);
    for (int i = 0; i < 256; i++) dst[i] = m_argb565(tmp[i]);
    return 256;
}

uint16_t machine_border_565(void)
{
    return m_argb565(machine_get_background_color());
}

#ifdef BAREMSX_MSX2
// MSX2-profiel: NMS-8245-achtige machine met de V9938.
//   Slot 0:  MSX2-BIOS (32KB, 0x0000)
//   Slot 1:  cartridge (zelfde mapper-pad als MSX1)
//   Slot 2:  leeg (FM-PAC later)
//   Slot 3:  expanded — 3-0 sub-ROM (EXT), 3-2 mapper-RAM (128KB, FC-FF)
// bios/ext worden door de frontend als 64KB-gepadde buffers aangeleverd
// (rom_read is bewust simpel en maskeert alleen op 16-bit).
// Begrensd ROM-window (voor de sub-ROM: 16KB op 0x0000 in zijn subslot;
// reads erbuiten zijn open bus).
typedef struct { const uint8_t *d; uint32_t n; } romwin_t;
static romwin_t ext_win;
static uint8_t romwin_read(void *ctx, uint16_t a)
{
    romwin_t *w = (romwin_t *)ctx;
    return a < w->n ? w->d[a] : 0xFF;
}
static void romwin_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; (void)a; (void)v; }

bool machine_init_msx2(const uint8_t *bios, uint32_t bios_size,
                       const uint8_t *ext, uint32_t ext_size,
                       const uint8_t *game, uint32_t game_size,
                       uint8_t *vram128k, uint8_t *sccplus_ram64k)
{
    (void)bios_size;
    g_msx2 = true;

    v9938_init(&v9938, vram128k);
    v9938_register_interrupt_func(&v9938, gen_interrupt);
    rtc_init(&rtc);
    mappedram_init(&mapram, machine_arena.mapram);
    scc_init(&konami_scc);
    ext_win.d = ext;
    ext_win.n = ext_size;

    slots_add_slot(&slots, 0, (void *)bios, rom_read, rom_write);

    mapper_type_t mt = (g_mapper_ovr >= 0) ? (mapper_type_t)g_mapper_ovr
                                           : mapper_detect(game, game_size);
    g_mapper_ovr = -1; // eenmalig; slot 2 detecteert altijd automatisch
    printf("[machine] msx2: cartridge mapper: %s (%u bytes)\n", mapper_name(mt), (unsigned)game_size);
    if (mt == MAPPER_KONAMI_SCC) {
        scc_set_rom(&konami_scc, (uint8_t *)game, game_size);
        slots_add_slot(&slots, 1, &konami_scc, scc_read, scc_write);
    } else if (mt != MAPPER_NONE) {
        mapper_init(&cart, game, game_size, mt);
        slots_add_slot(&slots, 1, &cart, mapper_read, mapper_write);
    } else {
        slots_add_slot(&slots, 1, NULL, empty_read, empty_write);
    }
    // Disk in PLAIN slot 2. De authentieke NMS-8245-plek is subslot 3-3,
    // maar met de disk dáár raakt een Konami-SCC-game (KV2) in een
    // boot-loop — een nog niet gevonden bug in onze expanded-slot-emulatie
    // (zelfde familie als het oude MSX1-issue). Tot die gevonden is: slot 2.
    if (disk_attached) {
        printf("[machine] disk interface in slot 2 (%u KB DISK.ROM, %u sides)\n",
               (unsigned)(diskrom.rom_size / 1024), diskrom.fdc.sides);
        slots_add_slot(&slots, 2, &diskrom, diskrom_read, diskrom_write);
    } else if (sccplus_ram64k) {
        // Konami Sound Cartridge (SCC-I) — alleen zonder disk (beide willen
        // slot 2); nodig voor Snatcher/SD Snatcher (werk in uitvoering).
        sccplus_init(&sccplus, sccplus_ram64k, &konami_scc);
        printf("[machine] Sound Cartridge (SCC-I, 64KB) in slot 2\n");
        slots_add_slot(&slots, 2, &sccplus, sccplus_read, sccplus_write);
    } else {
        slots_add_slot(&slots, 2, NULL, empty_read, empty_write);
    }

    // Slot 3 geëxpandeerd, NMS-8245-indeling: 3-0 sub-ROM, 3-2 mapper-RAM
    // (3-3 is de echte disk-plek — zie hierboven).
    subslots3.subslot_register = 0;
    subslots_add_subslot(&subslots3, 0, &ext_win, romwin_read, romwin_write); // sub-ROM (begrensd)
    subslots_add_subslot(&subslots3, 1, NULL, empty_read, empty_write);
    subslots_add_subslot(&subslots3, 2, &mapram, mappedram_read, mappedram_write);
    subslots_add_subslot(&subslots3, 3, NULL, empty_read, empty_write);
    slots_add_slot(&slots, 3, &subslots3, subslots_read, subslots_write);

    cpu.context = &slots;
    cpu.fetch_opcode = cpu.fetch = cpu.nop = cpu.read = (Z80Read)slots_read;
#ifdef SCCPLUS_DEBUG
    cpu.write = (Z80Write)dbg_slots_write;
#else
    cpu.write = (Z80Write)slots_write;
#endif
    cpu.in = zeta_in;
    cpu.out = zeta_out;
    cpu.halt = Z_NULL;
    cpu.nmia = Z_NULL;
    cpu.inta = Z_NULL;
    cpu.int_fetch = Z_NULL;
    cpu.ld_i_a = Z_NULL;
    cpu.ld_r_a = Z_NULL;
    cpu.reti = Z_NULL;
    cpu.retn = Z_NULL;
    cpu.hook = Z_NULL;
    cpu.illegal = Z_NULL;
    cpu.options = Z80_MODEL_ZILOG_NMOS;
    z80_power(&cpu, Z_TRUE);
    z80_instant_reset(&cpu);

    psg = PSG_new(3579545, AUDIO_SAMPLE_RATE);
    if (!psg) return false;
    PSG_setVolumeMode(psg, EMU2149_VOL_AY_3_8910);
    PSG_set_quality(psg, 0);
    PSG_reset(psg);

    return true;
}
#endif // BAREMSX_MSX2

// Dump de Z80/VDP-staat (via de Delete-toets bij een hang).
void machine_dbg_dump(void)
{
    uint16_t pc = Z80_PC(cpu);
    printf("[dump] PC=%04X bytes:", pc);
    for (int k = 0; k < 12; k++)
        printf(" %02X", slots_read(&slots, (uint16_t)(pc + k)));
    printf("\n  A=%02X BC=%04X DE=%04X HL=%04X SP=%04X IX=%04X IY=%04X  IFF1=%d IM=%d\n",
           Z80_A(cpu), Z80_BC(cpu), Z80_DE(cpu), Z80_HL(cpu),
           Z80_SP(cpu), Z80_IX(cpu), Z80_IY(cpu), cpu.iff1, cpu.im);
    printf("  slotreg=%02X subslotreg=%02X vdpStatus=%02X\n",
           slots_get_slot_register(&slots), subslots3.subslot_register, tms9918.vdpStatus);
    printf("  @0000:");
    for (int k = 0; k < 8; k++) printf(" %02X", slots_read(&slots, (uint16_t)k));
    printf("  @0038:");
    for (int k = 0; k < 4; k++) printf(" %02X", slots_read(&slots, (uint16_t)(0x38 + k)));
    printf("\n");
}

// Debug: lees een adres door de ogen van de CPU (werkt voor beide profielen).
uint8_t machine_dbg_read(uint16_t addr)
{
    return slots_read(&slots, addr);
}

void machine_generate_interrupt(void)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) return; // IRQs komen per scanline uit machine_do_cycles
#endif
    if (g_line_sink) return; // idem: beam-lus regelt de frame-interrupt
    check_and_generate_interrupt(&tms9918);
}


uint32_t machine_get_background_color(void)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) return v9938_backdrop_color(&v9938);
#endif
    return tms9918_get_backdrop_color(&tms9918);
}

static inline int16_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Mix-balans. GEMETEN (Aleste, alle 3 PSG-kanalen vol): emu2149 piekt op
// ~12240 — de kanaal-accumulator is een IIR (ch_out = (ch_out+voltbl<<4)>>1,
// convergeert naar 4080/kanaal), geen 0..765 zoals eerder aangenomen. De oude
// PSG_GAIN=8 kwam dan op ~98k = 3x over de int16-rail: harde clipping =
// "kraken" bij luide muziek (macOS maskeerde het; de DC-gekoppelde
// HDMI-keten niet). Nu: PSG effectief x2,5 (piek 30600, net onder de rail),
// SCC x0,625 — zelfde 4:1-balans als de oude 8:2.
#define PSG_NUM 20
#define SCC_NUM 5
#define MIX_DEN 8

// Genereer `len` interleaved stereo int16-samples (AY-3-8910 + Konami SCC).
// De MSX-PSG is mono, dus links == rechts.
void __not_in_flash_func(machine_get_audio)(int16_t *chunk, uint32_t len)
{
    // De audiosynthese draait op core 1 en start al tijdens het bootmenu — vóór
    // machine_init psg via PSG_new aanmaakt. Dan stilte i.p.v. een NULL-deref.
    if (!psg) {
        for (uint32_t i = 0; i < len; i++) chunk[i] = 0;
        return;
    }
    // Debug (SWD): PSG-kanaalmasker — bit0/1/2 = kanaal A/B/C muten, bit3 =
    // SCC muten. Voor het isoleren van het "niet lekker" klinkende kanaal.
    extern volatile uint32_t dbg_snd_mask;
    extern volatile uint32_t dbg_snd_lpf; // 0=uit, 1=1-pole ~7,6kHz, 2=2-pole
    psg->mask = dbg_snd_mask & 7;

    // DC-blocker (one-pole high-pass, fc ~2Hz). De PSG-uitgang is UNIPOLAIR
    // (0..~12240): zonder dit rijdt de mix op een grote DC-offset de
    // DC-gekoppelde HDMI-keten in en halveert de bruikbare headroom.
    // Centreren geeft symmetrische pieken; de clamp blijft vangnet.
    static int32_t hp_x1 = 0, hp_y1 = 0;
    for (uint32_t i = 0; i < len; i += 2) {
        int32_t psg_out = PSG_calc(psg);
        int32_t scc = (dbg_snd_mask & 8) ? 0 : scc_process(&konami_scc);
        int32_t x = (psg_out * PSG_NUM + scc * SCC_NUM) / MIX_DEN;
        int32_t y = x - hp_x1 + hp_y1 - (hp_y1 >> 12); // y += (x-x1) - y1/4096
        hp_x1 = x;
        hp_y1 = y;
        // Optionele low-pass (à la het RC-uitgangsfilter van echte MSX'en):
        // one-pole y += (x-y)/2 per trap, fc ≈ 7,6kHz @ 48kHz.
        static int32_t lp1 = 0, lp2 = 0;
        if (dbg_snd_lpf >= 1) { lp1 += (y - lp1) >> 1; y = lp1; }
        if (dbg_snd_lpf >= 2) { lp2 += (y - lp2) >> 1; y = lp2; }
        int16_t s = clamp16(y);
        chunk[i]     = s;
        chunk[i + 1] = s;
    }
}

void machine_keydown(uint32_t index)
{
    ppi_on_keydown(&ppi, index);
}

void machine_keyup(uint32_t index)
{
    ppi_on_keyup(&ppi, index);
}
