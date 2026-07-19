#ifndef FLASH_STAGE_H
#define FLASH_STAGE_H

// Grote cartridge-ROMs passen niet in RAM (heap ~60 KB na framebuffers etc.);
// die stagen we naar een gereserveerde regio bovenin de QSPI-flash en de
// mapper leest ze daarna gewoon via XIP (ruim snel genoeg voor de Z80).
//
// LET OP: flash_stage_rom() erased/programmeert de flash en mag dus alleen
// draaien vóórdat core 1 / HSTX-video gestart is (een flash-stall onder een
// draaiende HSTX-scanout hangt het beeld). Daarom zet het menu zijn keuze in
// de watchdog-scratch en doet een zachte reboot; bij de herstart wordt de ROM
// gestaged vóór video_hstx_init.

#include <stdint.h>

// Als de stage-regio al deze ROM bevat (zelfde naam + grootte): XIP-pointer,
// anders NULL. Veilig op elk moment (leest alleen).
const uint8_t *flash_stage_get(const char *name, uint32_t *size);

// Lees dir/name in chunks van SD en programmeer 'm in de stage-regio (skipt
// het flashen als de inhoud er al staat). Alleen aanroepen vóór core 1 draait!
// Returnt de XIP-pointer naar de ROM, of NULL bij een fout.
const uint8_t *flash_stage_rom(const char *dir, const char *name, uint32_t *size);

#endif // FLASH_STAGE_H
