/*-----------------------------------------------------------------------*/
/* Low level disk I/O module SKELETON for FatFs     (C)ChaN, 2019        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include <ff.h>         // Obtains integer types
#include <diskio.h>     // Declarations of disk functions
#include <glue.h>       // Declarations of SD card functions
#include "usb_diskio.h" // Declarations of USB MSD functions
#include <stddef.h>

#define USE_BLOCK_CACHE             1
#define USE_BLOCK_CACHE_READ_AHEAD  1       //  USE_BLOCK_CACHE must be 1 to use

#if USE_BLOCK_CACHE
#include "block_cache.h"

bool block_cache_initalized = false;
#endif

#if USE_BLOCK_CACHE_READ_AHEAD
bool read_ahead = false;
BYTE last_pdrv = 0;
LBA_t last_sector = 0;
#endif


// Definitions of physical drive number for each drive
#define DEV_SD      0   // Map MMC/SD card to physical drive 0
#define DEV_USB     1   // Map USB MSD to physical drive 1


void disk_init(void) {
#if USE_BLOCK_CACHE
    if (block_cache_initalized == false) {
        block_cache_init();
        block_cache_initalized = true;
    }
#endif
}

void disk_task(void) {
    bool more_work = true;              //  We want to limit the amount of work done here

#if USE_BLOCK_CACHE

#if USE_BLOCK_CACHE_READ_AHEAD
    if ((read_ahead == true) && (more_work == true)) {
        //  Touch the next block, NULL is OK here
        block_cache_read_block(last_pdrv, last_sector + 1, NULL);

        read_ahead = false;
        more_work = false;
    }
#endif

    if (more_work == true) {
        block_cache_flush(false);       //  If we have time, flush one block from the cache
        more_work = false;
    }
#endif
}
/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status(
    BYTE pdrv   // Physical drive number to identify the drive
) {
    switch (pdrv) {
        case DEV_SD:
            return sd_disk_status(DEV_SD);

#if MEDIUM == USB
            case DEV_USB:
            return usb_disk_status(DEV_USB);
#endif

        default:
            return STA_NOINIT;
    }
}


/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize(
    BYTE pdrv   // Physical drive number to identify the drive
) {
#if USE_BLOCK_CACHE
    
    //  Make sure the cache is flushed (ignores errors???)
    block_cache_flush(true);
#endif

    switch (pdrv) {
        case DEV_SD:
            return sd_disk_initialize(DEV_SD);

#if MEDIUM == USB
            case DEV_USB:
            return usb_disk_initialize(DEV_USB);
#endif

        default:
            return STA_NOINIT;
    }
}


/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read(
    BYTE pdrv,      // Physical drive number to identify the drive
    BYTE *buff,     // Data buffer to store read data
    LBA_t sector,   // Start sector in LBA
    UINT count      // Number of sectors to read
) {
    DRESULT result;

#if USE_BLOCK_CACHE
    if (count == 1) {
        result = block_cache_read_block(pdrv, sector, buff);
    }
    else {
        block_cache_flush(true);
        result = disk_read_no_cache(pdrv, buff, sector, count);
    }
#else
    result = disk_read_no_cache(pdrv, buff, sector, count);
#endif

#if USE_BLOCK_CACHE_READ_AHEAD
    if (result == RES_OK) {
        read_ahead = true;
        last_pdrv = pdrv;
        last_sector = sector;
    }
#endif

    return result;
}

DRESULT disk_read_no_cache(
    BYTE pdrv,      // Physical drive number to identify the drive
    BYTE *buff,     // Data buffer to store read data
    LBA_t sector,   // Start sector in LBA
    UINT count      // Number of sectors to read
) {
    switch (pdrv) {
        case DEV_SD:
            return sd_disk_read(DEV_SD, buff, sector, count);

#if MEDIUM == USB
            case DEV_USB:
            return usb_disk_read(DEV_USB, buff, sector, count);
#endif
        
        default:
            return RES_PARERR;
    }
}


/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

DRESULT disk_write(
    BYTE pdrv,          // Physical drive number to identify the drive
    const BYTE *buff,   // Data to be written
    LBA_t sector,       // Start sector in LBA
    UINT count          // Number of sectors to write
) {
    DRESULT result;

#if USE_BLOCK_CACHE
    if (count == 1) {
        result = block_cache_write_block(pdrv, sector, buff);
    }
    else {
        block_cache_flush(true);
        result = disk_write_no_cache(pdrv, buff, sector, count);
    }
#else
    result = disk_write_no_cache(pdrv, buff, sector, count);
#endif

    return result;
}

DRESULT disk_write_no_cache(
    BYTE pdrv,          // Physical drive number to identify the drive
    const BYTE *buff,   // Data to be written
    LBA_t sector,       // Start sector in LBA
    UINT count          // Number of sectors to write
) {
    switch (pdrv) {
        case DEV_SD:
            return sd_disk_write(DEV_SD, buff, sector, count);

#if MEDIUM == USB
            case DEV_USB:
            return usb_disk_write(DEV_USB, buff, sector, count);
#endif

        default:
            return RES_PARERR;
    }
}

#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(
    BYTE pdrv,  // Physical drive number to identify the drive
    BYTE cmd,   // Control code
    void *buff  // Buffer to send/receive control data
) {
#if USE_BLOCK_CACHE    
    //  Make sure the cache is flushed (ignores errors???)
    block_cache_flush(true);
#endif

    switch (pdrv) {
        case DEV_SD:
            return sd_disk_ioctl(DEV_SD, cmd, buff);

#if MEDIUM == USB
            case DEV_USB:
            return usb_disk_ioctl(DEV_USB, cmd, buff);
#endif

        default:
            return RES_PARERR;
    }
}
