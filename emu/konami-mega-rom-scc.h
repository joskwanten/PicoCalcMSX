#ifndef KONAMI_MEGA_ROM_SCC_H
#define KONAMI_MEGA_ROM_SCC_H

#include <stdint.h>

#define SCC_PAGE_SIZE 0x2000
#define SCC_SAMPLE_RATE 48000   // direct 48 kHz -> HDMI-native (toonhoogte komt van de 3.579545MHz-klok)

typedef struct {
    uint8_t scc[256];       // SCC-registers (waveforms signed, rest unsigned)
    uint32_t phase[5];      // fixed-point fase per kanaal (5 msb = 32 stappen)
    uint32_t step[5];       // voorberekende fase-stap per kanaal
    uint8_t selected_pages[0x10000 / SCC_PAGE_SIZE];
    uint8_t* rom;
    uint32_t bank_mask;     // aantal 8KB-banks - 1 (voor out-of-bounds bescherming)
} konami_scc_t;

void scc_init(konami_scc_t* context);
int16_t scc_process(konami_scc_t* context);
void scc_set_rom(konami_scc_t* context, uint8_t* rom, uint32_t size);
uint8_t scc_read(void* context, uint16_t address);
void scc_write(void* context, uint16_t address, uint8_t value);

// Directe registertoegang (layout als 9800-98FF: 00-7F golven, 80-89 freq,
// 8A-8E volume, 8F enable) — gebruikt door de SCC-I/Sound Cartridge (sccplus).
void scc_core_write(konami_scc_t* context, uint8_t reg, uint8_t value);
uint8_t scc_core_read(konami_scc_t* context, uint8_t reg);

#endif // KONAMI_MEGA_ROM_SCC_H
