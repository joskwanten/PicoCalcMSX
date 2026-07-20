// Zie sccplus.h. Gedrag naar de MSX Assembly Page-beschrijving van de
// Sound Cartridge; goed genoeg voor Snatcher/SD Snatcher (RAM + geluid).

#include "sccplus.h"
#include "pico.h"
#include <string.h>
#ifdef SCCPLUS_DEBUG
#include <stdio.h>
extern uint16_t machine_dbg_pc(void);
static int scp_log = 0;
#define SCP_LOG(...) do { if (scp_log < 20000) { fprintf(stderr, __VA_ARGS__); scp_log++; } } while (0)
#else
#define SCP_LOG(...)
#endif

void sccplus_init(sccplus_t *s, uint8_t *ram64k, konami_scc_t *scc)
{
    // 0x00: géén "AB"-boot-header, maar wél onderscheidbaar van open bus
    // (0xFF) — Konami's detectie kijkt eerst of er überhaupt geheugen in het
    // slot gedecodeerd wordt voordat hij het mode-register probeert.
    memset(ram64k, 0x00, 65536);
    s->ram = ram64k;
    s->scc = scc;
    s->mode = 0;
    for (int i = 0; i < 4; i++) s->bank[i] = (uint8_t)i;
}

static inline int window(uint16_t a) { return (a - 0x4000) >> 13; }

// Vertaal een SCC+-registeroffset (B800-B8FF) naar de gedeelde core
// (9800-layout). Kanaal-5-golf (80-9F) heeft eigen opslag (wave5).
static inline int sccplus_reg(uint8_t off)
{
    if (off < 0x80) return off;          // golven kanaal 1-4
    if (off < 0xA0) return -1;           // golf kanaal 5: aparte opslag
    if (off < 0xC0) return off - 0x20;   // A0-BF -> 80-9F (freq/vol/enable)
    return -1;
}

static uint8_t sccplus_read_inner(sccplus_t *s, uint16_t a);
uint8_t __not_in_flash_func(sccplus_read)(void *ctx, uint16_t a)
{
    sccplus_t *s = (sccplus_t *)ctx;
    uint8_t v = sccplus_read_inner(s, a);
#ifdef SCCPLUS_DEBUG
    SCP_LOG("[scp] r %04X->%02X mode=%02X pc=%04X\n", a, v, s->mode, machine_dbg_pc());
#endif
    return v;
}

static uint8_t __not_in_flash_func(sccplus_read_inner)(sccplus_t *s, uint16_t a)
{
    if (a < 0x4000 || a >= 0xC000) return 0xFF;
    int w = window(a);
    uint16_t off = a & 0x1FFF;

    if (!(s->mode & 0x10)) {
        // SCC (compat): venster 3 op pagina 0x3F -> 9800-99FF
        if (!(s->mode & 0x20) && w == 2 && (s->bank[2] & 0x3F) == 0x3F &&
            off >= 0x1800 && off < 0x1A00)
            return scc_core_read(s->scc, (uint8_t)(off & 0xFF));
        // SCC+: venster 4 met bit 7 -> B800-B9FF
        if ((s->mode & 0x20) && w == 3 && (s->bank[3] & 0x80) &&
            off >= 0x1800 && off < 0x1A00) {
            uint8_t ro = (uint8_t)(off & 0xFF);
            if (ro >= 0x80 && ro < 0xA0) return s->wave5[ro & 0x1F];
            int r = sccplus_reg(ro);
            return r >= 0 ? scc_core_read(s->scc, (uint8_t)r) : 0xFF;
        }
    }
    return s->ram[((uint32_t)(s->bank[w] & 7) << 13) + off];
}

void __not_in_flash_func(sccplus_write)(void *ctx, uint16_t a, uint8_t v)
{
    sccplus_t *s = (sccplus_t *)ctx;
    if (a < 0x4000 || a >= 0xC000) return;
    SCP_LOG("[scp] w %04X=%02X mode=%02X pc=%04X\n", a, v, s->mode, machine_dbg_pc());
    int w = window(a);
    uint16_t off = a & 0x1FFF;

    // Moderegister (BFFE/BFFF): altijd gedecodeerd.
    if (w == 3 && off >= 0x1FFE) {
        s->mode = v;
        return;
    }

    // RAM-schrijfbaar (openMSX-semantiek): bit 4 = venster 1-3, bit 0/1 =
    // venster 1/2, bit 2 = venster 3 (alleen samen met SCC+-bit 5).
    // Venster 4 (A000-BFFF) is nooit RAM-schrijfbaar — dat houdt ook de
    // BIOS-RAM-zoeker (BF00-test) netjes uit de cartridge.
    bool ram_mode =
        (w < 3 && (s->mode & 0x10)) ||
        (w == 0 && (s->mode & 0x01)) ||
        (w == 1 && (s->mode & 0x02)) ||
        (w == 2 && (s->mode & 0x04) && (s->mode & 0x20));
    if (ram_mode) {
        s->ram[((uint32_t)(s->bank[w] & 7) << 13) + off] = v;
        return;
    }

    // SCC-registers (compat: 9800-99FF bij bank 0x3F; SCC+: B800-B9FF bij
    // bit 7 van bankreg 4).
    if (!(s->mode & 0x20) && w == 2 && (s->bank[2] & 0x3F) == 0x3F &&
        off >= 0x1800 && off < 0x1A00) {
        scc_core_write(s->scc, (uint8_t)(off & 0xFF), v);
        return;
    }
    if ((s->mode & 0x20) && w == 3 && (s->bank[3] & 0x80) &&
        off >= 0x1800 && off < 0x1A00) {
        uint8_t ro = (uint8_t)(off & 0xFF);
        if (ro >= 0x80 && ro < 0xA0) { s->wave5[ro & 0x1F] = v; return; }
        int r = sccplus_reg(ro);
        if (r >= 0) scc_core_write(s->scc, (uint8_t)r, v);
        return;
    }

    // Bankregisters (5000-57FF / 7000-77FF / 9000-97FF / B000-B7FF).
    if (off >= 0x1000 && off < 0x1800)
        s->bank[w] = v;
}
