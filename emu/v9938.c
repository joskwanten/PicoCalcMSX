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
    if (v9938_mode(ctx)) {
        ctx->vram_addr = (ctx->vram_addr + 1) & 0x1FFFF;
        // R14 is de echte A16-A14-teller: de carry over de 16KB-grens hoort
        // erin terug, anders leest een volgende adres-setup een stale R14.
        ctx->regs[14] = (uint8_t)((ctx->vram_addr >> 14) & 7);
    } else {
        ctx->vram_addr = (ctx->vram_addr & 0x1C000) | ((ctx->vram_addr + 1) & 0x3FFF);
    }
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
    // TR "ready"; S2-bits 2/3 staan op echte hardware vast op 1 en S4/S6
    // hebben vaste 1-bits (detectieroutines onderscheiden zo een V99x8
    // van een TMS9929).
    ctx->status[2] = S2_TR | 0x0C;
    ctx->status[4] = 0xFE;
    ctx->status[6] = 0xFC;
    ctx->cops = 13662; // command-engine-budget (zie CMD_OPS_PER_LINE)
}

void v9938_register_interrupt_func(v9938_context_t *ctx, v9938_irq_func_t f)
{
    ctx->irq_func = f;
}

// ---- poortprotocol ----

// MXC (R45 bit 6) stuurt de datapoort naar expansion-RAM; dat hebben we
// niet, dus reads leveren 0xFF en writes verdwijnen (zoals op een kale
// V9938 zonder expansiegeheugen).
uint8_t __not_in_flash_func(v9938_read_data)(v9938_context_t *ctx) // 0x98 in
{
    ctx->has_latched = false;
    uint8_t v = ctx->read_ahead; // read-ahead-buffer (§4.1)
    ctx->read_ahead = (ctx->regs[45] & 0x40) ? 0xFF : ctx->vram[ctx->vram_addr];
    addr_inc(ctx);
    return v;
}

void __not_in_flash_func(v9938_write_data)(v9938_context_t *ctx, uint8_t v) // 0x98 out
{
    ctx->has_latched = false;
    if (!(ctx->regs[45] & 0x40))
        ctx->vram[ctx->vram_addr] = v;
    addr_inc(ctx);
}

static void cmd_start(v9938_context_t *ctx, uint8_t v);
static void cmd_cpu_step(v9938_context_t *ctx, uint8_t data);

// Registerschrijfactie met bijwerkingen. R0-R27 hebben hardware-maskers
// (ongebruikte bits lezen 0 terug); waarden uit MAME's reg_mask-tabel.
static const uint8_t reg_mask[28] = {
    0x7E, 0x7B, 0x7F, 0xFF, 0x3F, 0xFF, 0x3F, 0xFF,
    0xFB, 0xBF, 0x07, 0x03, 0xFF, 0xFF, 0x07, 0x0F,
    0x0F, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x7F, 0x3F, 0x07,
};

