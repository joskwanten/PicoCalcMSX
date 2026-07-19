#include "mapper.h"
#include "pico.h" // __not_in_flash_func

const char *mapper_name(mapper_type_t t)
{
    switch (t) {
    case MAPPER_PLAIN:      return "plain";
    case MAPPER_KONAMI:     return "konami";
    case MAPPER_ASCII8:     return "ascii8";
    case MAPPER_ASCII16:    return "ascii16";
    case MAPPER_KONAMI_SCC: return "konami-scc";
    default:                return "none";
    }
}

void mapper_init(mapper_t *m, const uint8_t *rom, uint32_t size, mapper_type_t type)
{
    m->rom = rom;
    m->size = size;
    m->type = type;
    m->nbanks8 = size / 8192u;
    if (m->nbanks8 == 0) m->nbanks8 = 1;

    // Default 8K bank mapping for the cartridge area (0x4000-0xBFFF = pages 2..5).
    for (int i = 0; i < 8; i++) m->page[i] = 0;
    switch (type) {
    case MAPPER_KONAMI:   // 0x4000 fixed at bank 0; others default to 1,2,3
        m->page[2] = 0; m->page[3] = 1; m->page[4] = 2; m->page[5] = 3;
        break;
    case MAPPER_ASCII16:  // 16K bank 0 = 8K banks 0,1 in both windows
        m->page[2] = 0; m->page[3] = 1; m->page[4] = 0; m->page[5] = 1;
        break;
    case MAPPER_ASCII8:   // all banks 0 at reset
    default:
        break;
    }
}

uint8_t __not_in_flash_func(mapper_read)(void *context, uint16_t address)
{
    mapper_t *m = (mapper_t *)context;
    if (m->type == MAPPER_PLAIN) {
        uint32_t off = (uint32_t)address - 0x4000u;
        return (off < m->size) ? m->rom[off] : 0xFF;
    }
    uint32_t bank = m->page[(address >> 13) & 7] % m->nbanks8;
    return m->rom[bank * 8192u + (address & 0x1FFF)];
}

void __not_in_flash_func(mapper_write)(void *context, uint16_t address, uint8_t value)
{
    mapper_t *m = (mapper_t *)context;
    switch (m->type) {
    case MAPPER_KONAMI: // 0x6000/0x8000/0xA000 select the 0x6000/0x8000/0xA000 windows
        if (address >= 0x6000 && address < 0x8000) m->page[3] = value;
        else if (address >= 0x8000 && address < 0xA000) m->page[4] = value;
        else if (address >= 0xA000 && address < 0xC000) m->page[5] = value;
        break;
    case MAPPER_ASCII8:
        if (address >= 0x6000 && address < 0x6800) m->page[2] = value;
        else if (address >= 0x6800 && address < 0x7000) m->page[3] = value;
        else if (address >= 0x7000 && address < 0x7800) m->page[4] = value;
        else if (address >= 0x7800 && address < 0x8000) m->page[5] = value;
        break;
    case MAPPER_ASCII16:
        if (address >= 0x6000 && address < 0x6800) {
            m->page[2] = (uint8_t)(value * 2);
            m->page[3] = (uint8_t)(value * 2 + 1);
        } else if (address >= 0x7000 && address < 0x7800) {
            m->page[4] = (uint8_t)(value * 2);
            m->page[5] = (uint8_t)(value * 2 + 1);
        }
        break;
    default:
        break;
    }
}

mapper_type_t mapper_detect(const uint8_t *rom, uint32_t size)
{
    if (rom == 0 || size == 0) return MAPPER_NONE;

    // Small ROMs are plain (16K/32K/48K without a mapper).
    if (size <= 32u * 1024u) return MAPPER_PLAIN;

    // Heuristic: count LD (nnnn),A (0x32) writes to each mapper's bank registers.
    int konami = 0, scc = 0, ascii8 = 0, ascii16 = 0;
    for (uint32_t i = 0; i + 2 < size; i++) {
        if (rom[i] != 0x32) continue; // LD (nnnn),A
        uint16_t a = (uint16_t)(rom[i + 1] | (rom[i + 2] << 8));
        switch (a) {
        case 0x4000: case 0x8000: case 0xA000: konami++; break;
        case 0x5000: case 0x9000: case 0xB000: scc++; break;
        case 0x6800: case 0x7800: ascii8++; break;
        case 0x6000: konami++; scc++; ascii8++; ascii16++; break; // shared
        case 0x7000: scc++; ascii8++; ascii16++; break;           // shared
        default: break;
        }
    }

    // Pick the highest score; prefer SCC then Konami on ties (Konami mega-ROMs).
    int best = scc;
    mapper_type_t t = MAPPER_KONAMI_SCC;
    if (konami > best) { best = konami; t = MAPPER_KONAMI; }
    if (ascii8 > best) { best = ascii8; t = MAPPER_ASCII8; }
    if (ascii16 > best) { best = ascii16; t = MAPPER_ASCII16; }
    if (best == 0) t = MAPPER_ASCII16; // >32K with no clear pattern -> ASCII16 is the safest default
    return t;
}
