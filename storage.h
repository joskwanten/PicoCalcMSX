#ifndef STORAGE_H
#define STORAGE_H

// Abstraction over the "sdcard" storage, with a fixed top-level layout:
//   system/  system ROMs (BIOS, sub-ROM, disk ROM, Nextor, ...)
//   roms/    cartridge ROM images
//   dsk/     .dsk disk images
//   hdd/     Nextor hard-disk images
//
// Two backends implement this: a host one (a real folder, for the SDL build)
// and a Pico one (FatFs on the SD card). The app/menu code is written once
// against this interface.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define STORAGE_MAX_NAME 128

#define SD_SYSTEM "system"
#define SD_ROMS   "roms"
#define SD_DSK    "dsk"
#define SD_HDD    "hdd"

typedef struct {
    char name[STORAGE_MAX_NAME];
    uint32_t size;
    bool is_dir;
} storage_entry_t;

// Locate/mount the sdcard root. Returns false if it can't be found/mounted.
bool storage_init(void);

// List entries in a top-level folder (e.g. SD_ROMS). Fills up to `max` entries;
// returns the count, or -1 on error. Hidden files (starting with '.') are skipped.
int storage_list(const char *dir, storage_entry_t *out, int max);

// Size in bytes of dir/name, or -1 if it doesn't exist.
long storage_size(const char *dir, const char *name);

// Read dir/name fully into `buf` (up to `max` bytes). Returns bytes read, or -1.
long storage_read(const char *dir, const char *name, uint8_t *buf, size_t max);

// Read `len` bytes at byte-offset `off` from dir/name. Returns bytes read
// (short at EOF), or -1. Used for chunked reads (e.g. staging ROMs to flash).
long storage_read_at(const char *dir, const char *name, uint32_t off, uint8_t *buf, size_t len);

// Allocate a buffer of the file's size, read the whole file into it, and return
// it (caller frees). *size receives the byte count. Returns NULL on error.
uint8_t *storage_load(const char *dir, const char *name, uint32_t *size);

#endif // STORAGE_H
