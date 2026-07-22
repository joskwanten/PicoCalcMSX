#include "menu.h"
#include "mapper.h" // mapper_detect_stream / mapper_name (slot 1-mapper-override)

#include <stdio.h>
#include <string.h>

#define SCR_W 256
#define SCR_H 192
#define COLS (SCR_W / 8) // 32
#define ROWS (SCR_H / 8) // 24

// MSX (TMS9918) palette colours, ARGB — same values as tms9918.c.
#define COL_BG       0xff5455ed // MSX default blue (palette 4)
#define COL_TEXT     0xffffffff // white (15)
#define COL_DIM      0xffcccccc // gray (14) for empty values
#define COL_TITLE    0xff42ebf5 // cyan (7)
#define COL_SEL_BG   0xffffffff // white highlight bar
#define COL_SEL_TEXT 0xff5455ed // blue text on the bar (palette 4)

// --- state ---
static const uint8_t *g_font;      // 256 chars x 8 bytes, from the BIOS
static menu_config_t *g_cfg;
static bool g_start;

typedef enum { MODE_MAIN, MODE_BROWSE } mode_t;
static mode_t g_mode;

// main screen: rows 0..2 = slot1/slot2/diskA, 3 = Start
static int g_sel;
// Selecteerbare items op het hoofdscherm.
enum { SEL_SLOT1 = 0, SEL_MAPPER, SEL_SLOT2, SEL_DISKA, SEL_START, SEL_N };

// --- slot 1-mapper: auto-detect (streamend — de ROM is niet geladen in het
// menu) + override. Gedetecteerde waarde gecached per slot1-naam. ---
static char g_map_for[STORAGE_MAX_NAME];
static mapper_type_t g_map_det;
static long menu_rom_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
    return storage_read_at(SD_ROMS, (const char *)ctx, off, buf, len);
}
static bool name_is_zip(const char *n)
{
    size_t l = strlen(n);
    return l >= 4 && n[l-4] == '.' && (n[l-3]|32) == 'z' && (n[l-2]|32) == 'i'
        && (n[l-1]|32) == 'p';
}
static mapper_type_t detected_mapper(void)
{
    // .zip: pas na uitpakken te bepalen -> geen hint.
    if (!g_cfg->slot1[0] || name_is_zip(g_cfg->slot1)) return MAPPER_NONE;
    if (strcmp(g_map_for, g_cfg->slot1) != 0) { // cache-miss -> scan
        snprintf(g_map_for, sizeof g_map_for, "%s", g_cfg->slot1);
        long sz = storage_size(SD_ROMS, g_cfg->slot1);
        g_map_det = (sz > 0)
            ? mapper_detect_stream(g_cfg->slot1, menu_rom_read, (uint32_t)sz)
            : MAPPER_NONE;
    }
    return g_map_det;
}
static const char *mapper_label(void)
{
    static char b[28];
    if (g_cfg->mapper1 < 0) { // auto
        mapper_type_t d = detected_mapper();
        if (d == MAPPER_NONE) return "auto";
        snprintf(b, sizeof b, "auto (%s)", mapper_name(d));
        return b;
    }
    return mapper_name((mapper_type_t)g_cfg->mapper1);
}
static void mapper_cycle(void)
{
    static const int8_t seq[] = { -1, MAPPER_PLAIN, MAPPER_KONAMI,
                                  MAPPER_ASCII8, MAPPER_ASCII16, MAPPER_KONAMI_SCC };
    int i = 0;
    for (int k = 0; k < 6; k++) if (seq[k] == g_cfg->mapper1) { i = k; break; }
    g_cfg->mapper1 = seq[(i + 1) % 6];
}

// browse sub-screen
static int g_field;                 // which config field is being set (0..3)
static const char *g_dir;           // storage folder for the browse
static char *g_target;              // config field to write
static storage_entry_t g_list[128]; // 128 entries/map: ruim, en ~17KB
                                    // RAM gespaard (heap moet de tijdelijke
                                    // zip-extractiebuffers ~15KB kunnen dragen)
static int g_list_n, g_bsel, g_btop;

// Zoekveld (browse): prefix-filter, hoofdletterongevoelig. g_fidx bevat de
// indices in g_list die matchen; g_bsel/g_btop lopen over de gefilterde lijst.
static char g_search[17];
static int g_search_len;
static int g_fidx[(int)(sizeof g_list / sizeof g_list[0])];
static int g_fn;

static inline char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static void refilter(void)
{
    g_fn = 0;
    for (int i = 0; i < g_list_n; i++) {
        int j = 0;
        while (j < g_search_len && lower(g_list[i].name[j]) == g_search[j]) j++;
        if (j == g_search_len) g_fidx[g_fn++] = i;
    }
    g_bsel = g_btop = 0;
}

