#include "tms9918.h"
#include <string.h>
#include <stdio.h>
#include "pico.h" // __not_in_flash_func: hete render-code in SRAM

enum StatusFlags
{
    S_INT = 0b10000000,
    S_5S = 0b01000000,
    S_C = 0b00100000,
    S_FS4 = 0b00010000,
    S_FS3 = 0b00001000,
    S_FS2 = 0b00000100,
    S_FS1 = 0b00000010,
    S_FS0 = 0b00000001,
};

uint32_t palette[] = {
    0xff000000,
    0xff000000,
    0xff21c842,
    0xff5edc78,
    0xff5455ed,
    0xff7d76fc,
    0xffd4524d,
    0xff42ebf5,
    0xfffc5554,
    0xffff7978,
    0xffd4c154,
    0xffe6ce80,
    0xff21b03b,
    0xffc95bba,
    0xffcccccc,
    0xffffffff
};

void tms9918_init(tms9918_context_t *context)
{
    memset(context, 0, sizeof(tms9918_context_t));
    context->lastRefresh = 60;
}

void tms9918_register_interrupt_func(tms9918_context_t *context, interrupt_func_t func)
{
    context->interrupt_func = func;
}

#define MODE(context) (((context->registers[1] & 0x10) >> 4) | ((context->registers[1] & 0x08) >> 1) | (context->registers[0] & 0x02))
#define GET_BLANK(context) ((context->registers[1] & 0x40) != 0x40)
#define GET_SPRITE_ATTR_TABLE(context) ((context->registers[5] & 0x7f) << 7)
#define GET_SPRITE_GEN_TABLE(context) ((context->registers[6] & 0x7) << 11)
#define GET_COLOR_TABLE(context) (MODE(context) == 2 ? ((context->registers[3] & 0x80) << 6) : (context->registers[3] << 6))
#define GET_PATTERN_GEN_TABLE(context) (MODE(context) == 2 ? ((context->registers[4] & 4) << 11) : ((context->registers[4] & 7) << 11))
#define GET_PATTERN_NAME_TABLE(context) ((context->registers[2] & 0xf) << 10)
#define GET_TEXT_COLOR(conext) ((context->registers[7]) >> 4)
#define GET_BACKDROP_COLOR(context) ((context->registers[7]) & 0xf)
#define MAGNIFIED(context) ((context->registers[1] & 1) != 0)
#define SIXTEEN(context) ((context->registers[1] & 2) != 0)
#define GINT(context) ((context->registers[1] & 0x20) != 0)

void __not_in_flash_func(tms9918_write)(tms9918_context_t *context, bool mode, uint8_t value)
{
    if (mode)
    {
        if (!(context->hasLatchedData))
        {
            context->latchedData = value;
            context->hasLatchedData = true;
        }
        else
        {
            context->hasLatchedData = false;
            if (value & 0x80)
            {
                context->registers[value & 0x7] = context->latchedData;
                // Interrupts net aangezet terwijl de F-flag al hangt? Dan de
                // INT-lijn meteen asserteren (niet pas bij de volgende vblank).
                if ((value & 0x7) == 1 && GINT(context) &&
                    (context->vdpStatus & S_INT) && context->interrupt_func)
                    (*context->interrupt_func)();
            }
            else if (value & 0x40)
            {
                // Setup video write address
                context->vramAddress = ((value & 0x3f) << 8) + context->latchedData;
            }
            else
            {
                // Setup video read address (internally the same)
                context->vramAddress = ((value & 0x3f) << 8) + context->latchedData;
            }
        }
    }
    else
    {
        context->hasLatchedData = false;
        context->vram[context->vramAddress] = value;
        context->vramAddress = (context->vramAddress + 1) % 0x4000;
    }
}

uint8_t __not_in_flash_func(tms9918_read)(tms9918_context_t *context, bool mode)
{
    context->hasLatchedData = false;
    if (mode)
    {
        uint8_t value = context->vdpStatus;
        context->vdpStatus = 0;
        return value;
    }
    else
    {
        uint8_t value = context->vram[context->vramAddress];
        context->vramAddress = (context->vramAddress + 1) % 16384;
        return value;
    }
}

