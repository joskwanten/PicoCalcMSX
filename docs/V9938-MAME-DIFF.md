# V9938-scan: verschillen t.o.v. MAME's v99x8

Systematische vergelijking van `emu/v9938.c` met MAME's `v99x8`-device
(`docs/refs/mame/v9938.{cpp,h}`, master van 2026-07-21, BSD-3-Clause —
zie MSX2-PORT-PLAN.md §bronnen: vertalen mag, mét attributie). MAME is
functioneel accuraat maar niet cycle-exact; bij timingtwijfel blijft de
datasheet + openMSX-kruispeiling leidend. Regelnummers: links onze code,
rechts MAME.

Drie onafhankelijke deelscans (command-engine; rendering/sprites;
registers/status/timing) — bevindingen die in meerdere scans opdoken
staan één keer, met de sterkste onderbouwing.

**Status (2026-07-21):** alle C-bevindingen (incl. C3: het MAME-budgetmodel,
13662 eenheden/scanline met de timingtabellen per commando/mode), alle
T-bevindingen en R1-R8/R10-R12 zijn doorgevoerd. Sprite mode 1+2 zijn
herschreven naar MAME-semantiek (S0-statusbits, CC-gating, kleurtabel-
mirroring, TP, Y-wrap 208/216). Regressietests: `tests/v9938_cmd_test.c`
(17 stuks, standalone, geen BIOS nodig).
Nog open: R9 (R18 set-adjust), R13 (G6/G7 VRAM-interleave) en de
ontbrekende features (T2-blink, interlace/EO-paginawissel, MC/screen 3).

**Emulatie-specifiek (geen MAME-diff):** onze lijnlus draait de Z80 vóór de
VDP-lijnevents. Daardoor miste een IE1-aanzet-write de FH-wis die de
hardware aan het begin van diezelfde (niet-matchende) lijn al gedaan had —
een in vblank geparkeerde FH vuurde dan als spookinterrupt zodra de
vblank-ISR IE1 aanzette (Quarth: heel frame uit de HUD-pagina = sporadisch
rommel-frame). Fix: de gemiste wis wordt op de IE1-flank ingehaald
(reg_write case 0, met beam_line-context). Gevonden met de nieuwe
`--glitch`/`--trace`-vlaggen van de SDL-frontend.

## Quarth-shortlist

Quarth (SCREEN 5) scrollt het speelveld met HMMM/HMMV, gebruikt
lijn-IRQ/S2-polling voor de split en pollt CE intensief. De verdachten,
in volgorde van waarschijnlijkheid:

1. **Lijn-IRQ negeert R#23** (T1) — split vuurt op de verkeerde beamlijn
   zodra er gescrold wordt.
2. **Command-wrap i.p.v. rand-clip** (C1) — blits over de schermrand
   corrumperen de andere kant van de rij.
3. **Geen register-terugschrijf na commando's** (C4) — opeenvolgende
   blits zonder her-set van DY/SY landen op verkeerde rijen.
4. **FH blijft hangen bij polling zonder IE1** (T2) — split triggert
   sporadisch te vroeg.
5. **CE-gedrag: instant klaar + ce_hold-artefacten** (C2/C3) — pollende
   loops zien "klaar" terwijl de engine op echte hardware nog loopt.
6. **INT-lijn niet herrekend bij IE-writes** (T3) — hangende of één
   frame vertraagde interrupts.

## Command-engine (C)

**C1. Geen X/Y-randclipping: wrap i.p.v. afbreken.**
Ons: blok-lussen maskeren X met `& (width-1)`, Y met `& 0x3FF`
(v9938.c:350, 361-362, 374, 383, 393-394, 405; cmd_cpu_step :487-517).
MAME: rij eindigt zodra X de rand kruist (`post__x_y`-macro's,
v9938.cpp:2159-2191); commando breekt af als SY/DY bij DIY=-1 onder 0
zakt (:2161, 2173, 2183); LINE clipt idem (:2462, 2472).
Gevolg: pixelvuil aan de andere schermkant bij rand-overschrijdende
blits. Fix: rand-terminatie per rij + verticale abort, conform MAME.

