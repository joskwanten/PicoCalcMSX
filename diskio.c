// FatFs disk I/O glue -> SD over SPI (single volume 0).

#include "ff.h"
#include "diskio.h"
#include "sd_spi.h"

static bool s_init = false;

DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return s_init ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;
    s_init = sd_init();
    return s_init ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    for (UINT i = 0; i < count; i++)
        if (!sd_read_block((uint32_t)sector + i, buff + i * 512))
            return RES_ERROR;
    return RES_OK;
}

#if !FF_FS_READONLY
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    for (UINT i = 0; i < count; i++)
        if (!sd_write_block((uint32_t)sector + i, buff + i * 512))
            return RES_ERROR;
    return RES_OK;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = sd_block_count();
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    return 0; // no RTC
}
