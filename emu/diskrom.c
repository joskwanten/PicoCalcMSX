#include "diskrom.h"
#include <stdio.h>
#include "pico.h"

// ROM op 4000-7FFF met de FDC-registers op 7FF8-7FFF. GEEN spiegel in
// pagina 2: de BIOS-RAM-scan schrijft bij boot heel pagina 2 door en zou
// via BFF8-BFFF ongewild FDC-commando's afvuren (disk-boot faalt), en een
// ROM-spiegel zet een tweede "AB"-header op 8000 (dubbele init ->
// resetloop). Konami-diskloaders (SD Snatcher) verwachten de registers
// wél op BFF8 — dat komt terug zodra disk-in-3-3 onderzocht is.

uint8_t __not_in_flash_func(diskrom_read)(void *context, uint16_t address)
{
    diskrom_t *d = (diskrom_t *)context;
    if (address >= 0x7FF8 && address < 0x8000)
        return wd2793_read(&d->fdc, address);
    if (address >= 0x4000 && address < 0x8000) {
        uint32_t off = address - 0x4000;
        return off < d->rom_size ? d->rom[off] : 0xFF;
    }
    return 0xFF;
}

void __not_in_flash_func(diskrom_write)(void *context, uint16_t address, uint8_t value)
{
    diskrom_t *d = (diskrom_t *)context;
    if (address >= 0x7FF8 && address < 0x8000) {
        wd2793_write(&d->fdc, address, value);
        return;
    }
#ifdef SLOT_DEBUG
    fprintf(stderr, "[diskrom] stray write %04X=%02X (game schrijft in ROM-window)\n", address, value);
#endif
}
