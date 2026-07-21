# MSX2-port: plan

Doel: MSX2 (V9938) als tweede machineprofiel, eerst op SDL, daarna Pico
(vereist PSRAM op de WeAct — 128KB VRAM + 128KB mapper-RAM past niet meer
naast de framebuffers in 520KB SRAM).

## Bronnen en licenties

- **Structuur/codebasis: msx_rs** (eigen werk, dus vrij te porten) —
  `~/Projects/rust/msx_rs/`. Eerder betrouwbaar gebleken bij de WD2793-port.
  Kanttekening: bevat bekende glitches (o.a. Quarth), vooral rond de
  command-engine-timing t.o.v. de beam — die delen NIET 1-op-1 porten.
- **Primaire spec: de V9938 Technical Data Book** (Yamaha/ASCII, aug 1985).
  Lokaal: `docs/refs/V9938_Technical_Data_Book.pdf` (niet in git); bron:
  https://www.bitsavers.org/pdf/yamaha/Yamaha_V9938_MSX-Video_Technical_Data_Book_Aug85.pdf
  Online naslag: Grauw's MSX Assembly Page, https://map.grauw.nl/resources/video/v9938/v9938.xhtml
  Bij elke twijfel wint de datasheet.
- **Gedragsreferentie: openMSX** (en waar handig fMSX) — raadplegen om
  hardwaregedrag te begrijpen, met nette vermelding in de documentatie.
  NOOIT code overnemen of vertalen: openMSX is GPL-2.0 en fMSX
  non-commercieel; afgeleide code zou de MIT-licentie van BareMSX breken.
  Lezen → begrijpen → zelf formuleren.
- **Kopieerbare codereferentie: MAME `v99x8`**
  (`src/devices/video/v9938.{cpp,h}`, license:BSD-3-Clause, copyright
  Aaron Giles & Nathan Woods) — de enige permissief-gelicentieerde
  complete V9938/V9958. Vertalen/overnemen MAG (BSD-3 is MIT-compatibel);
  bij substantiële overname het BSD-3-notice in het betreffende bestand
  of in de README-credits zetten. Lokaal: `docs/refs/mame/` (niet in git).
  Kanttekening: functioneel accuraat maar niet cycle-exact — voor
  timingkwesties (command-busy vs beam) blijft datasheet + openMSX-
  kruispeiling leidend. Vergelijkingsscan: `docs/V9938-MAME-DIFF.md`.

## Wat waar leeft in msx_rs

- **`src/vdp.rs` (2908 r.)** — CPU-kant van de V9938: 128KB VRAM,
  R0-R46-registeropslag, S0-S9-status, poortprotocol (0x98 data / 0x99
  ctrl+status / 0x9A palette / 0x9B indirect), 17-bit VRAM-adres via R14,
  9-bit palette, command-engine (HMMC/HMMM/HMMV/LMMC/LMCM/LMMM/LMMV/YMMM/
  LINE/SRCH/PSET/POINT — VRAM-effect direct, CE-busy-tijd gemodelleerd à la
  fMSX VdpOpsCnt, 12500/scanline), lijninterrupts (R19/IE1, ack via S1),
  HR-bit-timing (S2 bit 5, 228 T-states/lijn, HBlank vanaf T-state 170).
  LET OP: rendering zit NIET hier maar in de shader.
- **`src/vdp.wgsl` (923 r.)** — de mode-renderers: T1/T2, G1/G2/G3,
  G4 (scr5, 4bpp 256×212), G5 (scr6, 2bpp 512), G6 (scr7, 4bpp 512),
  G7 (scr8, 8bpp), sprite mode 1 én 2 (8/lijn, kleur-per-lijn, CC/IC).
  Per-scanline registersnapshots (R0-R8,R10,R11,R23) voor split-screens.
  → naar C-lijnrenderers vertalen.
- **`src/rtc.rs` (152 r.)** — RP-5C01 RTC, poorten 0xB4/0xB5; de echte
  MSX2-BIOS bewaart bootinstellingen in de CMOS-banken.
- **`src/bus.rs`** — NMS-8245-slotlayout (de ROM-set hebben we al in
  `~/Projects/rust/msx_rs/assets/NMS8245/`):
  - Slot 0: MSX2.ROM (BIOS+BASIC 2.1, 32KB op 0x0000)
  - Slot 1: cartridge
  - Slot 2: FM-PAC/MSX-MUSIC (optioneel, YM2413 op 0x7C/0x7D)
  - Slot 3 (expanded): 3-0 MSX2EXT.ROM (sub-ROM), 3-2 RAM-mapper
    (poorten 0xFC-0xFF, 128/256KB), 3-3 DISK.ROM + WD2793

## Portfases (taken 8-11)

1. **v9938.c core**: VRAM/registers/status/palette/poortprotocol +
   legacy-modes (T1/G1/G2) door de bestaande tms9918-renderers heen.
   Mijlpaal: NMS8245-BIOS boot naar BASIC op SDL.
2. **Bitmap-modes + sprite mode 2** uit vdp.wgsl. Mijlpaal: screen 5-games.
3. **Command-engine**: structuur uit vdp.rs, maar gedrag en timing
   spec-first (datasheet + openMSX-kruispeiling) — hier zitten de bekende
   msx_rs-glitches (Quarth: command-busy versus beam, split-registers,
   page-flip-cadans). Testplan: Quarth als lakmoesproef.
4. **Machine**: mapper-RAM, sub-ROM, RTC, lijn-granulaire emulatielus
   (228 T-states/lijn i.p.v. frame-in-één-keer — nodig voor lijn-IRQs),
   machineprofiel-keuze (MSX1/MSX2 o.b.v. system/-inhoud), menu.
