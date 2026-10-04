/******************************************************************************
 * partresize.c  ---- ventoy part resize util
 *
 * Copyright (c) 2021, longpanda <admin@ventoy.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <linux/fs.h>
#include <limits.h>
#include <fat_filelib.h>
#include "vtoycli.h"

#ifndef BLKGETDISKSEQ
#define BLKGETDISKSEQ _IOR(0x12, 128, UINT64)
#endif

static int g_disk_fd = 0;
static UINT64 g_disk_offset = 0;
static int g_disk_io_error = 0;
static GUID g_ZeroGuid = {0};
static GUID g_WindowsDataPartGuid = { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

static int vtoy_disk_read(uint32 sector, uint8 *buffer, uint32 sector_count)
{
    UINT64 offset = sector * 512ULL;
    
    if (lseek(g_disk_fd, g_disk_offset + offset, SEEK_SET) != g_disk_offset + offset ||
        read(g_disk_fd, buffer, sector_count * 512) != sector_count * 512)
    {
        g_disk_io_error = 1;
        return 0;
    }
    
    return 1;
}

static int vtoy_disk_write(uint32 sector, uint8 *buffer, uint32 sector_count)
{
    UINT64 offset = sector * 512ULL;
    
    if (lseek(g_disk_fd, g_disk_offset + offset, SEEK_SET) != g_disk_offset + offset ||
        write(g_disk_fd, buffer, sector_count * 512) != sector_count * 512)
    {
        g_disk_io_error = 1;
        return 0;
    }
    
    return 1;
}


static int gpt_check(const char *disk)
{
    int fd = -1;
    int rc = 2;
    VTOY_GPT_INFO *pGPT = NULL;

    fd = open(disk, O_RDONLY);
    if (fd < 0)
    {
        printf("Failed to open %s\n", disk);
        goto out;
    }

    pGPT = malloc(sizeof(VTOY_GPT_INFO));
    if (NULL == pGPT)
    {
        goto out;
    }
    memset(pGPT, 0, sizeof(VTOY_GPT_INFO));
    
    if (read(fd, pGPT, sizeof(VTOY_GPT_INFO)) != sizeof(VTOY_GPT_INFO) ||
        pGPT->MBR.Byte55 != 0x55 || pGPT->MBR.ByteAA != 0xAA)
    {
        fprintf(stderr, "Cannot read a valid partition table from %s\n", disk);
        goto out;
    }

    if (pGPT->MBR.PartTbl[0].FsFlag == 0xEE && memcmp(pGPT->Head.Signature, "EFI PART", 8) == 0)
	{
        rc = 0;
	}
    else if (pGPT->MBR.PartTbl[0].FsFlag != 0xEE)
    {
        rc = 1;
    }

out:
    check_close(fd);        
    check_free(pGPT);
    return rc;
}

static int part_check(const char *disk)
{
    int i;
    int fd = -1;
    int rc = 0;
    int Index = 0;
    int Count = 0;
    int PartStyle = 0;
    UINT64 Part1Start;
    UINT64 Part1End;
    UINT64 NextPartStart;
    UINT64 DiskSizeInBytes;
    VTOY_GPT_INFO *pGPT = NULL;

    DiskSizeInBytes = get_disk_size_in_byte(disk);
    if (DiskSizeInBytes == 0)
    {
        printf("Failed to get disk size of %s\n", disk);
        goto out;
    }
    
    fd = open(disk, O_RDONLY);
    if (fd < 0)
    {
        printf("Failed to open %s\n", disk);
        goto out;
    }

    pGPT = malloc(sizeof(VTOY_GPT_INFO));
    if (NULL == pGPT)
    {
        goto out;
    }
    memset(pGPT, 0, sizeof(VTOY_GPT_INFO));
    
    read(fd, pGPT, sizeof(VTOY_GPT_INFO));

    if (pGPT->MBR.PartTbl[0].FsFlag == 0xEE && memcmp(pGPT->Head.Signature, "EFI PART", 8) == 0)
	{
        PartStyle = 1;
	}
	else
	{
        PartStyle = 0;
	}

    if (PartStyle == 0)
    {
		PART_TABLE *PartTbl = pGPT->MBR.PartTbl;

        for (Count = 0, i = 0; i < 4; i++)
        {
            if (PartTbl[i].SectorCount > 0)
            {
				printf("MBR Part%d SectorStart:%u SectorCount:%u\n", i + 1, PartTbl[i].StartSectorId, PartTbl[i].SectorCount);
                Count++;
            }
        }

		//We must have a free partition table for VTOYEFI partition
		if (Count >= 4)
		{
			printf("###[FAIL] 4 MBR partition tables are all used.\n");
			goto out;
		}

		if (PartTbl[0].SectorCount > 0)
		{
			Part1Start = PartTbl[0].StartSectorId;
			Part1End = PartTbl[0].SectorCount + Part1Start;
		}
		else
		{
			printf("###[FAIL] MBR Partition 1 is invalid\n");
			goto out;
		}

		Index = -1;
		NextPartStart = DiskSizeInBytes / 512ULL;
		for (i = 1; i < 4; i++)
		{
			if (PartTbl[i].SectorCount > 0 && NextPartStart > PartTbl[i].StartSectorId)
			{
				Index = i;
				NextPartStart = PartTbl[i].StartSectorId;
			}
		}

        NextPartStart *= 512ULL;
		printf("DiskSize:%llu NextPartStart:%llu(LBA:%llu) Index:%d\n", 
            DiskSizeInBytes, NextPartStart, NextPartStart / 512ULL, Index);
    }
    else
    {
		VTOY_GPT_PART_TBL *PartTbl = pGPT->PartTbl;

        for (Count = 0, i = 0; i < 128; i++)
        {
            if (memcmp(&(PartTbl[i].PartGuid), &g_ZeroGuid, sizeof(GUID)))
            {
				printf("GPT Part%d StartLBA:%llu LastLBA:%llu\n", i + 1, PartTbl[i].StartLBA, PartTbl[i].LastLBA);
                Count++;
            }
        }

		if (Count >= 128)
		{
			printf("###[FAIL] 128 GPT partition tables are all used.\n");
			goto out;
		}

		if (memcmp(&(PartTbl[0].PartGuid), &g_ZeroGuid, sizeof(GUID)))
		{
			Part1Start = PartTbl[0].StartLBA;
			Part1End = PartTbl[0].LastLBA + 1;
		}
		else
		{
			printf("###[FAIL] GPT Partition 1 is invalid\n");
			goto out;
		}

		Index = -1;
		NextPartStart = (pGPT->Head.PartAreaEndLBA + 1);
		for (i = 1; i < 128; i++)
		{
			if (memcmp(&(PartTbl[i].PartGuid), &g_ZeroGuid, sizeof(GUID)) && NextPartStart > PartTbl[i].StartLBA)
			{
				Index = i;
				NextPartStart = PartTbl[i].StartLBA;
			}
		}

		NextPartStart *= 512ULL;
		printf("DiskSize:%llu NextPartStart:%llu(LBA:%llu) Index:%d\n",
            DiskSizeInBytes, NextPartStart, NextPartStart / 512ULL, Index);
    }

	printf("Valid partition table (%s): Valid partition count:%d\n", (PartStyle == 0) ? "MBR" : "GPT", Count);

	//Partition 1 MUST start at 1MB
	Part1Start *= 512ULL;
	Part1End *= 512ULL;

	printf("Partition 1 start at: %llu %lluKB, end:%llu, NextPartStart:%llu\n", 
		Part1Start, Part1Start / 1024, Part1End, NextPartStart);
    if (Part1Start != SIZE_1MB)
    {
        printf("###[FAIL] Partition 1 is not start at 1MB\n");
        goto out;
    }


	//If we have free space after partition 1
	if (NextPartStart - Part1End >= VENTOY_EFI_PART_SIZE)
	{
		printf("Free space after partition 1 (%llu) is enough for VTOYEFI part\n", NextPartStart - Part1End);
		rc = 1;
	}
	else if (NextPartStart == Part1End)
	{
		printf("There is no free space after partition 1\n");
        rc = 2;
	}
	else
	{
		printf("The free space after partition 1 is not enough\n");
        rc = 2;
	}

out:
    check_close(fd);        
    check_free(pGPT);
    return rc;
}

static int secureboot_proc(char *disk, UINT64 part2start)
{
	int rc = 0;
	int size;
    int fd = -1;
	char *filebuf = NULL;
	void *file = NULL;

    fd = open(disk, O_RDWR);
    if (fd < 0)
    {
        printf("Failed to open %s\n", disk);
        return 1;
    }

    g_disk_fd = fd;
    g_disk_offset = part2start * 512ULL;
    g_disk_io_error = 0;

	fl_init();

	if (0 == fl_attach_media(vtoy_disk_read, vtoy_disk_write))
	{
		file = fl_fopen("/EFI/BOOT/grubx64_real.efi", "rb");
		printf("Open ventoy efi file %p\n", file);
		if (file)
		{
			fl_fseek(file, 0, SEEK_END);
			size = (int)fl_ftell(file);
			fl_fseek(file, 0, SEEK_SET);

			printf("ventoy x64 efi file size %d ...\n", size);

            if (size <= 0 || size > VENTOY_EFI_PART_SIZE)
            {
                fl_fclose(file);
                rc = 1;
                goto finish;
            }

			filebuf = (char *)malloc(size);
			if (!filebuf || fl_fread(filebuf, 1, size, file) != size)
			{
                fl_fclose(file);
                free(filebuf);
                rc = 1;
                goto finish;
			}

			fl_fclose(file);

			fl_remove("/EFI/BOOT/BOOTX64.EFI");
			fl_remove("/EFI/BOOT/grubx64.efi");
			fl_remove("/EFI/BOOT/grubx64_real.efi");
			fl_remove("/EFI/BOOT/MokManager.efi");
			fl_remove("/EFI/BOOT/mmx64.efi");
            fl_remove("/ENROLL_THIS_KEY_IN_MOKMANAGER.cer");

			file = fl_fopen("/EFI/BOOT/BOOTX64.EFI", "wb");
			printf("Open bootx64 efi file %p\n", file);
			if (file)
			{
				if (filebuf)
				{
					if (fl_fwrite(filebuf, 1, size, file) != size) rc = 1;
				}
				
				fl_fflush(file);
				fl_fclose(file);
			}
            else rc = 1;

			if (filebuf)
			{
				free(filebuf);
			}
		}

        file = fl_fopen("/EFI/BOOT/grubia32_real.efi", "rb");
        printf("Open ventoy ia32 efi file %p\n", file);
        if (file)
        {
            fl_fseek(file, 0, SEEK_END);
            size = (int)fl_ftell(file);
            fl_fseek(file, 0, SEEK_SET);

            printf("ventoy efi file size %d ...\n", size);

            if (size <= 0 || size > VENTOY_EFI_PART_SIZE)
            {
                fl_fclose(file);
                rc = 1;
                goto finish;
            }

            filebuf = (char *)malloc(size);
            if (!filebuf || fl_fread(filebuf, 1, size, file) != size)
            {
                fl_fclose(file);
                free(filebuf);
                rc = 1;
                goto finish;
            }

            fl_fclose(file);

            fl_remove("/EFI/BOOT/BOOTIA32.EFI");
            fl_remove("/EFI/BOOT/grubia32.efi");
            fl_remove("/EFI/BOOT/grubia32_real.efi");
            fl_remove("/EFI/BOOT/mmia32.efi");            

            file = fl_fopen("/EFI/BOOT/BOOTIA32.EFI", "wb");
            printf("Open bootia32 efi file %p\n", file);
            if (file)
            {
                if (filebuf)
                {
                    if (fl_fwrite(filebuf, 1, size, file) != size) rc = 1;
                }

                fl_fflush(file);
                fl_fclose(file);
            }
            else rc = 1;

            if (filebuf)
            {
                free(filebuf);
            }
        }

	}
	else
	{
		rc = 1;
	}

finish:
	fl_shutdown();
    if (fsync(fd) || g_disk_io_error) rc = 1;
    close(fd);

	return rc;
}

static int VentoyFillMBRLocation(UINT64 DiskSizeInBytes, UINT32 StartSectorId, UINT32 SectorCount, PART_TABLE *Table)
{
    UINT8 Head;
    UINT8 Sector;
    UINT8 nSector = 63;
    UINT8 nHead = 8;    
    UINT32 Cylinder;
    UINT32 EndSectorId;

    while (nHead != 0 && (DiskSizeInBytes / 512 / nSector / nHead) > 1024)
    {
        nHead = (UINT8)nHead * 2;
    }

    if (nHead == 0)
    {
        nHead = 255;
    }

    Cylinder = StartSectorId / nSector / nHead;
    Head = StartSectorId / nSector % nHead;
    Sector = StartSectorId % nSector + 1;

    Table->StartHead = Head;
    Table->StartSector = Sector;
    Table->StartCylinder = Cylinder;

    EndSectorId = StartSectorId + SectorCount - 1;
    Cylinder = EndSectorId / nSector / nHead;
    Head = EndSectorId / nSector % nHead;
    Sector = EndSectorId % nSector + 1;

    Table->EndHead = Head;
    Table->EndSector = Sector;
    Table->EndCylinder = Cylinder;

    Table->StartSectorId = StartSectorId;
    Table->SectorCount = SectorCount;

    return 0;
}


static int WriteDataToPhyDisk(int fd, UINT64 offset, void *buffer, int len)
{
    ssize_t wrlen;
    off_t newseek;
        
    newseek = lseek(fd, offset, SEEK_SET);
    if (newseek != offset)
    {
        printf("Failed to lseek %llu %lld %d\n", offset, (long long)newseek, errno);
        return 0;
    }
    
    wrlen = write(fd, buffer, len);
    if ((int)wrlen != len)
    {
        printf("Failed to write %d %d %d\n", len, (int)wrlen, errno);
        return 0;
    }
    
    return 1;
}

typedef struct VTOY_FRONT_IO
{
    const char *stage;
    const char *disk;
    const char *directory;
    int media_failure;
} VTOY_FRONT_IO;

static int front_io_failure(VTOY_FRONT_IO *io, const char *target, const char *operation,
    UINT64 offset, int len, int completed, int error, int disk)
{
    fprintf(stderr, "Front EFI %s: %s %s; requested byte offset=%llu length=%d, completed=%d",
        io->stage, operation, target, offset, len, completed);
    if (disk && len)
        fprintf(stderr, "; associated request LBA range=%llu-%llu", offset / 512, (offset + len - 1) / 512);
    if (error > 0)
        fprintf(stderr, "; errno=%d (%s).\n", error, strerror(error));
    else if (error == 0)
        fprintf(stderr, "; content mismatch within the requested range (not an OS I/O error).\n");
    else
        fprintf(stderr, "; unexpected EOF or zero I/O progress (no OS error reported).\n");
    if (!strcmp(operation, "fsync"))
        fprintf(stderr, "The range identifies the associated request; fsync does not locate a failing sector.\n");
    if (error <= 0 || error == EIO || error == ENODEV || error == ENXIO || error == ETIMEDOUT)
    {
        if (!io->media_failure)
        {
            if (disk)
                fprintf(stderr, "Stop installation. Preserve recovery files and rescue or clone existing data to a known-good disk first. Check the affected device and connection; do not repeatedly install or format it.\n");
            else
                fprintf(stderr, "Stop using this backup/package location and use known-good storage. Preserve existing recovery files. The target disk write state is reported separately below; do not format the target.\n");
        }
        io->media_failure = 1;
    }
    return 1;
}

static int front_io_all(VTOY_FRONT_IO *io, int fd, const char *target, UINT64 offset,
    void *buffer, int len, int save, int disk)
{
    int done = 0;
    ssize_t actual;
    while (done < len)
    {
        if (save)
            actual = pwrite(fd, (UINT8 *)buffer + done, len - done, offset + done);
        else
            actual = pread(fd, (UINT8 *)buffer + done, len - done, offset + done);
        if (actual < 0 && errno == EINTR) continue;
        if (actual <= 0)
            return front_io_failure(io, target, save ? "write" : "read", offset, len,
                done, actual < 0 ? errno : -1, disk);
        done += (int)actual;
    }
    return 0;
}

static int front_sync(VTOY_FRONT_IO *io, int fd, const char *target, UINT64 offset, int len, int disk)
{
    int rc;
    do { rc = fsync(fd); } while (rc && errno == EINTR);
    return rc ? front_io_failure(io, target, "fsync", offset, len, 0, errno, disk) : 0;
}

static int front_ioctl(VTOY_FRONT_IO *io, int fd, unsigned long request, void *value, const char *operation)
{
    int rc;
    do { rc = ioctl(fd, request, value); } while (rc && errno == EINTR);
    return rc ? front_io_failure(io, io->disk, operation, 0, 0, 0, errno, 1) : 0;
}

static int front_verify(VTOY_FRONT_IO *io, int fd, const char *target, UINT64 offset,
    const UINT8 *buffer, int len, int disk)
{
    int done, size;
    UINT8 check[65536];
    for (done = 0; done < len; done += size)
    {
        size = len - done > sizeof(check) ? sizeof(check) : len - done;
        if (front_io_all(io, fd, target, offset + done, check, size, 0, disk)) return 1;
        if (memcmp(check, buffer + done, size))
        {
            return front_io_failure(io, target, "readback", offset, len, done + size, 0, disk);
        }
    }
    return 0;
}

static int front_write_verify(VTOY_FRONT_IO *io, int fd, UINT64 offset, void *buffer, int len)
{
    if (front_io_all(io, fd, io->disk, offset, buffer, len, 1, 1) ||
        front_sync(io, fd, io->disk, offset, len, 1)) return 1;
    return front_verify(io, fd, io->disk, offset, buffer, len, 1);
}

/* Front installation supports canonical basic disks only. Validate before any write. */
static int front_read_table(VTOY_FRONT_IO *io, int fd, UINT64 sectors, VTOY_GPT_INFO *gpt, int installed, int report)
{
    int i, j, count, empty = 0, logical = 0;
    UINT32 crc;
    UINT64 start[128] = {0}, end[128] = {0};
    VTOY_BK_GPT_INFO backup;
    VTOY_GPT_HDR header;
    int isgpt;
#define FRONT_REJECT(reason) do { if (report) fprintf(stderr, "%s\n", reason); return -1; } while (0)

    if (front_ioctl(io, fd, BLKSSZGET, &logical, "get logical sector size") ||
        front_io_all(io, fd, io->disk, 0, gpt, sizeof(*gpt), 0, 1)) return -1;
    if (logical != 512 || sectors <= 67584 || gpt->MBR.Byte55 != 0x55 || gpt->MBR.ByteAA != 0xAA)
        FRONT_REJECT("Cannot read a valid 512-byte-sector disk and partition table.");

    isgpt = gpt->MBR.PartTbl[0].FsFlag == 0xEE;
    count = isgpt ? 128 : 4;
    if (isgpt)
    {
        header = gpt->Head;
        crc = header.Crc;
        header.Crc = 0;
        if (memcmp(header.Signature, "EFI PART", 8) || header.Length != 92 ||
            header.EfiStartLBA != 1 || header.EfiBackupLBA != sectors - 1 ||
            header.PartTblStartLBA != 2 || header.PartTblTotNum != 128 ||
            header.PartTblEntryLen != 128 || header.PartAreaStartLBA < 34 ||
            header.PartAreaStartLBA > 2048 || header.PartAreaEndLBA >= sectors - 33 ||
            header.PartAreaEndLBA < 67584 ||
            VtoyCrc32(&header, header.Length) != crc ||
            VtoyCrc32(gpt->PartTbl, sizeof(gpt->PartTbl)) != header.PartTblCrc)
            FRONT_REJECT("GPT primary header/table is invalid, or is not the supported 128-entry layout.");
        for (i = 1; i < 4; i++)
            if (gpt->MBR.PartTbl[i].FsFlag || gpt->MBR.PartTbl[i].SectorCount)
                FRONT_REJECT("Hybrid MBR/GPT disks are unsupported.");
        if (front_io_all(io, fd, io->disk, (sectors - 33) * 512, &backup, sizeof(backup), 0, 1)) return -1;
        if (memcmp(backup.PartTbl, gpt->PartTbl, sizeof(gpt->PartTbl)))
        {
            front_io_failure(io, io->disk, "compare GPT backup with primary", (sectors - 33) * 512,
                sizeof(gpt->PartTbl), sizeof(gpt->PartTbl), 0, 1);
            return -1;
        }
        header = backup.Head;
        crc = header.Crc;
        header.Crc = 0;
        if (memcmp(header.Signature, "EFI PART", 8) || header.Length != 92 ||
            header.EfiStartLBA != sectors - 1 || header.EfiBackupLBA != 1 ||
            header.PartTblStartLBA != sectors - 33 || header.PartTblTotNum != 128 ||
            header.PartTblEntryLen != 128 || header.PartTblCrc != gpt->Head.PartTblCrc ||
            header.PartAreaStartLBA != gpt->Head.PartAreaStartLBA ||
            header.PartAreaEndLBA != gpt->Head.PartAreaEndLBA ||
            memcmp(&header.DiskGuid, &gpt->Head.DiskGuid, sizeof(GUID)) ||
            VtoyCrc32(&header, header.Length) != crc)
            FRONT_REJECT("GPT backup header or CRC is invalid. Repair the table before retrying.");
    }
    for (i = 0; i < count; i++)
    {
        if (isgpt)
        {
            static const GUID ldmdata = {0xaf9b60a0, 0x1431, 0x4f62, {0xbc,0x68,0x33,0x11,0x71,0x4a,0x69,0xad}};
            static const GUID ldmmeta = {0x5808c8aa, 0x7e8f, 0x42e0, {0x85,0xd2,0xe1,0xe9,0x04,0x34,0xcf,0xb3}};
            static const GUID spaces = {0xe75caf8f, 0xf680, 0x4cee, {0xaf,0xa3,0xb0,0x01,0xe5,0x6e,0xfc,0x2d}};
            static const GUID spacesdata = {0xe7addcb4, 0xdc34, 0x4539, {0x9a,0x76,0xeb,0xbd,0x07,0xbe,0x6f,0x7e}};
            if (!memcmp(&gpt->PartTbl[i].PartType, &g_ZeroGuid, sizeof(GUID)))
            {
                empty++;
                continue;
            }
            if (!memcmp(&gpt->PartTbl[i].PartGuid, &g_ZeroGuid, sizeof(GUID)))
                FRONT_REJECT("A used GPT partition has no unique GUID. Repair the partition table before retrying.");
            for (j = 0; j < i; j++)
                if (end[j] && !memcmp(&gpt->PartTbl[i].PartGuid, &gpt->PartTbl[j].PartGuid, sizeof(GUID)))
                    FRONT_REJECT("Used GPT partitions share the same GUID. Repair the partition table before retrying.");
            if (!memcmp(&gpt->PartTbl[i].PartType, &ldmdata, sizeof(GUID)) ||
                !memcmp(&gpt->PartTbl[i].PartType, &ldmmeta, sizeof(GUID)) ||
                !memcmp(&gpt->PartTbl[i].PartType, &spaces, sizeof(GUID)) ||
                !memcmp(&gpt->PartTbl[i].PartType, &spacesdata, sizeof(GUID)))
                FRONT_REJECT("Dynamic disks and Storage Spaces are unsupported.");
            start[i] = gpt->PartTbl[i].StartLBA;
            end[i] = gpt->PartTbl[i].LastLBA + 1;
            if (end[i] == 0 || end[i] > gpt->Head.PartAreaEndLBA + 1)
                FRONT_REJECT("A GPT partition extends outside the usable disk area.");
        }
        else
        {
            PART_TABLE *p = gpt->MBR.PartTbl + i;
            if (!p->FsFlag && !p->SectorCount)
            {
                empty++;
                continue;
            }
            if (!p->FsFlag || !p->SectorCount || p->FsFlag == 0x05 || p->FsFlag == 0x0F ||
                p->FsFlag == 0x85 || p->FsFlag == 0x42 || p->FsFlag == 0xEE ||
                p->FsFlag == 0xD7 || p->FsFlag == 0xE7)
                FRONT_REJECT("Invalid MBR entry, extended partition, dynamic disk or Storage Spaces detected.");
            start[i] = p->StartSectorId;
            end[i] = start[i] + p->SectorCount;
        }
        if (start[i] < (installed ? 2048 : 67584))
        {
            if (report)
                fprintf(stderr, "Partition %d begins at LBA %llu (%.2f MiB). Front space after 1 MiB: %.2f MiB; required: 32 MiB, with every existing partition starting at/after LBA 67584 (33 MiB).\n",
                    i + 1, start[i], (double)start[i] / 2048,
                    start[i] > 2048 ? (double)(start[i] - 2048) / 2048 : 0.0);
            return -1;
        }
        if (end[i] <= start[i] || end[i] > sectors)
            FRONT_REJECT("A partition has invalid bounds or extends beyond the disk.");
        for (j = 0; j < i; j++)
            if (end[j] && start[i] < end[j] && start[j] < end[i])
                FRONT_REJECT("Existing partitions overlap. Repair the layout before retrying.");
    }
    if (installed && (!end[0] || !end[1]))
        FRONT_REJECT("The existing Ventoy layout is missing its data or EFI partition. Inspect the partition table before updating.");
    if (!installed && (!end[0] || !empty || start[0] < 67584))
        FRONT_REJECT("Partition 1 must contain data, and installation needs at least one unused partition-table entry.");
    if (!installed)
        for (i = 1; i < count; i++)
            if (end[i] && start[i] < end[0])
                FRONT_REJECT("Data partition 1 must be physically before the other partitions. Check partition numbering/order in your partition tool; do not format the disk.");
    if (!installed && !isgpt)
    {
        UINT8 type = gpt->MBR.PartTbl[0].FsFlag;
        if (type != 0x07 && type != 0x0B && type != 0x0C && type != 0x06 && type != 0x0E && type != 0x83)
            FRONT_REJECT("MBR partition 1 is not a supported basic data partition.");
    }
    if (!installed && isgpt && memcmp(&gpt->PartTbl[0].PartType, &g_WindowsDataPartGuid, sizeof(GUID)))
    {
        static const GUID linuxdata = {0x0fc63daf, 0x8483, 0x4772, {0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4}};
        if (memcmp(&gpt->PartTbl[0].PartType, &linuxdata, sizeof(GUID)))
            FRONT_REJECT("GPT partition 1 must be a Windows basic-data or Linux filesystem partition.");
    }
#undef FRONT_REJECT
    return isgpt;
}

