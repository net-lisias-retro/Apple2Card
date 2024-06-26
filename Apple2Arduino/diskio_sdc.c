/*-----------------------------------------------------------------------*/
/* Low level disk I/O module glue functions         (C)ChaN, 2016        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"			/* Obtains integer types for FatFs */
#include "diskio_sdc.h"		/* FatFs lower layer API */
#include "mmc_avr.h"	/* Header file of existing SD control module */

/*-----------------------------------------------------------------------*/
/* Prepare Drive Access                                                  */
/*-----------------------------------------------------------------------*/

static void disk_prep (
	BYTE pdrv		/* Physical drive number to identify the drive */
)
{
  if (slotno != pdrv)
    mmc_wait_busy_spi();  // make sure the other slot is not busy/blocking the SPI
  slotno = pdrv;        // remember the current drive
}

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/


DSTATUS disk_status (
	BYTE pdrv		/* Physical drive number to identify the drive */
)
{
  disk_prep(pdrv);
  return mmc_disk_status();
}



/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive number to identify the drive */
)
{
  disk_prep(pdrv);
  return mmc_disk_initialize();
}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (
	BYTE pdrv,		/* Physical drive number to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Sector address in LBA */
	UINT count		/* Number of sectors to read */
)
{
  disk_prep(pdrv);
  return mmc_disk_read(buff, sector, count);
}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if !FF_FS_READONLY
DRESULT disk_write (
	BYTE pdrv,			/* Physical drive number to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Sector address in LBA */
	UINT count			/* Number of sectors to write */
)
{
  disk_prep(pdrv);
  return mmc_disk_write(buff, sector, count);
}
#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

#if _USE_IOCTL
DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive number (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
  disk_prep(pdrv);
  return mmc_disk_ioctl(cmd, buff);
}
#endif