5. **V9938 afronding**: de openstaande punten uit `docs/V9938-MAME-DIFF.md`
   (R#18 set-adjust, interlace/EO-paginawissel, T2-blink, G6/G7-interleave,
   MC/SCREEN 3). Losse, per stuk oppakbare klussen; de eerste twee zijn het
   voorwerk voor fase 6.
6. **V9958/MSX2+** (uitgewerkt onder "Fase 6" hieronder).

## Fase 6: V9958 / MSX2+

MSX2+ = de **V9958-VDP** (superset van de V9938, géén extra VRAM) plus een
**MSX2+-machineprofiel** (andere MAIN/SUB-ROM, meestal ingebouwde FM). Nog
steeds een Z80 — R800/turbo R valt hier NIET onder. MAME's v99x8 dekt beide
chips (incl. YJK), dus dit is vooral vertaal- en integratiewerk.

### Wat al klaarligt

- V9938-core zit functioneel op MAME-niveau voor SCREEN 0-8
  (`docs/V9938-MAME-DIFF.md`); de V9958 is grotendeels additief.
- **R#25-R#27 worden al opgeslagen en gemaskeerd** (v9938.c `reg_mask`:
  0x7F/0x3F/0x07) — de scroll/YJK-registers doen alleen nog niets.
- Mode-decode op één plek (`v_mode`), horizontale offset niet hardcoded.
- MAME als kopieerbare BSD-3-referentie; `mode_graphic4` toont de
  R#25/26/27-scrollogica al kant-en-klaar.

### Voorwerk (uit fase 5)

Fase 5 (V9938-afronding) levert de bouwstenen die MSX2+-software óók raakt;
vóór de V9958-modes zinvol zijn vooral:
- **Interlace / EO-paginawissel** (R#9 IL/EO) — 512×424 en page-flip-trucs.
- **R#18 set-adjust** — beeldcentrering/schud-effecten.
(T2-blink, G6/G7-interleave en MC/SCREEN 3 mogen later; niet blokkerend.)

### VDP-werk (V9958)

a. **Horizontale scroll** — R#26 (byte, 8-px) + R#27 (dot, 0-7) + de
   "mask left 8 pixels"-bit in R#25. Breed gebruikt, relatief goedkoop;
   MAME's mode-renderers laten het exact zien. Eerste zichtbare stap.
b. **YJK-modes** (SCREEN 10/11/12) — de hoofdmoot: Y/J/K-decode
   (19268 kleuren), nieuwe renderpaden, YJK+YAE-menging, spritegedrag.
   MAME heeft de decode-lookup kant-en-klaar.
c. **Versie-ID in het statusregister** zodat software de V9958 herkent
   (anders valt ze terug op V9938-gedrag).

### Machine-werk

d. **MSX2+-profiel** — de huidige tweeweg-autodetectie (MSX1 vs MSX2 op
   `msx2*`/`msx2ext*` in `system/`) uitbreiden naar drie profielen, met de
   MSX2+-MAIN/SUB-ROM en de slotlayout van een echte machine (bijv.
   Panasonic FS-A1WX). Bescheiden werk.
e. **Ingebouwde MSX-MUSIC (YM2413/OPLL)** — standaard op MSX2+, nog niet
   aanwezig (we hebben alleen emu2149/PSG). Permissieve core: **emu2413**
   (zelfde auteur als emu2149, MIT). Machine boot er zonder; aparte klus.

### Pico-aandachtspunt

Geen extra VRAM, dus geen nieuwe geheugendruk t.o.v. MSX2-op-Pico (dat
sowieso op PSRAM wacht). Wél: **YJK-decode is per-pixel rekenzwaar** en het
beam-budget is krap (~63 µs/lijn). MAME's decode-tabel kan in flash; of het
binnen het budget past is de enige echte openstaande performancevraag — en
speelt alléén op de Pico, niet op SDL.

### Aanbevolen volgorde

Voorwerk (interlace, R#18) → horizontale scroll (a) → YJK (b) + versie-ID
(c) → MSX2+-profiel/BIOS (d) → OPLL/FM (e, optioneel).

## Architectuurbeslissingen

- `emu/tms9918.c` blijft ONaangeraakt voor het MSX1-profiel; `emu/v9938.c`
  is een apart device met een vergelijkbare context+snapshot-API.
- Emulatielus wordt per-scanline (228 T-states); MSX1-profiel mag op de
  oude frame-lus blijven.
- Output wordt breedte-flexibel: 256 óf 512 pixels breed, 212 lijnen
  (HDMI-kant: 640×480 window, 512→640 met borders; Pico pas in fase 2).
- Per-scanline registersnapshots zoals msx_rs (nodig voor split-scroll,
  Vampire Killer/KV2-achtige effecten); VRAM-snapshot aan het einde van
  de zichtbare scan (niet post-ISR — zie vdp.rs-commentaar bij
  `vram_display` voor het waarom).

## Bekende afhankelijkheden/known issues

- Expanded-slot disk (3-3) ontspoorde met de MSX1-BIOS; msx_rs draait
  disk 3-3 prima met de MSX2-BIOS. Bij fase 4 hertesten; mogelijk lost
  het zichzelf op, anders alsnog de inter-slot-bug vinden.
- Flat-RAM DOS-bootfase (zie machine.c-commentaar) — relevant zodra
  Nextor/DOS2 in beeld komt.
- PSRAM-loading op de WeAct is een voorwaarde voor MSX2-op-Pico.
