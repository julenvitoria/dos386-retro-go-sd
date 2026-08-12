/*
 * Block device under our FatFs.
 *
 * Two backings, one interface:
 *
 *   memory  - the .dsk is already addressable (the bare-metal harness reads it
 *             memory-mapped out of extflash). Sector reads are memcpy.
 *   file    - the .dsk is a file on the SD card, read through the FIRMWARE's
 *             FatFs. This is the core's path: a 1.44 MB image costs no AXI,
 *             any .dsk in /roms/dos works without a rebuild, and we hold
 *             exactly one SD handle no matter how many files the guest opens.
 *
 * Note the two FatFs instances involved. Ours interprets the FAT12 inside the
 * .dsk; the firmware's reads the bytes of the .dsk off the card. They cannot
 * be the same instance: FF_VOLUMES is 1 in the firmware's build and its disk
 * I/O layer is compiled in, so it can hand us the image's contents but has no
 * way to mount them.
 */
#include <stdint.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"

#define SECTOR_BYTES 512u

static const uint8_t *mem_base;          /* memory backing, or NULL */
static uint32_t disk_sectors;

/* File backing. The reader is supplied by whoever owns the firmware's FatFs,
   so this file never has to know how to reach it. */
static int (*file_read)(void *ctx, void *dst, uint32_t offset, uint32_t len);
static void *file_ctx;

void x86_disk_set_memory(const void *base, uint32_t bytes)
{
    mem_base = (const uint8_t *)base;
    disk_sectors = bytes / SECTOR_BYTES;
    file_read = 0;
    file_ctx = 0;
}

void x86_disk_set_reader(int (*rd)(void *, void *, uint32_t, uint32_t),
                         void *ctx, uint32_t bytes)
{
    file_read = rd;
    file_ctx = ctx;
    disk_sectors = bytes / SECTOR_BYTES;
    mem_base = 0;
}

int x86_disk_ready(void)
{
    return (mem_base || file_read) && disk_sectors;
}

/* Compatibility with the harness's existing call site. */
void vdisk_init(void *base, uint32_t size)
{
    x86_disk_set_memory(base, size);
}

/* --------------------------------------------------------------------------
 * FatFs disk I/O
 */
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0 || !x86_disk_ready())
        return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !x86_disk_ready())
        return RES_NOTRDY;
    if (sector + count > disk_sectors)
        return RES_PARERR;

    if (mem_base) {
        memcpy(buff, mem_base + (uint32_t)sector * SECTOR_BYTES,
               count * SECTOR_BYTES);
        return RES_OK;
    }
    /* One call for the whole run: consecutive sectors are the common case and
       seeking per sector would multiply the SD latency by count. */
    if (file_read(file_ctx, buff, (uint32_t)sector * SECTOR_BYTES,
                  count * SECTOR_BYTES) != 0)
        return RES_ERROR;
    return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)buff; (void)sector; (void)count;
    if (pdrv != 0 || !x86_disk_ready())
        return RES_NOTRDY;
    /* Writes are refused rather than silently dropped: a guest that saves and
       is told it succeeded will lose the save and blame something else. */
    return RES_WRPRT;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !x86_disk_ready())
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = disk_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SECTOR_BYTES;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/*
 * FatFs asks the host for a timestamp when it writes. There is no RTC here and
 * the disk is read-only anyway, so return a fixed, valid DOS date rather than
 * zero -- zero is not a legal encoding and some tools reject it.
 */
DWORD get_fattime(void)
{
    /* 1990-01-01 00:00:00 */
    return ((DWORD)(1990 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}
