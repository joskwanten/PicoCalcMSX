// WD2793 FDC — zie wd2793.h. Port van msx_rs/src/fdc.rs.

#include "wd2793.h"
#include "pico.h"

#include <string.h>
#ifdef WD_DEBUG
#include <stdio.h>
#endif

#define SECTORS_PER_TRACK 9

void wd2793_init(wd2793_t *fdc, uint8_t sides, uint32_t total_sectors,
                 void *io_ctx, wd_sector_io_t io)
{
    memset(fdc, 0, sizeof *fdc);
    fdc->sides = sides;
    fdc->total_sectors = total_sectors;
    fdc->io_ctx = io_ctx;
    fdc->io = io;
    fdc->sector = 1;
    fdc->type1_status = true;
    fdc->tmode = WD_T_NONE;
}

// Drive A met een disk erin; drive B (bit 0 van de drive-latch) bestaat niet.
static bool drive_ready(const wd2793_t *fdc)
{
    return fdc->sides != 0 && (fdc->drive & 0x01) == 0;
}

static bool drq(const wd2793_t *fdc)
{
    return fdc->tmode != WD_T_NONE;
}

// Logische sectorindex van (track, side, sector); sector is 1-based.
// Return false als de positie buiten de disk valt.
static bool lba_of(const wd2793_t *fdc, uint32_t *lba)
{
    if (fdc->sector == 0 || fdc->sector > SECTORS_PER_TRACK || fdc->side >= fdc->sides)
        return false;
    uint32_t l = ((uint32_t)fdc->track * fdc->sides + fdc->side) * SECTORS_PER_TRACK
               + (fdc->sector - 1);
    if (l >= fdc->total_sectors) return false;
    *lba = l;
    return true;
}

// Type-I commando afronden: status weerspiegelt de koppositie, INTRQ meldt klaar.
static void finish_type1(wd2793_t *fdc)
{
    fdc->type1_status = true;
    fdc->status = 0x20; // head loaded
    fdc->intrq = true;
}

static void begin_read(wd2793_t *fdc)
{
    fdc->type1_status = false;
    uint32_t lba;
    if (!drive_ready(fdc) || !lba_of(fdc, &lba) ||
        fdc->io(fdc->io_ctx, lba, fdc->buf, false) < 0) {
        fdc->status = 0x10; // record not found
        fdc->intrq = true;
        return;
    }
    fdc->status = 0x01; // busy tot de laatste byte is opgehaald
    fdc->tmode = WD_T_READ;
    fdc->tpos = 0;
    fdc->tlen = WD_SECTOR_SIZE;
}

static void begin_write(wd2793_t *fdc)
{
    fdc->type1_status = false;
    uint32_t lba;
    if (!drive_ready(fdc) || !lba_of(fdc, &lba)) {
        fdc->status = 0x10;
        fdc->intrq = true;
        return;
    }
    fdc->status = 0x01;
    fdc->tmode = WD_T_WRITE;
    fdc->tpos = 0;
    fdc->tlen = WD_SECTOR_SIZE;
    fdc->tlba = lba;
}

// Commandobyte uitvoeren. Alles is synchroon klaar; BUSY (bit 0) is alleen
// zichtbaar tijdens een lopende datatransfer, waar DRQ het tempo bepaalt.
static void command(wd2793_t *fdc, uint8_t cmd)
{
#ifdef WD_DEBUG
    fprintf(stderr, "[fdc] cmd=%02X trk=%u sec=%u side=%u drv=%02X\n",
            cmd, fdc->track, fdc->sector, fdc->side, fdc->drive);
#endif
    fdc->intrq = false;
    switch (cmd >> 4) {
    // Type I — RESTORE: kop naar track 0.
    case 0x0:
        fdc->track = 0;
        finish_type1(fdc);
        break;
    // Type I — SEEK: doeltrack komt via het dataregister.
    case 0x1:
        fdc->track = fdc->data;
        finish_type1(fdc);
        break;
    // Type I — STEP / STEP-IN (kale STEP herhaalt de laatste richting; we
    // benaderen 'm met step-in, waarvoor drivers 'm in de praktijk gebruiken).
    case 0x2: case 0x3: case 0x4: case 0x5:
        if (fdc->track < 0xFF) fdc->track++;
        finish_type1(fdc);
        break;
    case 0x6: case 0x7:
        if (fdc->track > 0) fdc->track--;
        finish_type1(fdc);
        break;
    // Type II — READ SECTOR (0x8 enkel, 0x9 multiple; multiple wordt door de
    // MSX-driver niet gebruikt en als enkel bediend).
    case 0x8: case 0x9:
        begin_read(fdc);
        break;
    // Type II — WRITE SECTOR.
    case 0xA: case 0xB:
        begin_write(fdc);
        break;
    // Type III — READ ADDRESS: stream het 6-byte ID-veld van de "volgende"
    // sectorheader (track, side, sector, size=2, crc, crc).
    case 0xC:
        fdc->type1_status = false;
        fdc->status = 0;
        fdc->buf[0] = fdc->track;
        fdc->buf[1] = fdc->side;
        fdc->buf[2] = fdc->sector;
        fdc->buf[3] = 2;
        fdc->buf[4] = 0;
        fdc->buf[5] = 0;
        fdc->tmode = WD_T_READ;
        fdc->tpos = 0;
        fdc->tlen = 6;
        break;
    // Type IV — FORCE INTERRUPT: breek af wat er loopt.
    case 0xD:
#ifdef WD_DEBUG
        if (fdc->tmode != WD_T_NONE)
            fprintf(stderr, "[fdc]   D0 aborts transfer: mode=%d pos=%u/%u\n",
                    fdc->tmode, fdc->tpos, fdc->tlen);
#endif
        fdc->tmode = WD_T_NONE;
        fdc->status = 0;
        fdc->type1_status = true;
        fdc->intrq = true;
        break;
    // Type III — READ TRACK (ongebruikt door de MSX-driver): meteen klaar.
    case 0xE:
        fdc->type1_status = false;
        fdc->status = 0;
        fdc->intrq = true;
        break;
    // Type III — WRITE TRACK = FORMAT: slik een track aan formatbytes in
    // zodat een FORMAT lijkt te slagen; de sector-writes erna leggen de
    // echte data neer.
    case 0xF:
        fdc->type1_status = false;
        fdc->status = 0;
        fdc->tmode = WD_T_FORMAT;
        fdc->tpos = 0;
        break;
    }
}

