// Regressietests voor de V9938 command-engine en IRQ/status-semantiek.
// Standalone (geen SDL/BIOS nodig):
//   gcc -I emu -I sdl/compat tests/v9938_cmd_test.c emu/v9938.c -o v9938_test
// Verifieert de MAME-geijkte semantiek uit docs/V9938-MAME-DIFF.md:
// rand-terminatie (geen wrap), NX=0 = "tot de rand", DIY=-1-abort,
// register-terugschrijf, R44-masking, SRCH S8/S9-altijd, CE-gedrag,
// lijn-IRQ met R23, FH-wis bij IE1=0.

#include "v9938.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t vram[V9938_VRAM_SIZE];
static v9938_context_t ctx;
static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void wreg(int n, uint8_t v)
{
    v9938_write_ctrl(&ctx, v);
    v9938_write_ctrl(&ctx, (uint8_t)(0x80 | n));
}

static uint8_t rstat(int s)
{
    wreg(15, (uint8_t)s);
    return v9938_read_status(&ctx);
}

// SCREEN 5 (G4): M3=1 via R0, scherm aan via R1.
static void setup_g4(void)
{
    v9938_init(&ctx, vram);
    wreg(0, 0x06); // M3/M4 -> mode 0x0C (G4)
    wreg(1, 0x40); // scherm aan
}

// Blokcommando's zijn sinds het busy-timingmodel niet meer instant klaar:
// pomp scanlines (budget-refills) tot de engine idle is.
static void pump(void)
{
    for (int i = 0; ctx.cm != 0 && i < 5000; i++)
        v9938_scanline(&ctx, 300);
}

static void cmd(uint8_t sx_lo, uint8_t sx_hi, uint8_t sy_lo, uint8_t sy_hi,
                uint8_t dx_lo, uint8_t dx_hi, uint8_t dy_lo, uint8_t dy_hi,
                uint8_t nx_lo, uint8_t nx_hi, uint8_t ny_lo, uint8_t ny_hi,
                uint8_t clr, uint8_t arg, uint8_t op)
{
    wreg(32, sx_lo); wreg(33, sx_hi); wreg(34, sy_lo); wreg(35, sy_hi);
    wreg(36, dx_lo); wreg(37, dx_hi); wreg(38, dy_lo); wreg(39, dy_hi);
    wreg(40, nx_lo); wreg(41, nx_hi); wreg(42, ny_lo); wreg(43, ny_hi);
    wreg(44, clr); wreg(45, arg); wreg(46, op);
    uint8_t c = op >> 4;
    if ((c >= 0x6 && c <= 0x9) || (c >= 0xC && c <= 0xE))
        pump(); // blokcommando afmaken; CPU-transfers blijven event-gedreven
}

// C1: HMMV over de rechterrand clipt (geen wrap naar de linkerkant).
static void test_hmmv_edge_clip(void)
{
    setup_g4();
    cmd(0,0, 0,0, 240,0, 10,0, 32,0, 2,0, 0xAA, 0x00, 0xC0);
    CHECK(vram[10 * 128 + 120] == 0xAA, "byte binnen blok niet geschreven");
    CHECK(vram[10 * 128 + 127] == 0xAA, "laatste byte voor de rand niet geschreven");
    CHECK(vram[10 * 128 + 0] == 0x00, "wrap: byte aan linkerkant overschreven");
    CHECK(vram[11 * 128 + 127] == 0xAA, "tweede rij niet geschreven");
    CHECK(vram[12 * 128 + 120] == 0x00, "derde rij geschreven (NY=2)");
    // C4: terugschrijf DY=12 (10 + 2 rijen), NY=0.
    CHECK(ctx.regs[38] == 12 && ctx.regs[39] == 0, "R38/39 na HMMV: %d", ctx.regs[38] | (ctx.regs[39] << 8));
    CHECK(ctx.regs[42] == 0 && ctx.regs[43] == 0, "R42/43 na HMMV niet 0");
}