// Sprite-Y (255 = lijn 0) naar schermlijn van de bovenrand.
static inline int sprite_screen_y(uint8_t yraw)
{
    return (yraw > 238) ? ((int)yraw - 255) : ((int)yraw + 1);
}

// Statusvlaggen van de sprite-engine (collisie + 5e sprite) één keer per frame
// berekenen op de LIVE VRAM. De renderers draaien op een snapshot (core 1) en
// kunnen dus niet in het echte statusregister terugschrijven.
//  - 5S + nummer: de eerste sprite die als vijfde op één lijn valt.
//  - C: twee weergegeven sprites (eerste 4 per lijn) met overlappende
//    patroonbits. Collisie is patroon-gebaseerd: een kleur-0-sprite is
//    onzichtbaar maar botst gewoon (zo gebruiken games ze als hitbox).
static void update_sprite_status(tms9918_context_t *context)
{
    uint32_t SA = GET_SPRITE_ATTR_TABLE(context);
    uint32_t SG = GET_SPRITE_GEN_TABLE(context);
    int sixteen = SIXTEEN(context), mag = MAGNIFIED(context);
    int H = (sixteen ? 16 : 8) << (mag ? 1 : 0);

    bool have_c = false, have_5s = false;

    for (int ln = 0; ln < 192 && !(have_c && have_5s); ln++) {
        uint64_t m[4] = {0, 0, 0, 0}; // 256-bit dekkingsmasker van deze lijn
        int count = 0;
        for (int s = 0; s < 32; s++) {
            uint8_t yraw = context->vram[SA + (4 * s)];
            if (yraw == 208) break;
            int r = ln - sprite_screen_y(yraw);
            if (r < 0 || r >= H) continue;

            count++;
            if (count == 5) {
                if (!have_5s) {
                    context->vdpStatus = (uint8_t)((context->vdpStatus & ~0x1F) | S_5S | s);
                    have_5s = true;
                }
                break; // 5e en verder: niet zichtbaar, geen collisie
            }
            if (have_c) continue; // alleen nog 5S zoeken

            int xx = context->vram[SA + (4 * s) + 1];
            uint8_t p = context->vram[SA + (4 * s) + 2];
            if (context->vram[SA + (4 * s) + 3] & 0x80) xx -= 32; // early clock

            int rowidx = mag ? (r >> 1) : r;
            uint16_t bits;
            if (sixteen)
                bits = (uint16_t)((context->vram[SG + 8 * (p & 0xFC) + rowidx] << 8) |
                                  context->vram[SG + 8 * (p & 0xFC) + 16 + rowidx]);
            else
                bits = (uint16_t)(context->vram[SG + 8 * p + rowidx] << 8);

            int total = (sixteen ? 16 : 8) << (mag ? 1 : 0);
            for (int j = 0; j < total; j++) {
                int col = mag ? (j >> 1) : j;
                if (!(bits & (0x8000 >> col))) continue;
                int px = xx + j;
                if (px < 0 || px > 255) continue;
                uint64_t bit = 1ull << (px & 63);
                if (m[px >> 6] & bit) {
                    context->vdpStatus |= S_C;
                    have_c = true;
                } else {
                    m[px >> 6] |= bit;
                }
            }
        }
    }
}

void check_and_generate_interrupt(tms9918_context_t *context)
{
#ifndef VDP_NO_SPRITE_STATUS
    if (!GET_BLANK(context) && MODE(context) != 1)
        update_sprite_status(context); // sprites bestaan niet in text mode
#endif

    // De F-flag (VBlank) wordt ELKE frame gezet, ongeacht de interrupt-enable.
    // (Games die met interrupts uit op deze flag pollen hingen anders.)
    context->vdpStatus |= S_INT;

    // De INT-lijn wordt alleen geactiveerd als interrupts aanstaan (R1 bit 5).
    if (GINT(context))
    {
        (*context->interrupt_func)();
    }
}

uint32_t tms9918_get_backdrop_color(tms9918_context_t *context) {
    return palette[GET_BACKDROP_COLOR(context)];
}