typedef struct VTOY_FRONT_SNAPSHOT
{
    char magic[16];
    UINT64 bytes;
    UINT64 device;
    UINT64 diskseq;
    VTOY_GPT_INFO table;
} VTOY_FRONT_SNAPSHOT;

static int front_read_snapshot(VTOY_FRONT_IO *io, int fd, VTOY_FRONT_SNAPSHOT *snapshot)
{
    struct stat st;
    int logical = 0, rc;
    memset(snapshot, 0, sizeof(*snapshot));
    memcpy(snapshot->magic, "VTOY_FRONT_V1", 13);
    if (fstat(fd, &st)) return front_io_failure(io, io->disk, "fstat", 0, 0, 0, errno, 1);
    if (!S_ISBLK(st.st_mode))
        return front_io_failure(io, io->disk, "validate block device", 0, 0, 0, EINVAL, 1);
    if (front_ioctl(io, fd, BLKSSZGET, &logical, "get logical sector size") ||
        front_ioctl(io, fd, BLKGETSIZE64, &snapshot->bytes, "get disk size") ||
        front_io_all(io, fd, io->disk, 0, &snapshot->table, sizeof(snapshot->table), 0, 1)) return 1;
    if (logical != 512)
        return front_io_failure(io, io->disk, "validate 512-byte sectors", 0, 0, 0, EINVAL, 1);
    snapshot->device = (UINT64)st.st_rdev;
    /* Older kernels do not expose diskseq; the table, disk ID and size still bind the snapshot. */
    do { rc = ioctl(fd, BLKGETDISKSEQ, &snapshot->diskseq); } while (rc && errno == EINTR);
    if (rc)
    {
        if (errno != ENOTTY && errno != EINVAL && errno != ENOSYS)
            return front_io_failure(io, io->disk, "get disk sequence", 0, 0, 0, errno, 1);
        snapshot->diskseq = 0;
    }
    return 0;
}

