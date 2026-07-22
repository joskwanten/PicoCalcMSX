#include "mapper.h"
#include "pico.h" // __not_in_flash_func

const char *mapper_name(mapper_type_t t)
{
    switch (t) {
    case MAPPER_PLAIN:      return "plain";
    case MAPPER_KONAMI:     return "konami";
    case MAPPER_ASCII8:     return "ascii8";
    case MAPPER_ASCII16:    return "ascii16";
    case MAPPER_KONAMI_SCC: return "konami-scc";
    default:                return "none";
    }
}

void mapper_init(mapper_t *m, const uint8_t *rom, uint32_t size, mapper_type_t type)
{
    m->rom = rom;
    m->size = size;
    m->type = type;
    m->nbanks8 = size / 8192u;
    if (m->nbanks8 == 0) m->nbanks8 = 1;

    // Default 8K bank mapping for the cartridge area (0x4000-0xBFFF = pages 2..5).
    for (int i = 0; i < 8; i++) m->page[i] = 0;
    switch (type) {
    case MAPPER_KONAMI:   // 0x4000 fixed at bank 0; others default to 1,2,3
    case MAPPER_KONAMI_SCC:
        m->page[2] = 0; m->page[3] = 1; m->page[4] = 2; m->page[5] = 3;
        break;
    case MAPPER_ASCII16:  // 16K bank 0 = 8K banks 0,1 in both windows
        m->page[2] = 0; m->page[3] = 1; m->page[4] = 0; m->page[5] = 1;
        break;
    case MAPPER_ASCII8:   // all banks 0 at reset
    default:
        break;
    }
}

uint8_t __not_in_flash_func(mapper_read)(void *context, uint16_t address)
{
    mapper_t *m = (mapper_t *)context;
    if (m->type == MAPPER_PLAIN) {
        uint32_t off = (uint32_t)address - 0x4000u;
        return (off < m->size) ? m->rom[off] : 0xFF;
    }
    uint32_t bank = m->page[(address >> 13) & 7] % m->nbanks8;
    return m->rom[bank * 8192u + (address & 0x1FFF)];
}

void __not_in_flash_func(mapper_write)(void *context, uint16_t address, uint8_t value)
{
    mapper_t *m = (mapper_t *)context;
    switch (m->type) {
    case MAPPER_KONAMI: // 0x6000/0x8000/0xA000 select the 0x6000/0x8000/0xA000 windows
        if (address >= 0x6000 && address < 0x8000) m->page[3] = value;
        else if (address >= 0x8000 && address < 0xA000) m->page[4] = value;
        else if (address >= 0xA000 && address < 0xC000) m->page[5] = value;
        break;
    case MAPPER_ASCII8:
        if (address >= 0x6000 && address < 0x6800) m->page[2] = value;
        else if (address >= 0x6800 && address < 0x7000) m->page[3] = value;
        else if (address >= 0x7000 && address < 0x7800) m->page[4] = value;
        else if (address >= 0x7800 && address < 0x8000) m->page[5] = value;
        break;
    case MAPPER_ASCII16:
        if (address >= 0x6000 && address < 0x6800) {
            m->page[2] = (uint8_t)(value * 2);
            m->page[3] = (uint8_t)(value * 2 + 1);
        } else if (address >= 0x7000 && address < 0x7800) {
            m->page[4] = (uint8_t)(value * 2);
            m->page[5] = (uint8_t)(value * 2 + 1);
        }
        break;
    case MAPPER_KONAMI_SCC:
        // Alleen de paging (bankregisters op 0x5000/0x7000/0x9000/0xB000);
        // SCC-gelúid zit in het aparte konami_scc-device (slot 1). Deze route
        // wordt gebruikt voor een SCC-game in slot 2.
        if (address >= 0x5000 && address < 0x5800) m->page[2] = value;
        else if (address >= 0x7000 && address < 0x7800) m->page[3] = value;
        else if (address >= 0x9000 && address < 0x9800) m->page[4] = value;
        else if (address >= 0xB000 && address < 0xB800) m->page[5] = value;
        break;
    default:
        break;
    }
}