static void renderScreen0(tms9918_context_t *context, uint32_t *image)
{
    uint32_t PG = GET_PATTERN_GEN_TABLE(context);
    uint32_t PN = GET_PATTERN_NAME_TABLE(context);
    for (uint32_t y = 0; y < 24; y++)
    {
        for (uint32_t x = 0; x < 40; x++)
        {
            uint32_t index = (y * 40) + x;
            // Get Pattern name
            uint32_t c = context->vram[PN + index];
            // Get Colors from the Color table (kleur 0 = transparent -> backdrop)
            uint32_t bg = palette[GET_BACKDROP_COLOR(context)];
            uint32_t t = GET_TEXT_COLOR(context);
            uint32_t fg = t ? palette[t] : bg;
            for (uint32_t i = 0; i < 8; i++)
            {
                uint32_t p = context->vram[PG + (8 * c) + i];
                for (uint32_t j = 0; j < 6; j++)
                {
                    if (p & (1 << (7 - j)))
                    {
                        image[(256 * ((y * 8) + i) + ((x * 6) + j))] = fg;
                    }
                    else
                    {
                        image[(256 * ((y * 8) + i) + ((x * 6) + j))] = bg;
                    }
                }
            }
        }
    }
}

static void renderScreen1(tms9918_context_t *context, uint32_t *image) {
    uint32_t PG = GET_PATTERN_GEN_TABLE(context);
    uint32_t PN = GET_PATTERN_NAME_TABLE(context);
    uint32_t CT = GET_COLOR_TABLE(context);
    uint32_t bdc = palette[GET_BACKDROP_COLOR(context)]; // kleur 0 = transparent -> backdrop
    for (uint32_t y = 0; y < 24; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            uint32_t index = (y * 32) + x;
            // Get Pattern name
            uint32_t c = context->vram[PN + index];
            // Get Colors from the Color table
            uint32_t color = context->vram[CT + (c >> 3)];
            uint32_t fg = (color >> 4)  ? palette[color >> 4]  : bdc;
            uint32_t bg = (color & 0xf) ? palette[color & 0xf] : bdc;
            for (uint32_t i = 0; i < 8; i++) {
                uint32_t p = context->vram[PG + (8 * c) + i];
                for (uint32_t j = 0; j < 8; j++) {
                    if (p & (1 << (7 - j))) {
                        image[(256 * ((y * 8) + i) + ((x * 8) + j))] = fg;
                    } else {
                        image[(256 * ((y * 8) + i) + ((x * 8) + j))] = bg;
                    }
                }
            }
        }
    }
}

void renderScreen2(tms9918_context_t *context, uint32_t *image) {
    // Zie render_line_screen2: colourMask uit R3, patternMask uit R4, kleur 0
    // (transparent) toont de backdrop. Gelijk aan TsMSX renderScreen2.
    uint32_t PG = GET_PATTERN_GEN_TABLE(context);
    uint32_t PN = GET_PATTERN_NAME_TABLE(context);
    uint32_t CT = GET_COLOR_TABLE(context);
    uint32_t colourMask  = ((context->registers[3] & 0x7f) << 3) | 7;
    uint32_t patternMask = ((context->registers[4] & 3)   << 8) | 0xff;
    uint32_t bdc = palette[GET_BACKDROP_COLOR(context)];
    for (uint32_t y = 0; y < 192; y++) {
        uint32_t third = y >> 6, row = y & 7;
        for (uint32_t x = 0; x < 32; x++) {
            uint32_t charcode = context->vram[PN + ((y >> 3) * 32) + x] + (third << 8);
            uint32_t p   = context->vram[PG + ((charcode & patternMask) << 3) + row];
            uint32_t col = context->vram[CT + ((charcode & colourMask) << 3) + row];
            uint32_t fg = (col >> 4)  ? palette[col >> 4]  : bdc;
            uint32_t bg = (col & 0xf) ? palette[col & 0xf] : bdc;
            uint32_t imgIndex = (256 * y) + (x * 8);
            image[imgIndex + 0] = p & 0x80 ? fg : bg;
            image[imgIndex + 1] = p & 0x40 ? fg : bg;
            image[imgIndex + 2] = p & 0x20 ? fg : bg;
            image[imgIndex + 3] = p & 0x10 ? fg : bg;
            image[imgIndex + 4] = p & 0x08 ? fg : bg;
            image[imgIndex + 5] = p & 0x04 ? fg : bg;
            image[imgIndex + 6] = p & 0x02 ? fg : bg;
            image[imgIndex + 7] = p & 0x01 ? fg : bg;
        }
    }
}