static int front_file(VTOY_FRONT_IO *io, int dirfd, const char *name, void *buffer, int len, int save)
{
    int rc = 1, fd, pathlen;
    char path[PATH_MAX];
    struct stat st;
    pathlen = snprintf(path, sizeof(path), "%s/%s", io->directory, name);
    if (pathlen < 0 || pathlen >= sizeof(path))
        return front_io_failure(io, name, "resolve backup file", 0, len, 0, ENAMETOOLONG, 0);
    fd = openat(dirfd, name, (save ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY) | O_NOFOLLOW, 0600);
    if (fd < 0) return front_io_failure(io, path, "open", 0, len, 0, errno, 0);
    if (save)
    {
        if (front_io_all(io, fd, path, 0, buffer, len, 1, 0) ||
            front_sync(io, fd, path, 0, len, 0) ||
            front_verify(io, fd, path, 0, buffer, len, 0)) goto out;
    }
    else
    {
        if (fstat(fd, &st))
        {
            front_io_failure(io, path, "fstat", 0, len, 0, errno, 0);
            goto out;
        }
        if (!S_ISREG(st.st_mode) || st.st_size != len)
        {
            front_io_failure(io, path, "validate file type and length", 0, len, 0, EINVAL, 0);
            goto out;
        }
        if (front_io_all(io, fd, path, 0, buffer, len, 0, 0)) goto out;
    }
    rc = 0;
out:
    if (close(fd))
    {
        front_io_failure(io, path, "close", 0, len, 0, errno, 0);
        rc = 1;
    }
    return rc;
}

