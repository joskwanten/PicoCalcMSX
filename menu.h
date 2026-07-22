#ifndef MENU_H
#define MENU_H

// Platform-agnostic boot menu: a classic MSX-styled config screen (BIOS font +
// MSX palette) that lets you pick a ROM for slot 1 / slot 2 and a disk for
// A: / B:. Renders into a 256x192 RGB565 framebuffer — the Pico's native format
// (drawn straight into the HDMI back buffer); the SDL frontend converts to ARGB.

#include <stdint.h>
#include <stdbool.h>
#include "storage.h"

typedef struct {
    char slot1[STORAGE_MAX_NAME]; // cartridge ROM (roms/)   — "" = empty
    char slot2[STORAGE_MAX_NAME]; // cartridge ROM (roms/)
    char diskA[STORAGE_MAX_NAME]; // disk image (dsk/)        — wired later
    int8_t mapper1;               // slot 1-mapper: -1 = auto, anders MAPPER_*
} menu_config_t;

typedef enum {
    MENU_UP,
    MENU_DOWN,
    MENU_ENTER,
    MENU_BACK,  // Esc: annuleer browse / wis het geselecteerde veld
    MENU_PGUP,  // een lijstpagina omhoog (browse)
    MENU_PGDN,  // een lijstpagina omlaag (browse)
    MENU_DEL,   // Backspace: wis een zoekletter (browse) / wis veld (main)
} menu_input_t;

// `bios` is the loaded BIOS image (used for its 8x8 character font via CGTABL).
// `cfg` holds the (possibly persisted) config; the menu edits it in place.
void menu_init(const uint8_t *bios, menu_config_t *cfg);

void menu_input(menu_input_t in);
// Teken-invoer voor het zoekveld in de browse-lijst ('a'-'z', '0'-'9'):
// filtert op bestandsnamen die met de getypte letters beginnen.
void menu_char(char c);
void menu_render(uint16_t *fb); // 256x192 RGB565
bool menu_start_requested(void);

#endif // MENU_H