**C2. CE-vlag: S2-read wist CE tijdens lopende CPU-transfer; STOP laat
hem juist 4 reads hangen.**
Ons: S2-read herberekent CE alleen uit `ce_hold` (v9938.c:198-203) en
maskeert de CE weg die cmd_start zette (:466, 473) terwijl `cm` nog
actief is; STOP loopt via cmd_done en zet ook `ce_hold=4` (:335, 457).
MAME: CE hoog bij start (v9938.cpp:3077), laag pas bij voltooiing
(:2421, 2505, 2648, 2883) of direct bij ABRT (:2981-2984).
Fix: `dyn |= S2_CE` zolang `cm != 0`; in het STOP-pad `ce_hold=0`.
Vervalt grotendeels zodra C3 er is.

**C3. Geen busy-duur-model — MAME's timingmodel is direct overneembaar.**
Ons: commando's voeren synchroon uit bij de R46-write; CE daarna 4
S2-reads hoog (`ce_hold`, v9938.c:327-336).
MAME: budgetteller `m_vdp_ops_count`, één keer per scanline bijgevuld
naar 13662 eenheden (`update_command`, v9938.cpp:3089-3101, aangeroepen
uit :179); elke engine-iteratie (pixel voor L-ops, byte voor H-ops)
kost `delta` uit een tabel per commando/mode (:2202-2229 — bv. HMMV 439,
HMMM 818, LMMM 1160; index = scherm-aan | sprites-uit | PAL, :2382-2385);
lus stopt als budget op is, rest gaat verder op volgende scanlines
(:2153-2154). HMMV ≈ 24 bytes/scanline.
Gevolg bij ons: race-the-beam-gedrag en halve-blit-frames kloppen niet.
Fix: commandostate hebben we al (cwx/cwy); geef `v9938_scanline` het
budget en trek tabelkosten af. `ce_hold` kan dan weg.

**C4. Register-terugschrijf na commando's ontbreekt; SRCH zet S8/S9
alleen bij hit.**
Ons: cmd_run_vram/cmd_done raken R32-R45 nooit (v9938.c:327-444);
SRCH-S8/S9 alleen in het hit-pad (:426-427).
MAME: bij voltooiing gaan DY→R38/39, NY→R42/43, en voor MM-commando's
ook SY→R34/35 terug (v9938.cpp:2556-2561, 2612-2624, 2682-2687,
2734-2739, 2789-2801, 2849-2861, 2885-2890; LINE :2507-2508); SRCH
schrijft S8/S9 áltijd (:2423-2425).
Gevolg: software die op auto-advance van DY/SY vertrouwt blit alles op
dezelfde rij. Fix: MAME's terugschrijfset overnemen.

**C5. R44-kleur niet per mode gemaskeerd bij dot-ops.**
Ons: vdp_pset OR/XOR't met ongemaskeerde kleur (v9938.c:282-304) —
lekt bits in de buurnibble; transparantietest (:285) idem.
MAME: `R44 &= Mask[mode]` bij start (v9938.cpp:2970-2972, Mask
{0x0F,0x03,0x0F,0xFF} :2196); LMMC maskeert per byte (:2673).
Fix: kleur maskeren in cmd_start (en in het LMMC-pad).

**C6. NX 9-bit gelatcht i.p.v. 10-bit; NX=0 is "tot de rand", niet 512.**
Ons: R41 `& 1` (v9938.c:316), `blk_nx` 0→512 (:324).
MAME: NX & 1023 (v9938.cpp:3057); NX=0 loopt via pre-decrement tot de
X-rand van de rij (:2160-2168), niet 512 pixels.
Fix: `& 0x3FF` latchen; met C1's rand-terminatie is NX=0 vanzelf goed.

**C7. Commando's draaien in niet-bitmap-modes (met G7-geometrie).**
Ons: cmd_layout valt terug op G7 (v9938.c:260), geen mode-check in
cmd_start (:447-478). MAME: buiten G4-G7 complete no-op
(v9938.cpp:2963-2965). Fix: vroege return, CE laag.