static int front_snapshot_save(char *disk, char *directory)
{
    int fd = -1, dirfd = -1, rc = 1, i, style;
    VTOY_FRONT_IO io = {"disk snapshot", disk, directory, 0};
    VTOY_FRONT_SNAPSHOT snapshot;
    fd = open(disk, O_RDONLY);
    if (fd < 0)
    {
        front_io_failure(&io, disk, "open", 0, 0, 0, errno, 1);
        goto out;
    }
    dirfd = open(directory, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
    {
        front_io_failure(&io, directory, "open recovery directory", 0, 0, 0, errno, 0);
        goto out;
    }
    if (front_read_snapshot(&io, fd, &snapshot) ||
        front_file(&io, dirfd, "snapshot.bin", &snapshot, sizeof(snapshot), 1) ||
        front_sync(&io, dirfd, directory, 0, 0, 0)) goto out;
    printf("Capacity: %.2f GiB (%llu bytes); current partition table: %s\n",
        (double)snapshot.bytes / (1024 * 1024 * 1024), snapshot.bytes,
        snapshot.table.MBR.PartTbl[0].FsFlag == 0xEE ? "GPT" : "MBR");
    style = snapshot.table.MBR.PartTbl[0].FsFlag == 0xEE;
    for (i = 0; i < 2; i++)
    {
        UINT64 start = style ? snapshot.table.PartTbl[i].StartLBA : snapshot.table.MBR.PartTbl[i].StartSectorId;
        if (start)
            printf("Current partition %d starts at LBA %llu (%.2f MiB).\n", i + 1, start, (double)start / 2048);
    }
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "Snapshot capture failed; the target disk was NOT written. Preparation directory: %s\n", directory);
    check_close(fd);
    check_close(dirfd);
    return rc;
}

