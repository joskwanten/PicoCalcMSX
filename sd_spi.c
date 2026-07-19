#include "sd_spi.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// SPI0 on GP2-5 (clear of HSTX 12-19 and the WeAct PSRAM CS on GP0; works on a
// stock Pico 2 too).
#define SD_SPI   spi0
#define SD_SCK   2
#define SD_MOSI  3
#define SD_MISO  4
#define SD_CS    5

#define SD_INIT_BAUD (400 * 1000)
#define SD_FAST_BAUD (12 * 1000 * 1000)

static bool s_hc = false;      // high-capacity: block (LBA) addressing
static uint32_t s_blocks = 0;  // capacity in 512B blocks

static inline void cs_low(void)  { gpio_put(SD_CS, 0); }
static inline void cs_high(void) { gpio_put(SD_CS, 1); }

static uint8_t xfer(uint8_t v)
{
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &v, &r, 1);
    return r;
}

static void clock_bytes(int n)
{
    for (int i = 0; i < n; i++) xfer(0xFF);
}

// Wait until the card releases the bus (returns 0xFF). Returns false on timeout.
static bool wait_ready(void)
{
    for (int i = 0; i < 100000; i++)
        if (xfer(0xFF) == 0xFF) return true;
    return false;
}

// Send a command, return R1. CRC is only real for CMD0/CMD8; otherwise dummy.
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg)
{
    if (cmd != 12) wait_ready();
    uint8_t b[6];
    b[0] = 0x40 | cmd;
    b[1] = (uint8_t)(arg >> 24);
    b[2] = (uint8_t)(arg >> 16);
    b[3] = (uint8_t)(arg >> 8);
    b[4] = (uint8_t)arg;
    b[5] = (cmd == 0) ? 0x95 : (cmd == 8) ? 0x87 : 0x01;
    for (int i = 0; i < 6; i++) xfer(b[i]);

    uint8_t r = 0xFF;
    for (int i = 0; i < 12; i++) {
        r = xfer(0xFF);
        if (!(r & 0x80)) break;
    }
    return r;
}

static uint8_t sd_acmd(uint8_t cmd, uint32_t arg)
{
    sd_cmd(55, 0);
    return sd_cmd(cmd, arg);
}

static void read_bytes(uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) buf[i] = 0xFF;
    spi_write_read_blocking(SD_SPI, buf, buf, n); // buf holds 0xFF -> receives data
}

static bool read_data_block(uint8_t *buf, int n)
{
    uint8_t token = 0xFF;
    for (int i = 0; i < 100000; i++) {
        token = xfer(0xFF);
        if (token != 0xFF) break;
    }
    if (token != 0xFE) return false;
    read_bytes(buf, n);
    xfer(0xFF); // CRC
    xfer(0xFF);
    return true;
}

bool sd_init(void)
{
    spi_init(SD_SPI, SD_INIT_BAUD);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO, GPIO_FUNC_SPI);
    gpio_init(SD_CS);
    gpio_set_dir(SD_CS, GPIO_OUT);
    cs_high();

    // >= 74 clocks with CS high to wake the card.
    clock_bytes(10);

    cs_low();
    uint8_t r = 0xFF;
    for (int i = 0; i < 20 && r != 0x01; i++) r = sd_cmd(0, 0); // GO_IDLE
    if (r != 0x01) { cs_high(); return false; }

    bool v2 = false;
    r = sd_cmd(8, 0x1AA); // SEND_IF_COND
    if (r == 0x01) {
        uint8_t ocr[4];
        read_bytes(ocr, 4);
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) { cs_high(); return false; }
        v2 = true;
    }

    // ACMD41 until ready.
    r = 0xFF;
    for (int i = 0; i < 2000 && r != 0x00; i++) {
        r = sd_acmd(41, v2 ? 0x40000000u : 0u); // HCS
        sleep_ms(1);
    }
    if (r != 0x00) { cs_high(); return false; }

    // Capacity flag via OCR.
    s_hc = false;
    if (v2) {
        r = sd_cmd(58, 0); // READ_OCR
        if (r == 0x00) {
            uint8_t ocr[4];
            read_bytes(ocr, 4);
            s_hc = (ocr[0] & 0x40) != 0; // CCS
        }
    }

    // Capacity via CSD (CMD9) — best effort.
    s_blocks = 0;
    if (sd_cmd(9, 0) == 0x00) {
        uint8_t csd[16];
        if (read_data_block(csd, 16)) {
            if ((csd[0] >> 6) == 1) { // CSD v2 (SDHC/SDXC)
                uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | (csd[8] << 8) | csd[9];
                s_blocks = (c_size + 1) * 1024;
            }
        }
    }

    cs_high();
    xfer(0xFF);
    spi_set_baudrate(SD_SPI, SD_FAST_BAUD);
    return true;
}

bool sd_read_block(uint32_t lba, uint8_t *buf)
{
    uint32_t addr = s_hc ? lba : lba * 512u;
    cs_low();
    if (sd_cmd(17, addr) != 0x00) { cs_high(); return false; }
    bool ok = read_data_block(buf, 512);
    cs_high();
    xfer(0xFF);
    return ok;
}

bool sd_write_block(uint32_t lba, const uint8_t *buf)
{
    uint32_t addr = s_hc ? lba : lba * 512u;
    cs_low();
    if (sd_cmd(24, addr) != 0x00) { cs_high(); return false; }
    xfer(0xFF);
    xfer(0xFE); // data token
    spi_write_blocking(SD_SPI, buf, 512);
    xfer(0xFF); // CRC
    xfer(0xFF);
    uint8_t resp = xfer(0xFF);
    if ((resp & 0x1F) != 0x05) { cs_high(); return false; }
    wait_ready();
    cs_high();
    xfer(0xFF);
    return true;
}

uint32_t sd_block_count(void)
{
    return s_blocks;
}