// --- rendering (RGB565 output; colours defined as ARGB, converted here) ---
static inline uint16_t to565(uint32_t argb)
{
    return (uint16_t)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F));
}

static void draw_char(uint16_t *fb, int cx, int cy, char c, uint32_t fg, uint32_t bg)
{
    if (cx < 0 || cy < 0 || cx >= COLS || cy >= ROWS) return;
    uint16_t f = to565(fg), b = to565(bg);
    const uint8_t *g = &g_font[(uint8_t)c * 8];
    for (int y = 0; y < 8; y++) {
        uint8_t bits = g[y];
        uint16_t *dst = &fb[(cy * 8 + y) * SCR_W + cx * 8];
        for (int x = 0; x < 8; x++)
            dst[x] = (bits & (0x80 >> x)) ? f : b;
    }
}

static void draw_text(uint16_t *fb, int cx, int cy, const char *s, uint32_t fg, uint32_t bg)
{
    while (*s && cx < COLS)
        draw_char(fb, cx++, cy, *s++, fg, bg);
}

static void fill_screen(uint16_t *fb, uint32_t c)
{
    uint16_t v = to565(c);
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = v;
}

static void fill_row(uint16_t *fb, int cy, uint32_t c)
{
    uint16_t v = to565(c);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < SCR_W; x++)
            fb[(cy * 8 + y) * SCR_W + x] = v;
}

static void render_main(uint16_t *fb)
{
    fill_screen(fb, COL_BG);
    draw_text(fb, 12, 1, "BareMSX", COL_TITLE, COL_BG); // (32-7)/2 = gecentreerd
    draw_text(fb, 3, 3, "-- select cartridges/disks --", COL_DIM, COL_BG);

    // Mapper = slot 1-eigenschap (cyclet i.p.v. bladeren). Drive B bestaat
    // niet; diskwissel = F12 in-game.
    static const char *labels[4] = {"Slot 1:", "Mapper:", "Slot 2:", "Disk A:"};
    for (int i = 0; i < 4; i++) {
        int row = 6 + i * 2;
        bool sel = (g_sel == i);
        uint32_t fg = sel ? COL_SEL_TEXT : COL_TEXT;
        uint32_t bg = sel ? COL_SEL_BG : COL_BG;
        if (sel) fill_row(fb, row, COL_SEL_BG);
        draw_text(fb, 3, row, labels[i], fg, bg);

        const char *v = "(empty)"; bool empty = false;
        switch (i) {
        case SEL_SLOT1:  v = g_cfg->slot1; empty = !v[0]; break;
        case SEL_MAPPER: v = mapper_label(); break; // altijd gevuld (auto/...)
        case SEL_SLOT2:  v = g_cfg->slot2; empty = !v[0]; break;
        default:         v = g_cfg->diskA; empty = !v[0]; break;
        }
        if (empty)
            draw_text(fb, 11, row, "(empty)", sel ? fg : COL_DIM, bg);
        else
            draw_text(fb, 11, row, v, fg, bg);
    }

    // Start
    bool ssel = (g_sel == SEL_START);
    if (ssel) fill_row(fb, 15, COL_SEL_BG);
    draw_text(fb, 12, 15, "[ Start ]", ssel ? COL_SEL_TEXT : COL_TEXT, ssel ? COL_SEL_BG : COL_BG);

    draw_text(fb, 2, 22, "up/dn  enter=pick/cycle  esc=clear", COL_DIM, COL_BG);
}

#define BROWSE_ROWS 18 // zichtbare lijstregels (rows 3..20); ook de pgup/pgdn-stap

static void render_browse(uint16_t *fb)
{
    fill_screen(fb, COL_BG);
    char title[40];
    snprintf(title, sizeof title, "-- %s --", g_dir);
    draw_text(fb, 3, 1, title, COL_TITLE, COL_BG);

    // Zoekveld: getypte letters filteren de lijst op naam-prefix.
    if (g_search_len > 0) {
        char s[40];
        snprintf(s, sizeof s, "find: %s_", g_search);
        draw_text(fb, 2, 2, s, COL_TEXT, COL_BG);
    }

    if (g_bsel < g_btop) g_btop = g_bsel;
    if (g_bsel >= g_btop + BROWSE_ROWS) g_btop = g_bsel - BROWSE_ROWS + 1;

    if (g_list_n == 0)
        draw_text(fb, 3, 3, "(no files)", COL_DIM, COL_BG);
    else if (g_fn == 0)
        draw_text(fb, 3, 3, "(no matches)", COL_DIM, COL_BG);

    for (int i = 0; i < BROWSE_ROWS && (g_btop + i) < g_fn; i++) {
        int idx = g_btop + i;
        int row = 3 + i;
        bool sel = (idx == g_bsel);
        if (sel) fill_row(fb, row, COL_SEL_BG);
        draw_text(fb, 2, row, g_list[g_fidx[idx]].name,
                  sel ? COL_SEL_TEXT : COL_TEXT, sel ? COL_SEL_BG : COL_BG);
    }

    draw_text(fb, 2, 22, "type=find pgup/dn enter esc", COL_DIM, COL_BG);
}