static int front_partition(char *disk)
{
    int fd, style, i;
    VTOY_FRONT_IO io = {"front partition precheck", disk, NULL, 0};
    UINT64 bytes = 0;
    UINT64 first, available;
    VTOY_GPT_INFO gpt;
    fd = open(disk, O_RDONLY);
    if (fd < 0)
    {
        front_io_failure(&io, disk, "open", 0, 0, 0, errno, 1);
        return 1;
    }
    if (front_ioctl(&io, fd, BLKGETSIZE64, &bytes, "get disk size"))
    {
        close(fd);
        return 1;
    }
    style = front_read_table(&io, fd, bytes / 512, &gpt, 0, 1);
    close(fd);
    if (style < 0) return 1;
    first = style ? gpt.PartTbl[0].StartLBA : gpt.MBR.PartTbl[0].StartSectorId;
    available = first;
    for (i = 1; i < (style ? 128 : 4); i++)
    {
        UINT64 start = style ? gpt.PartTbl[i].StartLBA : gpt.MBR.PartTbl[i].StartSectorId;
        int used = style ? memcmp(&gpt.PartTbl[i].PartType, &g_ZeroGuid, sizeof(GUID)) != 0 : gpt.MBR.PartTbl[i].SectorCount != 0;
        if (used && start < available) available = start;
    }
    printf("VTOY_FRONT_EFI_V1: %s; data begins at LBA %llu (%.2f MiB).\n",
        style ? "GPT" : "MBR", first, (double)first / 2048);
    printf("Available front space after the reserved 1 MiB: %.2f MiB.\n", (double)(available - 2048) / 2048);
    printf("Required empty range: 1-33 MiB (32 MiB). All partition bounds and available table entries checked.\n");
    return 0;
}

static int front_efi_index(VTOY_GPT_INFO *gpt, int style)
{
    static const UINT8 boot_signature[16] = {
        0x56,0x54,0x00,0x47,0x65,0x00,0x48,0x44,0x00,0x52,0x64,0x00,0x20,0x45,0x72,0x0D
    };
    int efi, data;
    UINT64 start[2], size[2];
    if (gpt->MBR.BootCode[0] != 0xEB || gpt->MBR.BootCode[1] != 0x63 ||
        gpt->MBR.BootCode[2] != 0x90 || memcmp(gpt->MBR.BootCode + 0x190, boot_signature, sizeof(boot_signature)))
        return -1;
    for (efi = 0; efi < 2; efi++)
    {
        start[efi] = style ? gpt->PartTbl[efi].StartLBA : gpt->MBR.PartTbl[efi].StartSectorId;
        size[efi] = style ? gpt->PartTbl[efi].LastLBA - start[efi] + 1 : gpt->MBR.PartTbl[efi].SectorCount;
    }
    efi = start[0] == 2048 && size[0] == 65536 && start[1] >= 67584 &&
        (style ? (gpt->PartTbl[0].Name[0] == 'V' && gpt->PartTbl[0].Name[1] == 'T') :
                 (gpt->MBR.PartTbl[0].FsFlag == 0xEF)) ? 0 : 1;
    data = 1 - efi;
    if (size[efi] != 65536 || !size[data] ||
        (efi == 1 && (start[data] != 2048 || start[data] + size[data] != start[efi]))) return -1;
    if (style)
    {
        const char *name = "VTOYEFI";
        int i;
        for (i = 0; i < 8; i++) if (gpt->PartTbl[efi].Name[i] != name[i]) return -1;
    }
    else if (gpt->MBR.PartTbl[efi].FsFlag != 0xEF) return -1;
    return efi;
}