**C8. LINE-afronding: accumulator start op 0 i.p.v. (NX-1)/2.**
Ons: v9938.c:403-414. MAME: `ASX=(NX-1)>>1`, aftrek-variant
(v9938.cpp:3062, 2454-2474). Gevolg: minor-stap kan één pixel
verschoven vallen. Fix: MAME's initialisatie overnemen.

**C9. POINT schrijft R44 niet; LMCM-startpixel met ongemaskeerde X.**
Ons: POINT alleen S7 (v9938.c:438); LMCM-start zonder breedtemasker
(:474). MAME: POINT zet S7 én R44 (v9938.cpp:2988-2991); VRMP-macro's
maskeren X altijd (:2122-2127). Fix: beide overnemen.

**C10. Restpunten command-engine.**
- Ongeldige logische ops (5/6/7, 13/14/15): MAME schrijft niet
  (v9938.cpp:2322-2323); ons default = IMP (v9938.c:301).
- SRCH wist BD al bij start (v9938.c:420); MAME pas bij rand-zonder-hit
  (v9938.cpp:2406).
- YMMM met oneven DX, DIX=1, G4/G6: onze bytetelling één te laag
  (v9938.c:371 vs v9938.cpp:2172).
- MXS/MXD (R45 bits 4/5) genegeerd (v9938.c:318-319): commando's naar
  expansie-RAM horen bij ons no-op/0xFF te zijn (v9938.cpp:3047-3048).

## Registers, status, IRQ, timing (T)

**T1. Lijn-IRQ-vergelijking negeert R#23.** (⭐ Quarth)
Ons: `line == regs[19]` (v9938.c:535). MAME:
`((scanline + R23) & 255) == R19` (v9938.cpp:192-197).
Fix: `((line + ctx->regs[23]) & 0xFF) == ctx->regs[19]`.

**T2. FH wordt niet gewist als IE1 uit staat.** (⭐ Quarth)
Ons: FH alleen gewist door S1-read (v9938.c:188-191, 536-537).
MAME: elke niet-matchende lijn wist FH zodra IE1=0 (v9938.cpp:198-201);
met IE1 aan blijft hij gelatcht tot de read.
Gevolg: pollende software ziet een stokoude FH → split te vroeg.
Fix: in v9938_scanline FH wissen op niet-matchende lijnen bij IE1=0.

**T3. INT-lijn niet herrekend bij R#0/R#1-writes.**
Ons: alleen de IE0-aan-flank in case 1 (v9938.c:129-134); geen case 0;
uitschakelen deassert niets (host herrekent alleen na statusread,
machine.c:123-129). MAME: elke R0/R1-write → `check_int()` dat het
niveau volledig herrekent uit (IE0&&F)||(IE1&&FH) (v9938.cpp:789-816,
851-857).
Gevolg: IE1-aan met pending FH → IRQ één frame te laat; IE-uit via
registerwrite → INT blijft hangen (ISR-storm). Fix: volledige
herberekening bij n==0/1.

**T4. Sprite-statusbits ontbreken volledig; S0-ack wist te veel.**
Ons: C/5S/spritenummer worden nergens gezet (S0 alleen :548); S0-read
doet `status[0] = 0` (v9938.c:185-186); S3-S6 blijven 0 (:79).
MAME: collisie/5S/nummer in beide sprite-renderers (v9938.cpp:1587-1588,
1627, 1669-1670, 1710-1711, 1776, 1798-1799); S0-read `&= 0x1f` (:457);
reset zet S4=0xFE, S6=0xFC (:707-708 — detectieroutines checken die
vaste bits).
Gevolg: botsingsdetectie via S0.C faalt in élk spel dat hem gebruikt.
Fix: zetten in de spriterenderers (lijn-granulair volstaat), ack
`&= 0x1F`, vaste bits initialiseren.

**T5. R#9 PAL-bit genegeerd — altijd 262 lijnen/60 Hz.**
Ons: R9 alleen bit 7 (v9938.c:533); machine.c:366 loopt hard 262.
MAME: R9 bit 1 → 313 lijnen/50 Hz (v9938.cpp:220-226, 252-271).
Gevolg: PAL-software (vrijwel alles Europees) draait ~20% te snel.
Fix: lijnenaantal uit R9 bit 1 in machine_do_cycles.