// C6: NX=0 betekent "tot de rand", niet 512.
static void test_hmmv_nx0(void)
{
    setup_g4();
    cmd(0,0, 0,0, 100,0, 20,0, 0,0, 1,0, 0x55, 0x00, 0xC0);
    CHECK(vram[20 * 128 + 50] == 0x55, "NX=0: startbyte niet geschreven");
    CHECK(vram[20 * 128 + 127] == 0x55, "NX=0: liep niet tot de rand");
    CHECK(vram[20 * 128 + 49] == 0x00, "NX=0: voor DX geschreven");
    CHECK(vram[21 * 128 + 50] == 0x00, "NX=0: tweede rij geschreven (NY=1)");
}

// C1: DIY=-1 voorbij rij 0 breekt het commando af (geen wrap naar onderen).
static void test_hmmv_diy_abort(void)
{
    setup_g4();
    cmd(0,0, 0,0, 0,0, 1,0, 16,0, 10,0, 0x77, 0x08, 0xC0); // DIY=-1, DY=1, NY=10
    CHECK(vram[1 * 128 + 0] == 0x77, "rij 1 niet geschreven");
    CHECK(vram[0 * 128 + 0] == 0x77, "rij 0 niet geschreven");
    CHECK(vram[1023 * 128 + 0] == 0x00, "wrap naar rij 1023");
    CHECK(ctx.regs[38] == 0xFF && ctx.regs[39] == 3, "R38/39 na abort niet -1 (was %d/%d)", ctx.regs[38], ctx.regs[39]);
}

// C5: dot-commando's masken R44 op de pixelbreedte (G4: 4 bpp).
static void test_lmmv_color_mask(void)
{
    setup_g4();
    cmd(0,0, 0,0, 8,0, 30,0, 4,0, 1,0, 0x1A, 0x00, 0x80); // LMMV, kleur 0x1A
    CHECK(ctx.regs[44] == 0x0A, "R44 niet gemaskt: %02X", ctx.regs[44]);
    CHECK(vram[30 * 128 + 4] == 0xAA, "LMMV-pixels fout: %02X", vram[30 * 128 + 4]);
}

// C4/C10: SRCH schrijft S8/S9 ook zonder hit; BD alleen bij hit.
static void test_srch(void)
{
    setup_g4();
    // Geen hit: kleur 5 bestaat niet op rij 40.
    cmd(10,0, 40,0, 0,0, 0,0, 0,0, 0,0, 0x05, 0x00, 0x60);
    CHECK(!(rstat(2) & 0x10), "BD gezet zonder hit");
    CHECK(rstat(8) == 0 && rstat(9) == 0xFF, "S8/S9 zonder hit: %d/%02X", rstat(8), rstat(9));
    // Hit op x=200 (byte 100, hoge nibble).
    vram[40 * 128 + 100] = 0x50;
    cmd(10,0, 40,0, 0,0, 0,0, 0,0, 0,0, 0x05, 0x00, 0x60);
    CHECK(rstat(2) & 0x10, "BD niet gezet bij hit");
    CHECK(rstat(8) == 200 && rstat(9) == 0xFE, "S8/S9 bij hit: %d/%02X", rstat(8), rstat(9));
}

// C4: HMMM schrijft SY, DY en NY terug.
static void test_hmmm_writeback(void)
{
    setup_g4();
    memset(vram + 20 * 128, 0x33, 128);
    cmd(0,0, 20,0, 0,0, 50,0, 16,0, 3,0, 0x00, 0x00, 0xD0);
    CHECK(vram[50 * 128 + 0] == 0x33, "HMMM kopieerde niet");
    CHECK(ctx.regs[34] == 23 && ctx.regs[35] == 0, "R34/35 na HMMM: %d", ctx.regs[34]);
    CHECK(ctx.regs[38] == 53 && ctx.regs[39] == 0, "R38/39 na HMMM: %d", ctx.regs[38]);
    CHECK(ctx.regs[42] == 0 && ctx.regs[43] == 0, "R42/43 na HMMM niet 0");
}