void menu_render(uint16_t *fb)
{
    if (g_mode == MODE_MAIN) render_main(fb);
    else render_browse(fb);
}

// --- logic ---
void menu_init(const uint8_t *bios, menu_config_t *cfg)
{
    uint16_t cgtabl = (uint16_t)(bios[0x0004] | (bios[0x0005] << 8)); // MSX CGTABL pointer
    g_font = &bios[cgtabl];
    g_cfg = cfg;
    if (g_cfg->mapper1 == 0) g_cfg->mapper1 = -1; // default: auto-detect
    g_map_for[0] = 0;                             // scan-cache leegmaken
    g_start = false;
    g_mode = MODE_MAIN;
    g_sel = 0;
}

static void open_browse(int sel)
{
    g_field = sel;
    g_dir = (sel == SEL_DISKA) ? SD_DSK : SD_ROMS;
    g_target = (sel == SEL_SLOT1) ? g_cfg->slot1
             : (sel == SEL_SLOT2) ? g_cfg->slot2
             : g_cfg->diskA;
    int n = storage_list(g_dir, g_list, (int)(sizeof g_list / sizeof g_list[0]));
    g_list_n = (n < 0) ? 0 : n;
    g_search_len = 0;
    g_search[0] = 0;
    refilter(); // ook: g_bsel/g_btop naar 0
    g_mode = MODE_BROWSE;
}

void menu_input(menu_input_t in)
{
    if (g_mode == MODE_MAIN) {
        switch (in) {
        case MENU_UP:   g_sel = (g_sel + SEL_N - 1) % SEL_N; break;
        case MENU_DOWN: g_sel = (g_sel + 1) % SEL_N; break;
        case MENU_ENTER:
            if (g_sel == SEL_START)       g_start = true;
            else if (g_sel == SEL_MAPPER) mapper_cycle();  // override doorlopen
            else                          open_browse(g_sel);
            break;
        case MENU_BACK: // Esc/Backspace: veld wissen / mapper terug naar auto
        case MENU_DEL:
            if (g_sel == SEL_MAPPER) g_cfg->mapper1 = -1;
            else if (g_sel != SEL_START) {
                char *t = (g_sel == SEL_SLOT1) ? g_cfg->slot1
                        : (g_sel == SEL_SLOT2) ? g_cfg->slot2 : g_cfg->diskA;
                t[0] = 0;
            }
            break;
        default:
            break; // pgup/pgdn: alleen zinvol in de browse-lijst
        }
    } else { // MODE_BROWSE
        switch (in) {
        case MENU_UP:   if (g_bsel > 0) g_bsel--; break;
        case MENU_DOWN: if (g_bsel + 1 < g_fn) g_bsel++; break;
        case MENU_PGUP:
            g_bsel = (g_bsel > BROWSE_ROWS) ? g_bsel - BROWSE_ROWS : 0;
            break;
        case MENU_PGDN:
            if (g_fn > 0) {
                g_bsel += BROWSE_ROWS;
                if (g_bsel >= g_fn) g_bsel = g_fn - 1;
            }
            break;
        case MENU_ENTER:
            if (g_fn > 0) {
                snprintf(g_target, STORAGE_MAX_NAME, "%s", g_list[g_fidx[g_bsel]].name);
                if (g_field == SEL_SLOT1) g_cfg->mapper1 = -1; // nieuwe ROM -> auto
                g_mode = MODE_MAIN;
            }
            break;
        case MENU_DEL: // Backspace: een zoekletter wissen
            if (g_search_len > 0) {
                g_search[--g_search_len] = 0;
                refilter();
            }
            break;
        case MENU_BACK: g_mode = MODE_MAIN; break;
        }
    }
}

void menu_char(char c)
{
    if (g_mode != MODE_BROWSE) return;
    c = lower(c);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return;
    if (g_search_len >= (int)(sizeof g_search) - 1) return;
    g_search[g_search_len++] = c;
    g_search[g_search_len] = 0;
    refilter();
}

bool menu_start_requested(void)
{
    return g_start;
}
