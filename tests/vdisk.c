#include "ff.h"
#include "diskio.h"
#include <string.h>
#include <stdint.h>

// Assuming ALLEYCAT.dsk is loaded at a memory pointer
uint8_t *vdisk_base = (uint8_t *)0;
uint32_t vdisk_size = 0;

void vdisk_init(void *base, uint32_t size) {
    vdisk_base = (uint8_t *)base;
    vdisk_size = size;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!vdisk_base) return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!vdisk_base) return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !vdisk_base) return RES_NOTRDY;
    
    uint32_t offset = sector * 512;
    uint32_t len = count * 512;
    
    if (offset + len > vdisk_size) {
        return RES_PARERR;
    }
    
    memcpy(buff, vdisk_base + offset, len);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !vdisk_base) return RES_NOTRDY;
    
    uint32_t offset = sector * 512;
    uint32_t len = count * 512;
    
    if (offset + len > vdisk_size) {
        return RES_PARERR;
    }
    
    memcpy(vdisk_base + offset, buff, len);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0 || !vdisk_base) return RES_NOTRDY;
    
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(LBA_t*)buff = vdisk_size / 512;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD*)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD*)buff = 1;
            return RES_OK;
    }
    return RES_PARERR;
}

// Required by FatFs
DWORD get_fattime(void) {
    return 0;
}