// C2: CE hoog tijdens lopende HMMC; STOP maakt hem direct laag.
static void test_ce_transfer(void)
{
    setup_g4();
    cmd(0,0, 0,0, 0,0, 60,0, 16,0, 2,0, 0x11, 0x00, 0xF0); // HMMC start
    CHECK(rstat(2) & 0x01, "CE laag tijdens lopende HMMC");
    CHECK(rstat(2) & 0x01, "CE laag bij tweede read tijdens HMMC");
    wreg(46, 0x00); // STOP
    CHECK(!(rstat(2) & 0x01), "CE hoog direct na STOP");
    // Na een afgerond blokcommando mag CE nog even hoog lijken (ce_hold)...
    cmd(0,0, 0,0, 0,0, 70,0, 8,0, 1,0, 0x22, 0x00, 0xC0);
    CHECK(rstat(2) & 0x01, "ce_hold ontbreekt na blokcommando");
    // ...maar hooguit een paar reads.
    for (int i = 0; i < 8; i++) (void)rstat(2);
    CHECK(!(rstat(2) & 0x01), "CE blijft hangen na blokcommando");
}

// C7: buiten G4-G7 is een commando een no-op.
static void test_cmd_outside_bitmap(void)
{
    v9938_init(&ctx, vram);
    wreg(0, 0x00); wreg(1, 0x40); // G1
    cmd(0,0, 0,0, 0,0, 10,0, 16,0, 2,0, 0xEE, 0x00, 0xC0);
    for (int i = 0; i < V9938_VRAM_SIZE; i++)
        if (vram[i]) { CHECK(0, "commando schreef VRAM in G1 (offset %d)", i); break; }
}

// T1: lijn-IRQ vergelijkt (line + R23) & 255 met R19.
static void test_line_irq_r23(void)
{
    setup_g4();
    wreg(19, 100); wreg(23, 50);
    v9938_scanline(&ctx, 50); // (50+50)&255 == 100 -> FH
    CHECK(rstat(1) & 0x01, "FH niet gezet op gescrolde matchlijn");
    CHECK(!(rstat(1) & 0x01), "S1-read ackte FH niet");
    v9938_scanline(&ctx, 100); // (100+50)&255 = 150 != 100 -> geen FH
    CHECK(!(ctx.status[1] & 0x01), "FH gezet op ongescrolde (verkeerde) lijn");
}

// T2: met IE1 uit wist elke niet-matchende lijn een hangende FH.
static void test_fh_clear_ie1_off(void)
{
    setup_g4();
    wreg(19, 10); wreg(23, 0);
    v9938_scanline(&ctx, 10);
    CHECK(ctx.status[1] & 0x01, "FH niet gezet op matchlijn");
    v9938_scanline(&ctx, 11); // IE1 uit -> wissen
    CHECK(!(ctx.status[1] & 0x01), "FH blijft hangen met IE1 uit");
    wreg(0, 0x06 | 0x10); // IE1 aan
    v9938_scanline(&ctx, 10);
    v9938_scanline(&ctx, 11); // IE1 aan -> gelatcht laten
    CHECK(ctx.status[1] & 0x01, "FH gewist ondanks IE1 aan");
}

// T7: adres-carry over de 16KB-grens komt in R14 terecht.
static void test_r14_carry(void)
{
    setup_g4();
    wreg(14, 0);
    v9938_write_ctrl(&ctx, 0xFE); // A7-A0
    v9938_write_ctrl(&ctx, 0x7F); // schrijf-setup A13-A8 = 0x3F -> adres 0x3FFE
    v9938_write_data(&ctx, 1);    // 0x3FFE
    v9938_write_data(&ctx, 2);    // 0x3FFF
    v9938_write_data(&ctx, 3);    // 0x4000 (carry!)
    CHECK(vram[0x4000] == 3, "write na 16K-grens landde op %02X/%02X", vram[0x4000], vram[0x0000]);
    CHECK(ctx.regs[14] == 1, "R14 kreeg de carry niet: %d", ctx.regs[14]);
}