static void render_line_sprites(tms9918_context_t *context, uint32_t *line, int ln);

// Full-frame variant: delegeert per lijn naar render_line_sprites, zodat de
// sprite-regels (4-per-lijn, prioriteit, EC, MAG, transparantie) maar op één
// plek bestaan.
void renderSprites(tms9918_context_t *context, uint32_t *image) {
    for (int ln = 0; ln < 192; ln++)
        render_line_sprites(context, &image[256 * ln], ln);
}

// ---- Per-scanline renderers (embedded: geen volledige framebuffer nodig) ----
// Zelfde logica als de whole-image versies, maar voor één displayregel ln
// (0..191) in een lijnbuffer van 256 pixels.

static void __not_in_flash_func(render_line_screen0)(tms9918_context_t *context, uint32_t *line, int ln)
{
    uint32_t PG = GET_PATTERN_GEN_TABLE(context);
    uint32_t PN = GET_PATTERN_NAME_TABLE(context);
    int y = ln >> 3, i = ln & 7;
    uint32_t bgc = palette[GET_BACKDROP_COLOR(context)];
    uint32_t t = GET_TEXT_COLOR(context);
    uint32_t fgc = t ? palette[t] : bgc; // kleur 0 = transparent -> backdrop
    for (int x = 0; x < 40; x++) {
        uint32_t c = context->vram[PN + (y * 40) + x];
        uint32_t p = context->vram[PG + (8 * c) + i];
        for (int j = 0; j < 6; j++)
            line[(x * 6) + j] = (p & (1 << (7 - j))) ? fgc : bgc;
    }
}

static void __not_in_flash_func(render_line_screen1)(tms9918_context_t *context, uint32_t *line, int ln)
{
    uint32_t PG = GET_PATTERN_GEN_TABLE(context);
    uint32_t PN = GET_PATTERN_NAME_TABLE(context);
    uint32_t CT = GET_COLOR_TABLE(context);
    uint32_t bdc = palette[GET_BACKDROP_COLOR(context)]; // kleur 0 = transparent -> backdrop
    int y = ln >> 3, i = ln & 7;
    for (int x = 0; x < 32; x++) {
        uint32_t c = context->vram[PN + (y * 32) + x];
        uint32_t color = context->vram[CT + (c >> 3)];
        uint32_t fg = (color >> 4)  ? palette[color >> 4]  : bdc;
        uint32_t bg = (color & 0xf) ? palette[color & 0xf] : bdc;
        uint32_t p = context->vram[PG + (8 * c) + i];
        for (int j = 0; j < 8; j++)
            line[(x * 8) + j] = (p & (1 << (7 - j))) ? fg : bg;
    }
}

static void __not_in_flash_func(render_line_screen2)(tms9918_context_t *context, uint32_t *line, int ln)
{
    // Trouwe port van TsMSX renderScreen2: het kleur-tabel-adres wordt via een
    // eigen mask uit register 3 (colourMask) bepaald, het pattern-adres via
    // register 4 (patternMask). Kleur 0 is op de TMS9918 "transparent" en toont
    // de backdrop; TsMSX doet dat via alpha 0 + canvas-achtergrondkleur, hier
    // substitueren we expliciet (voor fg én bg).
    uint32_t PG = GET_PATTERN_GEN_TABLE(context);
    uint32_t PN = GET_PATTERN_NAME_TABLE(context);
    uint32_t CT = GET_COLOR_TABLE(context);
    uint32_t colourMask  = ((context->registers[3] & 0x7f) << 3) | 7;
    uint32_t patternMask = ((context->registers[4] & 3)   << 8) | 0xff;
    uint32_t bdc = palette[GET_BACKDROP_COLOR(context)];
    int third = ln >> 6, row = ln & 7, y = ln >> 3;
    for (int x = 0; x < 32; x++) {
        uint32_t charcode = context->vram[PN + (y * 32) + x] + (third << 8);
        uint32_t p   = context->vram[PG + ((charcode & patternMask) << 3) + row];
        uint32_t col = context->vram[CT + ((charcode & colourMask) << 3) + row];
        uint32_t fg = (col >> 4)  ? palette[col >> 4]  : bdc;
        uint32_t bg = (col & 0xf) ? palette[col & 0xf] : bdc;
        for (int j = 0; j < 8; j++)
            line[(x * 8) + j] = (p & (1 << (7 - j))) ? fg : bg;
    }
}

