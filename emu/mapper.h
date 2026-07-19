#ifndef MAPPER_H
#define MAPPER_H

// Cartridge ROM mappers (besides Konami-SCC, which lives in
// konami-mega-rom-scc.c because it also does sound). Covers plain 16/32K,
// Konami (Konami4, no SCC), ASCII8 and ASCII16, plus size/heuristic detection.

#include <stdint.h>

typedef enum {
    MAPPER_NONE = 0,     // empty slot
    MAPPER_PLAIN,        // linear 16/32K ROM at 0x4000
    MAPPER_KONAMI,       // Konami4 (no SCC): banks at 0x6000/0x8000/0xA000
    MAPPER_ASCII8,       // 8K banks: 0x6000/0x6800/0x7000/0x7800
    MAPPER_ASCII16,      // 16K banks: 0x6000/0x7000
    MAPPER_KONAMI_SCC,   // handled by konami-mega-rom-scc.c
} mapper_type_t;

typedef struct {
    const uint8_t *rom;
    uint32_t size;
    mapper_type_t type;
    uint8_t page[8];     // 8K page (addr>>13) -> 8K bank index into rom
    uint32_t nbanks8;    // size / 8192 (>= 1)
} mapper_t;

// Guess the mapper from the ROM (size + bank-switch write-pattern heuristic).
mapper_type_t mapper_detect(const uint8_t *rom, uint32_t size);

void mapper_init(mapper_t *m, const uint8_t *rom, uint32_t size, mapper_type_t type);
uint8_t mapper_read(void *context, uint16_t address);
void mapper_write(void *context, uint16_t address, uint8_t value);

const char *mapper_name(mapper_type_t t);

#endif // MAPPER_H
