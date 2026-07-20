#include <stdio.h>
#include <string.h>
#include "konami-mega-rom-scc.h"
#include "pico.h"

// Konami SCC: mapper (bank switching) + integer-based geluidsgenerator.
// De geluidslogica komt uit het RogueDrive-project (integer, fixed-point fase,
// voorberekende steps) — licht genoeg voor de RP2350, geen double/FIR.

void scc_init(konami_scc_t* context)
{
    memset(context, 0, sizeof(konami_scc_t));
    // Initiele bank-config (zoals de originele mapper)
    context->selected_pages[5] = 1;
    context->selected_pages[6] = 2;
    context->selected_pages[7] = 4;
}

void scc_set_rom(konami_scc_t* context, uint8_t* rom, uint32_t size)
{
    context->rom = rom;
    // Bank-mask = aantal 8KB-banks - 1 (ROM-groottes zijn machten van 2).
    // Voorkomt out-of-bounds reads bij bank-nummers > ROM (o.a. 0x3f = SCC-enable).
    context->bank_mask = (size / SCC_PAGE_SIZE) - 1;
}

// Fase-stap per sample voor een frequentie-register (op write voorberekend,
// zodat scc_process geen 64-bit deling hoeft te doen).
static uint32_t compute_step(konami_scc_t* c, uint32_t chan)
{
    uint8_t hi = c->scc[0x80 + 2 * chan + 1] & 0x0F;
    uint8_t lo = c->scc[0x80 + 2 * chan];
    uint32_t t = (hi << 8) | lo;
    uint64_t step64 = ((uint64_t)3579545 << 27) / (1LL * (t + 1) * SCC_SAMPLE_RATE);
    return (uint32_t)step64;
}

int16_t __not_in_flash_func(scc_process)(konami_scc_t* c)
{
    int32_t mixed = 0;
    for (int chan = 0; chan < 5; chan++) {
        c->phase[chan] += c->step[chan];
        uint32_t pos = (c->phase[chan] >> 27) & 31;
        int wave_off = (chan < 4) ? (chan << 5) : (3 << 5); // kanaal 4 deelt golf 3
        int16_t wave = (int8_t)c->scc[wave_off + pos];
        int16_t vol = (c->scc[0x8F] & (1 << chan)) ? (c->scc[0x8A + chan] & 0x0F) : 0;
        mixed += wave * vol;
    }
    return (int16_t)(mixed * 3);
}

uint8_t __not_in_flash_func(scc_read)(void* context, uint16_t address)
{
    konami_scc_t* c = (konami_scc_t*)context;
    uint32_t page_index = (address >> 13) % 8;
    uint32_t page = c->selected_pages[page_index] & c->bank_mask; // masker tegen out-of-bounds
    uint8_t rv = c->rom[(page * SCC_PAGE_SIZE) + (address % SCC_PAGE_SIZE)];
#ifdef SCC_DEBUG
    {
        // Eerste fetch op het F1SPIRIT-beslispunt: welke bank + welke byte?
        extern volatile uint32_t sccr_bf50_bank, sccr_bf50_val, sccr_bf50_hits;
        if (address == 0xBF50) {
            if (sccr_bf50_hits == 0) { sccr_bf50_bank = page; sccr_bf50_val = rv; }
            sccr_bf50_hits++;
        }
    }
#endif
    return rv;
}

void __not_in_flash_func(scc_write)(void* context, uint16_t address, uint8_t value)
{
#ifdef SCC_DEBUG
    {
        extern uint16_t machine_dbg_pc(void);
        // RAM-log (SWD-uitleesbaar op de Pico) + stderr (SDL).
        extern volatile uint16_t scc_log_a[64], scc_log_pc[64];
        extern volatile uint8_t scc_log_v[64];
        extern volatile int scc_log_n;
        if (scc_log_n < 64 && address >= 0x5000) {
            scc_log_a[scc_log_n] = address;
            scc_log_v[scc_log_n] = value;
            scc_log_pc[scc_log_n] = machine_dbg_pc();
            scc_log_n++;
#ifndef PICO_BUILD_NO_STDERR
            fprintf(stderr, "[scc] w %04X=%02X pc=%04X\n", address, value, machine_dbg_pc());
#endif
        }
    }
#endif

    konami_scc_t* c = (konami_scc_t*)context;
    if (address % 0x2000 >= 0x1800) {
        if (c->selected_pages[(address >> 13) % 8] == 0x3f) {
            uint32_t reg = (address % 0x2000) - 0x1800;
            if (reg <= 255)
                scc_core_write(c, (uint8_t)reg, value);
        }
    } else {
        c->selected_pages[(address >> 13) % 8] = value & 0x3f;
    }
}

void __not_in_flash_func(scc_core_write)(konami_scc_t* c, uint8_t reg, uint8_t value)
{
    c->scc[reg] = value;
    if (reg >= 0x80 && reg <= 0x89) {
        uint32_t chan = (reg - 0x80) >> 1;
        c->step[chan] = compute_step(c, chan);
    }
}

uint8_t __not_in_flash_func(scc_core_read)(konami_scc_t* c, uint8_t reg)
{
    return c->scc[reg];
}
