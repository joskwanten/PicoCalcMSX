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
}

// Power-on-palette (§7, tabel "initial palette") — de TMS9918-lookalike.
static const uint8_t default_palette[16][3] = {
    {0, 0, 0}, {0, 0, 0}, {1, 6, 1}, {3, 7, 3}, {1, 1, 7}, {2, 3, 7},
    {5, 1, 1}, {2, 6, 7}, {7, 1, 1}, {7, 3, 3}, {6, 6, 1}, {6, 6, 4},
    {1, 4, 1}, {6, 2, 5}, {5, 5, 5}, {7, 7, 7},
};

void v9938_init(v9938_context_t *ctx)
{
    memset(ctx, 0, sizeof *ctx);
    for (int i = 0; i < 16; i++)
        set_palette(ctx, i, default_palette[i][0], default_palette[i][1], default_palette[i][2]);
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

// Registerschrijfactie met bijwerkingen.
static void reg_write(v9938_context_t *ctx, int n, uint8_t v)
{
    n &= 0x3F;
    uint8_t old = ctx->regs[n];
    ctx->regs[n] = v;
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

void v9938_vblank(v9938_context_t *ctx)
{
    ctx->status[0] |= S0_F;
    ctx->status[2] |= S2_VR;
    if (IE0(ctx) && ctx->irq_func)
        ctx->irq_func();
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

// ---- lijnrenderers (legacy modes; bitmap-modes volgen in fase 2) ----
// Interne 256-brede buffer; v9938_render_line verdubbelt naar 512.

static void render_t1(v9938_context_t *ctx, uint32_t *px, int ln)
{
    uint32_t PG = pattern_table(ctx), PN = name_table(ctx);
    uint32_t bg = ctx->palette[BACKDROP_IDX(ctx)];
    uint32_t t = (ctx->regs[7] >> 4) & 0x0F;
    uint32_t fg = t ? ctx->palette[t] : bg;
    int y = ln >> 3, i = ln & 7;
    for (int k = 0; k < 256; k++) px[k] = bg;
    if (y >= 24) return;
    for (int x = 0; x < 40; x++) {
        uint32_t c = ctx->vram[PN + (uint32_t)(y * 40) + x];
        uint32_t p = ctx->vram[PG + 8 * c + i];
        for (int j = 0; j < 6; j++)
            px[8 + x * 6 + j] = (p & (0x80 >> j)) ? fg : bg; // 240px, 8px inspring
    }
}

static void render_g1(v9938_context_t *ctx, uint32_t *px, int ln)
{
    uint32_t PG = pattern_table(ctx), PN = name_table(ctx), CT = color_table(ctx);
    uint32_t bdc = ctx->palette[BACKDROP_IDX(ctx)];
    int y = ln >> 3, i = ln & 7;
    for (int x = 0; x < 32; x++) {
        uint32_t c = ctx->vram[PN + (uint32_t)(y * 32) + x];
        uint32_t col = ctx->vram[CT + (c >> 3)];
        uint32_t fg = (col >> 4) ? ctx->palette[col >> 4] : bdc;
        uint32_t bg = (col & 0xF) ? ctx->palette[col & 0xF] : bdc;
        uint32_t p = ctx->vram[PG + 8 * c + i];
        for (int j = 0; j < 8; j++)
            px[x * 8 + j] = (p & (0x80 >> j)) ? fg : bg;
    }
}

static void render_g2(v9938_context_t *ctx, uint32_t *px, int ln)
{
    // G2/G3-layout: naam + derde<<8, met AND-maskers uit R3/R4 (zoals de
    // TMS9918-screen-2, maar met de bredere V9938-basisregisters).
    uint32_t PG = ((uint32_t)ctx->regs[4] & 0x3C) << 11;
    uint32_t CT = (((uint32_t)ctx->regs[10] & 7) << 14) | (((uint32_t)ctx->regs[3] & 0x80) << 6);
    uint32_t PN = name_table(ctx);
    uint32_t colourMask = (((uint32_t)ctx->regs[3] & 0x7F) << 3) | 7;
    uint32_t patternMask = (((uint32_t)ctx->regs[4] & 3) << 8) | 0xFF;
    uint32_t bdc = ctx->palette[BACKDROP_IDX(ctx)];
    int third = ln >> 6, row = ln & 7, y = ln >> 3;
    for (int x = 0; x < 32; x++) {
        uint32_t charcode = ctx->vram[PN + (uint32_t)(y * 32) + x] + ((uint32_t)third << 8);
        uint32_t p = ctx->vram[PG + ((charcode & patternMask) << 3) + row];
        uint32_t col = ctx->vram[CT + ((charcode & colourMask) << 3) + row];
        uint32_t fg = (col >> 4) ? ctx->palette[col >> 4] : bdc;
        uint32_t bg = (col & 0xF) ? ctx->palette[col & 0xF] : bdc;
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

static void render_sprites_m1(v9938_context_t *ctx, uint32_t *px, int ln)
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
        uint32_t c = attr & 0x0F;
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
            if (xp >= 0 && xp < 256) px[xp] = ctx->palette[c];
        }
    }
}

void __not_in_flash_func(v9938_render_line)(v9938_context_t *ctx, uint32_t *line, int ln)
{
    uint32_t bdc = ctx->palette[BACKDROP_IDX(ctx)];
    // Actieve hoogte: 192 (LN=0) of 212 (R9 bit 7).
    int active_h = (ctx->regs[9] & 0x80) ? 212 : 192;

    uint32_t px[256];
    bool wide = false; // 512-brede modes volgen in fase 2

    if (ln >= active_h || BLANKED(ctx)) {
        for (int k = 0; k < 256; k++) px[k] = bdc;
    } else {
        switch (v_mode(ctx)) {
        case 0x01: render_t1(ctx, px, ln); break;
        case 0x00: render_g1(ctx, px, ln); render_sprites_m1(ctx, px, ln); break;
        case 0x04: render_g2(ctx, px, ln); render_sprites_m1(ctx, px, ln); break;
        default:
            // Nog niet geïmplementeerde mode: backdrop (fase 2: G3-G7, T2, MC).
            for (int k = 0; k < 256; k++) px[k] = bdc;
            break;
        }
    }

    if (!wide) {
        for (int k = 0; k < 256; k++) {
            line[2 * k] = px[k];
            line[2 * k + 1] = px[k];
        }
    }
}