static void __not_in_flash_func(reg_write)(v9938_context_t *ctx, int n, uint8_t v)
{
    n &= 0x3F;
    if (n <= 27) v &= reg_mask[n];
    ctx->regs[n] = v;
#ifdef VDP_CMD_DEBUG
    if (n == 23 || n == 19 || n == 2 || n == 9)
        fprintf(stderr, "[reg] R%d=%02X\n", n, v);
#endif
    switch (n) {
    // R0/R1 (IE1/IE0): geen actie hier — de host herrekent de INT-lijn na
    // elke 0x99/0x9B-write volledig uit v9938_irq_asserted(), zodat zowel
    // "IE aan met hangende flag" als "IE uit -> INT intrekken" klopt.
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
    if ((v & 0xC0) == 0x80) {
        reg_write(ctx, v & 0x3F, ctx->latched); // V9938: 6-bit registernummer
    } else if (v & 0x80) {
        // Bits 7 én 6 gezet: geen registerwrite, geen adres-setup (hardware
        // negeert het paar — MAME-gedrag).
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
        // S0-read wist F, 5S en C; het 5e/9e-spritenummer (bits 0-4)
        // blijft staan (MAME/hardware).
        ctx->status[0] &= 0x1F;
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
        // CE is hoog zolang een CPU-transfer (cm != 0) loopt, en blijft na
        // een afgerond commando nog een paar reads hoog (zie cmd_done).
        ctx->s2_phase++;
        uint8_t dyn = 0;
        if (ctx->cm) dyn |= S2_CE;
        else if (ctx->ce_hold) { dyn |= S2_CE; ctx->ce_hold--; }
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
// synchroon uit bij de R46-write; CPU-transfers (HMMC/LMMC/LMCM) blijven
// actief: elke R44-write of S7-read verwerkt één byte/pixel.
// De lusstructuur (rand-terminatie per rij i.p.v. wrap, NX=0 = "tot de rand",
// abort bij Y=-1, register-terugschrijf bij voltooiing) is vertaald uit MAME's
// v99x8-device (src/devices/video/v9938.cpp, BSD-3-Clause, © Aaron Giles,
// Nathan Woods) — zie docs/V9938-MAME-DIFF.md. Echte busy-TIJD (het
// 13662-eenheden/scanline-budget) komt met de scanline-lus van portfase 4.

#define S2_BD 0x10 // border detected (SRCH)

// Pixel-layout van de actieve bitmap-mode. Commando's zijn alleen geldig in
// G4-G7; cmd_start weigert andere modes (zoals de echte V9938).
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

// Pixelbreedte-masker voor R44/kleurdata in de actieve mode.
static inline uint8_t cmd_color_mask(const cmd_layout_t *L)
{
    return (L->ppb == 1) ? 0xFF : (L->ppb == 2) ? 0x0F : 0x03;
}

static inline uint32_t cmd_addr(const cmd_layout_t *L, uint32_t x, uint32_t y)
{
    return ((y & 0x3FF) * L->pitch + (x & (L->width - 1)) / L->ppb) & 0x1FFFF;
}

static uint8_t __not_in_flash_func(vdp_point)(v9938_context_t *ctx, uint32_t x, uint32_t y)
{
    if (ctx->cmxs) return 0xFF; // bron in (afwezig) expansie-RAM
    cmd_layout_t L = cmd_layout(ctx);
    uint8_t b = ctx->vram[cmd_addr(&L, x, y)];
    switch (L.ppb) {
    case 1: return b;
    case 2: return (x & 1) ? (b & 0x0F) : (b >> 4);
    default: return (uint8_t)((b >> ((3 - (x & 3)) * 2)) & 3);
    }
}

// Pixel-write met logische operatie (lage nibble van R46). T-varianten
// (bit 3) slaan transparant over als de bronkleur 0 is; de ongeldige ops
// (5/6/7 en hun T-varianten) schrijven niet (MAME-gedrag).
static void __not_in_flash_func(vdp_pset)(v9938_context_t *ctx, uint32_t x, uint32_t y, uint8_t color, uint8_t lo)
{
    if (ctx->cmxd) return; // doel in (afwezig) expansie-RAM
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
    default: return;                                     // 5/6/7: geen write
    }
    ctx->vram[addr] = (uint8_t)((cur & ~(mask << shift)) | ((uint32_t)px << shift));
}

// Latch de parameters uit R32-R45 (MAME: SX/DX 9 bits, SY/DY/NX/NY 10 bits).
// NX=0 en NY=0 zijn GEEN vaste 512/1024: door de pre-decrement-lussemantiek
// betekenen ze "tot de rand" resp. 1024 rijen — bij LINE zijn NX/NY de
// major/minor-lengtes en betekent 0 gewoon 0 (het Philips-bootlogo tekent
// zijn horizontale lijnen met NY=0).
static void __not_in_flash_func(cmd_latch)(v9938_context_t *ctx)
{
    ctx->csx = (int16_t)(ctx->regs[32] | ((ctx->regs[33] & 1) << 8));
    ctx->csy = (int16_t)(ctx->regs[34] | ((ctx->regs[35] & 3) << 8));
    ctx->cdx = (int16_t)(ctx->regs[36] | ((ctx->regs[37] & 1) << 8));
    ctx->cdy = (int16_t)(ctx->regs[38] | ((ctx->regs[39] & 3) << 8));
    ctx->cnx = (uint16_t)(ctx->regs[40] | ((ctx->regs[41] & 3) << 8));
    ctx->cny = (int16_t)(ctx->regs[42] | ((ctx->regs[43] & 3) << 8));
    ctx->cdix = (ctx->regs[45] & 0x04) ? -1 : 1;
    ctx->cdiy = (ctx->regs[45] & 0x08) ? -1 : 1;
    // MXS/MXD: bron/doel in expansie-RAM — niet aanwezig, dus bron leest
    // 0xFF en doel-writes verdwijnen (guards in vdp_point/vdp_pset en de
    // byte-lussen).
    ctx->cmxs = (ctx->regs[45] & 0x10) ? 1 : 0;
    ctx->cmxd = (ctx->regs[45] & 0x20) ? 1 : 0;
}

// Register-terugschrijf bij voltooiing (echte hardware doet dit; software
// vertrouwt op de auto-advance van DY/SY voor opeenvolgende blits).
static void wb_dy_ny(v9938_context_t *ctx, int dy, int ny)
{
    ctx->regs[38] = (uint8_t)(dy & 0xFF);
    ctx->regs[39] = (uint8_t)((dy >> 8) & 3);
    ctx->regs[42] = (uint8_t)(ny & 0xFF);
    ctx->regs[43] = (uint8_t)((ny >> 8) & 3);
}

static void wb_sy(v9938_context_t *ctx, int sy)
{
    ctx->regs[34] = (uint8_t)(sy & 0xFF);
    ctx->regs[35] = (uint8_t)((sy >> 8) & 3);
}

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

// Busy-tijd (MAME): elke engine-eenheid (byte voor H*-ops, pixel voor
// L*-ops) kost 'delta' eenheden uit een tabel per commando, geïndexeerd op
// scherm-aan | sprites-uit | PAL; het budget is 13662 eenheden per scanline
// (bijgevuld in v9938_scanline). Zo blijft CE realistisch lang hoog en
// muteert VRAM in de pas met de beam — precies waar race-the-beam-software
// (Quarth!) op leunt.
#define CMD_OPS_PER_LINE 13662

static const int srch_timing[8] = { 818, 1025, 818, 830, 696, 854, 696, 684 };
static const int line_timing[8] = { 1063, 1259, 1063, 1161, 904, 1026, 904, 953 };
static const int hmmv_timing[8] = { 439, 549, 439, 531, 366, 439, 366, 427 };
static const int lmmv_timing[8] = { 873, 1135, 873, 1056, 732, 909, 732, 854 };
static const int ymmm_timing[8] = { 586, 952, 586, 610, 488, 720, 488, 500 };
static const int hmmm_timing[8] = { 818, 1111, 818, 854, 684, 879, 684, 708 };
static const int lmmm_timing[8] = { 1160, 1599, 1160, 1172, 964, 1257, 964, 977 };

static inline int cmd_timing(const v9938_context_t *ctx, const int *t)
{
    return t[((ctx->regs[1] >> 6) & 1) | (ctx->regs[8] & 2) | ((ctx->regs[9] << 1) & 4)];
}

// Hervatbare engine voor de blokcommando's. Werkstate leeft in de context:
// casx/cadx/canx zijn de rijtellers (LINE: casx = Bresenham-accu, cadx =
// pixelteller; SRCH: casx = lopende SX, canx = NEQ-vlag), csy/cdy/cny de
// muterende Y-state; csx/cdx/cnx blijven gelatcht voor de rij-reset.
// Lussemantiek naar MAME:
//  - een rij eindigt zodra de teller op is OF X de rand kruist (x & width,
//    vangt zowel x==width als x==-1) — geen wrap naar de andere kant;
//  - het commando breekt af zodra SY/DY bij DIY=-1 onder 0 zakt;
//  - NY telt af met &1023 (NY=0 -> effectief 1024 rijen), en de eindstand
//    van SY/DY/NY gaat bij voltooiing terug de registers in.
static void __not_in_flash_func(cmd_engine)(v9938_context_t *ctx)
{
    cmd_layout_t L = cmd_layout(ctx);
    int width = (int)L.width;
    int tx = ctx->cdix, btx = ctx->cdix * (int)L.ppb, ty = ctx->cdiy;
    uint8_t clr = ctx->cclr;

    switch (ctx->cm) {
    case 0xC: { // HMMV: byte-vulling
        int delta = cmd_timing(ctx, hmmv_timing);
        while ((ctx->cops -= delta) > 0) {
            if (!ctx->cmxd)
                ctx->vram[cmd_addr(&L, (uint32_t)ctx->cadx, (uint32_t)ctx->cdy)] = clr;
            if (!--ctx->canx || ((ctx->cadx += btx) & width)) {
                if (!(--ctx->cny & 1023) || (ctx->cdy += ty) == -1) {
                    if (!ctx->cny) ctx->cdy += ty;
                    wb_dy_ny(ctx, ctx->cdy, ctx->cny);
                    cmd_done(ctx);
                    return;
                }
                ctx->cadx = ctx->cdx;
                ctx->canx = (int16_t)(ctx->cnx / L.ppb);
            }
        }
        break;
    }
    case 0xD: { // HMMM: byte-kopie
        int delta = cmd_timing(ctx, hmmm_timing);
        while ((ctx->cops -= delta) > 0) {
            if (!ctx->cmxd)
                ctx->vram[cmd_addr(&L, (uint32_t)ctx->cadx, (uint32_t)ctx->cdy)] = ctx->cmxs
                    ? 0xFF : ctx->vram[cmd_addr(&L, (uint32_t)ctx->casx, (uint32_t)ctx->csy)];
            if (!--ctx->canx || ((ctx->casx += btx) & width) || ((ctx->cadx += btx) & width)) {
                if (!(--ctx->cny & 1023) || (ctx->csy += ty) == -1 || (ctx->cdy += ty) == -1) {
                    if (!ctx->cny) { ctx->csy += ty; ctx->cdy += ty; }
                    else if (ctx->csy == -1) ctx->cdy += ty;
                    wb_sy(ctx, ctx->csy);
                    wb_dy_ny(ctx, ctx->cdy, ctx->cny);
                    cmd_done(ctx);
                    return;
                }
                ctx->casx = ctx->csx;
                ctx->cadx = ctx->cdx;
                ctx->canx = (int16_t)(ctx->cnx / L.ppb);
            }
        }
        break;
    }
    case 0xE: { // YMMM: byte-kopie in Y (bron én doel via MXD, zoals MAME)
        int delta = cmd_timing(ctx, ymmm_timing);
        while ((ctx->cops -= delta) > 0) {
            if (!ctx->cmxd)
                ctx->vram[cmd_addr(&L, (uint32_t)ctx->cadx, (uint32_t)ctx->cdy)] =
                    ctx->vram[cmd_addr(&L, (uint32_t)ctx->cadx, (uint32_t)ctx->csy)];
            if ((ctx->cadx += btx) & width) {
                if (!(--ctx->cny & 1023) || (ctx->csy += ty) == -1 || (ctx->cdy += ty) == -1) {
                    if (!ctx->cny) { ctx->csy += ty; ctx->cdy += ty; }
                    else if (ctx->csy == -1) ctx->cdy += ty;
                    wb_sy(ctx, ctx->csy);
                    wb_dy_ny(ctx, ctx->cdy, ctx->cny);
                    cmd_done(ctx);
                    return;
                }
                ctx->cadx = ctx->cdx;
            }
        }
        break;
    }
    case 0x8: { // LMMV: pixel-vulling met log. op
        int delta = cmd_timing(ctx, lmmv_timing);
        while ((ctx->cops -= delta) > 0) {
            vdp_pset(ctx, (uint32_t)ctx->cadx, (uint32_t)ctx->cdy, clr, ctx->clo);
            if (!--ctx->canx || ((ctx->cadx += tx) & width)) {
                if (!(--ctx->cny & 1023) || (ctx->cdy += ty) == -1) {
                    if (!ctx->cny) ctx->cdy += ty;
                    wb_dy_ny(ctx, ctx->cdy, ctx->cny);
                    cmd_done(ctx);
                    return;
                }
                ctx->cadx = ctx->cdx;
                ctx->canx = (int16_t)ctx->cnx;
            }
        }
        break;
    }
    case 0x9: { // LMMM: pixel-kopie met log. op
        int delta = cmd_timing(ctx, lmmm_timing);
        while ((ctx->cops -= delta) > 0) {
            vdp_pset(ctx, (uint32_t)ctx->cadx, (uint32_t)ctx->cdy,
                     vdp_point(ctx, (uint32_t)ctx->casx, (uint32_t)ctx->csy), ctx->clo);
            if (!--ctx->canx || ((ctx->casx += tx) & width) || ((ctx->cadx += tx) & width)) {
                if (!(--ctx->cny & 1023) || (ctx->csy += ty) == -1 || (ctx->cdy += ty) == -1) {
                    if (!ctx->cny) { ctx->csy += ty; ctx->cdy += ty; }
                    else if (ctx->csy == -1) ctx->cdy += ty;
                    wb_sy(ctx, ctx->csy);
                    wb_dy_ny(ctx, ctx->cdy, ctx->cny);
                    cmd_done(ctx);
                    return;
                }
                ctx->casx = ctx->csx;
                ctx->cadx = ctx->cdx;
                ctx->canx = (int16_t)ctx->cnx;
            }
        }
        break;
    }
    case 0x7: { // LINE: Bresenham langs de major-as (MAJ = ARG bit 0)
        int delta = cmd_timing(ctx, line_timing);
        bool ymajor = (ctx->regs[45] & 0x01) != 0;
        while ((ctx->cops -= delta) > 0) {
            vdp_pset(ctx, (uint32_t)ctx->cdx, (uint32_t)ctx->cdy, clr, ctx->clo);
            if (ymajor) {
                ctx->cdy += ty;
                if ((ctx->casx -= ctx->cny) < 0) { ctx->casx += (int16_t)ctx->cnx; ctx->cdx += tx; }
            } else {
                ctx->cdx += tx;
                if ((ctx->casx -= ctx->cny) < 0) { ctx->casx += (int16_t)ctx->cnx; ctx->cdy += ty; }
            }
            ctx->casx &= 1023;
            if (ctx->cadx++ == (int16_t)ctx->cnx || (ctx->cdx & width)) {
                // LINE schrijft alleen DY terug (MAME), NY blijft staan.
                ctx->regs[38] = (uint8_t)(ctx->cdy & 0xFF);
                ctx->regs[39] = (uint8_t)((ctx->cdy >> 8) & 3);
                cmd_done(ctx);
                return;
            }
        }
        break;
    }
    case 0x6: { // SRCH: zoek kleur (NEQ in canx) langs lijn SY vanaf SX
        int delta = cmd_timing(ctx, srch_timing);
        while ((ctx->cops -= delta) > 0) {
            uint8_t c = vdp_point(ctx, (uint32_t)ctx->casx, (uint32_t)ctx->csy);
            bool done = false;
            if ((c == clr) != (ctx->canx != 0)) { // hit
                ctx->status[2] |= S2_BD;
                done = true;
            } else if ((ctx->casx += tx) & width) { // rand zonder hit
                ctx->status[2] &= (uint8_t)~S2_BD;
                done = true;
            }
            if (done) {
                // S8/S9 krijgen de eindpositie ALTIJD (ook zonder hit).
                ctx->status[8] = (uint8_t)(ctx->casx & 0xFF);
                ctx->status[9] = (uint8_t)(((unsigned)ctx->casx >> 8) | 0xFE);
                cmd_done(ctx);
                return;
            }
        }
        break;
    }
    default:
        break;
    }
}

// R46-write: commando starten. Buiten G4-G7 is élke R46-write een no-op
// (MAME/hardware) — ook STOP; CE blijft dan gewoon staan.
static void __not_in_flash_func(cmd_start)(v9938_context_t *ctx, uint8_t v)
{
    int m = v_mode(ctx);
    if (m != 0x0C && m != 0x10 && m != 0x14 && m != 0x1C) return;

    uint8_t cm = v >> 4;
    ctx->clo = v & 0x0F;
    // Dot-commando's (alles behalve STOP en de HM-bytecommando's) masken
    // R44 hard op de pixelbreedte van de mode; S7 spiegelt dat (MAME).
    if (cm != 0 && (cm & 0x0C) != 0x0C) {
        cmd_layout_t L = cmd_layout(ctx);
        ctx->regs[44] &= cmd_color_mask(&L);
        ctx->status[7] = ctx->regs[44];
    }
    cmd_latch(ctx);
#ifdef VDP_CMD_DEBUG
    fprintf(stderr, "[cmd] %X lo=%X sx=%d sy=%d dx=%d dy=%d nx=%u ny=%d dix=%d diy=%d clr=%02X\n",
            cm, ctx->clo, ctx->csx, ctx->csy, ctx->cdx, ctx->cdy,
            ctx->cnx, ctx->cny, ctx->cdix, ctx->cdiy, ctx->regs[44]);
#endif
    if (cm == 0) {
        // STOP/ABRT: engine meteen idle, CE direct laag (geen ce_hold).
        ctx->cm = 0;
        ctx->ce_hold = 0;
        ctx->status[2] = (uint8_t)((ctx->status[2] & ~S2_CE) | S2_TR);
        return;
    }
    if (cm == 0xF || cm == 0xB) {
        // HMMC/LMMC: CPU->VRAM. SPEC-SUBTILITEIT (§9.7): de R44/CLR-waarde
        // op het moment van de commandostart is het EERSTE datum; de stream
        // via R44-writes begint dus bij pixel/byte 1. (Zonder dit schoof
        // alles één op en lekte de CLR-setup van het volgende glyph als
        // valse pixelkolom in de staart van het vorige — zie het MSX2-
        // bootscherm "128Kbytes"-artefact.)
        cmd_layout_t L = cmd_layout(ctx);
        ctx->cm = cm;
        ctx->cadx = ctx->cdx;
        ctx->canx = (cm == 0xF) ? (int16_t)(ctx->cnx / L.ppb) : (int16_t)ctx->cnx;
        ctx->status[2] |= S2_CE | S2_TR;
        cmd_cpu_step(ctx, ctx->regs[44]);
        return;
    }
    if (cm == 0xA) {
        // LMCM: VRAM->CPU; eerste pixel klaarzetten in S7.
        ctx->cm = cm;
        ctx->casx = ctx->csx;
        ctx->canx = (int16_t)ctx->cnx;
        ctx->status[2] |= S2_CE | S2_TR;
        ctx->status[7] = vdp_point(ctx, (uint32_t)ctx->casx, (uint32_t)ctx->csy);
        return;
    }
    if (cm == 0x5) { // PSET: synchroon (MAME)
        vdp_pset(ctx, (uint32_t)ctx->cdx, (uint32_t)ctx->cdy, ctx->regs[44], ctx->clo);
        cmd_done(ctx);
        return;
    }
    if (cm == 0x4) { // POINT -> S7 én R44 (hardware): synchroon
        ctx->status[7] = ctx->regs[44] =
            vdp_point(ctx, (uint32_t)ctx->csx, (uint32_t)ctx->csy);
        cmd_done(ctx);
        return;
    }
    if (cm < 0x6) return; // 1-3: ongeldig, negeren

    // Blokcommando: werkstate initialiseren en de engine starten met het
    // resterende scanline-budget (op=0 -> hij loopt verder in de volgende
    // v9938_scanline-aanroep, zoals MAME's update_command).
    cmd_layout_t L = cmd_layout(ctx);
    ctx->cm = cm;
    ctx->cclr = ctx->regs[44];
    ctx->status[2] |= S2_CE;
    switch (cm) {
    case 0x7: // LINE
        ctx->casx = (int16_t)((ctx->cnx - 1) >> 1); // Bresenham-afrondingsinit
        ctx->cadx = 0;                              // pixelteller
        break;
    case 0x6: // SRCH
        ctx->casx = ctx->csx;
        ctx->canx = (ctx->regs[45] & 0x02) ? 1 : 0; // NEQ-vlag
        break;
    case 0xC: case 0xD: // HMMV/HMMM: byte-eenheden
        ctx->casx = ctx->csx;
        ctx->cadx = ctx->cdx;
        ctx->canx = (int16_t)(ctx->cnx / L.ppb);
        break;
    default: // LMMV/LMMM/YMMM
        ctx->casx = ctx->csx;
        ctx->cadx = ctx->cdx;
        ctx->canx = (int16_t)ctx->cnx;
        break;
    }
    if (ctx->cops > 0)
        cmd_engine(ctx);
}

// Eén stap van een lopende CPU-transfer (R44-write of S7-read). Zelfde
// MAME-lussemantiek als cmd_run_vram, maar incrementeel: canx/cadx/casx
// zijn de rijtellers, csy/cdy/cny de (muterende) Y-state.
static void __not_in_flash_func(cmd_cpu_step)(v9938_context_t *ctx, uint8_t data)
{
    cmd_layout_t L = cmd_layout(ctx);
    int width = (int)L.width;
    int tx = ctx->cdix, btx = ctx->cdix * (int)L.ppb, ty = ctx->cdiy;
    switch (ctx->cm) {
    case 0xF: // HMMC: byte schrijven, dan tellers doorschuiven
        ctx->cops -= cmd_timing(ctx, hmmv_timing);
        if (!ctx->cmxd)
            ctx->vram[cmd_addr(&L, (uint32_t)ctx->cadx, (uint32_t)ctx->cdy)] = data;
        if (!--ctx->canx || ((ctx->cadx += btx) & width)) {
            if (!(--ctx->cny & 1023) || (ctx->cdy += ty) == -1) {
                if (!ctx->cny) ctx->cdy += ty;
                wb_dy_ny(ctx, ctx->cdy, ctx->cny);
                cmd_done(ctx);
            } else {
                ctx->cadx = ctx->cdx;
                ctx->canx = (int16_t)(ctx->cnx / L.ppb);
            }
        }
        break;
    case 0xB: // LMMC: pixel met log. op (data per byte op pixelbreedte gemaskt)
        ctx->cops -= cmd_timing(ctx, lmmv_timing);
        data &= cmd_color_mask(&L);
        ctx->status[7] = data;
        vdp_pset(ctx, (uint32_t)ctx->cadx, (uint32_t)ctx->cdy, data, ctx->clo);
        if (!--ctx->canx || ((ctx->cadx += tx) & width)) {
            if (!(--ctx->cny & 1023) || (ctx->cdy += ty) == -1) {
                if (!ctx->cny) ctx->cdy += ty;
                wb_dy_ny(ctx, ctx->cdy, ctx->cny);
                cmd_done(ctx);
            } else {
                ctx->cadx = ctx->cdx;
                ctx->canx = (int16_t)ctx->cnx;
            }
        }
        break;
    case 0xA: // LMCM: tellers doorschuiven, volgende pixel in S7
        ctx->cops -= cmd_timing(ctx, lmmv_timing);
        if (!--ctx->canx || ((ctx->casx += tx) & width)) {
            if (!(--ctx->cny & 1023) || (ctx->csy += ty) == -1) {
                wb_sy(ctx, ctx->csy);
                ctx->regs[42] = (uint8_t)(ctx->cny & 0xFF);
                ctx->regs[43] = (uint8_t)((ctx->cny >> 8) & 3);
                cmd_done(ctx);
                return;
            }
            ctx->casx = ctx->csx;
            ctx->canx = (int16_t)ctx->cnx;
        }
        ctx->status[7] = vdp_point(ctx, (uint32_t)ctx->casx, (uint32_t)ctx->csy);
        break;
    default:
        break;
    }
}

// Scanline-hook: de machine draait per displaylijn ~228 T-states Z80 en
// meldt daarna de lijn. Hier leven de echte VR- en FH-semantiek:
//  - de lijnvergelijking is (line + R23) & 255 == R19: de hardware
//    vergelijkt R19 met de GESCROLDE lijnenteller, dus een split schuift
//    mee met de verticale scroll (MAME/hardware-gedrag);
//  - FH wordt gezet ongeacht IE1 (IE1 bepaalt alleen de INT-lijn), maar
//    met IE1 UIT wist elke niet-matchende lijn hem weer — anders ziet
//    pollende software een stokoude FH van een vorig frame en triggert
//    een split te vroeg. Met IE1 aan blijft FH gelatcht tot de S1-read.
//  - VR is hoog in de verticale blanking (lijn >= actieve hoogte).
//  - Op de eerste vblank-lijn: S0.F + frame-IRQ (IE0).
// Blokcommando actief (CPU-transfers lopen event-gedreven via cmd_cpu_step)?
static inline bool cmd_block_active(const v9938_context_t *ctx)
{
    return ctx->cm != 0 && ctx->cm != 0xA && ctx->cm != 0xB && ctx->cm != 0xF;
}

void __not_in_flash_func(v9938_scanline)(v9938_context_t *ctx, int line)
{
    int active_h = (ctx->regs[9] & 0x80) ? 212 : 192;

    // Command-engine-budget bijvullen en een lopend blokcommando een stuk
    // verder laten lopen (MAME's update_command).
    if (ctx->cops <= 0)
        ctx->cops += CMD_OPS_PER_LINE;
    else
        ctx->cops = CMD_OPS_PER_LINE;
    if (cmd_block_active(ctx) && ctx->cops > 0)
        cmd_engine(ctx);

    if (((line + ctx->regs[23]) & 0xFF) == ctx->regs[19]) {
        ctx->status[1] |= S1_FH;
        ctx->line_irq_pending = true;
        if (IE1(ctx) && ctx->irq_func)
            ctx->irq_func();
    } else if (!IE1(ctx)) {
        ctx->status[1] &= (uint8_t)~S1_FH;
        ctx->line_irq_pending = false;
    }

    if (line < active_h)
        ctx->status[2] &= (uint8_t)~S2_VR;
    else
        ctx->status[2] |= S2_VR;

    if (line == active_h) {
        ctx->status[0] |= S0_F;
        ctx->status[2] ^= 0x02; // EO: even/oneven-veldbit toggelt per frame
        if (IE0(ctx) && ctx->irq_func)
            ctx->irq_func();
    }
}

// Frame-granulaire fallback (MSX1-lus / oude aanroepers): zonder scanline-
// lus is er geen budget-drip, dus een lopend blokcommando hier in één keer
// afmaken.
void v9938_vblank(v9938_context_t *ctx)
{
    if (cmd_block_active(ctx)) {
        ctx->cops = 1 << 28;
        cmd_engine(ctx);
    }
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
    int mode = v_mode(ctx);
    if (mode == 0x1C) { // G7: R7 = ruwe GRB332-byte
        uint8_t b = ctx->regs[7];
        uint32_t g = (b >> 5) & 7, r = (b >> 2) & 7, bl = b & 3;
        return 0xFF000000u | (c3to8(r) << 16) | (c3to8(g) << 8)
             | ((bl << 6) | (bl << 4) | (bl << 2) | bl);
    }
    if (mode == 0x10) // G5: dither-paar; benader met de even kleur
        return ctx->palette[(ctx->regs[7] >> 2) & 3];
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
    int sl = (ln + ctx->regs[23]) & 0xFF; // R23 geldt ook in tekstmodes
    int y = sl >> 3, i = sl & 7;          // 26,5 rijen bij 212 lijnen
    memset(px, bg, 256);
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
    int sl = (ln + ctx->regs[23]) & 0xFF;
    int y = sl >> 3, i = sl & 7;
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
    int sl = (ln + ctx->regs[23]) & 0xFF; // R23-scroll (G2 én G3)
    int third = sl >> 6, row = sl & 7, y = sl >> 3;
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

// Sprite mode 1 (G1/G2/MC — §8.1), MAME-semantiek: max 4 per lijn, laagste
// nummer wint, R23-scroll, Y-wrap op drempel 208, EC = 32px links, MAG,
// kleur 0 zichtbaar met TP (R8 bit 5). Zet ook de S0-statusbits: C bij
// pixel-overlap, 5S + spritenummer bij de 5e, anders het laatst verwerkte
// nummer. col[]-flags: 0x80 = zichtbaar, 0x40 = collision-pixel, 0-3 kleur.
static void __not_in_flash_func(render_sprites_m1)(v9938_context_t *ctx, uint8_t *px, int ln)
{
    if (ctx->regs[8] & 0x02) return; // SPD: sprites uit (V9938, R8 bit 1)
    uint8_t col[256];
    memset(col, 0, 256);
    uint32_t SA = sprite_attr_table(ctx), SG = sprite_pat_table(ctx);
    int sixteen = SIXTEEN(ctx), mag = MAGNIFIED(ctx);
    int height = (sixteen ? 16 : 8) << (mag ? 1 : 0);

    int p, p2 = 0;
    for (p = 0; p < 32; p++) {
        uint32_t a = SA + 4u * (uint32_t)p;
        int y = ctx->vram[a];
        if (y == 208) break;
        y = (y - ctx->regs[23]) & 255;
        y = (y > 208) ? -(~y & 255) : y + 1;
        if (ln < y || ln >= y + height) continue;
        if (p2 == 4) {
            // 5e sprite op deze lijn: 5S + nummer (alleen als nog niet gezet).
            if (!(ctx->status[0] & S0_5S))
                ctx->status[0] = (uint8_t)((ctx->status[0] & 0xA0) | S0_5S | p);
            break;
        }
        int x = ctx->vram[a + 1];
        uint8_t attr = ctx->vram[a + 3];
        if (attr & 0x80) x -= 32; // EC
        uint8_t patn = ctx->vram[a + 2];
        if (sixteen) patn &= 0xFC;
        int n = ln - y;
        if (mag) n >>= 1;
        uint32_t pa = SG + (uint32_t)patn * 8 + (uint32_t)n;
        uint16_t pattern = (uint16_t)(ctx->vram[pa] << 8);
        if (sixteen) pattern |= ctx->vram[pa + 16];
        uint8_t c = attr & 0x0F;
        int total = sixteen ? 16 : 8;
        for (int j = 0; j < total; j++) {
            if (pattern & 0x8000) {
                for (int i = 0; i <= mag; i++) {
                    int xp = x + ((j << (mag ? 1 : 0)) + i);
                    if (xp < 0 || xp > 255) continue;
                    if (col[xp] & 0x40) {
                        if (p2 < 4) ctx->status[0] |= S0_C;
                    }
                    if (!(col[xp] & 0x80)) {
                        if (c || (ctx->regs[8] & 0x20))
                            col[xp] |= (uint8_t)(0xC0 | c);
                        else
                            col[xp] |= 0x40; // onzichtbaar, telt wel voor collisie
                    }
                }
            }
            pattern <<= 1;
        }
        p2++;
    }
    if (!(ctx->status[0] & S0_5S))
        ctx->status[0] = (uint8_t)((ctx->status[0] & 0xA0) | ((p < 32) ? p : 31));

    for (int xp = 0; xp < 256; xp++)
        if (col[xp] & 0x80) px[xp] = (uint8_t)(col[xp] & 0x0F);
}

// ---- bitmap-modes (G4-G7, §6) ----
// Kleur 0 toont de backdrop tenzij TP (R8 bit 5) gezet is; verticale scroll
// (R23) verschuift de bitmap-bron per lijn. tp0 = het effectieve indexalias
// voor kleur 0.
static inline uint8_t tp0_idx(const v9938_context_t *ctx)
{
    return (ctx->regs[8] & 0x20) ? 0 : (uint8_t)BACKDROP_IDX(ctx);
}

// Linemask (R2 bits 0-4, verplichte 1-bits): programma's die ze op 0 zetten
// krijgen tabel-mirroring — kleinere naamtabellen die herhalen (split-trucs).
static inline uint32_t bitmap_line(const v9938_context_t *ctx, int ln)
{
    uint32_t linemask = (((uint32_t)ctx->regs[2] & 0x1F) << 3) | 7;
    return (((uint32_t)ln + ctx->regs[23]) & linemask) & 0xFF;
}

static void __not_in_flash_func(render_g4)(v9938_context_t *ctx, uint8_t *px, int ln) // scr5: 256px 4bpp
{
    uint32_t base = (((uint32_t)ctx->regs[2] >> 5) & 3) << 15;
    uint32_t y = bitmap_line(ctx, ln);
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
    uint32_t y = bitmap_line(ctx, ln);
    const uint8_t *row = &ctx->vram[base + y * 128];
    // Kleur 0 is in G5 een dither-PAAR uit R7: even pixel (R7>>2)&3,
    // oneven pixel R7&3 — tenzij TP (R8 bit 5): dan gewoon palet 0.
    uint8_t tp_ev = 0, tp_od = 0;
    if (!(ctx->regs[8] & 0x20)) {
        tp_ev = (uint8_t)((ctx->regs[7] >> 2) & 3);
        tp_od = (uint8_t)(ctx->regs[7] & 3);
    }
    for (int x = 0; x < 512; x += 4) {
        uint8_t b = row[x >> 2];
        uint8_t c0 = (b >> 6) & 3, c1 = (b >> 4) & 3, c2 = (b >> 2) & 3, c3 = b & 3;
        px512[x] = c0 ? c0 : tp_ev;
        px512[x + 1] = c1 ? c1 : tp_od;
        px512[x + 2] = c2 ? c2 : tp_ev;
        px512[x + 3] = c3 ? c3 : tp_od;
    }
}

static void __not_in_flash_func(render_g6)(v9938_context_t *ctx, uint8_t *px512, int ln) // scr7: 512px 4bpp
{
    uint32_t base = (((uint32_t)ctx->regs[2] >> 5) & 1) << 16;
    uint32_t y = bitmap_line(ctx, ln);
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
    uint32_t y = bitmap_line(ctx, ln);
    memcpy(px, &ctx->vram[base + y * 256], 256);
}

static void __not_in_flash_func(render_t2)(v9938_context_t *ctx, uint8_t *px512, int ln) // 80-koloms tekst
{
    uint32_t PG = pattern_table(ctx);
    uint32_t PN = ((uint32_t)ctx->regs[2] & 0x7C) << 10;
    uint32_t nameMask = (((uint32_t)ctx->regs[2] & 3) << 10) | 0x3FF; // mirroring
    uint8_t bg = (uint8_t)BACKDROP_IDX(ctx);
    uint8_t t = (ctx->regs[7] >> 4) & 0x0F;
    uint8_t fg = t ? t : bg;
    int sl = (ln + ctx->regs[23]) & 0xFF;
    int y = sl >> 3, i = sl & 7; // 26,5 rijen; blink (R12/R13) nog TODO
    memset(px512, bg, 512);
    for (int x = 0; x < 80; x++) {
        uint32_t c = ctx->vram[PN + (((uint32_t)(y * 80) + x) & nameMask)];
        uint32_t p = ctx->vram[PG + 8 * c + i];
        for (int j = 0; j < 6; j++)
            px512[16 + x * 6 + j] = (p & (0x80 >> j)) ? fg : bg;
    }
}

// ---- sprite mode 2 (G3-G7, §2.6), MAME-semantiek: 8 per lijn, laagste
// nummer wint, kleur-per-lijn uit de kleurtabel (basis (R5&0xF8)<<7 met
// indexmasker uit R5 bits 0-1 — mirroring-trucs), EC/CC/IC per lijn,
// sentinel Y=0xD8, R23-scroll, Y-wrap op drempel 216, TP-kleur-0.
// CC=1-sprites zijn verborgen tot een CC=0-sprite op de lijn gezien is en
// OR-en daarna in de kleur van de basissprite. Zet S0: C bij overlap van
// collisie-waardige (CC=0, IC=0) pixels, 5S + nummer bij de 9e.
// col[]-flags: 0x80 zichtbaar, 0x40 collisie-pixel, 0x20 CC0-basis,
// 0x10 geblokkeerd, bits 0-3 kleur. Levert palette-indices in ovr
// (0xFF = geen sprite).
static void __not_in_flash_func(render_sprites_m2)(v9938_context_t *ctx, uint8_t *ovr, int ln)
{
    memset(ovr, 0xFF, 256);
    if (ctx->regs[8] & 0x02) return; // SPD

    uint8_t col[256];
    memset(col, 0, 256);
    uint32_t SA = (((uint32_t)ctx->regs[11] & 3) << 15) | (((uint32_t)ctx->regs[5] & 0xFC) << 7);
    uint32_t SC = (((uint32_t)ctx->regs[11] & 3) << 15) | (((uint32_t)ctx->regs[5] & 0xF8) << 7);
    uint32_t SG = ((uint32_t)ctx->regs[6] & 0x3F) << 11;
    uint32_t colourmask = (((uint32_t)ctx->regs[5] & 3) << 3) | 7;
    int sixteen = SIXTEEN(ctx), mag = MAGNIFIED(ctx);
    int height = (sixteen ? 16 : 8) << (mag ? 1 : 0);

    int p, p2 = 0, first_cc_seen = 0;
    for (p = 0; p < 32; p++) {
        uint32_t a = SA + 4u * (uint32_t)p;
        int y = ctx->vram[a];
        if (y == 216) break;
        y = (y - ctx->regs[23]) & 255;
        y = (y > 216) ? -(~y & 255) : y + 1;
        if (ln < y || ln >= y + height) continue;
        if (p2 == 8) {
            // 9e sprite op deze lijn: 5S + nummer (alleen als nog niet gezet).
            if (!(ctx->status[0] & S0_5S))
                ctx->status[0] = (uint8_t)((ctx->status[0] & 0xA0) | S0_5S | p);
            break;
        }
        int n = ln - y;
        if (mag) n >>= 1;
        uint8_t c = ctx->vram[SC + ((uint32_t)p & colourmask) * 16 + (uint32_t)n];
        // CC=1 vóór de eerste CC=0-sprite op deze lijn: verbergen (telt wel mee).
        if (c & 0x40) {
            if (!first_cc_seen) { p2++; continue; }
        } else {
            first_cc_seen = 1;
        }
        uint8_t patn = ctx->vram[a + 2];
        if (sixteen) patn &= 0xFC;
        uint32_t pa = SG + (uint32_t)patn * 8 + (uint32_t)n;
        uint16_t pattern = (uint16_t)((ctx->vram[pa] << 8) | ctx->vram[pa + 16]);
        int x = ctx->vram[a + 1];
        if (c & 0x80) x -= 32; // EC per lijn
        int nn = sixteen ? 16 : 8;
        while (nn--) {
            for (int i = 0; i <= mag; i++, x++) {
                if (x < 0 || x > 255) continue;
                if ((pattern & 0x8000) && !(col[x] & 0x10)) {
                    if ((c & 15) || (ctx->regs[8] & 0x20)) {
                        if (!(c & 0x40)) {
                            // CC=0: eerste basissprite op deze pixel wint.
                            if (col[x] & 0x20) col[x] |= 0x10;
                            else col[x] |= (uint8_t)(0x20 | (c & 15));
                        } else {
                            col[x] |= (uint8_t)(c & 15); // CC=1: kleur-OR
                        }
                        col[x] |= 0x80;
                    }
                } else if (!(c & 0x40) && (col[x] & 0x20)) {
                    col[x] |= 0x10; // basis gepasseerd: latere CC=0 blokkeren
                }
                if (!(c & 0x60) && (pattern & 0x8000)) {
                    if (col[x] & 0x40) {
                        if (p2 < 8) ctx->status[0] |= S0_C;
                    } else {
                        col[x] |= 0x40;
                    }
                }
            }
            pattern <<= 1;
        }
        p2++;
    }
    if (!(ctx->status[0] & S0_5S))
        ctx->status[0] = (uint8_t)((ctx->status[0] & 0xA0) | ((p < 32) ? p : 31));

    for (int xp = 0; xp < 256; xp++)
        if (col[xp] & 0x80) ovr[xp] = (uint8_t)(col[xp] & 0x0F);
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
        if (mode == 0x1C) { // G7: R7 is een ruwe GRB332-byte, geen paletindex
            memset(buf, ctx->regs[7], 256);
            *g7 = true;
        } else {
            memset(buf, BACKDROP_IDX(ctx), 256);
        }
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
        if (mode == 0x10) {
            // G5: de 4-bit spritekleur is TWEE 2-bit halfpixels (512-res).
            for (int k = 0; k < 256; k++)
                if (ovr[k] != 0xFF) {
                    buf[2 * k] = (uint8_t)((ovr[k] >> 2) & 3);
                    buf[2 * k + 1] = (uint8_t)(ovr[k] & 3);
                }
        } else {
            for (int k = 0; k < 256; k++)
                if (ovr[k] != 0xFF) { buf[2 * k] = ovr[k]; buf[2 * k + 1] = ovr[k]; }
        }
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
        // G7-sprites gebruiken het VASTE hardware-spritepalet (paletregisters
        // doen er niet toe) — MAME's g7_ind16 (GRB333) omgezet naar GGGRRRBB.
        static const uint8_t g7_sprite_pal[16] = {
            0x00, 0x01, 0x60, 0x61, 0x18, 0x19, 0x78, 0x79,
            0xF1, 0x03, 0xE0, 0xE3, 0x1C, 0x1F, 0xFC, 0xFF,
        };
        for (int k = 0; k < 256; k++)
            if (ovr[k] != 0xFF) buf[k] = g7_sprite_pal[ovr[k]];
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