// Dataregister-read: volgende byte van een lopende leestransfer.
static uint8_t read_data(wd2793_t *fdc)
{
    if (fdc->tmode == WD_T_READ) {
        fdc->data = fdc->tpos < fdc->tlen ? fdc->buf[fdc->tpos] : 0;
        fdc->tpos++;
        if (fdc->tpos >= fdc->tlen) {
            fdc->tmode = WD_T_NONE;
            fdc->status = 0;
            fdc->intrq = true;
        }
    }
    return fdc->data;
}

// Dataregister-write: volgende byte van een lopende schrijftransfer.
static void write_data(wd2793_t *fdc, uint8_t value)
{
    fdc->data = value;
    if (fdc->tmode == WD_T_WRITE) {
        fdc->buf[fdc->tpos++] = value;
        if (fdc->tpos >= fdc->tlen) {
            if (fdc->io)
                fdc->io(fdc->io_ctx, fdc->tlba, fdc->buf, true);
            fdc->tmode = WD_T_NONE;
            fdc->status = 0;
            fdc->intrq = true;
        }
    } else if (fdc->tmode == WD_T_FORMAT) {
        // Formatstream: wegwerpen zodra er een track aan bytes binnen is.
        if (++fdc->tpos >= 6250) {
            fdc->tmode = WD_T_NONE;
            fdc->status = 0;
            fdc->intrq = true;
        }
    }
}

uint8_t __not_in_flash_func(wd2793_read)(wd2793_t *fdc, uint16_t addr)
{
    switch (addr & 0x07) {
    case 0: {
        // Statusread wist INTRQ (WD279x-semantiek).
        fdc->intrq = false;
        uint8_t st = fdc->status;
        if (fdc->type1_status) {
            // Type I-layout: bit7 NOT-READY, bit2 TRACK0, bit1 INDEX.
            if (!drive_ready(fdc)) st |= 0x80;
            if (fdc->track == 0) st |= 0x04;
            fdc->index_flip = !fdc->index_flip;
            if (fdc->index_flip) st |= 0x02;
        } else {
            // Type II/III-layout: bit7 NOT-READY, bit1 DRQ.
            if (!drive_ready(fdc)) st |= 0x80;
            if (drq(fdc)) st |= 0x02;
        }
        return st;
    }
    case 1: return fdc->track;
    case 2: return fdc->sector;
    case 3: return read_data(fdc);
    case 4: return fdc->side;
    case 5: return fdc->drive;
    // 0x7FFF: bit 7 = !DRQ, bit 6 = !INTRQ, rest hoog.
    default: {
        uint8_t v = 0xFF;
        if (drq(fdc)) v &= (uint8_t)~0x80;
        if (fdc->intrq) v &= (uint8_t)~0x40;
        return v;
    }
    }
}

void __not_in_flash_func(wd2793_write)(wd2793_t *fdc, uint16_t addr, uint8_t value)
{
    switch (addr & 0x07) {
    case 0: command(fdc, value); break;
    case 1: fdc->track = value; break;
    case 2: fdc->sector = value; break;
    case 3: write_data(fdc, value); break;
    case 4: fdc->side = value & 0x01; break;
    case 5: fdc->drive = value; break;
    default: break;
    }
}