// Bank-switch-write-histogram (LD (nnnn),A = 0x32). We tellen alleen de
// ONDERSCHEIDENDE adressen — 0x6000/0x7000 delen (bijna) alle mappers, dus
// die zeggen niets over SCC/Konami. Een echte SCC-game schrijft naar
// 0x5000/0x9000/0xB000, een Konami-game naar 0x8000/0xA000, een ASCII8-game
// naar 0x6800/0x7800. Wie alléén 0x6000/0x7000 gebruikt is ASCII16.
typedef struct { int scc_bits, kon_bits, scc_n, kon_n, a8; } bankhist_t;

// Tel de patronen in buf[0..n-3] (de laatste 2 bytes horen bij de volgende
// chunk-overlap bij streaming).
static void bankhist_scan(bankhist_t *h, const uint8_t *buf, uint32_t n)
{
    if (n < 3) return;
    for (uint32_t i = 0; i + 2 < n; i++) {
        if (buf[i] != 0x32) continue;
        uint16_t a = (uint16_t)(buf[i + 1] | (buf[i + 2] << 8));
        switch (a) {
        case 0x5000: h->scc_bits |= 1; h->scc_n++; break;
        case 0x9000: h->scc_bits |= 2; h->scc_n++; break;
        case 0xB000: h->scc_bits |= 4; h->scc_n++; break;
        case 0x4000: h->kon_bits |= 1; h->kon_n++; break;
        case 0x8000: h->kon_bits |= 2; h->kon_n++; break;
        case 0xA000: h->kon_bits |= 4; h->kon_n++; break;
        case 0x6800: case 0x7800: h->a8++; break;
        default: break;
        }
    }
}

// Een échte megaROM paget álle vensters: meerdere DISTINCT bank-registers en
// vaak. Eén losse write is toevallige data (Aleste: één 0x5000 -> werd fout
// konami-scc). Drempel: >=2 distinct onderscheidende registers of >=6 writes.
static mapper_type_t bankhist_classify(const bankhist_t *h)
{
    int scc_d = __builtin_popcount((unsigned)h->scc_bits);
    int kon_d = __builtin_popcount((unsigned)h->kon_bits);
    bool is_scc = scc_d >= 2 || h->scc_n >= 6;
    bool is_kon = kon_d >= 2 || h->kon_n >= 6;
    if (is_scc && h->scc_n >= h->kon_n) return MAPPER_KONAMI_SCC;
    if (is_kon) return MAPPER_KONAMI;
    if (h->a8) return MAPPER_ASCII8;
    return MAPPER_ASCII16; // alleen gedeelde/geen writes -> veilige default
}

mapper_type_t mapper_detect(const uint8_t *rom, uint32_t size)
{
    if (rom == 0 || size == 0) return MAPPER_NONE;
    if (size <= 32u * 1024u) return MAPPER_PLAIN; // 16/32/48K zonder mapper
    bankhist_t h = {0};
    bankhist_scan(&h, rom, size);
    return bankhist_classify(&h);
}

// Streaming-variant: scan een ROM zonder 'm helemaal te laden (menu op de
// Pico) via een leescallback. Leest in overlappende 4KB-chunks zodat het
// 3-byte-patroon nooit op een chunkgrens verloren gaat.
mapper_type_t mapper_detect_stream(void *ctx, mapper_read_fn read, uint32_t size)
{
    if (read == 0 || size == 0) return MAPPER_NONE;
    if (size <= 32u * 1024u) return MAPPER_PLAIN;
    bankhist_t h = {0};
    // STATIC (geen 4KB op de stack): dit draait vanuit menu_render op core 0,
    // en een grote stackbuffer liep de main-stack over -> geheugencorruptie.
    // Enkeldradig menugebruik, dus static is veilig.
    static uint8_t buf[2048];
    for (uint32_t off = 0; off + 2 < size; ) {
        uint32_t want = size - off;
        if (want > sizeof buf) want = sizeof buf;
        long got = read(ctx, off, buf, want);
        if (got < 3) break;
        bankhist_scan(&h, buf, (uint32_t)got);
        if ((uint32_t)got < want) break;   // korter gelezen -> EOF
        off += (uint32_t)got - 2;          // 2 bytes overlap voor het patroon
    }
    return bankhist_classify(&h);
}
