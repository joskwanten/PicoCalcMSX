// Pico storage backend: implements storage.h via FatFs on the SD card.
// The sdcard's top-level folders (system/roms/dsk/hdd) live at the FAT root.

#include "storage.h"

#include "ff.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static FATFS s_fs;

bool storage_init(void)
{
    return f_mount(&s_fs, "", 1) == FR_OK; // mount immediately
}

int storage_list(const char *dir, storage_entry_t *out, int max)
{
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return -1;

    int n = 0;
    FILINFO fi;
    while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fname[0] == '.') continue;
        snprintf(out[n].name, STORAGE_MAX_NAME, "%s", fi.fname);
        out[n].is_dir = (fi.fattrib & AM_DIR) != 0;
        out[n].size = (uint32_t)fi.fsize;
        n++;
    }
    f_closedir(&d);
    return n;
}

long storage_size(const char *dir, const char *name)
{
    char path[300];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) return -1;
    return (long)fi.fsize;
}

long storage_read(const char *dir, const char *name, uint8_t *buf, size_t max)
{
    char path[300];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    FRESULT fr = f_read(&f, buf, (UINT)max, &br);
    f_close(&f);
    return (fr == FR_OK) ? (long)br : -1;
}

uint8_t *storage_load(const char *dir, const char *name, uint32_t *size)
{
    long sz = storage_size(dir, name);
    if (sz < 0) return NULL;
    uint8_t *buf = malloc((size_t)(sz > 0 ? sz : 1));
    if (!buf) return NULL;
    if (storage_read(dir, name, buf, (size_t)sz) != sz) {
        free(buf);
        return NULL;
    }
    if (size) *size = (uint32_t)sz;
    return buf;
}