static int ventoy_layout(char *disk)
{
    int fd, style, efi = -1;
    UINT64 bytes = 0;
    VTOY_FRONT_IO io = {"existing partition layout", disk, NULL, 0};
    VTOY_GPT_INFO gpt;
    fd = open(disk, O_RDONLY);
    if (fd < 0) return front_io_failure(&io, disk, "open", 0, 0, 0, errno, 1);
    if (front_ioctl(&io, fd, BLKGETSIZE64, &bytes, "get disk size"))
    {
        close(fd);
        return 1;
    }
    style = front_read_table(&io, fd, bytes / 512, &gpt, 1, 0);
    if (style >= 0) efi = front_efi_index(&gpt, style);
    close(fd);
    if (efi < 0) return 1;
    printf("%d\n", efi + 1);
    return 0;
}

static int front_commit_table(VTOY_FRONT_IO *io, int fd, UINT64 bytes, VTOY_GPT_INFO *gpt, int style)
{
    int i, freeidx;
    VTOY_GPT_HDR backup;
    const char *name = "VTOYEFI";
    if (style)
    {
        for (freeidx = 1; freeidx < 128; freeidx++)
            if (!memcmp(&gpt->PartTbl[freeidx].PartType, &g_ZeroGuid, sizeof(GUID))) break;
        for (i = freeidx; i > 0; i--) gpt->PartTbl[i] = gpt->PartTbl[i - 1];
        memset(&gpt->PartTbl[0], 0, sizeof(gpt->PartTbl[0]));
        gpt->PartTbl[0].PartType = g_WindowsDataPartGuid;
        ventoy_gen_preudo_uuid(&gpt->PartTbl[0].PartGuid);
        gpt->PartTbl[0].StartLBA = 2048;
        gpt->PartTbl[0].LastLBA = 67583;
        gpt->PartTbl[0].Attr = VENTOY_EFI_PART_ATTR;
        for (i = 0; name[i]; i++) gpt->PartTbl[0].Name[i] = name[i];
        gpt->Head.PartTblCrc = VtoyCrc32(gpt->PartTbl, sizeof(gpt->PartTbl));
        gpt->Head.Crc = 0;
        gpt->Head.Crc = VtoyCrc32(&gpt->Head, gpt->Head.Length);
        backup = gpt->Head;
        backup.EfiStartLBA = gpt->Head.EfiBackupLBA;
        backup.EfiBackupLBA = 1;
        backup.PartTblStartLBA = backup.EfiStartLBA - 32;
        backup.Crc = 0;
        backup.Crc = VtoyCrc32(&backup, backup.Length);
        io->stage = "GPT backup partition table write/readback";
        if (front_write_verify(io, fd, backup.PartTblStartLBA * 512, gpt->PartTbl, sizeof(gpt->PartTbl))) return 1;
        io->stage = "GPT backup header write/readback";
        if (front_write_verify(io, fd, backup.EfiStartLBA * 512, &backup, sizeof(backup))) return 1;
        io->stage = "GPT primary partition table write/readback";
        if (front_write_verify(io, fd, 0, gpt, sizeof(*gpt))) return 1;
    }
    else
    {
        for (freeidx = 1; freeidx < 4; freeidx++)
            if (!gpt->MBR.PartTbl[freeidx].SectorCount) break;
        for (i = freeidx; i > 0; i--) gpt->MBR.PartTbl[i] = gpt->MBR.PartTbl[i - 1];
        memset(&gpt->MBR.PartTbl[0], 0, sizeof(PART_TABLE));
        VentoyFillMBRLocation(bytes, 2048, 65536, &gpt->MBR.PartTbl[0]);
        gpt->MBR.PartTbl[0].FsFlag = 0xEF;
        gpt->MBR.PartTbl[0].Active = 0x80;
        gpt->MBR.PartTbl[1].Active = 0;
        io->stage = "MBR partition table write/readback";
        if (front_write_verify(io, fd, 0, &gpt->MBR, 512)) return 1;
    }
    return 0;
}

