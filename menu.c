#include "menu.h"

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

// main screen: rows 0..3 = slot1/slot2/diskA/diskB, 4 = Start
static int g_sel;

// browse sub-screen
static int g_field;                 // which config field is being set (0..3)
static const char *g_dir;           // storage folder for the browse
static char *g_target;              // config field to write
static storage_entry_t g_list[256];
static int g_list_n, g_bsel, g_btop;

// --- rendering ---
static void draw_char(uint32_t *fb, int cx, int cy, char c, uint32_t fg, uint32_t bg)
{
    if (cx < 0 || cy < 0 || cx >= COLS || cy >= ROWS) return;
    const uint8_t *g = &g_font[(uint8_t)c * 8];
    for (int y = 0; y < 8; y++) {
        uint8_t bits = g[y];
        uint32_t *dst = &fb[(cy * 8 + y) * SCR_W + cx * 8];
        for (int x = 0; x < 8; x++)
            dst[x] = (bits & (0x80 >> x)) ? fg : bg;
    }
}

static void draw_text(uint32_t *fb, int cx, int cy, const char *s, uint32_t fg, uint32_t bg)
{
    while (*s && cx < COLS)
        draw_char(fb, cx++, cy, *s++, fg, bg);
}

static void fill_screen(uint32_t *fb, uint32_t c)
{
    for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = c;
}

static void fill_row(uint32_t *fb, int cy, uint32_t c)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < SCR_W; x++)
            fb[(cy * 8 + y) * SCR_W + x] = c;
}

static void render_main(uint32_t *fb)
{
    fill_screen(fb, COL_BG);
    draw_text(fb, 10, 1, "PicoCalcMSX", COL_TITLE, COL_BG);
    draw_text(fb, 3, 3, "-- select cartridges/disks --", COL_DIM, COL_BG);

    static const char *labels[4] = {"Slot 1:", "Slot 2:", "Disk A:", "Disk B:"};
    const char *vals[4] = {g_cfg->slot1, g_cfg->slot2, g_cfg->diskA, g_cfg->diskB};

    for (int i = 0; i < 4; i++) {
        int row = 6 + i * 2;
        bool sel = (g_sel == i);
        uint32_t fg = sel ? COL_SEL_TEXT : COL_TEXT;
        uint32_t bg = sel ? COL_SEL_BG : COL_BG;
        if (sel) fill_row(fb, row, COL_SEL_BG);
        draw_text(fb, 3, row, labels[i], fg, bg);
        if (vals[i][0])
            draw_text(fb, 11, row, vals[i], fg, bg);
        else
            draw_text(fb, 11, row, "(empty)", sel ? fg : COL_DIM, bg);
    }

    // Start
    bool ssel = (g_sel == 4);
    if (ssel) fill_row(fb, 17, COL_SEL_BG);
    draw_text(fb, 12, 17, "[ Start ]", ssel ? COL_SEL_TEXT : COL_TEXT, ssel ? COL_SEL_BG : COL_BG);

    draw_text(fb, 2, 22, "up/dn  enter=pick  esc=clear", COL_DIM, COL_BG);
}

static void render_browse(uint32_t *fb)
{
    fill_screen(fb, COL_BG);
    char title[40];
    snprintf(title, sizeof title, "-- %s --", g_dir);
    draw_text(fb, 3, 1, title, COL_TITLE, COL_BG);

    const int visible = 18; // rows 3..20
    if (g_bsel < g_btop) g_btop = g_bsel;
    if (g_bsel >= g_btop + visible) g_btop = g_bsel - visible + 1;

    if (g_list_n == 0)
        draw_text(fb, 3, 3, "(no files)", COL_DIM, COL_BG);

    for (int i = 0; i < visible && (g_btop + i) < g_list_n; i++) {
        int idx = g_btop + i;
        int row = 3 + i;
        bool sel = (idx == g_bsel);
        if (sel) fill_row(fb, row, COL_SEL_BG);
        draw_text(fb, 2, row, g_list[idx].name, sel ? COL_SEL_TEXT : COL_TEXT, sel ? COL_SEL_BG : COL_BG);
    }

    draw_text(fb, 2, 22, "enter=select  esc=cancel", COL_DIM, COL_BG);
}

void menu_render(uint32_t *fb)
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
    g_start = false;
    g_mode = MODE_MAIN;
    g_sel = 0;
}

static void open_browse(int field)
{
    g_field = field;
    g_dir = (field < 2) ? SD_ROMS : SD_DSK;
    g_target = (field == 0) ? g_cfg->slot1 : (field == 1) ? g_cfg->slot2
             : (field == 2) ? g_cfg->diskA : g_cfg->diskB;
    int n = storage_list(g_dir, g_list, (int)(sizeof g_list / sizeof g_list[0]));
    g_list_n = (n < 0) ? 0 : n;
    g_bsel = 0;
    g_btop = 0;
    g_mode = MODE_BROWSE;
}

void menu_input(menu_input_t in)
{
    if (g_mode == MODE_MAIN) {
        switch (in) {
        case MENU_UP:   g_sel = (g_sel + 4) % 5; break;
        case MENU_DOWN: g_sel = (g_sel + 1) % 5; break;
        case MENU_ENTER:
            if (g_sel == 4) g_start = true;
            else open_browse(g_sel);
            break;
        case MENU_BACK: // clear the selected field
            if (g_sel < 4) {
                char *t = (g_sel == 0) ? g_cfg->slot1 : (g_sel == 1) ? g_cfg->slot2
                        : (g_sel == 2) ? g_cfg->diskA : g_cfg->diskB;
                t[0] = 0;
            }
            break;
        }
    } else { // MODE_BROWSE
        switch (in) {
        case MENU_UP:   if (g_bsel > 0) g_bsel--; break;
        case MENU_DOWN: if (g_bsel + 1 < g_list_n) g_bsel++; break;
        case MENU_ENTER:
            if (g_list_n > 0) {
                snprintf(g_target, STORAGE_MAX_NAME, "%s", g_list[g_bsel].name);
                g_mode = MODE_MAIN;
            }
            break;
        case MENU_BACK: g_mode = MODE_MAIN; break;
        }
    }
}

bool menu_start_requested(void)
{
    return g_start;
}
