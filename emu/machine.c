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
#endif
#include "pico.h"

// Platform-agnostische machine: BIOS/cartridge komen via machine_init binnen.
// Verschillen met de SDL-versie:
//  - BIOS komt uit flash (bios_rom[] in bios_rom.h) i.p.v. van disk
//  - geen cartridge/SCC en geen audio (komt later)

uint8_t ram[0x10000];   // MSX RAM in SRAM
Z80 cpu;

slots_context_t slots;
static subslot_context_t subslots3; // slot 3: RAM op 3-2 (NMS-8245-stijl)
tms9918_context_t tms9918;
tms9918_context_t tms9918_snap; // snapshot voor core 1 (blit) — vermijdt VDP-race
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
#ifdef BAREMSX_MSX2
v9938_context_t v9938;
v9938_context_t v9938_snap; // snapshot voor de renderlus (zoals tms9918_snap)
static rtc_t rtc;
static mappedram_t mapram;
static uint8_t mapram_pool[MAPPEDRAM_SIZE];
#endif

void machine_attach_disk(const uint8_t *disk_rom, uint32_t disk_rom_size,
                         uint8_t sides, uint32_t total_sectors,
                         void *io_ctx, wd_sector_io_t io)
{
    diskrom.rom = disk_rom;
    diskrom.rom_size = disk_rom_size;
    wd2793_init(&diskrom.fdc, sides, total_sectors, io_ctx, io);
    disk_attached = true;
}

uint8_t psg_register = 0;

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
        case 0x99: v9938_write_ctrl(&v9938, value); return;
        case 0x9a: v9938_write_palette(&v9938, value); return;
        case 0x9b: v9938_write_indirect(&v9938, value); return;
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

    mapper_type_t mt = mapper_detect(game, game_size);
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

void __not_in_flash_func(machine_do_cycles)(void)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) {
        // Scanline-granulair: 262 lijnen x 228 T-states (NTSC-cadans); de
        // V9938 krijgt na elke lijn zijn beam-událost (FH/VR/F + IRQs).
        for (int line = 0; line < 262; line++) {
            z80_run(&cpu, 228);
            v9938_scanline(&v9938, line);
        }
        return;
    }
#endif
    z80_run(&cpu, CYC_PER_INT);
}

#ifdef BAREMSX_MSX2
// MSX2-profiel: NMS-8245-achtige machine met de V9938.
//   Slot 0:  MSX2-BIOS (32KB, 0x0000)
//   Slot 1:  cartridge (zelfde mapper-pad als MSX1)
//   Slot 2:  leeg (FM-PAC later)
//   Slot 3:  expanded — 3-0 sub-ROM (EXT), 3-2 mapper-RAM (128KB, FC-FF)
// bios/ext worden door de frontend als 64KB-gepadde buffers aangeleverd
// (rom_read is bewust simpel en maskeert alleen op 16-bit).
bool machine_init_msx2(const uint8_t *bios, uint32_t bios_size,
                       const uint8_t *ext, uint32_t ext_size,
                       const uint8_t *game, uint32_t game_size)
{
    (void)bios_size; (void)ext_size;
    g_msx2 = true;

    v9938_init(&v9938);
    v9938_register_interrupt_func(&v9938, gen_interrupt);
    rtc_init(&rtc);
    mappedram_init(&mapram, mapram_pool);
    scc_init(&konami_scc);

    slots_add_slot(&slots, 0, (void *)bios, rom_read, rom_write);

    mapper_type_t mt = mapper_detect(game, game_size);
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
    slots_add_slot(&slots, 2, NULL, empty_read, empty_write);

    subslots3.subslot_register = 0;
    subslots_add_subslot(&subslots3, 0, (void *)ext, rom_read, rom_write); // sub-ROM
    subslots_add_subslot(&subslots3, 1, NULL, empty_read, empty_write);
    subslots_add_subslot(&subslots3, 2, &mapram, mappedram_read, mappedram_write);
    subslots_add_subslot(&subslots3, 3, NULL, empty_read, empty_write);
    slots_add_slot(&slots, 3, &subslots3, subslots_read, subslots_write);

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
    check_and_generate_interrupt(&tms9918);
}

void machine_get_rendered_image_rgba(uint32_t *image)
{
    tms9918_render_rgba(&tms9918, image);
}

void machine_get_rendered_line(uint32_t *line, int y)
{
    tms9918_render_line(&tms9918, line, y);
}

// --- VDP snapshot voor de blit op core 1 ---
// Core 0 kopieert de live VDP-state; core 1 rendert uit de snapshot.
void machine_snapshot_vdp(void)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) {
        memcpy(&v9938_snap, &v9938, sizeof(v9938_context_t));
        return;
    }
#endif
    memcpy(&tms9918_snap, &tms9918, sizeof(tms9918_context_t));
}

void __not_in_flash_func(machine_render_snapshot_line)(uint32_t *line, int y)
{
    tms9918_render_line(&tms9918_snap, line, y);
}

// MSX2: displaygeometrie + 512-brede lijnrender uit de snapshot.
// (Literals i.p.v. V9938_LINE_W/V9938_LINES: v9938.h bestaat niet in
// MSX1-only builds en g_msx2 is daar altijd false.)
int machine_display_width(void)  { return g_msx2 ? 512 : 256; }
int machine_display_height(void) { return g_msx2 ? 212 : 192; }

#ifdef BAREMSX_MSX2
void machine_render_snapshot_line_wide(uint32_t *line, int y)
{
    v9938_render_line(&v9938_snap, line, y);
}
#endif

uint32_t machine_snapshot_background_color(void)
{
#ifdef BAREMSX_MSX2
    if (g_msx2) return v9938_backdrop_color(&v9938_snap);
#endif
    return tms9918_get_backdrop_color(&tms9918_snap);
}

uint32_t machine_get_background_color(void)
{
    return tms9918_get_backdrop_color(&tms9918);
}

static inline int16_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Mix-balans (twee knoppen) + master-volume tegen oversturing.
// emu2149 geeft ~0..765; de SCC zit bij games vaak op bescheiden volumes.
#define PSG_GAIN 8
#define SCC_GAIN 2
#define MASTER_NUM 3   // totaal op 75% voor headroom
#define MASTER_DEN 4

// Genereer `len` interleaved stereo int16-samples (AY-3-8910 + Konami SCC).
// De MSX-PSG is mono, dus links == rechts.
void __not_in_flash_func(machine_get_audio)(int16_t *chunk, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 2) {
        int32_t psg_out = PSG_calc(psg);
        int32_t scc = scc_process(&konami_scc);
        int32_t mix = (psg_out * PSG_GAIN + scc * SCC_GAIN) * MASTER_NUM / MASTER_DEN;
        int16_t s = clamp16(mix);
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
