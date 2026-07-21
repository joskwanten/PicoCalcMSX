// Host (desktop) storage backend: the "sdcard" is a real folder. Located
// relative to the executable (SDL_GetBasePath), with a couple of dev-friendly
// fallbacks, and an MSX_SDCARD env override.

#include "storage.h"

#include <SDL.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

static char g_root[1024];

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool has_layout(const char *p)
{
    char sub[1200];
    snprintf(sub, sizeof sub, "%s/%s", p, SD_SYSTEM);
    return is_dir(p) && is_dir(sub);
}

bool storage_init(void)
{
    // 1. explicit override
    const char *env = getenv("MSX_SDCARD");
    if (env && has_layout(env)) {
        snprintf(g_root, sizeof g_root, "%s", env);
        return true;
    }
    // 2. next to the executable
    char *base = SDL_GetBasePath();
    if (base) {
        char cand[1024];
        snprintf(cand, sizeof cand, "%ssdcard", base);
        SDL_free(base);
        if (has_layout(cand)) {
            snprintf(g_root, sizeof g_root, "%s", cand);
            return true;
        }
    }
    // 3. cwd-relative fallbacks (running from a build/ dir during dev)
    const char *fb[] = {"sdcard", "../sdcard", "../../sdcard"};
    for (int i = 0; i < 3; i++) {
        if (has_layout(fb[i])) {
            snprintf(g_root, sizeof g_root, "%s", fb[i]);
            return true;
        }
    }
    fprintf(stderr, "storage: no 'sdcard' folder found (need sdcard/%s, ...)\n", SD_SYSTEM);
    return false;
}

static void path_of(char *out, size_t n, const char *dir, const char *name)
{
    if (name) snprintf(out, n, "%s/%s/%s", g_root, dir, name);
    else snprintf(out, n, "%s/%s", g_root, dir);
}

int storage_list(const char *dir, storage_entry_t *out, int max)
{
    char path[1024];
    path_of(path, sizeof path, dir, NULL);
    DIR *d = opendir(path);
    if (!d) return -1;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue; // hidden, "." and ".."
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        snprintf(out[n].name, STORAGE_MAX_NAME, "%s", e->d_name);
        out[n].is_dir = S_ISDIR(st.st_mode);
        out[n].size = (uint32_t)st.st_size;
        n++;
    }
    closedir(d);
    return n;
}

long storage_size(const char *dir, const char *name)
{
    char path[1024];
    path_of(path, sizeof path, dir, name);
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

long storage_read(const char *dir, const char *name, uint8_t *buf, size_t max)
{
    char path[1024];
    path_of(path, sizeof path, dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, max, f);
    fclose(f);
    return (long)r;
}

long storage_read_at(const char *dir, const char *name, uint32_t off, uint8_t *buf, size_t len)
{
    char path[1024];
    path_of(path, sizeof path, dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t r = fread(buf, 1, len, f);
    fclose(f);
    return (long)r;
}

long storage_write_at(const char *dir, const char *name, uint32_t off, const uint8_t *buf, size_t len)
{
    char path[1024];
    path_of(path, sizeof path, dir, name);
    FILE *f = fopen(path, "r+b"); // bestaand bestand, in-place
    if (!f) return -1;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t w = fwrite(buf, 1, len, f);
    fclose(f);
    return (long)w;
}

bool storage_create(const char *dir, const char *name)
{
    char path[1024];
    path_of(path, sizeof path, dir, NULL);
    mkdir(path, 0755); // bestaat al -> prima
    path_of(path, sizeof path, dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fclose(f);
    return true;
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
