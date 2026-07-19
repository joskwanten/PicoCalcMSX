#ifndef SD_SPI_H
#define SD_SPI_H

// Minimal SD-card driver over hardware SPI0 (GP2 SCK, GP3 MOSI, GP4 MISO,
// GP5 CS). 512-byte blocks. Used by the FatFs diskio glue.

#include <stdint.h>
#include <stdbool.h>

bool sd_init(void);                                  // power-up + identify
bool sd_read_block(uint32_t lba, uint8_t *buf);      // read 512 bytes
bool sd_write_block(uint32_t lba, const uint8_t *buf); // write 512 bytes
uint32_t sd_block_count(void);                       // capacity in 512B blocks (0 = unknown)

#endif // SD_SPI_H
