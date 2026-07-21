// RP-5C01 real-time clock (I/O-poorten 0xB4/0xB5) — port van msx_rs rtc.rs.
//
// De MSX2-BIOS bewaart zijn bootinstellingen (schermbreedte, kleuren, beep)
// als nibbles in de CMOS-banken 2/3 van deze chip, met een checksum. Zonder
// chip die schrijfacties VASTHOUDT blijft de Philips sub-ROM eeuwig zijn
// defaults herschrijven en her-verifiëren — de boot haalt de scherm-init
// nooit. (C-BIOS raakt de RTC niet aan.)
//
// Registerkaart (per bank 13 nibble-registers):
//   bank 0    — kloktijd (host-klok op reads; writes genegeerd)
//   bank 1    — alarm + modevlaggen (opgeslagen, verder inert)
//   bank 2/3  — CMOS-RAM, het deel dat moet blijven staan
//   reg 13    — mode: bits 0-1 bankselect
//   reg 14/15 — test/reset; lezen als 0xF, writes genegeerd
// Alle reads komen terug met de bovenste vier bits hoog (open bus).

#include "rtc.h"
#include <string.h>
#include <time.h>

void rtc_init(rtc_t *rtc)
{
    memset(rtc, 0, sizeof *rtc);
}

void rtc_select(rtc_t *rtc, uint8_t value) // 0xB4 out
{
    rtc->reg = value & 0x0F;
}

void rtc_write(rtc_t *rtc, uint8_t value) // 0xB5 out
{
    value &= 0x0F;
    if (rtc->reg <= 12)
        rtc->banks[rtc->mode & 0x03][rtc->reg] = value;
    else if (rtc->reg == 13)
        rtc->mode = value;
    // 14/15: test/reset — genegeerd
}

// Bank-0-tijd uit de hostklok. Op de Pico levert time() epoch 0 (1970) op:
// year80 klemt dan op 0 — de BIOS heeft alleen CONSISTENTE reads nodig,
// niet de juiste tijd.
static uint8_t host_time_nibble(uint8_t reg)
{
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t); // MinGW/MSVC hebben geen gmtime_r
#else
    gmtime_r(&t, &tmv);
#endif
    unsigned year80 = tmv.tm_year >= 80 ? (unsigned)(tmv.tm_year - 80) : 0;
    switch (reg) {
    case 0:  return (uint8_t)(tmv.tm_sec % 10);
    case 1:  return (uint8_t)(tmv.tm_sec / 10);
    case 2:  return (uint8_t)(tmv.tm_min % 10);
    case 3:  return (uint8_t)(tmv.tm_min / 10);
    case 4:  return (uint8_t)(tmv.tm_hour % 10);
    case 5:  return (uint8_t)(tmv.tm_hour / 10);
    case 6:  return (uint8_t)tmv.tm_wday;
    case 7:  return (uint8_t)(tmv.tm_mday % 10);
    case 8:  return (uint8_t)(tmv.tm_mday / 10);
    case 9:  return (uint8_t)((tmv.tm_mon + 1) % 10);
    case 10: return (uint8_t)((tmv.tm_mon + 1) / 10);
    case 11: return (uint8_t)(year80 % 10);
    case 12: return (uint8_t)((year80 / 10) % 10);
    default: return 0x0F;
    }
}

uint8_t rtc_read(rtc_t *rtc) // 0xB5 in
{
    uint8_t nibble;
    if (rtc->reg == 13)
        nibble = rtc->mode;
    else if (rtc->reg >= 14)
        nibble = 0x0F;
    else {
        int bank = rtc->mode & 0x03;
        nibble = (bank == 0) ? host_time_nibble(rtc->reg)
                             : rtc->banks[bank][rtc->reg];
    }
    return (uint8_t)(nibble | 0xF0);
}