// Sprites op één displaylijn, volgens de echte TMS9918-regels:
//  - maximaal 4 sprites per lijn (in tabelvolgorde); de rest is onzichtbaar
//  - sprite 0 heeft de HOOGSTE prioriteit: we tekenen de 4 in omgekeerde
//    volgorde zodat lagere nummers bovenop komen
//  - kleur 0 is transparant (telt wél mee voor de lijnlimiet en collisie)
//  - early clock (attr bit 7) schuift de sprite 32 pixels naar LINKS
//  - MAG (R1 bit 0) verdubbelt de sprite in beide richtingen
static void __not_in_flash_func(render_line_sprites)(tms9918_context_t *context, uint32_t *line, int ln)
{
    uint32_t SA = GET_SPRITE_ATTR_TABLE(context);
    uint32_t SG = GET_SPRITE_GEN_TABLE(context);
    int sixteen = SIXTEEN(context), mag = MAGNIFIED(context);
    int H = (sixteen ? 16 : 8) << (mag ? 1 : 0);

    // Verzamel de (max 4) sprites die deze lijn raken, in tabelvolgorde.
    int idx[4], n = 0;
    for (int s = 0; s < 32 && n < 4; s++) {
        uint8_t yraw = context->vram[SA + (4 * s)];
        if (yraw == 208) break; // einde sprite attribute table
        int r = ln - sprite_screen_y(yraw);
        if (r >= 0 && r < H) idx[n++] = s;
    }

    for (int k = n - 1; k >= 0; k--) {
        int s = idx[k];
        uint8_t yraw = context->vram[SA + (4 * s)];
        int xx = context->vram[SA + (4 * s) + 1];
        uint8_t p = context->vram[SA + (4 * s) + 2];
        uint8_t attr = context->vram[SA + (4 * s) + 3];
        uint32_t c = attr & 0xf;
        if (attr & 0x80) xx -= 32; // early clock: 32 pixels naar links
        if (c == 0) continue;      // transparant: niets te tekenen

        int r = ln - sprite_screen_y(yraw);
        int rowidx = mag ? (r >> 1) : r;
        uint16_t bits;
        if (sixteen)
            bits = (uint16_t)((context->vram[SG + 8 * (p & 0xFC) + rowidx] << 8) |
                              context->vram[SG + 8 * (p & 0xFC) + 16 + rowidx]);
        else
            bits = (uint16_t)(context->vram[SG + 8 * p + rowidx] << 8);

        int total = (sixteen ? 16 : 8) << (mag ? 1 : 0);
        for (int j = 0; j < total; j++) {
            int col = mag ? (j >> 1) : j;
            if (!(bits & (0x8000 >> col))) continue;
            int xp = xx + j;
            if (xp >= 0 && xp < 256) line[xp] = palette[c];
        }
    }
}

void __not_in_flash_func(tms9918_render_line)(tms9918_context_t *context, uint32_t *line, int ln)
{
    uint8_t bd = GET_BACKDROP_COLOR(context);
    for (int k = 0; k < 256; k++) line[k] = palette[bd];

    if (GET_BLANK(context)) return;

    switch (MODE(context)) {
    case 1:
        render_line_screen0(context, line, ln);
        break;
    case 0:
        render_line_screen1(context, line, ln);
        render_line_sprites(context, line, ln);
        break;
    case 2:
        render_line_screen2(context, line, ln);
        render_line_sprites(context, line, ln);
        break;
    }
}

void tms9918_render_rgba(tms9918_context_t *context, uint32_t *image)
{
    uint8_t c = GET_BACKDROP_COLOR(context);
    for (uint32_t i = 0; i < (256 * 192); i++)
    {
        image[i] = palette[c];
    }

    if (GET_BLANK(context))
    {
        //  Blank done
    }
    else if (MODE(context) == 1)
    {
        renderScreen0(context, image);
    }
    else if (MODE(context) == 0)
    {
        renderScreen1(context, image);
        renderSprites(context, image);
    }
    else if (MODE(context) == 2)
    {
        renderScreen2(context, image);
        renderSprites(context, image);
    }
}