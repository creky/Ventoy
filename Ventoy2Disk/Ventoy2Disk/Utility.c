/******************************************************************************
 * Utility.c
 *
 * Copyright (c) 2021, longpanda <admin@ventoy.net>
 * Copyright (c) 2011-2020, Pete Batard <pete@akeo.ie>
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
#include <Windows.h>
#include <wincrypt.h>
#include <limits.h>
#include "Ventoy2Disk.h"

void VentoyFrontRecordIoError(VTOY_FRONT_IO_ERROR *Error, BOOL IsDisk, const CHAR *Target,
    const CHAR *Phase, const CHAR *Operation, UINT64 Offset, DWORD Requested,
    DWORD Transferred, DWORD SystemError, BOOL Mismatch)
{
    BOOL nonMedia = FALSE;
    if (!Error || Error->Valid) return;
    switch (SystemError)
    {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        nonMedia = TRUE;
        break;
    }
    Error->Valid = TRUE;
    Error->IsDisk = IsDisk;
    Error->StopWrites = Mismatch || !nonMedia;
    Error->Mismatch = Mismatch;
    Error->SystemError = SystemError;
    Error->Requested = Requested;
    Error->Transferred = Transferred;
    Error->Offset = Offset;
    safe_strcpy(Error->Phase, Phase);
    safe_strcpy(Error->Operation, Operation);
    safe_strcpy(Error->Target, Target);
    Log("Front EFI I/O failure: target=%s phase=%s operation=%s offset=%llu requested=%lu transferred=%lu system_error=%lu mismatch=%d stop_writes=%d",
        Target, Phase, Operation, Offset, Requested, Transferred, SystemError, Mismatch, Error->StopWrites);
    if (IsDisk && Requested)
        Log("Failed request covers LBA %llu through %llu; this is the requested range, not confirmed bad sectors.",
            Offset / 512, (Offset + Requested - 1) / 512);
}

BOOL VentoyFrontRead(HANDLE Handle, VTOY_FRONT_IO_ERROR *Error, BOOL IsDisk, const CHAR *Target,
    const CHAR *Phase, UINT64 Offset, VOID *Buffer, DWORD Length)
{
    LARGE_INTEGER pos, actual;
    DWORD transferred = 0, code;
    BOOL ok;
    pos.QuadPart = Offset;
    if (!SetFilePointerEx(Handle, pos, &actual, FILE_BEGIN))
    {
        code = GetLastError();
        VentoyFrontRecordIoError(Error, IsDisk, Target, Phase, "seek", Offset, Length, 0, code, FALSE);
        return FALSE;
    }
    if (actual.QuadPart != pos.QuadPart)
    {
        VentoyFrontRecordIoError(Error, IsDisk, Target, Phase, "seek mismatch", Offset, 0, 0, ERROR_SUCCESS, FALSE);
        return FALSE;
    }
    ok = ReadFile(Handle, Buffer, Length, &transferred, NULL);
    code = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok || transferred != Length)
    {
        VentoyFrontRecordIoError(Error, IsDisk, Target, Phase, "read", Offset, Length, transferred, code, FALSE);
        return FALSE;
    }
    return TRUE;
}

BOOL VentoyCheckFrontLayout(const VTOY_GPT_INFO *Gpt, UINT64 DiskBytes, BOOL Installed, int *Reason)
{
    int i, j, count = 0;
    int ignored;
    int data = Installed ? 1 : 0;
    int limit = Gpt->MBR.PartTbl[0].FsFlag == 0xEE ? 128 : 4;
    UINT64 start[128] = { 0 }, end[128] = { 0 };
    UINT64 last = DiskBytes / 512;
    GUID zero = { 0 };
    static const GUID unsupported[] = {
        { 0xaf9b60a0, 0x1431, 0x4f62, { 0xbc, 0x68, 0x33, 0x11, 0x71, 0x4a, 0x69, 0xad } },
        { 0x5808c8aa, 0x7e8f, 0x42e0, { 0x85, 0xd2, 0xe1, 0xe9, 0x04, 0x34, 0xcf, 0xb3 } },
        { 0xe75caf8f, 0xf680, 0x4cee, { 0xaf, 0xa3, 0xb0, 0x01, 0xe5, 0x6e, 0xfc, 0x2d } },
        { 0xe7addcb4, 0xdc34, 0x4539, { 0x9a, 0x76, 0xeb, 0xbd, 0x07, 0xbe, 0x6f, 0x7e } }
    };
    VTOY_GPT_HDR head;

    if (!Reason) Reason = &ignored;
    *Reason = FRONT_ERR_LAYOUT;

    if (DiskBytes % 512 || last <= 67584 || Gpt->MBR.Byte55 != 0x55 || Gpt->MBR.ByteAA != 0xAA)
        return FALSE;

    if (limit == 128)
    {
        head = Gpt->Head;
        if (memcmp(head.Signature, "EFI PART", 8) || head.Length != 92 ||
            head.EfiStartLBA != 1 || head.EfiBackupLBA != last - 1 ||
            head.PartTblStartLBA != 2 || head.PartTblTotNum != 128 || head.PartTblEntryLen != 128 ||
            head.PartAreaStartLBA < 34 || head.PartAreaStartLBA > 2048 ||
            head.PartAreaEndLBA >= last - 33 || head.PartAreaEndLBA < 67584)
            return FALSE;
        head.Crc = 0;
        if (VentoyCrc32(&head, head.Length) != Gpt->Head.Crc ||
            VentoyCrc32((void *)Gpt->PartTbl, sizeof(Gpt->PartTbl)) != head.PartTblCrc)
            return FALSE;
        last = head.PartAreaEndLBA + 1;
        for (i = 1; i < 4; i++)
            if (Gpt->MBR.PartTbl[i].FsFlag || Gpt->MBR.PartTbl[i].SectorCount) return FALSE;
    }

    for (i = 0; i < limit; i++)
    {
        if (limit == 128)
        {
            if (!memcmp(&Gpt->PartTbl[i].PartType, &zero, sizeof(zero)))
                continue;
            if (!memcmp(&Gpt->PartTbl[i].PartGuid, &zero, sizeof(zero)) ||
                Gpt->PartTbl[i].LastLBA >= last)
                return FALSE;
            for (j = 0; j < i; j++)
                if (memcmp(&Gpt->PartTbl[j].PartType, &zero, sizeof(zero)) &&
                    !memcmp(&Gpt->PartTbl[i].PartGuid, &Gpt->PartTbl[j].PartGuid, sizeof(GUID)))
                    return FALSE;
            for (j = 0; j < sizeof(unsupported) / sizeof(unsupported[0]); j++)
                if (!memcmp(&Gpt->PartTbl[i].PartType, &unsupported[j], sizeof(GUID)))
                { *Reason = FRONT_ERR_UNSUPPORTED_DISK; return FALSE; }
            start[i] = Gpt->PartTbl[i].StartLBA;
            end[i] = Gpt->PartTbl[i].LastLBA + 1;
        }
        else
        {
            const PART_TABLE *part = &Gpt->MBR.PartTbl[i];
            if (!part->SectorCount) continue;
            if (!part->FsFlag || part->FsFlag == 0x05 || part->FsFlag == 0x0F ||
                part->FsFlag == 0x85 || part->FsFlag == 0x42 || part->FsFlag == 0xEE ||
                part->FsFlag == 0xD7 || part->FsFlag == 0xE7)
                { *Reason = FRONT_ERR_UNSUPPORTED_DISK; return FALSE; }
            start[i] = part->StartSectorId;
            end[i] = start[i] + part->SectorCount;
        }
        if (!Installed && start[i] < 67584ULL)
            { *Reason = FRONT_ERR_SPACE; return FALSE; }
        if (start[i] < ((Installed && i == 0) ? 2048ULL : 67584ULL) ||
            end[i] <= start[i] || end[i] > last)
            return FALSE;
        count++;
        for (j = 0; j < i; j++)
            if (end[j] && start[i] < end[j] && start[j] < end[i]) return FALSE;
    }
    if (!end[data]) return FALSE;
    if (!Installed && count == limit)
        { *Reason = FRONT_ERR_PARTITION_SLOTS; return FALSE; }
    if (!Installed)
    {
        if (limit == 128)
        {
            static const GUID basic = { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };
            static const GUID linuxData = { 0x0fc63daf, 0x8483, 0x4772, { 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 } };
            if (memcmp(&Gpt->PartTbl[0].PartType, &basic, sizeof(GUID)) &&
                memcmp(&Gpt->PartTbl[0].PartType, &linuxData, sizeof(GUID))) return FALSE;
        }
        else
        {
            BYTE type = Gpt->MBR.PartTbl[0].FsFlag;
            if (type != 0x07 && type != 0x0B && type != 0x0C && type != 0x06 && type != 0x0E && type != 0x83)
                return FALSE;
        }
    }
    for (i = data + 1; i < limit; i++)
        if (end[i] && start[i] < end[data]) return FALSE;
    if (Installed)
    {
        if (start[0] != 2048 || end[0] != 67584) return FALSE;
        if (limit == 128)
        {
            if (memcmp(Gpt->PartTbl[0].Name, L"VTOYEFI", 7 * sizeof(WCHAR))) return FALSE;
        }
        else if (Gpt->MBR.PartTbl[0].FsFlag != 0xEF) return FALSE;
    }
    *Reason = FRONT_ERR_NONE;
    return TRUE;
}

BOOL VentoyValidateFrontLayout(const VTOY_GPT_INFO *Gpt, UINT64 DiskBytes, BOOL Installed)
{
    return VentoyCheckFrontLayout(Gpt, DiskBytes, Installed, NULL);
}

BOOL VentoyCheckFrontEfiPackage(void)
{
    static const char *paths[] = { "boot/boot.img", "boot/core.img.xz", "ventoy/ventoy.disk.img.xz" };
    FILE *manifest = NULL, *file = NULL;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE buffer[65536], digest[32];
    char line[256], expected[65], path[160], actual[65];
    DWORD length;
    size_t bytes;
    int i, j;
    BOOL ok = FALSE;

    if (fopen_s(&manifest, "ventoy/front-efi.sha256", "rb") || !manifest)
        goto out;
    if (!CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto out;
    for (i = 0; i < 3; i++)
    {
        if (!fgets(line, sizeof(line), manifest) ||
            sscanf_s(line, "%64s %159s", expected, (unsigned)sizeof(expected), path, (unsigned)sizeof(path)) != 2 ||
            strlen(expected) != 64 || strcmp(path, paths[i]))
            goto out;
        if (fopen_s(&file, paths[i], "rb") || !file ||
            !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) goto out;
        while ((bytes = fread(buffer, 1, sizeof(buffer), file)) != 0)
            if (!CryptHashData(hash, buffer, (DWORD)bytes, 0)) goto out;
        if (ferror(file)) goto out;
        fclose(file);
        file = NULL;
        length = sizeof(digest);
        if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) || length != sizeof(digest)) goto out;
        CryptDestroyHash(hash);
        hash = 0;
        for (j = 0; j < 32; j++) sprintf_s(actual + j * 2, 3, "%02x", digest[j]);
        if (_stricmp(actual, expected)) goto out;
    }
    if (fgets(line, sizeof(line), manifest) || ferror(manifest)) goto out;
    ok = TRUE;
out:
    if (file) fclose(file);
    if (manifest) fclose(manifest);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (!ok) Log("Front EFI requires a rebuilt package with a valid ventoy/front-efi.sha256 manifest.");
    return ok;
}

BOOL VentoyCheckFrontDataSignature(PHY_DRIVE_INFO *pPhyDrive, HANDLE Disk)
{
    BYTE boot[512];
    CHAR target[64];
    const char *fs = NULL;
    WORD reserved, rootEntries, total16, fat16, version;
    DWORD total32, fat32, rootCluster;
    UINT64 total, fatSize, overhead, clusters;
    static const GUID bitlocker = { 0x4967d63b, 0x2e29, 0x4ad8, { 0x83, 0x99, 0xf6, 0xa3, 0x39, 0xe3, 0xd0, 0x01 } };
    static const GUID usedSpace = { 0x92a84d3b, 0xdd80, 0x4d0e, { 0x9e, 0x4e, 0xb1, 0xe3, 0x28, 0x4e, 0xae, 0xd8 } };

    pPhyDrive->FrontEfiError = FRONT_ERR_DISK_CHANGED;
    if (pPhyDrive->DataStartSector < 67584 ||
        pPhyDrive->DataStartSector >= pPhyDrive->SizeInBytes / 512) return FALSE;
    safe_sprintf(target, "\\\\.\\PhysicalDrive%d", pPhyDrive->PhyDrive);
    pPhyDrive->FrontEfiError = FRONT_ERR_DISK_IO;
    if (!VentoyFrontRead(Disk, &pPhyDrive->FrontIoError, TRUE, target, "data filesystem header",
            pPhyDrive->DataStartSector * 512ULL, boot, sizeof(boot))) return FALSE;

    /* BitLocker To Go can retain a FAT32-looking boot sector; its identifier is at 424. */
    if (!memcmp(boot + 3, "-FVE-FS-", 8) || !memcmp(boot, "LUKS\xba\xbe", 6) ||
        !memcmp(boot + 160, &bitlocker, sizeof(bitlocker)) ||
        !memcmp(boot + 160, &usedSpace, sizeof(usedSpace)) ||
        !memcmp(boot + 424, &bitlocker, sizeof(bitlocker)))
    {
        pPhyDrive->FrontEfiError = FRONT_ERR_ENCRYPTED;
        Log("Front EFI data partition has an encrypted raw volume header.");
        return FALSE;
    }
    pPhyDrive->FrontEfiError = FRONT_ERR_FILESYSTEM;
    if (boot[510] != 0x55 || boot[511] != 0xAA) return FALSE;
    if (!memcmp(boot + 3, "EXFAT   ", 8) && boot[108] == 9)
        fs = "exFAT";
    else if (boot[11] == 0 && boot[12] == 2)
    {
        if (!memcmp(boot + 3, "NTFS    ", 8)) fs = "NTFS";
        else if (boot[13] && !(boot[13] & (boot[13] - 1)) && boot[16])
        {
            memcpy(&reserved, boot + 14, sizeof(reserved));
            memcpy(&rootEntries, boot + 17, sizeof(rootEntries));
            memcpy(&total16, boot + 19, sizeof(total16));
            memcpy(&fat16, boot + 22, sizeof(fat16));
            memcpy(&total32, boot + 32, sizeof(total32));
            memcpy(&fat32, boot + 36, sizeof(fat32));
            memcpy(&version, boot + 42, sizeof(version));
            memcpy(&rootCluster, boot + 44, sizeof(rootCluster));
            total = total16 ? total16 : total32;
            fatSize = fat16 ? fat16 : fat32;
            overhead = reserved + (UINT64)boot[16] * fatSize + ((UINT64)rootEntries * 32 + 511) / 512;
            if (reserved && fatSize && total > overhead && total <= pPhyDrive->SizeInBytes / 512 - pPhyDrive->DataStartSector)
            {
                clusters = (total - overhead) / boot[13];
                if (fat16 && rootEntries && clusters && clusters < 65525 &&
                    fatSize * 512 >= ((clusters + 2) * (clusters < 4085 ? 12 : 16) + 7) / 8)
                    fs = "FAT";
                else if (!fat16 && !rootEntries && !version && clusters && clusters < 0x0ffffff5 &&
                    rootCluster >= 2 && rootCluster < clusters + 2 && fatSize * 512 >= (clusters + 2) * 4)
                    fs = "FAT32";
            }
        }
    }
    if (!fs || (pPhyDrive->FsName[0] && _stricmp(pPhyDrive->FsName, fs)))
    {
        Log("Front EFI data filesystem mismatch or unsupported raw header (Windows=%s raw=%s).",
            pPhyDrive->FsName, fs ? fs : "unknown");
        return FALSE;
    }
    safe_strcpy(pPhyDrive->FsName, fs);
    pPhyDrive->FrontEfiError = FRONT_ERR_NONE;
    return TRUE;
}

