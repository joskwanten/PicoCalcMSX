// ROM-staging naar QSPI-flash — zie flash_stage.h voor het waarom en de
// beperking (alleen vóór de video/core 1 start).

#include "flash_stage.h"
#include "storage.h"

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// 2 MB bovenin de flash: 1 header-sector + ROM-data. De applicatie zelf is
// ~300 KB onderin, dus dit bijt nooit (ook niet op een 4 MB Pico 2).
#define STAGE_SIZE   (2u * 1024 * 1024)
#define STAGE_OFFSET (PICO_FLASH_SIZE_BYTES - STAGE_SIZE)
#define STAGE_MAX_ROM (STAGE_SIZE - FLASH_SECTOR_SIZE)
#define STAGE_MAGIC  0x4D535852u // "MSXR"

typedef struct {
    uint32_t magic;
    uint32_t size;
    char name[STORAGE_MAX_NAME];
} stage_hdr_t;

static const stage_hdr_t *stage_hdr(void)
{
    return (const stage_hdr_t *)(XIP_BASE + STAGE_OFFSET);
}

static const uint8_t *stage_data(void)
{
    return (const uint8_t *)(XIP_BASE + STAGE_OFFSET + FLASH_SECTOR_SIZE);
}

const uint8_t *flash_stage_get(const char *name, uint32_t *size)
{
    const stage_hdr_t *h = stage_hdr();
    if (h->magic != STAGE_MAGIC) return NULL;
    if (h->size == 0 || h->size > STAGE_MAX_ROM) return NULL;
    if (strncmp(h->name, name, STORAGE_MAX_NAME) != 0) return NULL;
    if (size) *size = h->size;
    return stage_data();
}

const uint8_t *flash_stage_rom(const char *dir, const char *name, uint32_t *size)
{
    long sz = storage_size(dir, name);
    if (sz <= 0 || (uint32_t)sz > STAGE_MAX_ROM) return NULL;

    // Staat 'ie er al (zelfde naam + grootte)? Dan niets flashen.
    const stage_hdr_t *h = stage_hdr();
    if (h->magic == STAGE_MAGIC && h->size == (uint32_t)sz &&
        strncmp(h->name, name, STORAGE_MAX_NAME) == 0) {
        if (size) *size = h->size;
        return stage_data();
    }

    printf("[stage] %s (%ld bytes) -> flash @0x%08x\n", name, sz, (unsigned)STAGE_OFFSET);

    // Erase: header-sector + de data-sectoren die we nodig hebben.
    uint32_t data_bytes = ((uint32_t)sz + FLASH_SECTOR_SIZE - 1) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(STAGE_OFFSET, FLASH_SECTOR_SIZE + data_bytes);
    restore_interrupts(ints);

    // Data in chunks van SD lezen en programmeren.
    static uint8_t chunk[FLASH_SECTOR_SIZE];
    for (uint32_t off = 0; off < (uint32_t)sz; off += FLASH_SECTOR_SIZE) {
        size_t want = (uint32_t)sz - off;
        if (want > sizeof chunk) want = sizeof chunk;
        long n = storage_read_at(dir, name, off, chunk, want);
        if (n != (long)want) return NULL; // header blijft ge-erased -> regio ongeldig
        if (want < sizeof chunk) memset(chunk + want, 0xFF, sizeof chunk - want);
        ints = save_and_disable_interrupts();
        flash_range_program(STAGE_OFFSET + FLASH_SECTOR_SIZE + off, chunk, sizeof chunk);
        restore_interrupts(ints);
    }

    // Header als laatste schrijven: pas dan is de staging geldig.
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    stage_hdr_t nh;
    memset(&nh, 0, sizeof nh);
    nh.magic = STAGE_MAGIC;
    nh.size = (uint32_t)sz;
    snprintf(nh.name, sizeof nh.name, "%s", name);
    memcpy(page, &nh, sizeof nh);
    ints = save_and_disable_interrupts();
    flash_range_program(STAGE_OFFSET, page, sizeof page);
    restore_interrupts(ints);

    if (size) *size = (uint32_t)sz;
    return stage_data();
}