// T9: MXC (R45 bit 6) stuurt de datapoort naar afwezig expansie-RAM.
static void test_mxc_data_port(void)
{
    setup_g4();
    wreg(45, 0x40);
    v9938_write_ctrl(&ctx, 0x00);
    v9938_write_ctrl(&ctx, 0x40); // schrijf-setup adres 0
    v9938_write_data(&ctx, 0xAB);
    CHECK(vram[0] == 0x00, "MXC-write kwam in VRAM terecht");
    wreg(45, 0x00);
    vram[0x10] = 0x5A;
    v9938_write_ctrl(&ctx, 0x10);
    v9938_write_ctrl(&ctx, 0x00); // lees-setup adres 0x10 (prefetch)
    CHECK(v9938_read_data(&ctx) == 0x5A, "normale read na MXC faalt");
}

// T6: S2-bits 2/3 staan vast op 1; S4/S6 hebben vaste 1-bits (T4).
static void test_fixed_status_bits(void)
{
    setup_g4();
    CHECK((rstat(2) & 0x0C) == 0x0C, "S2 vaste bits 2/3 niet hoog: %02X", rstat(2));
    CHECK(rstat(4) == 0xFE, "S4 niet 0xFE: %02X", rstat(4));
    CHECK(rstat(6) == 0xFC, "S6 niet 0xFC: %02X", rstat(6));
}

// T8: tweede 0x99-byte met bits 7+6 gezet wordt genegeerd.
static void test_ctrl_write_gate(void)
{
    setup_g4();
    uint8_t before = ctx.regs[7];
    v9938_write_ctrl(&ctx, 0x99);
    v9938_write_ctrl(&ctx, 0xC7); // zou "R7 = 0x99" zijn zonder gate
    CHECK(ctx.regs[7] == before, "0xC0-write muteerde R7: %02X", ctx.regs[7]);
}

// T4/R5: sprite mode 2 — collisie zet S0.C, laagste nummer wint.
static void test_sprite_collision_m2(void)
{
    setup_g4();
    wreg(5, 0xFC); wreg(11, 0x00); // SAT = 0x7E00, kleurtabel = 0x7C00
    wreg(6, 0x00);                 // patroontabel = 0x0000
    wreg(8, 0x00);                 // sprites aan
    memset(vram, 0, sizeof vram);
    for (int i = 0; i < 8; i++) vram[i] = 0xFF; // patroon 0: vol blok
    // Sprite 0 en 1 overlappen volledig op x=10, y-lijnen 1-8.
    vram[0x7E00 + 0] = 0; vram[0x7E01] = 10; vram[0x7E02] = 0;
    vram[0x7E04 + 0] = 0; vram[0x7E05] = 10; vram[0x7E06] = 0;
    vram[0x7E08] = 216; // sentinel
    for (int n = 0; n < 8; n++) {
        vram[0x7C00 + 0 * 16 + n] = 5; // sprite 0: kleur 5, CC=0
        vram[0x7C00 + 1 * 16 + n] = 7; // sprite 1: kleur 7, CC=0
    }
    uint32_t line[512];
    v9938_render_line(&ctx, line, 1);
    uint8_t s0 = rstat(0);
    CHECK(s0 & 0x20, "S0.C niet gezet bij sprite-overlap: %02X", s0);
    CHECK(line[2 * 12] == ctx.palette[5], "sprite 0 (laagste nummer) wint niet");
}

// T4: 9e sprite op een lijn zet 5S + het spritenummer.
static void test_sprite_9th_m2(void)
{
    setup_g4();
    wreg(5, 0xFC); wreg(11, 0x00); wreg(6, 0x00); wreg(8, 0x00);
    memset(vram, 0, sizeof vram);
    for (int i = 0; i < 8; i++) vram[i] = 0xFF;
    for (int s = 0; s < 10; s++) { // 10 sprites allemaal op dezelfde lijnen
        vram[0x7E00 + 4 * s] = 0;
        vram[0x7E01 + 4 * s] = (uint8_t)(s * 20);
        vram[0x7E02 + 4 * s] = 0;
        for (int n = 0; n < 8; n++) vram[0x7C00 + s * 16 + n] = 5;
    }
    vram[0x7E00 + 4 * 10] = 216;
    uint32_t line[512];
    v9938_render_line(&ctx, line, 1);
    uint8_t s0 = rstat(0);
    CHECK(s0 & 0x40, "5S niet gezet bij 9 sprites: %02X", s0);
    CHECK((s0 & 0x1F) == 8, "5S-nummer niet 8: %d", s0 & 0x1F);
}