BOOL VentoyCheckFrontDataVolume(PHY_DRIVE_INFO *pPhyDrive, HANDLE Disk)
{
    HANDLE search = INVALID_HANDLE_VALUE, volume = INVALID_HANDLE_VALUE;
    struct { DWORD NumberOfDiskExtents; DISK_EXTENT Extents[128]; } extents;
    CHAR name[MAX_PATH], openName[MAX_PATH], rawFs[64], paths[1024], *path;
    DWORD bytes, required;
    UINT64 start, sectors;
    int data, gpt;
    BOOL rawOk, ok = FALSE;

    pPhyDrive->Part1DriveLetter = 0;
    pPhyDrive->ResizeVolumeGuid[0] = 0;
    pPhyDrive->FsName[0] = 0;
    rawOk = VentoyCheckFrontDataSignature(pPhyDrive, Disk);
    if (!rawOk && pPhyDrive->FrontEfiError != FRONT_ERR_FILESYSTEM) return FALSE;
    safe_strcpy(rawFs, pPhyDrive->FsName);
    gpt = pPhyDrive->Gpt.MBR.PartTbl[0].FsFlag == 0xEE;
    start = gpt ? pPhyDrive->Gpt.PartTbl[0].StartLBA : pPhyDrive->Gpt.MBR.PartTbl[0].StartSectorId;
    data = start == pPhyDrive->DataStartSector ? 0 : 1;
    start = gpt ? pPhyDrive->Gpt.PartTbl[data].StartLBA : pPhyDrive->Gpt.MBR.PartTbl[data].StartSectorId;
    sectors = gpt ? pPhyDrive->Gpt.PartTbl[data].LastLBA - start + 1 : pPhyDrive->Gpt.MBR.PartTbl[data].SectorCount;
    pPhyDrive->FrontEfiError = FRONT_ERR_DISK_CHANGED;
    if (start != pPhyDrive->DataStartSector || !sectors || sectors > pPhyDrive->SizeInBytes / 512 - start) return FALSE;
    pPhyDrive->FrontEfiError = FRONT_ERR_NO_VOLUME;
    search = FindFirstVolumeA(name, sizeof(name));
    if (search == INVALID_HANDLE_VALUE) goto out;
    do
    {
        safe_strcpy(openName, name);
        openName[strlen(openName) - 1] = 0;
        volume = CreateFileA(openName, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (volume == INVALID_HANDLE_VALUE) continue;
        if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                &extents, sizeof(extents), &bytes, NULL) || extents.NumberOfDiskExtents != 1 ||
            bytes < FIELD_OFFSET(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT) ||
            extents.Extents[0].DiskNumber != (DWORD)pPhyDrive->PhyDrive ||
            (UINT64)extents.Extents[0].StartingOffset.QuadPart != start * 512 ||
            (UINT64)extents.Extents[0].ExtentLength.QuadPart != sectors * 512)
        {
            CHECK_CLOSE_HANDLE(volume);
            continue;
        }
        CHECK_CLOSE_HANDLE(volume);
        pPhyDrive->FrontEfiError = FRONT_ERR_FILESYSTEM;
        if (!GetVolumeInformationA(name, NULL, 0, NULL, NULL, NULL, pPhyDrive->FsName, sizeof(pPhyDrive->FsName)))
        {
            DWORD code = GetLastError();
            VentoyFrontRecordIoError(&pPhyDrive->FrontIoError, TRUE, name, "data volume filesystem", "query", 0, 0, 0, code, FALSE);
            Log("Cannot query the actual data volume %s: %u", name, code);
            goto out;
        }
        if (!rawOk || _stricmp(pPhyDrive->FsName, rawFs))
        {
            Log("Unsupported or encrypted data filesystem on %s: Windows=%s raw=%s", name, pPhyDrive->FsName, rawFs);
            goto out;
        }
        if (strlen(name) >= sizeof(pPhyDrive->ResizeVolumeGuid)) goto out;
        safe_strcpy(pPhyDrive->ResizeVolumeGuid, name);
        if (GetVolumePathNamesForVolumeNameA(name, paths, sizeof(paths), &required))
            for (path = paths; *path; path += strlen(path) + 1)
                if (strlen(path) == 3 && path[1] == ':' && path[2] == '\\')
                {
                    pPhyDrive->Part1DriveLetter = path[0];
                    break;
                }
        Log("Front EFI data volume: %s letter=%c filesystem=%s offset=%llu length=%llu", name,
            pPhyDrive->Part1DriveLetter ? pPhyDrive->Part1DriveLetter : '-', pPhyDrive->FsName, start * 512, sectors * 512);
        pPhyDrive->FrontEfiError = FRONT_ERR_NONE;
        ok = TRUE;
        break;
    } while (FindNextVolumeA(search, name, sizeof(name)));
