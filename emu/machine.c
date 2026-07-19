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
#include "pico.h"

// Platform-agnostische machine: BIOS/cartridge komen via machine_init binnen.
// Verschillen met de SDL-versie:
//  - BIOS komt uit flash (bios_rom[] in bios_rom.h) i.p.v. van disk
//  - geen cartridge/SCC en geen audio (komt later)

uint8_t ram[0x10000];   // MSX RAM in SRAM
Z80 cpu;

slots_context_t slots;
subslot_context_t subslots;
tms9918_context_t tms9918;
tms9918_context_t tms9918_snap; // snapshot voor core 1 (blit) — vermijdt VDP-race
ppi_context_t ppi;
konami_scc_t konami_scc;
mapper_t cart; // niet-SCC cartridge-mapper (plain/konami/ascii8/16)
PSG *psg; // AY-3-8910 (emu2149, integer)

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
                  const uint8_t *game, uint32_t game_size)
{
    (void)bios_size; // rom_read maskeert op 16-bit; de BIOS beslaat slot 0 (0x0000-0x7FFF)

    // Subslots in slot 3: subslot 2 = 64KB RAM (zoals de SDL-config)
    subslots_add_subslot(&subslots, 0, NULL, empty_read, empty_write);
    subslots_add_subslot(&subslots, 1, NULL, empty_read, empty_write);
    subslots_add_subslot(&subslots, 2, ram, ram_read, ram_write);
    subslots_add_subslot(&subslots, 3, NULL, empty_read, empty_write);

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
    slots_add_slot(&slots, 2, NULL, empty_read, empty_write);
    slots_add_slot(&slots, 3, &subslots, subslots_read, subslots_write);

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
    z80_run(&cpu, CYC_PER_INT);
}

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
           slots_get_slot_register(&slots), subslots.subslot_register, tms9918.vdpStatus);
    printf("  @0000:");
    for (int k = 0; k < 8; k++) printf(" %02X", slots_read(&slots, (uint16_t)k));
    printf("  @0038:");
    for (int k = 0; k < 4; k++) printf(" %02X", slots_read(&slots, (uint16_t)(0x38 + k)));
    printf("\n");
}

void machine_generate_interrupt(void)
{
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
    memcpy(&tms9918_snap, &tms9918, sizeof(tms9918_context_t));
}

void __not_in_flash_func(machine_render_snapshot_line)(uint32_t *line, int y)
{
    tms9918_render_line(&tms9918_snap, line, y);
}

uint32_t machine_snapshot_background_color(void)
{
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