static int front_transaction(char *disk, char *mode, char *directory)
{
    int fd = -1, dirfd = -1, rc = 1, written = 0, backup_ready = 0;
    int style, update, core_start, core_len, textlen, tail_len, markerfd = -1, marker_created = 0;
    UINT8 boot[512], tail[33 * 512];
    UINT8 *front = NULL, *core = NULL, *efi = NULL;
    VTOY_FRONT_SNAPSHOT expected, current;
    VTOY_GPT_INFO gpt, verify;
    char fullpath[PATH_MAX], marker_path[PATH_MAX], details[1024];
    VTOY_FRONT_IO io = {"preparation", disk, fullpath, 0};

    update = !strcmp(mode, "update");
    if (!update && strcmp(mode, "install")) return 1;
    if (!realpath(directory, fullpath))
    {
        front_io_failure(&io, directory, "resolve recovery directory", 0, 0, 0, errno, 0);
        fprintf(stderr, "Cannot resolve recovery directory. Target disk was NOT written.\n");
        return 1;
    }
    textlen = snprintf(marker_path, sizeof(marker_path), "%s/backup.ready", fullpath);
    if (textlen < 0 || textlen >= sizeof(marker_path))
    {
        front_io_failure(&io, fullpath, "resolve backup marker", 0, 0, 0, ENAMETOOLONG, 0);
        goto out;
    }
    dirfd = open(fullpath, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
    {
        front_io_failure(&io, fullpath, "open recovery directory", 0, 0, 0, errno, 0);
        goto out;
    }
    if (front_file(&io, dirfd, "snapshot.bin", &expected, sizeof(expected), 0)) goto out;
    front = malloc(33 * SIZE_1MB);
    core = malloc(2047 * 512);
    efi = malloc(VENTOY_EFI_PART_SIZE);
    if (!front || !core || !efi)
    {
        fprintf(stderr, "Not enough memory to prepare the complete transaction.\n");
        goto out;
    }
    if (front_file(&io, dirfd, "boot.img", boot, sizeof(boot), 0) ||
        front_file(&io, dirfd, "core.img", core, 2047 * 512, 0) ||
        front_file(&io, dirfd, "efi.img", efi, VENTOY_EFI_PART_SIZE, 0)) goto out;

    io.stage = "exclusive disk access";
    fd = open(disk, O_RDWR | O_EXCL);
    if (fd < 0)
    {
        int error = errno;
        front_io_failure(&io, disk, "open exclusive", 0, 0, 0, error, 1);
        if (error == EBUSY)
            fprintf(stderr, "Unmount the target partitions, disable swap and stop RAID/device-mapper users.\n");
        goto out;
    }
    io.stage = "disk identity and partition snapshot validation";
    if (front_read_snapshot(&io, fd, &current)) goto out;
    if (memcmp(&current, &expected, sizeof(current)))
    {
        fprintf(stderr, "The disk identity, size or partition table changed since confirmation.\n");
        goto out;
    }
    style = front_read_table(&io, fd, current.bytes / 512, &gpt, update, 1);
    if (style < 0 || (update && front_efi_index(&gpt, style) != 0)) goto out;

    io.stage = "persistent recovery backup";
    tail_len = style ? sizeof(tail) : 0;
    if (front_io_all(&io, fd, disk, 0, front, 33 * SIZE_1MB, 0, 1) ||
        (tail_len && front_io_all(&io, fd, disk, current.bytes - tail_len, tail, tail_len, 0, 1))) goto out;
    if (front_file(&io, dirfd, "front-33MiB.bin", front, 33 * SIZE_1MB, 1) ||
        front_file(&io, dirfd, "tail-33sectors.bin", tail, tail_len, 1)) goto out;
    textlen = snprintf(details, sizeof(details),
        "disk=%s\nbytes=%llu\ndevice=%llu\ndiskseq=%llu\noperation=%s\nfront_crc32=%08x\ntail_bytes=%d\ntail_crc32=%08x\n",
        disk, current.bytes, current.device, current.diskseq, mode,
        VtoyCrc32(front, 33 * SIZE_1MB), tail_len, VtoyCrc32(tail, tail_len));
    if (textlen < 0 || textlen >= sizeof(details) || front_file(&io, dirfd, "disk.txt", details, textlen, 1) ||
        front_sync(&io, dirfd, fullpath, 0, 0, 0)) goto out;
    markerfd = openat(dirfd, "backup.ready", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (markerfd < 0)
    {
        front_io_failure(&io, marker_path, "create backup marker", 0, 0, 0, errno, 0);
        goto out;
    }
    marker_created = 1;
    if (front_sync(&io, markerfd, marker_path, 0, 0, 0)) goto out;
    if (close(markerfd))
    {
        markerfd = -1;
        front_io_failure(&io, marker_path, "close backup marker", 0, 0, 0, errno, 0);
        goto out;
    }
    markerfd = -1;
    if (front_sync(&io, dirfd, fullpath, 0, 0, 0)) goto out;
    backup_ready = 1;
    printf("Complete recovery backup: %s\n", fullpath);
    fflush(stdout);

    core_start = style ? 34 : 1;
    core_len = (2048 - core_start) * 512;
    if (style)
    {
        boot[92] = 0x22;
        core[500] = 0x23;
    }
    if (update)
    {
        memcpy(boot + 384, front + 384, 16);
        memcpy(core + (2040 - core_start) * 512, front + 2040 * 512, 8 * 512);
    }
    else
        ventoy_gen_preudo_uuid(boot + 384);
    io.stage = "final snapshot validation before writing";
    if (front_read_snapshot(&io, fd, &current)) goto out;
    if (memcmp(&current, &expected, sizeof(current)))
    {
        fprintf(stderr, "The disk changed while preparing the backup.\n");
        goto out;
    }

    /* From here onward no files are created, and every disk access uses the same claimed device. */
    written = 1;
    io.stage = "EFI write/readback";
    if (front_write_verify(&io, fd, 2048 * 512ULL, efi, VENTOY_EFI_PART_SIZE)) goto out;
    io.stage = "BIOS core write/readback";
    if (front_write_verify(&io, fd, core_start * 512ULL, core, core_len)) goto out;
    io.stage = "boot code write/readback";
    if (front_write_verify(&io, fd, 0, boot, 440)) goto out;
    memcpy(gpt.MBR.BootCode, boot, 440);
    if (!update)
    {
        io.stage = "partition table commit";
        if (front_commit_table(&io, fd, current.bytes, &gpt, style)) goto out;
    }
    io.stage = "final partition verification";
    if (front_read_table(&io, fd, current.bytes / 512, &verify, 1, 1) != style) goto out;
    if (memcmp(&gpt, &verify, style ? sizeof(gpt) : sizeof(gpt.MBR)) || front_efi_index(&verify, style) != 0)
    {
        front_io_failure(&io, disk, "compare final partition layout", 0,
            style ? sizeof(gpt) : sizeof(gpt.MBR), style ? sizeof(gpt) : sizeof(gpt.MBR), 0, 1);
        goto out;
    }
    if (ioctl(fd, BLKRRPART))
        printf("Disk write verified, but the kernel could not refresh its partition table. Safely reconnect the disk before use.\n");
    printf("Front EFI %s finished and verified. Recovery files: %s\n", mode, fullpath);
    rc = 0;
out:
    if (markerfd >= 0 && close(markerfd))
        front_io_failure(&io, marker_path, "close failed backup marker", 0, 0, 0, errno, 0);
    if (marker_created && !backup_ready)
    {
        int cleanup_failed = 0;
        if (unlinkat(dirfd, "backup.ready", 0))
        {
            front_io_failure(&io, marker_path, "remove failed backup marker", 0, 0, 0, errno, 0);
            cleanup_failed = 1;
        }
        if (front_sync(&io, dirfd, fullpath, 0, 0, 0)) cleanup_failed = 1;
        if (cleanup_failed)
            fprintf(stderr, "The backup.ready marker in %s is NOT trustworthy. This operation did NOT write the target disk. Keep all recovery files for independent verification.\n", fullpath);
    }
    if (rc)
    {
        fprintf(stderr, "Front EFI failed during %s.\n", io.stage);
        if (written)
        {
            fprintf(stderr, "The target disk WAS written and may need recovery. Complete backup: %s\nDo not format or run a normal install. Keep this folder and identify the original disk using disk.txt before restoring.\n", fullpath);
            rc = 3;
        }
        else
            fprintf(stderr, "The target disk was NOT written. %s: %s\nKeep these files and resolve the reported cause before any further disk operation.\n",
                backup_ready ? "Complete recovery backup" : "Preparation files (backup may be incomplete)", fullpath);
    }
    check_close(fd);
    check_close(dirfd);
    check_free(front);
    check_free(core);
    check_free(efi);
    return rc;
}

static int VentoyFillBackupGptHead(VTOY_GPT_INFO *pInfo, VTOY_GPT_HDR *pHead)
{
    UINT64 LBA;
    UINT64 BackupLBA;

    memcpy(pHead, &pInfo->Head, sizeof(VTOY_GPT_HDR));

    LBA = pHead->EfiStartLBA;
    BackupLBA = pHead->EfiBackupLBA;
    
    pHead->EfiStartLBA = BackupLBA;
    pHead->EfiBackupLBA = LBA;
    pHead->PartTblStartLBA = BackupLBA + 1 - 33;

    pHead->Crc = 0;
    pHead->Crc = VtoyCrc32(pHead, pHead->Length);

    return 0;
}

static int update_part_table(char *disk, UINT64 part2start)
{
    int i;
    int j;
    int fd = -1;
    int rc = 1;
    int PartStyle = 0;
    ssize_t len = 0;
    UINT64 DiskSizeInBytes;
    VTOY_GPT_INFO *pGPT = NULL;
    VTOY_GPT_HDR *pBack = NULL;

    DiskSizeInBytes = get_disk_size_in_byte(disk);
    if (DiskSizeInBytes == 0)
    {
        printf("Failed to get disk size of %s\n", disk);
        goto out;
    }
    
    fd = open(disk, O_RDWR);
    if (fd < 0)
    {
        printf("Failed to open %s\n", disk);
        goto out;
    }

    pGPT = malloc(sizeof(VTOY_GPT_INFO) + sizeof(VTOY_GPT_HDR));
    if (NULL == pGPT)
    {
        goto out;
    }
    memset(pGPT, 0, sizeof(VTOY_GPT_INFO) + sizeof(VTOY_GPT_HDR));

    pBack = (VTOY_GPT_HDR *)(pGPT + 1);
    
    len = read(fd, pGPT, sizeof(VTOY_GPT_INFO));
    if (len != (ssize_t)sizeof(VTOY_GPT_INFO))
    {
        printf("Failed to read partition table %d err:%d\n", (int)len, errno);
        goto out;
    }

    if (pGPT->MBR.PartTbl[0].FsFlag == 0xEE && memcmp(pGPT->Head.Signature, "EFI PART", 8) == 0)
	{
        PartStyle = 1;
	}
	else
	{
        PartStyle = 0;
	}

    if (PartStyle == 0)
    {
		PART_TABLE *PartTbl = pGPT->MBR.PartTbl;

        for (i = 1; i < 4; i++)
        {
            if (PartTbl[i].SectorCount == 0)
            {
				break;
            }
        }

		if (i >= 4)
		{
			printf("###[FAIL] Can not find a free MBR partition table.\n");
			goto out;
		}

        for (j = i - 1; j > 0; j--)
		{
			printf("Move MBR partition table %d --> %d\n", j + 1, j + 2);
			memcpy(PartTbl + (j + 1), PartTbl + j, sizeof(PART_TABLE));
		}

        memset(PartTbl + 1, 0, sizeof(PART_TABLE));
        VentoyFillMBRLocation(DiskSizeInBytes, (UINT32)part2start, VENTOY_EFI_PART_SIZE / 512, PartTbl + 1);
		PartTbl[1].Active = 0x00;
		PartTbl[1].FsFlag = 0xEF; // EFI System Partition

        PartTbl[0].Active = 0x80; // bootable
        PartTbl[0].SectorCount = (UINT32)part2start - 2048;
        
        if (!WriteDataToPhyDisk(fd, 0, &(pGPT->MBR), 512))
		{
			printf("MBR write MBR failed\n");
			goto out;
		}

        fsync(fd);
        printf("MBR update partition table success.\n");
        rc = 0;
    }
    else
    {
		VTOY_GPT_PART_TBL *PartTbl = pGPT->PartTbl;

        for (i = 1; i < 128; i++)
        {
            if (memcmp(&(PartTbl[i].PartGuid), &g_ZeroGuid, sizeof(GUID)) == 0)
            {
				break;
            }
        }

		if (i >= 128)
		{
			printf("###[FAIL] Can not find a free GPT partition table.\n");
			goto out;
		}

		for (j = i - 1; j > 0; j--)
		{
			printf("Move GPT partition table %d --> %d\n", j + 1, j + 2);
			memcpy(PartTbl + (j + 1), PartTbl + j, sizeof(VTOY_GPT_PART_TBL));
		}

        // to fix windows issue
        memset(PartTbl + 1, 0, sizeof(VTOY_GPT_PART_TBL));
		memcpy(&(PartTbl[1].PartType), &g_WindowsDataPartGuid, sizeof(GUID));
		ventoy_gen_preudo_uuid(&(PartTbl[1].PartGuid));

        PartTbl[0].LastLBA = part2start - 1;

        PartTbl[1].StartLBA = PartTbl[0].LastLBA + 1;
		PartTbl[1].LastLBA = PartTbl[1].StartLBA + VENTOY_EFI_PART_SIZE / 512 - 1;
		PartTbl[1].Attr = VENTOY_EFI_PART_ATTR;
        PartTbl[1].Name[0] = 'V';
        PartTbl[1].Name[1] = 'T';
        PartTbl[1].Name[2] = 'O';
        PartTbl[1].Name[3] = 'Y';
        PartTbl[1].Name[4] = 'E';
        PartTbl[1].Name[5] = 'F';
        PartTbl[1].Name[6] = 'I';
        PartTbl[1].Name[7] = 0;

		//Update CRC
		pGPT->Head.PartTblCrc = VtoyCrc32(pGPT->PartTbl, sizeof(pGPT->PartTbl));
		pGPT->Head.Crc = 0;
		pGPT->Head.Crc = VtoyCrc32(&(pGPT->Head), pGPT->Head.Length);

		printf("pGPT->Head.EfiStartLBA=%llu\n", pGPT->Head.EfiStartLBA);
		printf("pGPT->Head.EfiBackupLBA=%llu\n", pGPT->Head.EfiBackupLBA);

		VentoyFillBackupGptHead(pGPT, pBack);
		if (!WriteDataToPhyDisk(fd, pGPT->Head.EfiBackupLBA * 512, pBack, 512))
		{
			printf("GPT write backup head failed\n");
			goto out;
		}

		if (!WriteDataToPhyDisk(fd, (pGPT->Head.EfiBackupLBA - 32) * 512, pGPT->PartTbl, 512 * 32))
		{
			printf("GPT write backup partition table failed\n");
			goto out;
		}

		if (!WriteDataToPhyDisk(fd, 0, pGPT, 512 * 34))
		{
			printf("GPT write MBR & Main partition table failed\n");
			goto out;
		}

        fsync(fd);
        printf("GPT update partition table success.\n");
        rc = 0;
    }

out:
    check_close(fd);        
    check_free(pGPT);
    return rc;
}

int partresize_main(int argc, char **argv)
{
    UINT64 sector;
    
    if (argc == 2 && strcmp(argv[1], "--front-efi-api") == 0)
    {
        puts("VTOY_FRONT_EFI_SAFE_V3");
        return 0;
    }
    if (argc != 3 && argc != 4 && argc != 5)
    {
        printf("usage: partresize -C/-L/-t DISK | -Q DISK DIRECTORY | -W DISK install/update DIRECTORY\n");
        return 1;
    }

    if (argc == 3 && strcmp(argv[1], "-C") == 0)
    {
        return front_partition(argv[2]);
    }
    else if (argc == 4 && strcmp(argv[1], "-Q") == 0)
    {
        return front_snapshot_save(argv[2], argv[3]);
    }
    else if (argc == 5 && strcmp(argv[1], "-W") == 0)
    {
        return front_transaction(argv[2], argv[3], argv[4]);
    }
    else if (argc == 3 && strcmp(argv[1], "-L") == 0)
    {
        return ventoy_layout(argv[2]);
    }
    else if (strcmp(argv[1], "-c") == 0)
    {
        return part_check(argv[2]);
    }
    else if (strcmp(argv[1], "-s") == 0)
    {
        if (argc != 4) return 1;
        sector = strtoull(argv[3], NULL, 10);
        return secureboot_proc(argv[2], sector);
    }
    else if (strcmp(argv[1], "-p") == 0)
    {
        if (argc != 4) return 1;
        sector = strtoull(argv[3], NULL, 10);    
        return update_part_table(argv[2], sector);
    }
    else if (strcmp(argv[1], "-t") == 0)
    {
        return gpt_check(argv[2]);
    }
    else
    {
        return 1;
    }
}