out:
    CHECK_CLOSE_HANDLE(volume);
    if (search != INVALID_HANDLE_VALUE) FindVolumeClose(search);
    return ok;
}

BOOL VentoyPrepareFrontUpdate(PHY_DRIVE_INFO *pPhyDrive)
{
    HANDLE disk = INVALID_HANDLE_VALUE;
    VTOY_GPT_INFO table;
    CHAR target[64];
    BOOL ok = FALSE;

    memset(&pPhyDrive->FrontIoError, 0, sizeof(pPhyDrive->FrontIoError));
    memset(&pPhyDrive->FrontRestoreIoError, 0, sizeof(pPhyDrive->FrontRestoreIoError));
    safe_sprintf(target, "\\\\.\\PhysicalDrive%d", pPhyDrive->PhyDrive);
    pPhyDrive->FrontEfiState = FRONT_STATE_UNTOUCHED;
    pPhyDrive->FrontBackupPath[0] = 0;
    pPhyDrive->FrontEfiError = FRONT_ERR_SECTOR;
    if (pPhyDrive->BytesPerLogicalSector != 512) return FALSE;
    pPhyDrive->FrontEfiError = FRONT_ERR_PACKAGE;
    if (!VentoyCheckFrontEfiPackage()) return FALSE;
    pPhyDrive->FrontEfiError = FRONT_ERR_DISK_CHANGED;
    if (!VentoyPhydriveMatch(pPhyDrive)) return FALSE;
    disk = CreateFileA(target, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (disk == INVALID_HANDLE_VALUE)
    {
        VentoyFrontRecordIoError(&pPhyDrive->FrontIoError, TRUE, target, "update precheck", "open", 0, 0, 0, GetLastError(), FALSE);
        return FALSE;
    }
    pPhyDrive->FrontEfiError = FRONT_ERR_DISK_IO;
    if (!VentoyFrontRead(disk, &pPhyDrive->FrontIoError, TRUE, target, "update partition precheck", 0, &table, sizeof(table))) goto out;
    pPhyDrive->FrontEfiError = FRONT_ERR_DISK_CHANGED;
    if (memcmp(&table.MBR, &pPhyDrive->MBR, sizeof(table.MBR))) goto out;
    if (!VentoyCheckFrontLayout(&table, pPhyDrive->SizeInBytes, TRUE, &pPhyDrive->FrontEfiError)) goto out;
    pPhyDrive->Gpt = table;
    pPhyDrive->DataStartSector = table.MBR.PartTbl[0].FsFlag == 0xEE ? table.PartTbl[1].StartLBA : table.MBR.PartTbl[1].StartSectorId;
    if (!VentoyCheckFrontDataVolume(pPhyDrive, disk)) goto out;
    ok = TRUE;
out:
    CHECK_CLOSE_HANDLE(disk);
    return ok;
}

void TraceOut(const char *Fmt, ...)
{
    va_list Arg;
    int Len = 0;
    FILE *File = NULL;
    char szBuf[1024];

    va_start(Arg, Fmt);
    Len += vsnprintf_s(szBuf + Len, sizeof(szBuf)-Len, sizeof(szBuf)-Len, Fmt, Arg);
    va_end(Arg);

    fopen_s(&File, VENTOY_FILE_LOG, "a+");
    if (File)
    {
        fwrite(szBuf, 1, Len, File);
        fclose(File);
    }
}

typedef struct LogBuf
{
    int Len; 
    char szBuf[1024];    
    struct LogBuf* next;
}LogBuf;

static BOOL g_LogCache = FALSE;
static LogBuf* g_LogHead = NULL;
static LogBuf* g_LogTail = NULL;

void LogCache(BOOL cache)
{
    g_LogCache = cache;
}

void LogFlush(void)
{
    FILE* File = NULL;
    LogBuf* Node = NULL;
    LogBuf* Next = NULL;

    if (g_CLI_Mode)
    {
        fopen_s(&File, VENTOY_CLI_LOG, "a+");
    }
    else
    {
        fopen_s(&File, VENTOY_FILE_LOG, "a+");
    }

    if (File)
    {
        for (Node = g_LogHead; Node; Node = Node->next)
        {
            fwrite(Node->szBuf, 1, Node->Len, File);
            fwrite("\n", 1, 1, File);
        }
        fclose(File);
    }

    for (Node = g_LogHead; Node; Node = Next)
    {
        Next = Node->next;
        free(Node);
    }

    g_LogHead = g_LogTail = NULL;
}

void Log(const char *Fmt, ...)
{
    va_list Arg;
    int Len = 0;
    FILE *File = NULL;
    SYSTEMTIME Sys;
    char szBuf[1024];

    GetLocalTime(&Sys);
    Len += safe_sprintf(szBuf,
        "[%4d/%02d/%02d %02d:%02d:%02d.%03d] ",
        Sys.wYear, Sys.wMonth, Sys.wDay,
        Sys.wHour, Sys.wMinute, Sys.wSecond,
        Sys.wMilliseconds);

    va_start(Arg, Fmt);
    Len += vsnprintf_s(szBuf + Len, sizeof(szBuf)-Len - 1, sizeof(szBuf)-Len-1, Fmt, Arg);
    va_end(Arg);

    if (g_LogCache)
    {
        LogBuf* Node = NULL;
        Node = malloc(sizeof(LogBuf));
        if (Node)
        {
            memcpy(Node->szBuf, szBuf, Len);
            Node->next = NULL;
            Node->Len = Len;

            if (g_LogTail)
            {
                g_LogTail->next = Node;
                g_LogTail = Node;
            }
            else
            {
                g_LogHead = g_LogTail = Node;
            }
        }

        return;
    }

    if (g_CLI_Mode)
    {
        fopen_s(&File, VENTOY_CLI_LOG, "a+");
    }
    else
    {
        fopen_s(&File, VENTOY_FILE_LOG, "a+");
    }
    if (File)
    {
        fwrite(szBuf, 1, Len, File);
        fwrite("\n", 1, 1, File);
        fclose(File);
    }
}

const char* GUID2String(void *guid, char *buf, int len)
{
    GUID* pGUID = (GUID*)guid;
    sprintf_s(buf, len, "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
        pGUID->Data1, pGUID->Data2, pGUID->Data3,
        pGUID->Data4[0], pGUID->Data4[1],
        pGUID->Data4[2], pGUID->Data4[3], pGUID->Data4[4], pGUID->Data4[5], pGUID->Data4[6], pGUID->Data4[7]
    );
    return buf;
}

BOOL IsPathExist(BOOL Dir, const char *Fmt, ...)
{
    va_list Arg;
    HANDLE hFile;
    DWORD Attr;
    CHAR FilePath[MAX_PATH];

    va_start(Arg, Fmt);
    vsnprintf_s(FilePath, sizeof(FilePath), sizeof(FilePath), Fmt, Arg);
    va_end(Arg);

    hFile = CreateFileA(FilePath, FILE_READ_EA, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (INVALID_HANDLE_VALUE == hFile)
    {
        return FALSE;
    }

    CloseHandle(hFile);

    Attr = GetFileAttributesA(FilePath);

    if (Dir)
    {
        if ((Attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return FALSE;
        }
    }
    else
    {
        if (Attr & FILE_ATTRIBUTE_DIRECTORY)
        {
            return FALSE;
        }
    }

    return TRUE;
}

int SaveBufToFile(const CHAR *FileName, const void *Buffer, int BufLen)
{
    FILE *File = NULL;
    void *Data = NULL;

    fopen_s(&File, FileName, "wb");
    if (File == NULL)
    {
        Log("Failed to open file %s", FileName);
        return 1;
    }

    fwrite(Buffer, 1, BufLen, File);
    fclose(File);
    return 0;
}

int ReadWholeFileToBuf(const CHAR *FileName, int ExtLen, void **Bufer, int *BufLen)
{
    __int64 FileSize;
    FILE *File = NULL;
    void *Data = NULL;

    *Bufer = NULL;
    *BufLen = 0;
    if (ExtLen < 0) return 1;
    fopen_s(&File, FileName, "rb");
    if (File == NULL)
    {
        Log("Failed to open file %s", FileName);
        return 1;
    }

    if (_fseeki64(File, 0, SEEK_END) || (FileSize = _ftelli64(File)) <= 0 ||
        FileSize > INT_MAX - ExtLen || _fseeki64(File, 0, SEEK_SET)) goto failed;
    Data = malloc((size_t)FileSize + ExtLen);
    if (!Data || fread(Data, 1, (size_t)FileSize, File) != (size_t)FileSize || ferror(File)) goto failed;
    if (ExtLen) memset((BYTE *)Data + (size_t)FileSize, 0, ExtLen);
    if (fclose(File)) { File = NULL; goto failed; }

    *Bufer = Data;
    *BufLen = (int)FileSize;
    return 0;
failed:
    Log("Failed to read the complete file: %s", FileName);
    if (File) fclose(File);
    free(Data);
    return 1;
}

const CHAR* GetLocalVentoyVersion(void)
{
    int rc;
    int FileSize;
    CHAR *Pos = NULL;
    CHAR *Buf = NULL;
    static CHAR LocalVersion[64] = { 0 };

    if (LocalVersion[0] == 0)
    {
        rc = ReadWholeFileToBuf(VENTOY_FILE_VERSION, 1, (void **)&Buf, &FileSize);
        if (rc)
        {
            return "";
        }
        Buf[FileSize] = 0;

        for (Pos = Buf; *Pos; Pos++)
        {
            if (*Pos == '\r' || *Pos == '\n')
            {
                *Pos = 0;
                break;
            }
        }

        safe_sprintf(LocalVersion, "%s", Buf);
        free(Buf);
    }
    
    return LocalVersion;
}

const CHAR* ParseVentoyVersionFromString(CHAR *Buf)
{
    CHAR *Pos = NULL;
    CHAR *End = NULL;
    static CHAR LocalVersion[64] = { 0 };

    Pos = strstr(Buf, "VENTOY_VERSION=");
    if (Pos)
    {
        Pos += strlen("VENTOY_VERSION=");
        if (*Pos == '"')
        {
            Pos++;
        }

        End = Pos;
        while (*End != 0 && *End != '"' && *End != '\r' && *End != '\n')
        {
            End++;
        }

        *End = 0;

        safe_sprintf(LocalVersion, "%s", Pos);
        return LocalVersion;
    }

    return "";
}

BOOL IsWow64(void)
{
    typedef BOOL(WINAPI *LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);
    LPFN_ISWOW64PROCESS fnIsWow64Process;
    BOOL bIsWow64 = FALSE;
	CHAR Wow64Dir[MAX_PATH];

    fnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(GetModuleHandleA("kernel32"), "IsWow64Process");
    if (NULL != fnIsWow64Process)
    {
        fnIsWow64Process(GetCurrentProcess(), &bIsWow64);
    }

	if (!bIsWow64)
	{
		if (GetSystemWow64DirectoryA(Wow64Dir, sizeof(Wow64Dir)))
		{
			Log("GetSystemWow64DirectoryA=<%s>", Wow64Dir);
			bIsWow64 = TRUE;
		}
	}

    return bIsWow64;
}

/*
* Some code and functions in the file are copied from rufus.
* https://github.com/pbatard/rufus
*/

/* Windows versions */
enum WindowsVersion {
    WINDOWS_UNDEFINED = -1,
    WINDOWS_UNSUPPORTED = 0,
    WINDOWS_XP = 0x51,
    WINDOWS_2003 = 0x52,	// Also XP_64
    WINDOWS_VISTA = 0x60,	// Also Server 2008
    WINDOWS_7 = 0x61,		// Also Server 2008_R2
    WINDOWS_8 = 0x62,		// Also Server 2012
    WINDOWS_8_1 = 0x63,		// Also Server 2012_R2
    WINDOWS_10_PREVIEW1 = 0x64,
    WINDOWS_10 = 0xA0,		// Also Server 2016, also Server 2019
    WINDOWS_11 = 0xB0,		// Also Server 2022
    WINDOWS_MAX
};

static const char* GetEdition(DWORD ProductType)
{
    // From: https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getproductinfo
    // These values can be found in the winnt.h header.
    switch (ProductType) {
		case 0x00000000: return "";	//  Undefined
		case 0x00000001: return "Ultimate";
		case 0x00000002: return "Home Basic";
		case 0x00000003: return "Home Premium";
		case 0x00000004: return "Enterprise";
		case 0x00000005: return "Home Basic N";
		case 0x00000006: return "Business";
		case 0x00000007: return "Server Standard";
		case 0x00000008: return "Server Datacenter";
		case 0x00000009: return "Smallbusiness Server";
		case 0x0000000A: return "Server Enterprise";
		case 0x0000000B: return "Starter";
		case 0x0000000C: return "Server Datacenter (Core)";
		case 0x0000000D: return "Server Standard (Core)";
		case 0x0000000E: return "Server Enterprise (Core)";
		case 0x00000010: return "Business N";
		case 0x00000011: return "Web Server";
		case 0x00000012: return "HPC Edition";
		case 0x00000013: return "Storage Server (Essentials)";
		case 0x0000001A: return "Home Premium N";
		case 0x0000001B: return "Enterprise N";
		case 0x0000001C: return "Ultimate N";
		case 0x00000022: return "Home Server";
		case 0x00000024: return "Server Standard without Hyper-V";
		case 0x00000025: return "Server Datacenter without Hyper-V";
		case 0x00000026: return "Server Enterprise without Hyper-V";
		case 0x00000027: return "Server Datacenter without Hyper-V (Core)";
		case 0x00000028: return "Server Standard without Hyper-V (Core)";
		case 0x00000029: return "Server Enterprise without Hyper-V (Core)";
		case 0x0000002A: return "Hyper-V Server";
		case 0x0000002F: return "Starter N";
		case 0x00000030: return "Pro";
		case 0x00000031: return "Pro N";
		case 0x00000034: return "Server Solutions Premium";
		case 0x00000035: return "Server Solutions Premium (Core)";
		case 0x00000040: return "Server Hyper Core V";
		case 0x00000042: return "Starter E";
		case 0x00000043: return "Home Basic E";
		case 0x00000044: return "Premium E";
		case 0x00000045: return "Pro E";
		case 0x00000046: return "Enterprise E";
		case 0x00000047: return "Ultimate E";
		case 0x00000048: return "Enterprise (Eval)";
		case 0x0000004F: return "Server Standard (Eval)";
		case 0x00000050: return "Server Datacenter (Eval)";
		case 0x00000054: return "Enterprise N (Eval)";
		case 0x00000057: return "Thin PC";
		case 0x00000058: case 0x00000059: case 0x0000005A: case 0x0000005B: case 0x0000005C: return "Embedded";
		case 0x00000062: return "Home N";
		case 0x00000063: return "Home China";
		case 0x00000064: return "Home Single Language";
		case 0x00000065: return "Home";
		case 0x00000067: return "Pro with Media Center";
		case 0x00000069: case 0x0000006A: case 0x0000006B: case 0x0000006C: return "Embedded";
		case 0x0000006F: return "Home Connected";
		case 0x00000070: return "Pro Student";
		case 0x00000071: return "Home Connected N";
		case 0x00000072: return "Pro Student N";
		case 0x00000073: return "Home Connected Single Language";
		case 0x00000074: return "Home Connected China";
		case 0x00000079: return "Education";
		case 0x0000007A: return "Education N";
		case 0x0000007D: return "Enterprise LTSB";
		case 0x0000007E: return "Enterprise LTSB N";
		case 0x0000007F: return "Pro S";
		case 0x00000080: return "Pro S N";
		case 0x00000081: return "Enterprise LTSB (Eval)";
		case 0x00000082: return "Enterprise LTSB N (Eval)";
		case 0x0000008A: return "Pro Single Language";
		case 0x0000008B: return "Pro China";
		case 0x0000008C: return "Enterprise Subscription";
		case 0x0000008D: return "Enterprise Subscription N";
		case 0x00000091: return "Server Datacenter SA (Core)";
		case 0x00000092: return "Server Standard SA (Core)";
		case 0x00000095: return "Utility VM";
		case 0x000000A1: return "Pro for Workstations";
		case 0x000000A2: return "Pro for Workstations N";
		case 0x000000A4: return "Pro for Education";
		case 0x000000A5: return "Pro for Education N";
		case 0x000000AB: return "Enterprise G";	// I swear Microsoft are just making up editions...
		case 0x000000AC: return "Enterprise G N";
		case 0x000000B6: return "Home OS";
		case 0x000000B7: return "Cloud E";
		case 0x000000B8: return "Cloud E N";
		case 0x000000BD: return "Lite";
		case 0xABCDABCD: return "(Unlicensed)";
		default: return "(Unknown Edition)";
    }
}

#define is_x64 IsWow64
#define static_strcpy safe_strcpy 
#define REGKEY_HKCU HKEY_CURRENT_USER
#define REGKEY_HKLM HKEY_LOCAL_MACHINE
static int  nWindowsVersion = WINDOWS_UNDEFINED;
static int  nWindowsBuildNumber = -1;
static char WindowsVersionStr[128] = "";

/* Helpers for 32 bit registry operations */

/*
* Read a generic registry key value. If a short key_name is used, assume that
* it belongs to the application and create the app subkey if required
*/
static __inline BOOL _GetRegistryKey(HKEY key_root, const char* key_name, DWORD reg_type,
    LPBYTE dest, DWORD dest_size)
{
    const char software_prefix[] = "SOFTWARE\\";
    char long_key_name[MAX_PATH] = { 0 };
    BOOL r = FALSE;
    size_t i;
    LONG s;
    HKEY hSoftware = NULL, hApp = NULL;
    DWORD dwType = -1, dwSize = dest_size;

    memset(dest, 0, dest_size);

    if (key_name == NULL)
        return FALSE;

    for (i = strlen(key_name); i>0; i--) {
        if (key_name[i] == '\\')
            break;
    }

    if (i > 0) {
        // Prefix with "SOFTWARE" if needed
        if (_strnicmp(key_name, software_prefix, sizeof(software_prefix)-1) != 0) {
            if (i + sizeof(software_prefix) >= sizeof(long_key_name))
                return FALSE;
            strcpy_s(long_key_name, sizeof(long_key_name), software_prefix);
            strcat_s(long_key_name, sizeof(long_key_name), key_name);
            long_key_name[sizeof(software_prefix)+i - 1] = 0;
        }
        else {
            if (i >= sizeof(long_key_name))
                return FALSE;
            static_strcpy(long_key_name, key_name);
            long_key_name[i] = 0;
        }
        i++;
        if (RegOpenKeyExA(key_root, long_key_name, 0, KEY_READ, &hApp) != ERROR_SUCCESS) {
            hApp = NULL;
            goto out;
        }
    }
    else {
        if (RegOpenKeyExA(key_root, "SOFTWARE", 0, KEY_READ | KEY_CREATE_SUB_KEY, &hSoftware) != ERROR_SUCCESS) {
            hSoftware = NULL;
            goto out;
        }        
    }

    s = RegQueryValueExA(hApp, &key_name[i], NULL, &dwType, (LPBYTE)dest, &dwSize);
    // No key means default value of 0 or empty string
    if ((s == ERROR_FILE_NOT_FOUND) || ((s == ERROR_SUCCESS) && (dwType == reg_type) && (dwSize > 0))) {
        r = TRUE;
    }
out:
    if (hSoftware != NULL)
        RegCloseKey(hSoftware);
    if (hApp != NULL)
        RegCloseKey(hApp);
    return r;
}

#define GetRegistryKey32(root, key, pval) _GetRegistryKey(root, key, REG_DWORD, (LPBYTE)pval, sizeof(DWORD))
static __inline INT32 ReadRegistryKey32(HKEY root, const char* key) {
    DWORD val;
    GetRegistryKey32(root, key, &val);
    return (INT32)val;
}

/*
* Modified from smartmontools' os_win32.cpp
*/
void GetWindowsVersion(void)
{   
    OSVERSIONINFOEXA vi, vi2;
    DWORD dwProductType;
    const char* w = 0;
    const char* w64 = "32 bit";
    char *vptr;
    size_t vlen;
    unsigned major, minor;
    ULONGLONG major_equal, minor_equal;
    BOOL ws;

    nWindowsVersion = WINDOWS_UNDEFINED;
    static_strcpy(WindowsVersionStr, "Windows Undefined");

    // suppress the C4996 warning for GetVersionExA
    #pragma warning(push)
    #pragma warning(disable:4996)

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!GetVersionExA((OSVERSIONINFOA *)&vi)) {
        memset(&vi, 0, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
        if (!GetVersionExA((OSVERSIONINFOA *)&vi))
            return;
    }

    #pragma warning(pop)

    if (vi.dwPlatformId == VER_PLATFORM_WIN32_NT) {

        if (vi.dwMajorVersion > 6 || (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 2)) {
            // Starting with Windows 8.1 Preview, GetVersionEx() does no longer report the actual OS version
            // See: http://msdn.microsoft.com/en-us/library/windows/desktop/dn302074.aspx
            // And starting with Windows 10 Preview 2, Windows enforces the use of the application/supportedOS
            // manifest in order for VerSetConditionMask() to report the ACTUAL OS major and minor...

            major_equal = VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL);
            for (major = vi.dwMajorVersion; major <= 9; major++) {
                memset(&vi2, 0, sizeof(vi2));
                vi2.dwOSVersionInfoSize = sizeof(vi2); vi2.dwMajorVersion = major;
                if (!VerifyVersionInfoA(&vi2, VER_MAJORVERSION, major_equal))
                    continue;
                if (vi.dwMajorVersion < major) {
                    vi.dwMajorVersion = major; vi.dwMinorVersion = 0;
                }

                minor_equal = VerSetConditionMask(0, VER_MINORVERSION, VER_EQUAL);
                for (minor = vi.dwMinorVersion; minor <= 9; minor++) {
                    memset(&vi2, 0, sizeof(vi2)); vi2.dwOSVersionInfoSize = sizeof(vi2);
                    vi2.dwMinorVersion = minor;
                    if (!VerifyVersionInfoA(&vi2, VER_MINORVERSION, minor_equal))
                        continue;
                    vi.dwMinorVersion = minor;
                    break;
                }

                break;
            }
        }

        if (vi.dwMajorVersion <= 0xf && vi.dwMinorVersion <= 0xf) {
            ws = (vi.wProductType <= VER_NT_WORKSTATION);
            nWindowsVersion = vi.dwMajorVersion << 4 | vi.dwMinorVersion;
            switch (nWindowsVersion) {
            case WINDOWS_XP: w = "XP";
                break;
            case WINDOWS_2003: w = (ws ? "XP_64" : (!GetSystemMetrics(89) ? "Server 2003" : "Server 2003_R2"));
                break;
            case WINDOWS_VISTA: w = (ws ? "Vista" : "Server 2008");
                break;
            case WINDOWS_7: w = (ws ? "7" : "Server 2008_R2");
                break;
            case WINDOWS_8: w = (ws ? "8" : "Server 2012");
                break;
            case WINDOWS_8_1: w = (ws ? "8.1" : "Server 2012_R2");
                break;
            case WINDOWS_10_PREVIEW1: w = (ws ? "10 (Preview 1)" : "Server 10 (Preview 1)");
                break;
                // Starting with Windows 10 Preview 2, the major is the same as the public-facing version
            case WINDOWS_10:
                if (vi.dwBuildNumber < 20000) {
                    w = (ws ? "10" : ((vi.dwBuildNumber < 17763) ? "Server 2016" : "Server 2019"));
                    break;
                }
                nWindowsVersion = WINDOWS_11;
                // Fall through
            case WINDOWS_11: w = (ws ? "11" : "Server 2022");
                break;
            default:
                if (nWindowsVersion < WINDOWS_XP)
                    nWindowsVersion = WINDOWS_UNSUPPORTED;
                else
                    w = "12 or later";
                break;
            }
        }
    }

    if (is_x64())
        w64 = "64-bit";

    GetProductInfo(vi.dwMajorVersion, vi.dwMinorVersion, vi.wServicePackMajor, vi.wServicePackMinor, &dwProductType);
    vptr = WindowsVersionStr;
    vlen = sizeof(WindowsVersionStr) - 1;

    if (!w)
        sprintf_s(vptr, vlen, "%s %u.%u %s", (vi.dwPlatformId == VER_PLATFORM_WIN32_NT ? "NT" : "??"),
        (unsigned)vi.dwMajorVersion, (unsigned)vi.dwMinorVersion, w64);
    else if (vi.wServicePackMinor)
        sprintf_s(vptr, vlen, "%s SP%u.%u %s", w, vi.wServicePackMajor, vi.wServicePackMinor, w64);
    else if (vi.wServicePackMajor)
        sprintf_s(vptr, vlen, "%s SP%u %s", w, vi.wServicePackMajor, w64);
    else
        sprintf_s(vptr, vlen, "%s%s%s, %s",
        w, (dwProductType != PRODUCT_UNDEFINED) ? " " : "", GetEdition(dwProductType), w64);

    // Add the build number (including UBR if available) for Windows 8.0 and later
    nWindowsBuildNumber = vi.dwBuildNumber;
    if (nWindowsVersion >= 0x62) {
        int nUbr = ReadRegistryKey32(REGKEY_HKLM, "Software\\Microsoft\\Windows NT\\CurrentVersion\\UBR");
        vptr = WindowsVersionStr + strlen(WindowsVersionStr);
        vlen = sizeof(WindowsVersionStr) - strlen(WindowsVersionStr) - 1;
        if (nUbr > 0)
            sprintf_s(vptr, vlen, " (Build %d.%d)", nWindowsBuildNumber, nUbr);
        else
            sprintf_s(vptr, vlen, " (Build %d)", nWindowsBuildNumber);
    }
}



void DumpWindowsVersion(void)
{
    GetWindowsVersion();
    Log("Windows Version: <<Windows %s>>", WindowsVersionStr);
    return;
}


BOOL IsVentoyLogicalDrive(CHAR DriveLetter)
{
    int i;
    CONST CHAR *Files[] =
    {
        "EFI\\BOOT\\BOOTX64.EFI",
        "grub\\themes\\ventoy\\theme.txt",
        "ventoy\\ventoy.cpio",
    };

    for (i = 0; i < sizeof(Files) / sizeof(Files[0]); i++)
    {
        if (!IsFileExist("%C:\\%s", DriveLetter, Files[i]))
        {
            return FALSE;
        }
    }

    return TRUE;
}


int VentoyFillMBRLocation(UINT64 DiskSizeInBytes, UINT32 StartSectorId, UINT32 SectorCount, PART_TABLE *Table)
{
    BYTE Head;
    BYTE Sector;
    BYTE nSector = 63;
    BYTE nHead = 8;    
    UINT32 Cylinder;
    UINT32 EndSectorId;

    while (nHead != 0 && (DiskSizeInBytes / 512 / nSector / nHead) > 1024)
    {
        nHead = (BYTE)nHead * 2;
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

int VentoyFillMBR(UINT64 DiskSizeBytes, MBR_HEAD *pMBR, int PartStyle, UINT8 FsFlag)
{
    GUID Guid;
	int ReservedValue;
    UINT32 DiskSignature;
    UINT32 DiskSectorCount;
    UINT32 PartSectorCount;
    UINT32 PartStartSector;
	UINT32 ReservedSector;

    VentoyGetLocalBootImg(pMBR);

    CoCreateGuid(&Guid);

    memcpy(&DiskSignature, &Guid, sizeof(UINT32));

    Log("Disk signature: 0x%08x", DiskSignature);

    *((UINT32 *)(pMBR->BootCode + 0x1B8)) = DiskSignature;
	memcpy(pMBR->BootCode + 0x180, &Guid, 16);

    if (DiskSizeBytes / 512 > 0xFFFFFFFF)
    {
        DiskSectorCount = 0xFFFFFFFF;
    }
    else
    {
        DiskSectorCount = (UINT32)(DiskSizeBytes / 512);
    }

	ReservedValue = GetReservedSpaceInMB();
	if (ReservedValue <= 0)
	{
		ReservedSector = 0;
	}
	else
	{
		ReservedSector = (UINT32)(ReservedValue * 2048);
	}

    if (PartStyle)
    {
        ReservedSector += 33; // backup GPT part table
    }

    // check aligned with 4KB
    if (IsPartNeed4KBAlign())
    {
        UINT64 sectors = DiskSizeBytes / 512;
        if (sectors % 8)
        {
            Log("Disk need to align with 4KB %u", (UINT32)(sectors % 8));
            ReservedSector += (UINT32)(sectors % 8);
        }
    }

	Log("ReservedSector: %u", ReservedSector);

    //Part1
    PartStartSector = VENTOY_PART1_START_SECTOR;
	PartSectorCount = DiskSectorCount - ReservedSector - VENTOY_EFI_PART_SIZE / 512 - PartStartSector;
    VentoyFillMBRLocation(DiskSizeBytes, PartStartSector, PartSectorCount, pMBR->PartTbl);

    pMBR->PartTbl[0].Active = 0x80; // bootable
    pMBR->PartTbl[0].FsFlag = FsFlag; // File system flag  07:exFAT/NTFS/HPFS  0C:FAT32

    //Part2
    PartStartSector += PartSectorCount;
    PartSectorCount = VENTOY_EFI_PART_SIZE / 512;
    VentoyFillMBRLocation(DiskSizeBytes, PartStartSector, PartSectorCount, pMBR->PartTbl + 1);

    pMBR->PartTbl[1].Active = 0x00; 
    pMBR->PartTbl[1].FsFlag = 0xEF; // EFI System Partition

    pMBR->Byte55 = 0x55;
    pMBR->ByteAA = 0xAA;

    return 0;
}


static int VentoyFillProtectMBR(UINT64 DiskSizeBytes, MBR_HEAD *pMBR)
{
    GUID Guid;
    UINT32 DiskSignature;
    UINT64 DiskSectorCount;

    VentoyGetLocalBootImg(pMBR);

    CoCreateGuid(&Guid);

    memcpy(&DiskSignature, &Guid, sizeof(UINT32));

    Log("Disk signature: 0x%08x", DiskSignature);

    *((UINT32 *)(pMBR->BootCode + 0x1B8)) = DiskSignature;
	memcpy(pMBR->BootCode + 0x180, &Guid, 16);

    DiskSectorCount = DiskSizeBytes / 512 - 1;
    if (DiskSectorCount > 0xFFFFFFFF)
    {
        DiskSectorCount = 0xFFFFFFFF;
    }

    memset(pMBR->PartTbl, 0, sizeof(pMBR->PartTbl));

    pMBR->PartTbl[0].Active = 0x00;
    pMBR->PartTbl[0].FsFlag = 0xee; // EE

    pMBR->PartTbl[0].StartHead = 0;
    pMBR->PartTbl[0].StartSector = 1;
    pMBR->PartTbl[0].StartCylinder = 0;
    pMBR->PartTbl[0].EndHead = 254;
    pMBR->PartTbl[0].EndSector = 63;
    pMBR->PartTbl[0].EndCylinder = 1023;

    pMBR->PartTbl[0].StartSectorId = 1;
    pMBR->PartTbl[0].SectorCount = (UINT32)DiskSectorCount;

    pMBR->Byte55 = 0x55;
    pMBR->ByteAA = 0xAA;

    pMBR->BootCode[92] = 0x22;

    return 0;
}

int VentoyFillWholeGpt(UINT64 DiskSizeBytes, VTOY_GPT_INFO *pInfo)
{
    UINT64 Part1SectorCount = 0;
    UINT64 DiskSectorCount = DiskSizeBytes / 512;
    VTOY_GPT_HDR *Head = &pInfo->Head;
    VTOY_GPT_PART_TBL *Table = pInfo->PartTbl;
    static GUID WindowsDataPartType = { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

    VentoyFillProtectMBR(DiskSizeBytes, &pInfo->MBR);

    Part1SectorCount = DiskSectorCount - 33 - 2048;

    memcpy(Head->Signature, "EFI PART", 8);
    Head->Version[2] = 0x01;
    Head->Length = 92;
    Head->Crc = 0;
    Head->EfiStartLBA = 1;
    Head->EfiBackupLBA = DiskSectorCount - 1;
    Head->PartAreaStartLBA = 34;
    Head->PartAreaEndLBA = DiskSectorCount - 34;
    CoCreateGuid(&Head->DiskGuid);
    Head->PartTblStartLBA = 2;
    Head->PartTblTotNum = 128;
    Head->PartTblEntryLen = 128;


    memcpy(&(Table[0].PartType), &WindowsDataPartType, sizeof(GUID));
    CoCreateGuid(&(Table[0].PartGuid));
    Table[0].StartLBA = 2048;
    Table[0].LastLBA = 2048 + Part1SectorCount - 1;
    Table[0].Attr = 0;
    memcpy(Table[0].Name, L"Data", 4 * 2);

    //Update CRC
    Head->PartTblCrc = VentoyCrc32(Table, sizeof(pInfo->PartTbl));
    Head->Crc = VentoyCrc32(Head, Head->Length);

    return 0;
}

int VentoyFillGpt(UINT64 DiskSizeBytes, VTOY_GPT_INFO *pInfo)
{
    INT64 ReservedValue = 0;
    UINT64 ModSectorCount = 0;
    UINT64 ReservedSector = 33;
    UINT64 Part1SectorCount = 0;
    UINT64 DiskSectorCount = DiskSizeBytes / 512;
    VTOY_GPT_HDR *Head = &pInfo->Head;
    VTOY_GPT_PART_TBL *Table = pInfo->PartTbl;
    static GUID WindowsDataPartType = { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };
    static GUID EspPartType = { 0xc12a7328, 0xf81f, 0x11d2, { 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b } };
	static GUID BiosGrubPartType = { 0x21686148, 0x6449, 0x6e6f, { 0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 } };

    VentoyFillProtectMBR(DiskSizeBytes, &pInfo->MBR);

    ReservedValue = GetReservedSpaceInMB();
    if (ReservedValue > 0)
    {
        ReservedSector += ReservedValue * 2048;
    }

    Part1SectorCount = DiskSectorCount - ReservedSector - (VENTOY_EFI_PART_SIZE / 512) - 2048;

    ModSectorCount = (Part1SectorCount % 8);
    if (ModSectorCount)
    {
        Log("Part1SectorCount:%llu is not aligned by 4KB (%llu)", (ULONGLONG)Part1SectorCount, (ULONGLONG)ModSectorCount);
    }

    // check aligned with 4KB
    if (IsPartNeed4KBAlign())
    {
        if (ModSectorCount)
        {
            Log("Disk need to align with 4KB %u", (UINT32)ModSectorCount);
            Part1SectorCount -= ModSectorCount;
        }
        else
        {
            Log("no need to align with 4KB");
        }
    }

    memcpy(Head->Signature, "EFI PART", 8);
    Head->Version[2] = 0x01;
    Head->Length = 92;
    Head->Crc = 0;
    Head->EfiStartLBA = 1;
    Head->EfiBackupLBA = DiskSectorCount - 1;
    Head->PartAreaStartLBA = 34;
    Head->PartAreaEndLBA = DiskSectorCount - 34;
    CoCreateGuid(&Head->DiskGuid);
    Head->PartTblStartLBA = 2;
    Head->PartTblTotNum = 128;
    Head->PartTblEntryLen = 128;


    memcpy(&(Table[0].PartType), &WindowsDataPartType, sizeof(GUID));
    CoCreateGuid(&(Table[0].PartGuid));
    Table[0].StartLBA = 2048;
    Table[0].LastLBA = 2048 + Part1SectorCount - 1;
    Table[0].Attr = 0;
    memcpy(Table[0].Name, L"Ventoy", 6 * 2);

    // to fix windows issue
    //memcpy(&(Table[1].PartType), &EspPartType, sizeof(GUID));
    memcpy(&(Table[1].PartType), &WindowsDataPartType, sizeof(GUID));
    CoCreateGuid(&(Table[1].PartGuid));
    Table[1].StartLBA = Table[0].LastLBA + 1;
    Table[1].LastLBA = Table[1].StartLBA + VENTOY_EFI_PART_SIZE / 512 - 1;
    Table[1].Attr = VENTOY_EFI_PART_ATTR;
    memcpy(Table[1].Name, L"VTOYEFI", 7 * 2);

#if 0
	memcpy(&(Table[2].PartType), &BiosGrubPartType, sizeof(GUID));
	CoCreateGuid(&(Table[2].PartGuid));
	Table[2].StartLBA = 34;
	Table[2].LastLBA = 2047;
	Table[2].Attr = 0;
#endif

    //Update CRC
    Head->PartTblCrc = VentoyCrc32(Table, sizeof(pInfo->PartTbl));
    Head->Crc = VentoyCrc32(Head, Head->Length);

    return 0;
}

int VentoyFillBackupGptHead(VTOY_GPT_INFO *pInfo, VTOY_GPT_HDR *pHead)
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
    pHead->Crc = VentoyCrc32(pHead, pHead->Length);

    return 0;
}

CHAR GetFirstUnusedDriveLetter(void)
{
    CHAR Letter = 'D';
    DWORD Drives = GetLogicalDrives();

    Drives >>= 3;
    while (Drives & 0x1)
    {
        Letter++;
        Drives >>= 1;
    }

    return Letter;
}

const CHAR * GetBusTypeString(STORAGE_BUS_TYPE Type)
{
    switch (Type)
    {
        case BusTypeUnknown: return "unknown";
        case BusTypeScsi: return "SCSI";
        case BusTypeAtapi: return "Atapi";
        case BusTypeAta: return "ATA";
        case BusType1394: return "1394";
        case BusTypeSsa: return "SSA";
        case BusTypeFibre: return "Fibre";
        case BusTypeUsb: return "USB";
        case BusTypeRAID: return "RAID";
        case BusTypeiScsi: return "iSCSI";
        case BusTypeSas: return "SAS";
        case BusTypeSata: return "SATA";
        case BusTypeSd: return "SD";
        case BusTypeMmc: return "MMC";
        case BusTypeVirtual: return "Virtual";
        case BusTypeFileBackedVirtual: return "FileBackedVirtual";
        case BusTypeSpaces: return "Spaces";
        case BusTypeNvme: return "Nvme";
    }
    return "unknown";
}

int VentoyGetLocalBootImg(MBR_HEAD *pMBR)
{
    int Len = 0;
    BYTE *ImgBuf = NULL;
    static int Loaded = 0;
    static MBR_HEAD MBR;

    if (Loaded)
    {
        memcpy(pMBR, &MBR, 512);
        return 0;
    }

    if (0 == ReadWholeFileToBuf(VENTOY_FILE_BOOT_IMG, 0, (void **)&ImgBuf, &Len))
    {
        if (Len != 512)
        {
            free(ImgBuf);
            return 1;
        }
        Log("Copy boot img success");
        memcpy(pMBR, ImgBuf, 512);
        free(ImgBuf);
        
        CoCreateGuid((GUID *)(pMBR->BootCode + 0x180));

        memcpy(&MBR, pMBR, 512);
        Loaded = 1;

        return 0;
    }
    else
    {
        Log("Copy boot img failed");
        return 1;
    }
}

int GetHumanReadableGBSize(UINT64 SizeBytes)
{
    int i;
    int Pow2 = 1;
    double Delta;
    double GB = SizeBytes * 1.0 / 1000 / 1000 / 1000;

    if ((SizeBytes % 1073741824) == 0)
    {
        return (int)(SizeBytes / 1073741824);
    }

    for (i = 0; i < 12; i++)
    {
        if (Pow2 > GB)
        {
            Delta = (Pow2 - GB) / Pow2;
        }
        else
        {
            Delta = (GB - Pow2) / Pow2;
        }

        if (Delta < 0.05)
        {
            return Pow2;
        }

        Pow2 <<= 1;
    }

    return (int)GB;
}

void TrimString(CHAR *String)
{
    CHAR *Pos1 = String;
    CHAR *Pos2 = String;
    size_t Len = strlen(String);

    while (Len > 0)
    {
        if (String[Len - 1] != ' ' && String[Len - 1] != '\t')
        {
            break;
        }
        String[Len - 1] = 0;
        Len--;
    }

    while (*Pos1 == ' ' || *Pos1 == '\t')
    {
        Pos1++;
    }

    while (*Pos1)
    {
        *Pos2++ = *Pos1++;
    }
    *Pos2++ = 0;

    return;
}

int GetRegDwordValue(HKEY Key, LPCSTR SubKey, LPCSTR ValueName, DWORD *pValue)
{
    HKEY hKey;
    DWORD Type;
    DWORD Size;
    LSTATUS lRet;
    DWORD Value;

    lRet = RegOpenKeyExA(Key, SubKey, 0, KEY_QUERY_VALUE, &hKey);
    Log("RegOpenKeyExA <%s> Ret:%ld", SubKey, lRet);

    if (ERROR_SUCCESS == lRet)
    {
        Size = sizeof(Value);
        lRet = RegQueryValueExA(hKey, ValueName, NULL, &Type, (LPBYTE)&Value, &Size);
        Log("RegQueryValueExA <%s> ret:%u  Size:%u Value:%u", ValueName, lRet, Size, Value);

        *pValue = Value;
        RegCloseKey(hKey);

        return 0;
    }
    else
    {
        return 1;
    }
}

int GetPhysicalDriveCount(void)
{
    DWORD Value;
    int Count = 0;

    if (GetRegDwordValue(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\disk\\Enum", "Count", &Value) == 0)
    {
        Count = (int)Value;
    }

    Log("GetPhysicalDriveCount: %d", Count);
    return Count;
}

void VentoyStringToUpper(CHAR* str)
{
    while (str && *str)
    {
        if (*str >= 'a' && *str <= 'z')
        {
            *str = toupper(*str);
        }        
        str++;
    }
}