// Quarth-rommel-frame: een FH die met IE1 uit in vblank geparkeerd latcht,
// mag geen spookinterrupt vuren zodra de vblank-ISR IE1 aanzet — de
// hardware had hem aan het begin van de niet-matchende lijn al gewist.
static void test_ie1_enable_stale_fh(void)
{
    setup_g4();
    wreg(19, 214); wreg(23, 0);
    v9938_scanline(&ctx, 214); // matchlijn: FH latcht (IE1 uit)
    CHECK(ctx.status[1] & 0x01, "FH niet gelatcht op matchlijn");
    wreg(0, 0x16); // IE1 aan tijdens lijn 215 (beam_line+1)
    CHECK(!(ctx.status[1] & 0x01), "stale FH overleeft de IE1-flank");
    CHECK(!v9938_irq_asserted(&ctx), "spook-INT bij IE1-flank");
    // Maar op de matchlijn zelf aangezet: FH blijft en INT vuurt.
    wreg(0, 0x06);
    wreg(19, 100);
    v9938_scanline(&ctx, 99); // beam_line=99: "we zitten in lijn 100"
    ctx.status[1] |= 0x01;    // FH van matchlijn 100 (gesimuleerd)
    wreg(0, 0x16);
    CHECK(ctx.status[1] & 0x01, "FH van de actuele matchlijn gewist");
    CHECK(v9938_irq_asserted(&ctx), "INT blijft uit op de matchlijn");
}

// C3: een groot commando blijft realistische tijd busy (MAME-budgetmodel).
static void test_cmd_busy_duration(void)
{
    setup_g4();
    // 200x200-px LMMV rechtstreeks (zonder pump-helper).
    wreg(36, 0); wreg(37, 0); wreg(38, 0); wreg(39, 0);
    wreg(40, 200); wreg(41, 0); wreg(42, 200); wreg(43, 0);
    wreg(44, 9); wreg(45, 0); wreg(46, 0x80);
    CHECK(rstat(2) & 0x01, "CE laag direct na start van grote LMMV");
    for (int i = 0; i < 10; i++) v9938_scanline(&ctx, 300);
    CHECK(rstat(2) & 0x01, "grote LMMV al klaar na 10 scanlines");
    int lines = 10;
    while (ctx.cm != 0 && lines < 5000) { v9938_scanline(&ctx, 300); lines++; }
    CHECK(ctx.cm == 0, "LMMV niet klaar na 5000 lijnen");
    // 40000 px * 1135 eenheden (NTSC, scherm aan) / 13662 = ~3323 lijnen.
    CHECK(lines > 3000 && lines < 3700, "busy-duur onverwacht: %d lijnen", lines);
    CHECK(vram[100 * 128 + 25] == 0x99, "LMMV-vulling incompleet: %02X", vram[100 * 128 + 25]);
}

int main(void)
{
    test_hmmv_edge_clip();
    test_hmmv_nx0();
    test_hmmv_diy_abort();
    test_lmmv_color_mask();
    test_srch();
    test_hmmm_writeback();
    test_ce_transfer();
    test_cmd_outside_bitmap();
    test_line_irq_r23();
    test_fh_clear_ie1_off();
    test_r14_carry();
    test_mxc_data_port();
    test_fixed_status_bits();
    test_ctrl_write_gate();
    test_sprite_collision_m2();
    test_sprite_9th_m2();
    test_ie1_enable_stale_fh();
    test_cmd_busy_duration();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("alle tests OK\n");
    return 0;
}