**T6. S2: vaste bits (2/3 altijd 1) en EO-veldtoggle ontbreken.**
Ons: init `status[2] = S2_TR` (v9938.c:90). MAME: reset 0x0C
(v9938.cpp:689), bit 1 toggelt per frame (:2075), interlace leest hem
(:913-915). Fix: vaste bits + toggle in vblank.

**T7. Adres-carry draagt niet door naar R#14.**
Ons: addr_inc verhoogt alleen het interne 17-bit adres (v9938.c:45-51);
volgende setup leest stale R14 (:168). MAME: 14-bit latch met carry naar
R14 (v9938.cpp:433-437, 559-563).
Gevolg: >16KB streamen en dan alleen lage adresbytes her-setten landt
16KB te laag. Fix: carry terugschrijven of MAME's compositie overnemen.

**T8. Registerwrite-gate: tweede byte met bit 6 gezet moet genegeerd.**
Ons: `if (v & 0x80)` (v9938.c:163-164). MAME: `0x80 && !0x40`
(v9938.cpp:570-573). Fix: extra gate.

**T9. Expansion-RAM (R#45 MXC) genegeerd op de datapoort.**
Ons: geen R45-check (v9938.c:100-114). MAME: MXC → aparte 64K of 0xFF
(v9938.cpp:418-427, 547-552). Fix: writes negeren, reads 0xFF.

**T10. Registermaskers (laag risico).**
MAME maskeert R0-R27 met `reg_mask[]` (v9938.cpp:826-838); ons schrijft
raw (v9938.c:124). Meestal onschadelijk; optioneel overnemen. Onze
palette-latch-reset op R#16 komt overeen met openMSX (MAME doet het op
R#15 — vermoedelijk MAME-eigenaardigheid, niet overnemen).

## Rendering en sprites (R)

**R1. G3 (screen 4): achtergrond negeert R#23, sprites niet.**
Ons: render_g2 gebruikt `ln` direct (v9938.c:638-658, aangeroepen :892);
sprites trekken vscroll wél af (:800, 809). MAME: scrolled_y overal
(v9938.cpp:1144, 1169; sprites :1698).
Gevolg: achtergrond staat stil, sprites verspringen. Fix: rij uit
`(ln + R23) & 0xFF` incl. het derde-deel (`(y & 0xC0) << 2`).

**R2. G7-sprites: vast hardwarepalet niet gebruikt.**
Ons: sprites via het CPU-palet, met TODO (v9938.c:909-919). MAME: vaste
tabel g7_ind16 (GRB 3:3:3), paletonafhankelijk (v9938.cpp:1527-1549).
Fix: const-tabel naar GGGRRRBB/565.

**R3. G5 (screen 6): kleur 0 en border zijn TWEE 2-bit kleuren uit R#7.**
Ons: één 4-bit index `R7 & 0x0F` (v9938.c:712-715, 731-744, 873,
567-570). MAME: even pixel `(R7>>2)&3`, oneven `R7&3` (v9938.cpp:
1276-1297; border :939-948). Fix: dither-paar per 512-halfpixel.

**R4. G5-sprites: 4-bit kleur = twee 2-bit halfpixels.**
Ons: één index gedupliceerd (v9938.c:886-888). MAME: v9938.cpp:1507-1523.

**R5. CC=1-sprites vóór de eerste CC=0-sprite moeten verborgen.**
Ons: or-keten tekent ze zelfstandig (v9938.c:817-859). MAME:
first-cc-seen-regel (v9938.cpp:1720-1728). Gevolg: kleurspook-lagen.
Let op: onze lus loopt hoog→laag; bepaal het vooraf in pas 1.

**R6. Sprite-Y-wrap: drempel 216 (m2) / 208 (m1), grootte-onafhankelijk.**
Ons: drempel afhankelijk van patroonhoogte, off-by-one (v9938.c:810,
827). MAME: `if (y > 216) y = -(~y & 255)` (v9938.cpp:1698-1702; m1 208
:1575-1579). Gevolg: sprites die van boven binnenkomen verdwijnen.

**R7. Sprite-kleurtabel: R#5-mirroring en indexmasker.**
Ons: `SC = SA - 0x200` onvoorwaardelijk, lookup zonder masker
(v9938.c:795-796, 831). MAME: `((R5 & 0xF8) << 7) | (R11 << 15)` en
`colourmask = ((R5 & 3) << 3) | 7` (v9938.cpp:1684-1686, 1718).

**R8. Kleur-0-sprites met TP-bit (R#8 bit 5) moeten zichtbaar zijn.**
Ons: overgeslagen (v9938.c:689, 834). MAME: `(c & 15) || (R8 & 0x20)`
(v9938.cpp:1631, 1750).

**R9. R#18 set-adjust volledig genegeerd.**
MAME: offset_x/offset_y uit R18 (v9938.cpp:228, 859-865). Gevolg: geen
schud-effecten, beeld tot ±7px verschoven.

**R10. T1/T2: rijen ≥24 geblankt bij 212 lijnen.**
Ons: `if (y >= 24) return` (v9938.c:613, 778). MAME rendert 26,5 rijen
door (v9938.cpp:963-981, 1009). Gevolg: onderste tekstrijen weg.

**R11. Tabel-mirroring (verplichte-bits-laag) ontbreekt in G4-G7/T2.**
Ons: alleen R2-bits 6:5 (v9938.c:719, 733, 749, 763; T2 :771).
MAME: linemask `((R2 & 0x1F) << 3) | 7` op de gescrolde Y
(v9938.cpp:1202-1203, 1268-1270, 1313-1315, 1361-1363); T2 patternmask
(:1002).

**R12. G7-blank/border: R7 is een ruwe GRB332-byte, geen paletindex.**
Ons: memset met `R7 & 0x0F` ook in G7 (v9938.c:872-875, 567-570).
MAME: pen256(R7) (v9938.cpp:929-937).

**R13. G6/G7 VRAM-interleave ontbreekt (CPU-poort, commando's, renderer).**
Ons: overal lineair (v9938.c:100-114, 259-260, 264-267, 747-766) —
intern consistent, dus single-mode-software werkt. MAME: fysiek
geïnterleaved: poort `((A&1)<<16)|(A>>1)` (v9938.cpp:764-787), commando's
VDP_VRMP7/8 (:2125-2127), tabellen idem (:1330, 1444).
Gevolg: alleen zichtbaar bij mode-switch-trucs (schrijven in de ene
mode-familie, tonen in de andere). Structurele keuze: interleaved opslaan
(MAME-stijl) of vertalen op de poort — één keer beslissen, overal
doorvoeren.

## Volledig ontbrekend

- **T2-blink** (R#12/R#13 + blinkkleurtabel via R#3/R#10): MAME
  v9938.cpp:995-1036 + blinkteller :2077-2095. Raakt 80-koloms software
  (MSX-DOS2, tekstverwerkers).
- **Interlace / EO-paginawissel** (R#9 IL/EO + R#13-alternatie in
  bitmapmodes): MAME :913-916 en de veldafhankelijke +0x8000/+0x10000
  in mode_graphic4-7 (:1208, 1273, 1318, 1366). Raakt 512×424-beelden
  en flikker-transparantie-demos.
- **MC / screen 3**: onze default-tak rendert backdrop (v9938.c:902-905);
  MAME mode_multi :1055-1099.
- **R#23 in T1/T2/G1/G2 en sprite mode 1**: patroonmodes en m1-sprites
  negeren verticale scroll (v9938.c:605-706, 768-784); MAME past hem
  overal toe (:973, 1024, 1064, 1111, 1575).

## Aanpak (voorstel)

Quick wins eerst (klein, groot effect): T1, T2, T3, C2-quickfix, C7,
T8 — daarna de Quarth-cluster afmaken met C1+C6 (rand-terminatie) en C4
(terugschrijf), en C3 (busy-timingmodel) als de scanline-lus van
portfase 4 er is. T4 (sprite-status) is klein en raakt veel spellen.
Rendering-punten (R1-R12) kunnen per stuk, met de headless
`--frames/--dump`-harness als regressietest. R13 (interleave) en de
ontbrekende features zijn aparte, grotere klussen.
