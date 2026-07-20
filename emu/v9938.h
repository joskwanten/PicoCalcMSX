#ifndef V9938_H
#define V9938_H

// Yamaha V9938 (MSX2-VDP) — spec-first implementatie tegen de officiële
// "V9938 MSX-VIDEO Technical Data Book" (Yamaha/ASCII, aug 1985; zie
// docs/MSX2-PORT-PLAN.md voor bron en aanpak). Structuur schuin afgekeken
// van onze eigen msx_rs-emulator; gedrag gevalideerd tegen de datasheet en
// waar die zwijgt tegen openMSX (als referentie gelezen, niet overgenomen).
//
// Apart device naast tms9918.c: het MSX1-profiel blijft de TMS9918 gebruiken,
// het MSX2-profiel dit. Zelfde context+snapshot-patroon: core 0 emuleert en
// snapshot, core 1 (of de SDL-lus) rendert per lijn uit de snapshot.

#include <stdint.h>
#include <stdbool.h>

#define V9938_VRAM_SIZE (128 * 1024)

// Displaygeometrie: V9938-modes zijn 212 (of 192) lijnen hoog en 256 of 512
// pixels breed. De lijnrenderer levert altijd V9938_LINE_W pixels; in
// 256-modes wordt elke pixel verdubbeld zodat de host één vaste breedte kent.
#define V9938_LINE_W 512
#define V9938_LINES 212

typedef void (*v9938_irq_func_t)(void);

typedef struct {
    // VRAM (128KB) leeft buiten de context: de host levert 'm aan v9938_init
    // (op de Pico is dit de hergebruikte menu-arena).
    uint8_t *vram;

    // R0-R46 (R24-R31/R47+ bestaan niet; opslag is 64 voor eenvoud).
    uint8_t regs[64];
    // S0-S9, geselecteerd via R15.
    uint8_t status[10];

    // 9-bit palette: 16 entries, rood/blauw uit byte 1, groen uit byte 2
    // (poort 0x9A). We bewaren zowel de ruwe 3-bit kanalen als de
    // voorberekende ARGB-kleur voor de renderers.
    uint16_t palette_raw[16]; // 0b0rrr0bbb00000ggg zoals geschreven (intern)
    uint32_t palette[16];     // ARGB8888
    uint16_t palette565[16];  // RGB565, voorberekend (Pico-renderpad)
    bool palette_first_pending;
    uint8_t palette_first;    // eerste byte van een 0x9A-paar

    // Poortprotocol 0x99: schrijfparen met één-byte-latch.
    bool has_latched;
    uint8_t latched;

    // 17-bit VRAM-adrespointer (A16-A14 uit R14 bij setup) + read-ahead
    // buffer: een leesadres-setup prefetcht meteen (datasheet §4.1).
    uint32_t vram_addr;
    uint8_t read_ahead;

    // Interrupts: frame-IRQ (IE0/S0-F) en lijn-IRQ (IE1/R19, ack via S1).
    bool line_irq_pending;
    v9938_irq_func_t irq_func;

    // Command-engine (R32-R46). VRAM-commando's voeren synchroon uit bij de
    // R46-write; CPU-transfers (HMMC/LMMC/LMCM) blijven actief en verwerken
    // per R44-write / S7-read één eenheid. Parameters worden bij de start
    // gelatcht; cwx/cwy zijn de werk-tellers.
    uint32_t s2_phase;                     // pseudo-beamfase voor S2 VR/HR
    uint8_t ce_hold;                       // CE nog N S2-reads hoog houden
    uint8_t cm, clo;                       // commando (hoge nibble) + log. op
    uint16_t csx, csy, cdx, cdy, cnx, cny; // gelatchte SX/SY/DX/DY/NX/NY
    int8_t cdix, cdiy;                     // richting (+1/-1) uit ARG
    uint16_t cwx, cwy;                     // voortgang binnen het commando
} v9938_context_t;

void v9938_init(v9938_context_t *ctx, uint8_t *vram128k);
void v9938_register_interrupt_func(v9938_context_t *ctx, v9938_irq_func_t f);

// I/O-poorten (MSX: 0x98=data, 0x99=ctrl/status, 0x9A=palette, 0x9B=indirect).
uint8_t v9938_read_data(v9938_context_t *ctx);            // 0x98 in
void v9938_write_data(v9938_context_t *ctx, uint8_t v);   // 0x98 out
uint8_t v9938_read_status(v9938_context_t *ctx);          // 0x99 in (S# = R15)
void v9938_write_ctrl(v9938_context_t *ctx, uint8_t v);   // 0x99 out
void v9938_write_palette(v9938_context_t *ctx, uint8_t v);// 0x9A out
void v9938_write_indirect(v9938_context_t *ctx, uint8_t v);// 0x9B out

// Actuele stand van de INT-lijn (frame- of lijn-IRQ nog niet ge-ackt).
bool v9938_irq_asserted(v9938_context_t *ctx);

// Scanline-hook: aanroepen na elke ~228 T-states met het lijnnummer
// (0..261); regelt FH/VR/S0.F + IRQs. v9938_vblank is de frame-granulaire
// fallback voor aanroepers zonder scanline-lus.
void v9938_scanline(v9938_context_t *ctx, int line);
void v9938_vblank(v9938_context_t *ctx);

// Render één displaylijn (0..V9938_LINES-1) naar een V9938_LINE_W-brede
// ARGB-buffer. 256-pixel-modes worden pixel-verdubbeld.
void v9938_render_line(v9938_context_t *ctx, uint32_t *line, int ln);

// 565-variant voor de Pico: rendert op bronbreedte (retourwaarde 256 of
// 512, geen pixelverdubbeling) rechtstreeks in RGB565.
int v9938_render_line_565(v9938_context_t *ctx, uint16_t *dst, int ln);

uint32_t v9938_backdrop_color(v9938_context_t *ctx);

#endif // V9938_H
