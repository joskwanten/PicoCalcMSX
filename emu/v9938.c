// V9938 (MSX2-VDP) — zie v9938.h. Spec-first tegen de Technical Data Book;
// paragraafverwijzingen hieronder slaan op dat document.

#include "v9938.h"
#include "pico.h"

#include <string.h>
#include <stdio.h>

// ---- statusbits ----
#define S0_F   0x80 // vblank-interrupt
#define S0_5S  0x40 // vijfde (mode 1) / negende (mode 2) sprite
#define S0_C   0x20 // sprite-collisie
#define S1_FH  0x01 // lijn-interrupt (ack door S1 te lezen)
#define S2_TR  0x80 // transfer ready (command-engine data poort)
#define S2_VR  0x40 // verticale blanking
#define S2_HR  0x20 // horizontale blanking
#define S2_CE  0x01 // command-engine bezig

// ---- registerhelpers ----
#define R(ctx, n) ((ctx)->regs[(n)])
#define IE0(ctx) ((R(ctx, 1) & 0x20) != 0)
#define IE1(ctx) ((R(ctx, 0) & 0x10) != 0)
#define BLANKED(ctx) ((R(ctx, 1) & 0x40) == 0)
#define MAGNIFIED(ctx) ((R(ctx, 1) & 1) != 0)
#define SIXTEEN(ctx) ((R(ctx, 1) & 2) != 0)
#define BACKDROP_IDX(ctx) (R(ctx, 7) & 0x0F)

// Mode-decode (§4.4): M1..M5 gecombineerd tot M5M4M3M2M1.
// G1=0x00 G2=0x04 G3=0x08 G4=0x0C G5=0x10 G6=0x14 G7=0x1C T1=0x01 T2=0x09 MC=0x02
static inline int v_mode(const v9938_context_t *ctx)
{
    return ((ctx->regs[1] >> 4) & 1)        // M1
         | ((ctx->regs[1] >> 2) & 2)        // M2
         | ((ctx->regs[0] & 0x0E) << 1);    // M3 M4 M5
}

// V9938-only modes: 17-bit adres-doorloop; TMS-modes wrappen op 14 bits (§4.1).
static inline bool v9938_mode(const v9938_context_t *ctx)
{
    int m = v_mode(ctx);
    return m == 0x08 || m == 0x09 || m == 0x0C || m == 0x10 || m == 0x14 || m == 0x1C;
}

static inline void addr_inc(v9938_context_t *ctx)
{
    if (v9938_mode(ctx))
        ctx->vram_addr = (ctx->vram_addr + 1) & 0x1FFFF;
    else
        ctx->vram_addr = (ctx->vram_addr & 0x1C000) | ((ctx->vram_addr + 1) & 0x3FFF);
}

// ---- palette ----

// 3-bit kanaal -> 8-bit (0..7 -> 0..255, gelijkmatig).
static inline uint32_t c3to8(uint32_t c) { return (c << 5) | (c << 2) | (c >> 1); }

