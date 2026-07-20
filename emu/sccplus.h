#ifndef SCCPLUS_H
#define SCCPLUS_H

// Konami Sound Cartridge (SCC-I, 2312P001): 64KB RAM + SCC+ — de cartridge
// die bij Snatcher/SD Snatcher zat en die die spellen VEREISEN (de RAM dient
// als extra opslag). Vier 8KB-vensters op 0x4000-0xBFFF met bankregisters op
// 5000/7000/9000/B000 (Konami-SCC-stijl) en een moderegister op 0xBFFE:
//   bit 0-2: venster 1-3 RAM-schrijfbaar
//   bit 4:   SCC+-modus (SCC+ op B800 via bit 7 van bankreg 4)
//   bit 5:   alles-RAM-modus (alle vier de vensters schrijfbaar)
// De SCC(+)-registers delen de audio-core met de cartridge-SCC (kanaal 5
// deelt daar golfvorm 4 — benadering; SD Snatcher gebruikt de RAM vooral).

#include <stdint.h>
#include "konami-mega-rom-scc.h"

typedef struct {
    uint8_t *ram;       // 64KB, door de host geleverd
    konami_scc_t *scc;  // gedeelde SCC-audio-core (registerfile + mixer)
    uint8_t bank[4];    // 8KB-pagina per venster (power-on: 0,1,2,3)
    uint8_t mode;       // moderegister 0xBFFE
    uint8_t wave5[32];  // kanaal-5-golfvorm (apart opgeslagen: leesbaar voor
                        // Konami's detectie; audio deelt golf 4 — benadering)
} sccplus_t;

void sccplus_init(sccplus_t *s, uint8_t *ram64k, konami_scc_t *scc);
uint8_t sccplus_read(void *ctx, uint16_t addr);
void sccplus_write(void *ctx, uint16_t addr, uint8_t value);

#endif // SCCPLUS_H