static void set_palette(v9938_context_t *ctx, int idx, uint32_t r3, uint32_t g3, uint32_t b3)
{
    ctx->palette_raw[idx] = (uint16_t)((r3 << 4) | b3 | (g3 << 8));
    ctx->palette[idx] = 0xFF000000u | (c3to8(r3) << 16) | (c3to8(g3) << 8) | c3to8(b3);
    // RGB565 voorberekend voor het Pico-renderpad (3 bits -> 5/6, gespreid).
    uint32_t r5 = (r3 << 2) | (r3 >> 1), g6 = (g3 << 3) | g3, b5 = (b3 << 2) | (b3 >> 1);
    ctx->palette565[idx] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// G7 (screen 8): GGGRRRBB-byte -> RGB565, voorberekend (gevuld in init).
static uint16_t g7_565[256];

// Power-on-palette (§7, tabel "initial palette") — de TMS9918-lookalike.
static const uint8_t default_palette[16][3] = {
    {0, 0, 0}, {0, 0, 0}, {1, 6, 1}, {3, 7, 3}, {1, 1, 7}, {2, 3, 7},
    {5, 1, 1}, {2, 6, 7}, {7, 1, 1}, {7, 3, 3}, {6, 6, 1}, {6, 6, 4},
    {1, 4, 1}, {6, 2, 5}, {5, 5, 5}, {7, 7, 7},
};

void v9938_init(v9938_context_t *ctx, uint8_t *vram128k)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->vram = vram128k;
    memset(vram128k, 0, V9938_VRAM_SIZE);
    for (int i = 0; i < 16; i++)
        set_palette(ctx, i, default_palette[i][0], default_palette[i][1], default_palette[i][2]);
    for (int b = 0; b < 256; b++) {
        uint32_t g3 = (b >> 5) & 7, r3 = (b >> 2) & 7, b2 = b & 3;
        uint32_t r5 = (r3 << 2) | (r3 >> 1), g6 = (g3 << 3) | g3;
        uint32_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
        g7_565[b] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
    ctx->status[2] = S2_TR; // transfer altijd "ready" zolang de engine synchroon is
}

void v9938_register_interrupt_func(v9938_context_t *ctx, v9938_irq_func_t f)
{
    ctx->irq_func = f;
}

// ---- poortprotocol ----

uint8_t __not_in_flash_func(v9938_read_data)(v9938_context_t *ctx) // 0x98 in
{
    ctx->has_latched = false;
    uint8_t v = ctx->read_ahead; // read-ahead-buffer (§4.1)
    ctx->read_ahead = ctx->vram[ctx->vram_addr];
    addr_inc(ctx);
    return v;
}

void __not_in_flash_func(v9938_write_data)(v9938_context_t *ctx, uint8_t v) // 0x98 out
{
    ctx->has_latched = false;
    ctx->vram[ctx->vram_addr] = v;
    addr_inc(ctx);
}

static void cmd_start(v9938_context_t *ctx, uint8_t v);
static void cmd_cpu_step(v9938_context_t *ctx, uint8_t data);

// Registerschrijfactie met bijwerkingen.
static void __not_in_flash_func(reg_write)(v9938_context_t *ctx, int n, uint8_t v)
{
    n &= 0x3F;
    uint8_t old = ctx->regs[n];
    ctx->regs[n] = v;
#ifdef VDP_CMD_DEBUG
    if (n == 23 || n == 19 || n == 2 || n == 9)
        fprintf(stderr, "[reg] R%d=%02X\n", n, v);
#endif
    switch (n) {
    case 1:
        // IE0 gaat aan terwijl F al hangt -> INT-lijn meteen op.
        if (!(old & 0x20) && IE0(ctx) && (ctx->status[0] & S0_F) && ctx->irq_func)
            ctx->irq_func();
        break;
    case 14:
        // A16-A14 van de actieve pointer (§4.1).
        ctx->vram_addr = (ctx->vram_addr & 0x3FFF) | (((uint32_t)v & 7) << 14);
        break;
    case 16:
        ctx->palette_first_pending = false; // nieuwe palette-pointer reset het paar
        break;
    case 44:
        // Lopende CPU->VRAM-transfer (HMMC/LMMC): één byte/pixel verwerken.
        if (ctx->cm == 0xF || ctx->cm == 0xB)
            cmd_cpu_step(ctx, v);
        break;
    case 46:
        cmd_start(ctx, v);
        break;
    default:
        break;
    }
}

void __not_in_flash_func(v9938_write_ctrl)(v9938_context_t *ctx, uint8_t v) // 0x99 out
{
    if (!ctx->has_latched) {
        ctx->latched = v;
        ctx->has_latched = true;
        return;
    }
    ctx->has_latched = false;
    if (v & 0x80) {
        reg_write(ctx, v & 0x3F, ctx->latched); // V9938: 6-bit registernummer
    } else {
        // VRAM-adres-setup: A13-A8 in de tweede byte, A7-A0 in de eerste,
        // A16-A14 uit R14. Bit 6 = schrijf-setup; lees-setup prefetcht.
        ctx->vram_addr = (((uint32_t)R(ctx, 14) & 7) << 14) |
                         (((uint32_t)v & 0x3F) << 8) | ctx->latched;
        if (!(v & 0x40)) {
            ctx->read_ahead = ctx->vram[ctx->vram_addr];
            addr_inc(ctx);
        }
    }
}

uint8_t __not_in_flash_func(v9938_read_status)(v9938_context_t *ctx) // 0x99 in
{
    ctx->has_latched = false;
    int s = R(ctx, 15) & 0x0F;
    if (s > 9) return 0xFF;
    uint8_t v = ctx->status[s];
    switch (s) {
    case 0:
        // S0-read wist F, 5S en C (TMS-semantiek, §5.1).
        ctx->status[0] = 0;
        break;
    case 1:
        // S1-read ackt de lijn-interrupt.
        ctx->status[1] &= (uint8_t)~S1_FH;
        ctx->line_irq_pending = false;
        break;
    case 2: {
        // VR is echt (per scanline bijgehouden); HR (horizontale blanking)
        // valt onder onze lijn-granulariteit en wisselt per read met een
        // oneven-modulusfase, zodat pollende lussen altijd flanken zien.
        // CE blijft na een commando een paar reads hoog (zie cmd_done).
        ctx->s2_phase++;
        uint8_t dyn = 0;
        if (ctx->ce_hold) { dyn |= S2_CE; ctx->ce_hold--; }
        if ((ctx->s2_phase % 3) < 1) dyn |= S2_HR;
        ctx->status[2] = (uint8_t)((ctx->status[2] & ~(S2_HR | S2_CE)) | dyn);
        v = (uint8_t)((v & ~(S2_HR | S2_CE)) | dyn | (ctx->status[2] & S2_VR));
        break;
    }
    case 7:
        // LMCM: S7-read consumeert de pixel en zet de volgende klaar.
        if (ctx->cm == 0xA)
            cmd_cpu_step(ctx, 0);
        break;
    default:
        break;
    }
    return v;
}

void v9938_write_palette(v9938_context_t *ctx, uint8_t v) // 0x9A out
{
    if (!ctx->palette_first_pending) {
        ctx->palette_first = v;
        ctx->palette_first_pending = true;
        return;
    }
    ctx->palette_first_pending = false;
    int idx = R(ctx, 16) & 0x0F;
    set_palette(ctx, idx,
                (ctx->palette_first >> 4) & 7, // R
                v & 7,                          // G
                ctx->palette_first & 7);        // B
    R(ctx, 16) = (uint8_t)((idx + 1) & 0x0F); // palette-pointer auto-increment
}

void v9938_write_indirect(v9938_context_t *ctx, uint8_t v) // 0x9B out
{
    int n = R(ctx, 17) & 0x3F;
    if (n != 17) // R17 zelf is niet via 0x9B te wijzigen (§4.2)
        reg_write(ctx, n, v);
    if (!(R(ctx, 17) & 0x80)) // AII=0 -> auto-increment
        R(ctx, 17) = (uint8_t)(((n + 1) & 0x3F) | (R(ctx, 17) & 0x80));
}

// ==== command-engine (§9) ====================================================
// VRAM-commando's (HMMV/HMMM/YMMM/LMMV/LMMM/LINE/SRCH/PSET/POINT) voeren we
// synchroon uit bij de R46-write; CE meldt meteen "klaar". CPU-transfers
// (HMMC/LMMC/LMCM) blijven actief: elke R44-write of S7-read verwerkt één
// byte/pixel. Beam-race-nauwkeurigheid (Quarth!) komt met de scanline-lus.

#define S2_BD 0x10 // border detected (SRCH)

// Pixel-layout van de actieve bitmap-mode; buiten G4-G7 valt de engine
// terug op G7-geometrie (zoals fMSX — commando's zijn daar toch ongeldig).
typedef struct { uint32_t ppb, pitch, width; } cmd_layout_t;

static cmd_layout_t cmd_layout(const v9938_context_t *ctx)
{
    switch (v_mode(ctx)) {
    case 0x0C: return (cmd_layout_t){2, 128, 256}; // G4 (scr5)
    case 0x10: return (cmd_layout_t){4, 128, 512}; // G5 (scr6)
    case 0x14: return (cmd_layout_t){2, 256, 512}; // G6 (scr7)
    case 0x1C: default: return (cmd_layout_t){1, 256, 256}; // G7 (scr8)
    }
}

static inline uint32_t cmd_addr(const cmd_layout_t *L, uint32_t x, uint32_t y)
{
    return (y * L->pitch + x / L->ppb) & 0x1FFFF;
}

static uint8_t __not_in_flash_func(vdp_point)(v9938_context_t *ctx, uint32_t x, uint32_t y)
{
    cmd_layout_t L = cmd_layout(ctx);
    uint8_t b = ctx->vram[cmd_addr(&L, x, y)];
    switch (L.ppb) {
    case 1: return b;
    case 2: return (x & 1) ? (b & 0x0F) : (b >> 4);
    default: return (uint8_t)((b >> ((3 - (x & 3)) * 2)) & 3);
    }
}

// Pixel-write met logische operatie (lage nibble van R46). T-varianten
// (bit 3) slaan transparant over als de bronkleur 0 is.
static void __not_in_flash_func(vdp_pset)(v9938_context_t *ctx, uint32_t x, uint32_t y, uint8_t color, uint8_t lo)
{
    if ((lo & 0x08) && color == 0) return;
    cmd_layout_t L = cmd_layout(ctx);
    uint32_t addr = cmd_addr(&L, x, y);
    uint32_t shift, mask;
    switch (L.ppb) {
    case 1: shift = 0; mask = 0xFF; break;
    case 2: shift = (x & 1) ? 0 : 4; mask = 0x0F; break;
    default: shift = (3 - (x & 3)) * 2; mask = 0x03; break;
    }
    uint8_t cur = ctx->vram[addr];
    uint8_t px = (uint8_t)((cur >> shift) & mask);
    switch (lo & 0x07) {
    case 0: px = (uint8_t)(color & mask); break;        // IMP
    case 1: px &= color; break;                          // AND
    case 2: px |= color; break;                          // OR
    case 3: px ^= color; break;                          // XOR
    case 4: px = (uint8_t)(~color & mask); break;        // NOT
    default: px = (uint8_t)(color & mask); break;
    }
    ctx->vram[addr] = (uint8_t)((cur & ~(mask << shift)) | ((uint32_t)px << shift));
}

// Latch de parameters uit R32-R45. LET OP: de "NX=0 -> 512 / NY=0 -> 1024"-
// regel geldt alleen voor BLOK-commando's; bij LINE zijn NX/NY de major/
// minor-lengtes en betekent 0 gewoon 0 (het Philips-bootlogo tekent zijn
// horizontale lijnen met NY=0 — met de blok-default werd dat een diagonaal).
static void __not_in_flash_func(cmd_latch)(v9938_context_t *ctx)
{
    ctx->csx = (uint16_t)(ctx->regs[32] | ((ctx->regs[33] & 1) << 8));
    ctx->csy = (uint16_t)(ctx->regs[34] | ((ctx->regs[35] & 3) << 8));
    ctx->cdx = (uint16_t)(ctx->regs[36] | ((ctx->regs[37] & 1) << 8));
    ctx->cdy = (uint16_t)(ctx->regs[38] | ((ctx->regs[39] & 3) << 8));
    ctx->cnx = (uint16_t)(ctx->regs[40] | ((ctx->regs[41] & 1) << 8));
    ctx->cny = (uint16_t)(ctx->regs[42] | ((ctx->regs[43] & 3) << 8));
    ctx->cdix = (ctx->regs[45] & 0x04) ? -1 : 1;
    ctx->cdiy = (ctx->regs[45] & 0x08) ? -1 : 1;
    ctx->cwx = ctx->cwy = 0;
}

// Blok-defaults (§9.4): 0 betekent maximaal.
static inline uint16_t blk_nx(const v9938_context_t *ctx) { return ctx->cnx ? ctx->cnx : 512; }
static inline uint16_t blk_ny(const v9938_context_t *ctx) { return ctx->cny ? ctx->cny : 1024; }

static void __not_in_flash_func(cmd_done)(v9938_context_t *ctx)
{
    ctx->cm = 0;
    ctx->status[2] = (uint8_t)((ctx->status[2] & ~S2_CE) | S2_TR);
    // CE een paar S2-reads "bezig" laten lijken: sommige software (o.a. de
    // Philips-bootanimatie) wacht eerst TOT de engine bezig is en dan tot
    // hij klaar is — met instant-voltooiing hangt die eerste wachtlus.
    // (Echte cycle-gebaseerde busy-tijd komt met de scanline-lus.)
    ctx->ce_hold = 4;
}

// VRAM-vullingen/kopieën. Byte-commando's (H*) werken per byte, de
// pixelcommando's (L*) per pixel met logische op.
static void __not_in_flash_func(cmd_run_vram)(v9938_context_t *ctx, uint8_t cm)
{
    cmd_layout_t L = cmd_layout(ctx);
    uint8_t clr = ctx->regs[44];
    switch (cm) {
    case 0xC: // HMMV: byte-vulling
        for (uint32_t j = 0; j < blk_ny(ctx); j++) {
            uint32_t y = (uint32_t)(ctx->cdy + (int32_t)j * ctx->cdiy) & 0x3FF;
            uint32_t nb = blk_nx(ctx) / L.ppb;
            for (uint32_t i = 0; i < nb; i++) {
                uint32_t x = (uint32_t)(ctx->cdx + (int32_t)(i * L.ppb) * ctx->cdix) & (L.width - 1);
                ctx->vram[cmd_addr(&L, x, y)] = clr;
            }
        }
        break;
    case 0xD: // HMMM: byte-kopie
        for (uint32_t j = 0; j < blk_ny(ctx); j++) {
            uint32_t sy = (uint32_t)(ctx->csy + (int32_t)j * ctx->cdiy) & 0x3FF;
            uint32_t dy = (uint32_t)(ctx->cdy + (int32_t)j * ctx->cdiy) & 0x3FF;
            uint32_t nb = blk_nx(ctx) / L.ppb;
            for (uint32_t i = 0; i < nb; i++) {
                uint32_t sx = (uint32_t)(ctx->csx + (int32_t)(i * L.ppb) * ctx->cdix) & (L.width - 1);
                uint32_t dx = (uint32_t)(ctx->cdx + (int32_t)(i * L.ppb) * ctx->cdix) & (L.width - 1);
                ctx->vram[cmd_addr(&L, dx, dy)] = ctx->vram[cmd_addr(&L, sx, sy)];
            }
        }
        break;
    case 0xE: // YMMM: byte-kopie in Y, X loopt van DX naar de rand
        for (uint32_t j = 0; j < blk_ny(ctx); j++) {
            uint32_t sy = (uint32_t)(ctx->csy + (int32_t)j * ctx->cdiy) & 0x3FF;
            uint32_t dy = (uint32_t)(ctx->cdy + (int32_t)j * ctx->cdiy) & 0x3FF;
            uint32_t nb = (ctx->cdix > 0) ? (L.width - ctx->cdx) / L.ppb
                                          : ctx->cdx / L.ppb + 1;
            for (uint32_t i = 0; i < nb; i++) {
                uint32_t x = (uint32_t)(ctx->cdx + (int32_t)(i * L.ppb) * ctx->cdix) & (L.width - 1);
                ctx->vram[cmd_addr(&L, x, dy)] = ctx->vram[cmd_addr(&L, x, sy)];
            }
        }
        break;
    case 0x8: // LMMV: pixel-vulling met log. op
        for (uint32_t j = 0; j < blk_ny(ctx); j++) {
            uint32_t y = (uint32_t)(ctx->cdy + (int32_t)j * ctx->cdiy) & 0x3FF;
            for (uint32_t i = 0; i < blk_nx(ctx); i++) {
                uint32_t x = (uint32_t)(ctx->cdx + (int32_t)i * ctx->cdix) & (L.width - 1);
                vdp_pset(ctx, x, y, clr, ctx->clo);
            }
        }
        break;
    case 0x9: // LMMM: pixel-kopie met log. op
        for (uint32_t j = 0; j < blk_ny(ctx); j++) {
            uint32_t sy = (uint32_t)(ctx->csy + (int32_t)j * ctx->cdiy) & 0x3FF;
            uint32_t dy = (uint32_t)(ctx->cdy + (int32_t)j * ctx->cdiy) & 0x3FF;
            for (uint32_t i = 0; i < blk_nx(ctx); i++) {
                uint32_t sx = (uint32_t)(ctx->csx + (int32_t)i * ctx->cdix) & (L.width - 1);
                uint32_t dx = (uint32_t)(ctx->cdx + (int32_t)i * ctx->cdix) & (L.width - 1);
                vdp_pset(ctx, dx, dy, vdp_point(ctx, sx, sy), ctx->clo);
            }
        }
        break;
    case 0x7: { // LINE: Bresenham langs de major-as (MAJ = ARG bit 0)
        bool ymajor = (ctx->regs[45] & 0x01) != 0;
        uint32_t x = ctx->cdx, y = ctx->cdy;
        uint32_t maj = ctx->cnx, min = ctx->cny;
        uint32_t acc = 0;
        for (uint32_t i = 0; i <= maj; i++) {
            vdp_pset(ctx, x & (L.width - 1), y & 0x3FF, clr, ctx->clo);
            acc += min;
            if (2 * acc >= maj) {
                acc -= maj;
                if (ymajor) x = (uint32_t)((int32_t)x + ctx->cdix);
                else y = (uint32_t)((int32_t)y + ctx->cdiy);
            }
            if (ymajor) y = (uint32_t)((int32_t)y + ctx->cdiy);
            else x = (uint32_t)((int32_t)x + ctx->cdix);
        }
        break;
    }
    case 0x6: { // SRCH: zoek kleur (EQ = ARG bit 1) langs lijn SY vanaf SX
        bool neq = (ctx->regs[45] & 0x02) != 0;
        int32_t x = ctx->csx;
        ctx->status[2] &= (uint8_t)~S2_BD;
        while (x >= 0 && x < (int32_t)L.width) {
            uint8_t c = vdp_point(ctx, (uint32_t)x, ctx->csy & 0x3FF);
            bool hit = neq ? (c != clr) : (c == clr);
            if (hit) {
                ctx->status[2] |= S2_BD;
                ctx->status[8] = (uint8_t)(x & 0xFF);
                ctx->status[9] = (uint8_t)(((uint32_t)x >> 8) | 0xFE);
                break;
            }
            x += ctx->cdix;
        }
        break;
    }
    case 0x5: // PSET
        vdp_pset(ctx, ctx->cdx & (L.width - 1), ctx->cdy & 0x3FF, clr, ctx->clo);
        break;
    case 0x4: // POINT -> S7
        ctx->status[7] = vdp_point(ctx, ctx->csx & (L.width - 1), ctx->csy & 0x3FF);
        break;
    default:
        break;
    }
    cmd_done(ctx);
}

// R46-write: commando starten.
static void __not_in_flash_func(cmd_start)(v9938_context_t *ctx, uint8_t v)
{
    uint8_t cm = v >> 4;
    ctx->clo = v & 0x0F;
    cmd_latch(ctx);
#ifdef VDP_CMD_DEBUG
    fprintf(stderr, "[cmd] %X lo=%X sx=%u sy=%u dx=%u dy=%u nx=%u ny=%u dix=%d diy=%d clr=%02X\n",
            cm, ctx->clo, ctx->csx, ctx->csy, ctx->cdx, ctx->cdy,
            ctx->cnx, ctx->cny, ctx->cdix, ctx->cdiy, ctx->regs[44]);
#endif
    if (cm == 0) { cmd_done(ctx); return; } // STOP
    if (cm == 0xF || cm == 0xB) {
        // HMMC/LMMC: CPU->VRAM. SPEC-SUBTILITEIT (§9.7): de R44/CLR-waarde
        // op het moment van de commandostart is het EERSTE datum; de stream
        // via R44-writes begint dus bij pixel/byte 1. (Zonder dit schoof
        // alles één op en lekte de CLR-setup van het volgende glyph als
        // valse pixelkolom in de staart van het vorige — zie het MSX2-
        // bootscherm "128Kbytes"-artefact.)
        ctx->cm = cm;
        ctx->status[2] |= S2_CE | S2_TR;
        cmd_cpu_step(ctx, ctx->regs[44]);
        return;
    }
    if (cm == 0xA) {
        // LMCM: VRAM->CPU; eerste pixel klaarzetten in S7.
        ctx->cm = cm;
        ctx->status[2] |= S2_CE | S2_TR;
        ctx->status[7] = vdp_point(ctx, ctx->csx, ctx->csy & 0x3FF);
        return;
    }
    cmd_run_vram(ctx, cm);
}

// Eén stap van een lopende CPU-transfer (R44-write of S7-read).
static void __not_in_flash_func(cmd_cpu_step)(v9938_context_t *ctx, uint8_t data)
{
    cmd_layout_t L = cmd_layout(ctx);
    uint32_t x, y;
    switch (ctx->cm) {
    case 0xF: // HMMC: byte
        x = (uint32_t)(ctx->cdx + (int32_t)(ctx->cwx * L.ppb) * ctx->cdix) & (L.width - 1);
        y = (uint32_t)(ctx->cdy + (int32_t)ctx->cwy * ctx->cdiy) & 0x3FF;
        ctx->vram[cmd_addr(&L, x, y)] = data;
        ctx->cwx++;
        if (ctx->cwx >= blk_nx(ctx) / L.ppb) {
            ctx->cwx = 0;
            if (++ctx->cwy >= blk_ny(ctx)) cmd_done(ctx);
        }
        break;
    case 0xB: // LMMC: pixel met log. op
        x = (uint32_t)(ctx->cdx + (int32_t)ctx->cwx * ctx->cdix) & (L.width - 1);
        y = (uint32_t)(ctx->cdy + (int32_t)ctx->cwy * ctx->cdiy) & 0x3FF;
#ifdef VDP_CMD_DEBUG
        if (y >= 110 && y < 120)
            fprintf(stderr, "[lmmc] x=%u y=%u d=%02X\n", x, y, data);
#endif
        vdp_pset(ctx, x, y, data, ctx->clo);
        ctx->cwx++;
        if (ctx->cwx >= blk_nx(ctx)) {
            ctx->cwx = 0;
            if (++ctx->cwy >= blk_ny(ctx)) cmd_done(ctx);
        }
        break;
    case 0xA: // LMCM: volgende pixel in S7
        ctx->cwx++;
        if (ctx->cwx >= blk_nx(ctx)) {
            ctx->cwx = 0;
            if (++ctx->cwy >= blk_ny(ctx)) { cmd_done(ctx); return; }
        }
        x = (uint32_t)(ctx->csx + (int32_t)ctx->cwx * ctx->cdix) & (L.width - 1);
        y = (uint32_t)(ctx->csy + (int32_t)ctx->cwy * ctx->cdiy) & 0x3FF;
        ctx->status[7] = vdp_point(ctx, x, y);
        break;
    default:
        break;
    }
}

// Scanline-hook: de machine draait per displaylijn ~228 T-states Z80 en
// meldt daarna de lijn. Hier leven de echte VR- en FH-semantiek:
//  - FH wordt gezet zodra de beam lijn R19 passeert, ONGEACHT IE1 (IE1
//    bepaalt alleen de INT-lijn); software pollt FH ook met IRQs uit.
//  - VR is hoog in de verticale blanking (lijn >= actieve hoogte).
//  - Op de eerste vblank-lijn: S0.F + frame-IRQ (IE0).
void __not_in_flash_func(v9938_scanline)(v9938_context_t *ctx, int line)
{
    int active_h = (ctx->regs[9] & 0x80) ? 212 : 192;

    if (line == ctx->regs[19]) {
        ctx->status[1] |= S1_FH;
        ctx->line_irq_pending = true;
        if (IE1(ctx) && ctx->irq_func)
            ctx->irq_func();
    }

    if (line < active_h)
        ctx->status[2] &= (uint8_t)~S2_VR;
    else
        ctx->status[2] |= S2_VR;

    if (line == active_h) {
        ctx->status[0] |= S0_F;
        if (IE0(ctx) && ctx->irq_func)
            ctx->irq_func();
    }
}

// Frame-granulaire fallback (MSX1-lus / oude aanroepers).
void v9938_vblank(v9938_context_t *ctx)
{
    v9938_scanline(ctx, (ctx->regs[9] & 0x80) ? 212 : 192);
}

// Hangt de INT-lijn (nog) hoog? Frame-IRQ: IE0 && F; lijn-IRQ: IE1 && FH.
bool v9938_irq_asserted(v9938_context_t *ctx)
{
    return (IE0(ctx) && (ctx->status[0] & S0_F)) ||
           (IE1(ctx) && (ctx->status[1] & S1_FH));
}

uint32_t v9938_backdrop_color(v9938_context_t *ctx)
{
    return ctx->palette[BACKDROP_IDX(ctx)];
}

// ---- tabeladressen (V9938-brede registers, §4.5) ----

static inline uint32_t name_table(const v9938_context_t *ctx)
{
    return ((uint32_t)ctx->regs[2] & 0x7F) << 10;
}

static inline uint32_t color_table(const v9938_context_t *ctx)
{
    return ((((uint32_t)ctx->regs[10] & 7) << 8) | ctx->regs[3]) << 6;
}

static inline uint32_t pattern_table(const v9938_context_t *ctx)
{
    return ((uint32_t)ctx->regs[4] & 0x3F) << 11;
}

static inline uint32_t sprite_attr_table(const v9938_context_t *ctx)
{
    return ((((uint32_t)ctx->regs[11] & 3) << 8) | ctx->regs[5]) << 7;
}

static inline uint32_t sprite_pat_table(const v9938_context_t *ctx)
{
    return ((uint32_t)ctx->regs[6] & 0x3F) << 11;
}

// ---- lijnrenderers ----
// Alle modes renderen palet-INDICES (uint8; G7: het ruwe GGGRRRBB-byte) in
// een 256/512-brede buffer; de uitvoerpassen zetten die in één keer om naar
// ARGB (SDL) of RGB565 (Pico). Zo blijft het hete pad één lookup per pixel —
// nodig om op de Pico binnen het beam-budget (~63 µs/lijn) te blijven.

static void __not_in_flash_func(render_t1)(v9938_context_t *ctx, uint8_t *px, int ln)
{
    uint32_t PG = pattern_table(ctx), PN = name_table(ctx);
    uint8_t bg = (uint8_t)BACKDROP_IDX(ctx);
    uint8_t t = (ctx->regs[7] >> 4) & 0x0F;
    uint8_t fg = t ? t : bg;
    int y = ln >> 3, i = ln & 7;
    memset(px, bg, 256);
    if (y >= 24) return;
    for (int x = 0; x < 40; x++) {
        uint32_t c = ctx->vram[PN + (uint32_t)(y * 40) + x];
        uint32_t p = ctx->vram[PG + 8 * c + i];
        for (int j = 0; j < 6; j++)
            px[8 + x * 6 + j] = (p & (0x80 >> j)) ? fg : bg; // 240px, 8px inspring
    }
}

static void __not_in_flash_func(render_g1)(v9938_context_t *ctx, uint8_t *px, int ln)
{
    uint32_t PG = pattern_table(ctx), PN = name_table(ctx), CT = color_table(ctx);
    uint8_t bdc = (uint8_t)BACKDROP_IDX(ctx);
    int y = ln >> 3, i = ln & 7;
    for (int x = 0; x < 32; x++) {
        uint32_t c = ctx->vram[PN + (uint32_t)(y * 32) + x];
        uint32_t col = ctx->vram[CT + (c >> 3)];
        uint8_t fg = (col >> 4) ? (uint8_t)(col >> 4) : bdc;
        uint8_t bg = (col & 0xF) ? (uint8_t)(col & 0xF) : bdc;
        uint32_t p = ctx->vram[PG + 8 * c + i];
        for (int j = 0; j < 8; j++)
            px[x * 8 + j] = (p & (0x80 >> j)) ? fg : bg;
    }
}

static void __not_in_flash_func(render_g2)(v9938_context_t *ctx, uint8_t *px, int ln)
{
    // G2/G3-layout: naam + derde<<8, met AND-maskers uit R3/R4 (zoals de
    // TMS9918-screen-2, maar met de bredere V9938-basisregisters).
    uint32_t PG = ((uint32_t)ctx->regs[4] & 0x3C) << 11;
    uint32_t CT = (((uint32_t)ctx->regs[10] & 7) << 14) | (((uint32_t)ctx->regs[3] & 0x80) << 6);
    uint32_t PN = name_table(ctx);
    uint32_t colourMask = (((uint32_t)ctx->regs[3] & 0x7F) << 3) | 7;
    uint32_t patternMask = (((uint32_t)ctx->regs[4] & 3) << 8) | 0xFF;
    uint8_t bdc = (uint8_t)BACKDROP_IDX(ctx);
    int third = ln >> 6, row = ln & 7, y = ln >> 3;
    for (int x = 0; x < 32; x++) {
        uint32_t charcode = ctx->vram[PN + (uint32_t)(y * 32) + x] + ((uint32_t)third << 8);
        uint32_t p = ctx->vram[PG + ((charcode & patternMask) << 3) + row];
        uint32_t col = ctx->vram[CT + ((charcode & colourMask) << 3) + row];
        uint8_t fg = (col >> 4) ? (uint8_t)(col >> 4) : bdc;
        uint8_t bg = (col & 0xF) ? (uint8_t)(col & 0xF) : bdc;
        for (int j = 0; j < 8; j++)
            px[x * 8 + j] = (p & (0x80 >> j)) ? fg : bg;
    }
}

// Sprite mode 1 (G1/G2/MC — §8.1), zelfde regels als onze TMS9918:
// max 4 per lijn, sprite 0 wint, kleur 0 transparant, EC = 32px links, MAG.
static inline int sprite_y_to_line(uint8_t yraw)
{
    return (yraw > 238) ? ((int)yraw - 255) : ((int)yraw + 1);
}

static void __not_in_flash_func(render_sprites_m1)(v9938_context_t *ctx, uint8_t *px, int ln)
{
    if (ctx->regs[8] & 0x02) return; // SPD: sprites uit (V9938, R8 bit 1)
    uint32_t SA = sprite_attr_table(ctx), SG = sprite_pat_table(ctx);
    int sixteen = SIXTEEN(ctx), mag = MAGNIFIED(ctx);
    int H = (sixteen ? 16 : 8) << (mag ? 1 : 0);

    int idx[4], n = 0;
    for (int s = 0; s < 32 && n < 4; s++) {
        uint8_t yraw = ctx->vram[SA + 4 * (uint32_t)s];
        if (yraw == 208) break;
        int r = ln - sprite_y_to_line(yraw);
        if (r >= 0 && r < H) idx[n++] = s;
    }
    for (int k = n - 1; k >= 0; k--) {
        int s = idx[k];
        uint8_t yraw = ctx->vram[SA + 4 * (uint32_t)s];
        int xx = ctx->vram[SA + 4 * (uint32_t)s + 1];
        uint8_t p = ctx->vram[SA + 4 * (uint32_t)s + 2];
        uint8_t attr = ctx->vram[SA + 4 * (uint32_t)s + 3];
        uint8_t c = attr & 0x0F;
        if (attr & 0x80) xx -= 32;
        if (c == 0) continue;
        int r = ln - sprite_y_to_line(yraw);
        int rowidx = mag ? (r >> 1) : r;
        uint16_t bits;
        if (sixteen)
            bits = (uint16_t)((ctx->vram[SG + 8u * (p & 0xFC) + rowidx] << 8) |
                              ctx->vram[SG + 8u * (p & 0xFC) + 16 + rowidx]);
        else
            bits = (uint16_t)(ctx->vram[SG + 8u * p + rowidx] << 8);
        int total = (sixteen ? 16 : 8) << (mag ? 1 : 0);
        for (int j = 0; j < total; j++) {
            int col = mag ? (j >> 1) : j;
            if (!(bits & (0x8000 >> col))) continue;
            int xp = xx + j;
            if (xp >= 0 && xp < 256) px[xp] = c;
        }
    }
}

// ---- bitmap-modes (G4-G7, §6) ----
// Kleur 0 toont de backdrop tenzij TP (R8 bit 5) gezet is; verticale scroll
// (R23) verschuift de bitmap-bron per lijn. tp0 = het effectieve indexalias
// voor kleur 0.
static inline uint8_t tp0_idx(const v9938_context_t *ctx)
{
    return (ctx->regs[8] & 0x20) ? 0 : (uint8_t)BACKDROP_IDX(ctx);
}

static void __not_in_flash_func(render_g4)(v9938_context_t *ctx, uint8_t *px, int ln) // scr5: 256px 4bpp
{
    uint32_t base = (((uint32_t)ctx->regs[2] >> 5) & 3) << 15;
    uint32_t y = ((uint32_t)ln + ctx->regs[23]) & 0xFF;
    const uint8_t *row = &ctx->vram[base + y * 128];
    uint8_t tp0 = tp0_idx(ctx);
    for (int x = 0; x < 256; x += 2) {
        uint8_t b = row[x >> 1];
        uint8_t hi = b >> 4, lo = b & 0x0F;
        px[x] = hi ? hi : tp0;
        px[x + 1] = lo ? lo : tp0;
    }
}

static void __not_in_flash_func(render_g5)(v9938_context_t *ctx, uint8_t *px512, int ln) // scr6: 512px 2bpp
{
    uint32_t base = (((uint32_t)ctx->regs[2] >> 5) & 3) << 15;
    uint32_t y = ((uint32_t)ln + ctx->regs[23]) & 0xFF;
    const uint8_t *row = &ctx->vram[base + y * 128];
    uint8_t tp0 = tp0_idx(ctx);
    for (int x = 0; x < 512; x += 4) {
        uint8_t b = row[x >> 2];
        uint8_t c0 = (b >> 6) & 3, c1 = (b >> 4) & 3, c2 = (b >> 2) & 3, c3 = b & 3;
        px512[x] = c0 ? c0 : tp0;
        px512[x + 1] = c1 ? c1 : tp0;
        px512[x + 2] = c2 ? c2 : tp0;
        px512[x + 3] = c3 ? c3 : tp0;
    }
}

static void __not_in_flash_func(render_g6)(v9938_context_t *ctx, uint8_t *px512, int ln) // scr7: 512px 4bpp
{
    uint32_t base = (((uint32_t)ctx->regs[2] >> 5) & 1) << 16;
    uint32_t y = ((uint32_t)ln + ctx->regs[23]) & 0xFF;
    const uint8_t *row = &ctx->vram[base + y * 256];
    uint8_t tp0 = tp0_idx(ctx);
    for (int x = 0; x < 512; x += 2) {
        uint8_t b = row[x >> 1];
        uint8_t hi = b >> 4, lo = b & 0x0F;
        px512[x] = hi ? hi : tp0;
        px512[x + 1] = lo ? lo : tp0;
    }
}

static void __not_in_flash_func(render_g7)(v9938_context_t *ctx, uint8_t *px, int ln) // scr8: ruwe GGGRRRBB-bytes
{
    uint32_t base = (((uint32_t)ctx->regs[2] >> 5) & 1) << 16;
    uint32_t y = ((uint32_t)ln + ctx->regs[23]) & 0xFF;
    memcpy(px, &ctx->vram[base + y * 256], 256);
}

static void __not_in_flash_func(render_t2)(v9938_context_t *ctx, uint8_t *px512, int ln) // 80-koloms tekst
{
    uint32_t PG = pattern_table(ctx);
    uint32_t PN = ((uint32_t)ctx->regs[2] & 0x7C) << 10;
    uint8_t bg = (uint8_t)BACKDROP_IDX(ctx);
    uint8_t t = (ctx->regs[7] >> 4) & 0x0F;
    uint8_t fg = t ? t : bg;
    int y = ln >> 3, i = ln & 7;
    memset(px512, bg, 512);
    if (y >= 24) return; // (26.5-regelmodus + blink: later)
    for (int x = 0; x < 80; x++) {
        uint32_t c = ctx->vram[PN + (uint32_t)(y * 80) + x];
        uint32_t p = ctx->vram[PG + 8 * c + i];
        for (int j = 0; j < 6; j++)
            px512[16 + x * 6 + j] = (p & (0x80 >> j)) ? fg : bg;
    }
}

// ---- sprite mode 2 (G3-G7, §2.6): 8 per lijn, kleur-per-lijn uit de
// kleurtabel (SAT-512), EC per lijn, CC-keten (OR-combinatie met de
// eerstvolgende lagere sprite), sentinel Y=0xD8, scrollt mee met R23.
// Levert palette-indices in een 256-brede overlay (0xFF = geen sprite).
static void __not_in_flash_func(render_sprites_m2)(v9938_context_t *ctx, uint8_t *ovr, int ln)
{
    memset(ovr, 0xFF, 256);
    if (ctx->regs[8] & 0x02) return; // SPD

    uint32_t SA = (((uint32_t)ctx->regs[11] & 3) << 15) | (((uint32_t)ctx->regs[5] & 0xFC) << 7);
    uint32_t SC = SA - 0x200; // kleurtabel 512 bytes onder de SAT
    uint32_t SG = ((uint32_t)ctx->regs[6] & 0x3F) << 11;
    int size16 = SIXTEEN(ctx), mag = MAGNIFIED(ctx);
    int OH = (size16 ? 16 : 8) << (mag ? 1 : 0); // schermhoogte
    int IH = size16 ? 16 : 8;                    // patroonhoogte (Y-wrap!)
    uint8_t vscroll = ctx->regs[23];

    // Pas 1: markeer de eerste 8 sprites op deze lijn (incl. transparante).
    uint32_t marked = 0;
    int count = 0;
    for (int s = 0; s < 32; s++) {
        uint8_t yr = ctx->vram[SA + 4u * (uint32_t)s];
        if (yr == 0xD8) break;
        uint8_t k = (uint8_t)(yr - vscroll);
        int sy = (k > 256 - IH) ? ((int)k + 1 - 256) : ((int)k + 1);
        int dy = ln - sy;
        if (dy < 0 || dy >= OH) continue;
        if (++count > 8) break; // 9e en verder valt weg op echte hardware
        marked |= 1u << s;
    }

    // Pas 2: hoog→laag zodat het laagste nummer wint; CC-keten à la de
    // fMSX-OrThem-regel — een sprite OR-t als de eerstvolgende HOGERE
    // gemarkeerde sprite CC=1 had, en de keten schuift bij ELKE
    // gemarkeerde sprite door (ook transparante).
    uint32_t or_them = 0;
    for (int s = 31; s >= 0; s--) {
        if (!(marked & (1u << s))) continue;
        uint32_t a = SA + 4u * (uint32_t)s;
        uint8_t yr = ctx->vram[a];
        uint8_t k = (uint8_t)(yr - vscroll);
        int sy = (k > 256 - IH) ? ((int)k + 1 - 256) : ((int)k + 1);
        int ly = ln - sy;
        if (mag) ly >>= 1;

        uint8_t cb = ctx->vram[SC + 16u * (uint32_t)s + (uint32_t)ly];
        or_them |= cb & 0x40;
        bool or_mode = (or_them & 0x20) != 0;
        uint32_t color = cb & 0x0F;
        if (color) {
            int sx = ctx->vram[a + 1];
            if (cb & 0x80) sx -= 32; // EC per lijn
            uint8_t pat = ctx->vram[a + 2];
            for (int dx = 0; dx < OH; dx++) {
                int xp = sx + dx;
                if (xp < 0 || xp > 255) continue;
                int lx = mag ? (dx >> 1) : dx;
                uint32_t off;
                if (size16) {
                    // 16x16 = 4 patronen in TL/BL/TR/BR-volgorde.
                    uint32_t pi = (uint32_t)(pat & 0xFC) + ((uint32_t)lx >> 3) * 2 + ((uint32_t)ly >> 3);
                    off = pi * 8 + ((uint32_t)ly & 7);
                } else {
                    off = (uint32_t)pat * 8 + (uint32_t)ly;
                }
                if (ctx->vram[SG + off] & (0x80 >> (lx & 7))) {
                    if (or_mode && ovr[xp] != 0xFF)
                        ovr[xp] |= (uint8_t)color;
                    else
                        ovr[xp] = (uint8_t)color;
                }
            }
        }
        or_them >>= 1;
    }
}

// Gemeenschappelijke kern: render de lijn als palet-indices (of ruwe
// G7-bytes, *g7 = true). Retourneert de bronbreedte: 256 of 512.
static int __not_in_flash_func(render_line_idx)(v9938_context_t *ctx, uint8_t *buf, int ln, bool *g7)
{
    *g7 = false;
    int active_h = (ctx->regs[9] & 0x80) ? 212 : 192;
    int mode = v_mode(ctx);
    uint8_t ovr[256];

    if (ln >= active_h || BLANKED(ctx)) {
        memset(buf, BACKDROP_IDX(ctx), 256);
        return 256;
    }

    switch (mode) {
    case 0x01: render_t1(ctx, buf, ln); return 256; // tekst: geen sprites
    case 0x00: render_g1(ctx, buf, ln); render_sprites_m1(ctx, buf, ln); return 256;
    case 0x04: render_g2(ctx, buf, ln); render_sprites_m1(ctx, buf, ln); return 256;

    case 0x10: // G5 (scr6) en G6 (scr7): 512px met sprite mode 2 (2x breed)
    case 0x14:
        if (mode == 0x10) render_g5(ctx, buf, ln);
        else              render_g6(ctx, buf, ln);
        render_sprites_m2(ctx, ovr, ln);
        for (int k = 0; k < 256; k++)
            if (ovr[k] != 0xFF) { buf[2 * k] = ovr[k]; buf[2 * k + 1] = ovr[k]; }
        return 512;
    case 0x09: render_t2(ctx, buf, ln); return 512; // 80-koloms: geen sprites

    case 0x08: // G3 (scr4): G2-layout met sprite mode 2
        render_g2(ctx, buf, ln);
        break;
    case 0x0C: // G4 (scr5)
        render_g4(ctx, buf, ln);
        break;
    case 0x1C: // G7 (scr8)
        render_g7(ctx, buf, ln);
        *g7 = true;
        break;
    default:
        // MC (multicolor) en restmodes: backdrop.
        memset(buf, BACKDROP_IDX(ctx), 256);
        return 256;
    }

    render_sprites_m2(ctx, ovr, ln);
    if (*g7) {
        // G7-sprites: paletkleur -> dichtstbijzijnd GGGRRRBB-byte, zodat de
        // buffer uniform blijft. (Echte G7-sprites gebruiken een vast
        // hardware-sprite-palet — TODO, benadering.)
        uint8_t spr[16];
        for (int c = 0; c < 16; c++) {
            uint16_t raw = ctx->palette_raw[c]; // 0b0rrr0bbb00000ggg
            spr[c] = (uint8_t)((((raw >> 8) & 7) << 5) | (((raw >> 4) & 7) << 2) | ((raw & 7) >> 1));
        }
        for (int k = 0; k < 256; k++)
            if (ovr[k] != 0xFF) buf[k] = spr[ovr[k]];
    } else {
        for (int k = 0; k < 256; k++)
            if (ovr[k] != 0xFF) buf[k] = ovr[k];
    }
    return 256;
}

// SDL/ARGB-pad: altijd 512 breed (256-modes pixelverdubbeld).
void __not_in_flash_func(v9938_render_line)(v9938_context_t *ctx, uint32_t *line, int ln)
{
    uint8_t buf[512];
    bool g7;
    int w = render_line_idx(ctx, buf, ln, &g7);
    if (g7) {
        for (int k = 0; k < 256; k++) {
            uint8_t b = buf[k];
            uint32_t g = (b >> 5) & 7, r = (b >> 2) & 7, bl = b & 3;
            uint32_t c = 0xFF000000u | (c3to8(r) << 16) | (c3to8(g) << 8)
                       | ((bl << 6) | (bl << 4) | (bl << 2) | bl);
            line[2 * k] = line[2 * k + 1] = c;
        }
    } else if (w == 512) {
        for (int k = 0; k < 512; k++) line[k] = ctx->palette[buf[k]];
    } else {
        for (int k = 0; k < 256; k++)
            line[2 * k] = line[2 * k + 1] = ctx->palette[buf[k]];
    }
}

// Pico/565-pad: retourneert de bronbreedte (256 of 512); de HSTX-scanout
// verdubbelt 256-brede lijnen zelf. Dit is het hete beam-pad.
int __not_in_flash_func(v9938_render_line_565)(v9938_context_t *ctx, uint16_t *dst, int ln)
{
    uint8_t buf[512];
    bool g7;
    int w = render_line_idx(ctx, buf, ln, &g7);
    if (g7) {
        for (int k = 0; k < 256; k++) dst[k] = g7_565[buf[k]];
        return 256;
    }
    const uint16_t *pal = ctx->palette565;
    for (int k = 0; k < w; k++) dst[k] = pal[buf[k]];
    return w;
}
