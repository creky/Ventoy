#include <Windows.h>
#include <Shlobj.h>
#include <tlhelp32.h>
#include <Psapi.h>
#include <commctrl.h>
#include <limits.h>
#include "resource.h"
#include "Language.h"
#include "Ventoy2Disk.h"
#include "DiskService.h"
#include "VentoyJson.h"

extern void CLISetReserveSpace(int MB);

typedef struct CLI_CFG
{
    int op;
    int PartStyle;
    int ReserveMB;
    BOOL USBCheck;
    BOOL NonDest;
    BOOL FrontEfi;
    int fstype;
}CLI_CFG;

BOOL g_CLI_Mode = FALSE;
static int g_CLI_OP;
static int g_CLI_PhyDrive;

static PHY_DRIVE_INFO* g_CLI_PhyDrvInfo = NULL;

static int CLI_GetPhyDriveInfo(int PhyDrive, PHY_DRIVE_INFO* pInfo)
{
    int rc = 1;
    BOOL bRet;
    DWORD dwBytes;
    HANDLE Handle = INVALID_HANDLE_VALUE;
    CHAR PhyDrivePath[128];
    GET_LENGTH_INFORMATION LengthInfo;
    STORAGE_PROPERTY_QUERY Query;
    STORAGE_DESCRIPTOR_HEADER DevDescHeader;
    STORAGE_DEVICE_DESCRIPTOR* pDevDesc = NULL;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR diskAlignment;

    safe_sprintf(PhyDrivePath, "\\\\.\\PhysicalDrive%d", PhyDrive);
    Handle = CreateFileA(PhyDrivePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    Log("Create file Handle:%p %s status:%u", Handle, PhyDrivePath, LASTERR);

    if (Handle == INVALID_HANDLE_VALUE)
    {
        goto out;
    }

    bRet = DeviceIoControl(Handle,
        IOCTL_DISK_GET_LENGTH_INFO, NULL,
        0,
        &LengthInfo,
        sizeof(LengthInfo),
        &dwBytes,
        NULL);
    if (!bRet)
    {
        Log("DeviceIoControl IOCTL_DISK_GET_LENGTH_INFO failed error:%u", LASTERR);
        goto out;
    }

    Log("PHYSICALDRIVE%d size %llu bytes", PhyDrive, (ULONGLONG)LengthInfo.Length.QuadPart);

    Query.PropertyId = StorageDeviceProperty;
    Query.QueryType = PropertyStandardQuery;

    bRet = DeviceIoControl(Handle,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &Query,
        sizeof(Query),
        &DevDescHeader,
        sizeof(STORAGE_DESCRIPTOR_HEADER),
        &dwBytes,
        NULL);
    if (!bRet)
    {
        Log("DeviceIoControl1 error:%u dwBytes:%u", LASTERR, dwBytes);
        goto out;
    }

    if (DevDescHeader.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {
        Log("Invalid DevDescHeader.Size:%u", DevDescHeader.Size);
        goto out;
    }

    pDevDesc = (STORAGE_DEVICE_DESCRIPTOR*)malloc(DevDescHeader.Size);
    if (!pDevDesc)
    {
        Log("failed to malloc error:%u len:%u", LASTERR, DevDescHeader.Size);
        goto out;
    }

    bRet = DeviceIoControl(Handle,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &Query,
        sizeof(Query),
        pDevDesc,
        DevDescHeader.Size,
        &dwBytes,
        NULL);
    if (!bRet)
    {
        Log("DeviceIoControl2 error:%u dwBytes:%u", LASTERR, dwBytes);
        goto out;
    }


    memset(&Query, 0, sizeof(STORAGE_PROPERTY_QUERY));
    Query.PropertyId = StorageAccessAlignmentProperty;
    Query.QueryType = PropertyStandardQuery;
    memset(&diskAlignment, 0, sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR));

    bRet = DeviceIoControl(Handle,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &Query,
        sizeof(STORAGE_PROPERTY_QUERY),
        &diskAlignment,
        sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR),
        &dwBytes,
        NULL);
    if (!bRet)
    {
        Log("DeviceIoControl3 error:%u dwBytes:%u", LASTERR, dwBytes);
        goto out;
    }


    pInfo->PhyDrive = PhyDrive;
    pInfo->SizeInBytes = LengthInfo.Length.QuadPart;
    pInfo->DeviceType = pDevDesc->DeviceType;
    pInfo->RemovableMedia = pDevDesc->RemovableMedia;
    pInfo->BusType = pDevDesc->BusType;

    pInfo->BytesPerLogicalSector = diskAlignment.BytesPerLogicalSector;
    pInfo->BytesPerPhysicalSector = diskAlignment.BytesPerPhysicalSector;

    if (pDevDesc->VendorIdOffset)
    {
        safe_strcpy(pInfo->VendorId, (char*)pDevDesc + pDevDesc->VendorIdOffset);
        TrimString(pInfo->VendorId);
    }

    if (pDevDesc->ProductIdOffset)
    {
        safe_strcpy(pInfo->ProductId, (char*)pDevDesc + pDevDesc->ProductIdOffset);
        TrimString(pInfo->ProductId);
    }

    if (pDevDesc->ProductRevisionOffset)
    {
        safe_strcpy(pInfo->ProductRev, (char*)pDevDesc + pDevDesc->ProductRevisionOffset);
        TrimString(pInfo->ProductRev);
    }

    if (pDevDesc->SerialNumberOffset)
    {
        safe_strcpy(pInfo->SerialNumber, (char*)pDevDesc + pDevDesc->SerialNumberOffset);
        TrimString(pInfo->SerialNumber);
    }

    rc = 0;
out:
    CHECK_FREE(pDevDesc);
    CHECK_CLOSE_HANDLE(Handle);

    return rc;
}

static BOOL CLI_ParseNumber(const char* str, int* value)
{
    int number = 0;

    if (!str || !*str)
    {
        return FALSE;
    }

    for (; *str; str++)
    {
        if (*str < '0' || *str > '9' || number > (INT_MAX - (*str - '0')) / 10)
        {
            return FALSE;
        }
        number = number * 10 + (*str - '0');
    }

    *value = number;
    return TRUE;
}

static int CLI_CheckParam(int argc, char** argv, PHY_DRIVE_INFO* pDrvInfo, CLI_CFG *pCfg)
{
    int i;
    int fstype = VTOY_FS_EXFAT;
    int op = -1;
    char* opt = NULL;
    int PhyDrive = -1;
    int PartStyle = 0;
    int ReserveMB = 0;
    BOOL USBCheck = TRUE;
    BOOL NonDest = FALSE;
    BOOL FrontEfi = FALSE;
    BOOL SecureBoot = g_SecureBoot;
    char LogicalDrive = 0;
    UINT SeenOptions = 0;
    UINT OptionFlag;
    enum
    {
        OPT_OP = 1, OPT_TARGET = 2, OPT_GPT = 4, OPT_NOSB = 8,
        OPT_NOUSB = 16, OPT_FRONT = 32, OPT_NONDEST = 64,
        OPT_RESERVE = 128, OPT_FS = 256
    };
    MBR_HEAD MBR;
    UINT64 Part2GPTAttr = 0;
    UINT64 Part2StartSector = 0;

    if (argc < 4 || !argv || !argv[1] || _stricmp(argv[1], "VTOYCLI") != 0)
    {
        goto invalid_parameter;
    }

    for (i = 2; i < argc; i++)
    {
        opt = argv[i];
        if (!opt || !*opt)
        {
            goto invalid_parameter;
        }
        if (_stricmp(opt, "/I") == 0)
        {
            OptionFlag = OPT_OP;
            op = 0;
        }
        else if (_stricmp(opt, "/U") == 0)
        {
            OptionFlag = OPT_OP;
            op = 1;
        }
        else if (_stricmp(opt, "/GPT") == 0)
        {
            OptionFlag = OPT_GPT;
            PartStyle = 1;
        }
        else if (_stricmp(opt, "/NoSB") == 0)
        {
            OptionFlag = OPT_NOSB;
            SecureBoot = FALSE;
        }
        else if (_stricmp(opt, "/NoUSBCheck") == 0)
        {
            OptionFlag = OPT_NOUSB;
            USBCheck = FALSE;
        }
        else if (_stricmp(opt, "/FrontEfi") == 0)
        {
            OptionFlag = OPT_FRONT;
            FrontEfi = TRUE;
            NonDest = TRUE;
        }
        else if (_stricmp(opt, "/NonDest") == 0)
        {
            OptionFlag = OPT_NONDEST;
            NonDest = TRUE;
        }
        else if (_strnicmp(opt, "/Drive:", 7) == 0)
        {
            OptionFlag = OPT_TARGET;
            if (!((opt[7] >= 'A' && opt[7] <= 'Z') || (opt[7] >= 'a' && opt[7] <= 'z')) ||
                (opt[8] != 0 && (opt[8] != ':' || opt[9] != 0)))
            {
                goto invalid_parameter;
            }
            LogicalDrive = opt[7];
        }
        else if (_strnicmp(opt, "/PhyDrive:", 10) == 0)
        {
            OptionFlag = OPT_TARGET;
            if (!CLI_ParseNumber(opt + 10, &PhyDrive))
            {
                goto invalid_parameter;
            }
        }
        else if (_strnicmp(opt, "/R:", 3) == 0)
        {
            OptionFlag = OPT_RESERVE;
            if (!CLI_ParseNumber(opt + 3, &ReserveMB))
            {
                goto invalid_parameter;
            }
        }
        else if (_strnicmp(opt, "/FS:", 4) == 0)
        {
            OptionFlag = OPT_FS;
            if (_stricmp(opt + 4, "EXFAT") == 0)
            {
                fstype = VTOY_FS_EXFAT;
            }
            else if (_stricmp(opt + 4, "NTFS") == 0)
            {
                fstype = VTOY_FS_NTFS;
            }
            else if (_stricmp(opt + 4, "FAT32") == 0)
            {
                fstype = VTOY_FS_FAT32;
            }
            else if (_stricmp(opt + 4, "UDF") == 0)
            {
                fstype = VTOY_FS_UDF;
            }
            else
            {
                goto invalid_parameter;
            }
        }
        else
        {
            goto invalid_parameter;
        }

        if (SeenOptions & OptionFlag)
        {
            Log("[ERROR] Duplicate or conflicting CLI option: %s", opt);
            goto invalid_parameter;
        }
        SeenOptions |= OptionFlag;
    }

    if (op < 0 || !(SeenOptions & OPT_TARGET) ||
        (op != 0 && (NonDest || (SeenOptions & (OPT_GPT | OPT_RESERVE | OPT_FS)))) ||
        (NonDest && (SeenOptions & (OPT_GPT | OPT_RESERVE | OPT_FS))))
    {
        Log("[ERROR] Invalid CLI mode combination: /FrontEfi and /NonDest require /I and preserve partition style, size and filesystem.");
        goto invalid_parameter;
    }

    if (LogicalDrive)
    {
        Log("Get PhyDrive by logical drive %C:", LogicalDrive);
        PhyDrive = GetPhyDriveByLogicalDrive(LogicalDrive, NULL);
        if (PhyDrive < 0)
        {
            Log("[ERROR] Failed to resolve logical drive %C:", LogicalDrive);
            return 1;
        }
    }

    if (CLI_GetPhyDriveInfo(PhyDrive, pDrvInfo))
    {
        Log("[ERROR] Failed to get phydrive%d info", PhyDrive);
        return 1;
    }

    Log("PhyDrive:%d BusType:%-4s Removable:%u Size:%dGB(%llu) Name:%s %s",
        pDrvInfo->PhyDrive, GetBusTypeString(pDrvInfo->BusType), pDrvInfo->RemovableMedia,
        GetHumanReadableGBSize(pDrvInfo->SizeInBytes), pDrvInfo->SizeInBytes,
        pDrvInfo->VendorId, pDrvInfo->ProductId);

    if (IsVentoyPhyDrive(PhyDrive, pDrvInfo->SizeInBytes, &MBR, &Part2StartSector, &Part2GPTAttr, &pDrvInfo->DataStartSector))
    {
        memcpy(&(pDrvInfo->MBR), &MBR, sizeof(MBR));
        pDrvInfo->PartStyle = (MBR.PartTbl[0].FsFlag == 0xEE) ? 1 : 0;
        pDrvInfo->Part2GPTAttr = Part2GPTAttr;
        pDrvInfo->FrontEfi = (Part2StartSector == 2048);
        GetVentoyVerInPhyDrive(pDrvInfo, Part2StartSector, pDrvInfo->VentoyVersion, sizeof(pDrvInfo->VentoyVersion), &(pDrvInfo->SecureBootSupport));
        Log("PhyDrive %d is Ventoy Disk ver:%s SecureBoot:%u", pDrvInfo->PhyDrive, pDrvInfo->VentoyVersion, pDrvInfo->SecureBootSupport);

        GetVentoyFsNameInPhyDrive(pDrvInfo);

        if (pDrvInfo->VentoyVersion[0] == 0)
        {
            pDrvInfo->VentoyVersion[0] = '?';
            Log("Unknown Ventoy Version");
        }
    }

    if (op == 0 && NonDest)
    {
        GetLettersBelongPhyDrive(PhyDrive, pDrvInfo->DriveLetters, sizeof(pDrvInfo->DriveLetters));
    }

    g_SecureBoot = (FrontEfi || (op == 1 && pDrvInfo->FrontEfi)) ? FALSE : SecureBoot;
    Log("Ventoy CLI mode:%s PhyDrive:%d Size:%llu bytes PartStyle:%s SecureBoot:%d ReserveSpace:%dMB USBCheck:%u FS:%s",
        op == 1 ? (pDrvInfo->FrontEfi ? "front-efi-update" : "update") :
        (FrontEfi ? "front-efi-install" : (NonDest ? "non-destructive-install" : "format-install")),
        PhyDrive, pDrvInfo->SizeInBytes,
        NonDest ? "preserve" : ((op == 1 ? pDrvInfo->PartStyle : PartStyle) ? "GPT" : "MBR"),
        g_SecureBoot, ReserveMB, USBCheck,
        (NonDest || op == 1) ? "preserve" : GetVentoyFsFmtNameByTypeA(fstype));
    if (FrontEfi || (op == 1 && pDrvInfo->FrontEfi))
    {
        Log("Front EFI preserves the existing data partition; SecureBoot is disabled. The partition table and package are checked before writing.");
    }

    pCfg->op = op;
    pCfg->PartStyle = PartStyle;
    pCfg->ReserveMB = ReserveMB;
    pCfg->USBCheck = USBCheck;
    pCfg->NonDest = NonDest;
    pCfg->FrontEfi = FrontEfi;
    pCfg->fstype = fstype;

    return 0;

invalid_parameter:
    Log("[ERROR] Invalid CLI parameters%s%s", opt ? ": " : "", opt ? opt : "");
    Log("Usage: Ventoy2Disk.exe VTOYCLI { /I | /U } { /Drive:F: | /PhyDrive:1 } [options]");
    Log("/I formats the disk by default. Options: /GPT /R:MB /FS:EXFAT|NTFS|FAT32|UDF /NoSB /NoUSBCheck.");
    Log("/I /NonDest preserves existing data. /I /FrontEfi uses a pre-reserved front gap, preserves data and disables SecureBoot.");
    Log("/NonDest and /FrontEfi reject /GPT, /R and /FS. /U automatically detects the installed layout; front EFI updates disable SecureBoot.");
    Log("Specify exactly one operation and one target. Unknown, duplicate, conflicting or malformed options are rejected.");
    return 1;
}

static int Ventoy_CLI_NonDestInstall(PHY_DRIVE_INFO* pDrvInfo, CLI_CFG* pCfg)
{
    int rc;

    Log("Ventoy_CLI_NonDestInstall start ...");

    if (pDrvInfo->BytesPerLogicalSector == 4096 && pDrvInfo->BytesPerPhysicalSector == 4096)
    {
        Log("Ventoy does not support 4k native disk.");
        rc = 1;
        goto out;
    }

    if (!PartResizePreCheck(NULL, pCfg->FrontEfi))
    {
        Log("#### Part Resize PreCheck Failed ####");
        rc = 1;
        goto out;
    }

    if (pCfg->FrontEfi)
    {
        Log("Front EFI install target: PhysicalDrive%d Size:%llu bytes DataStartLBA:%llu SecureBoot:%d",
            pDrvInfo->PhyDrive, pDrvInfo->SizeInBytes,
            pDrvInfo->PartStyle ? pDrvInfo->Gpt.PartTbl[0].StartLBA : (UINT64)pDrvInfo->Gpt.MBR.PartTbl[0].StartSectorId,
            g_SecureBoot);
    }

    rc = PartitionResizeForVentoy(pDrvInfo);

out:
    Log("Ventoy_CLI_NonDestInstall [%s]", rc == 0 ? "SUCCESS" : "FAILED");
    if (pCfg->FrontEfi)
    {
        VentoyShowFrontEfiResult(pDrvInfo, rc == 0);
    }

    return rc;
}


static int Ventoy_CLI_Install(PHY_DRIVE_INFO* pDrvInfo, CLI_CFG *pCfg)
{
    int rc; 
    int TryId = 1;    

    Log("Ventoy_CLI_Install start ...");
    
    if (pDrvInfo->BytesPerLogicalSector == 4096 && pDrvInfo->BytesPerPhysicalSector == 4096)
    {
        Log("Ventoy does not support 4k native disk.");
        rc = 1;
        goto out;
    }

    if (pCfg->ReserveMB > 0)
    {
        CLISetReserveSpace(pCfg->ReserveMB);
    }

    SetVentoyFsType(pCfg->fstype);

    rc = InstallVentoy2PhyDrive(pDrvInfo, pCfg->PartStyle, TryId++);
    if (rc)
    {
        Log("This time install failed, clean disk by disk, wait 3s and retry...");
        DISK_CleanDisk(pDrvInfo->PhyDrive);

        Sleep(3000);

        Log("Now retry to install...");
        rc = InstallVentoy2PhyDrive(pDrvInfo, pCfg->PartStyle, TryId++);

        if (rc)
        {
            Log("This time install failed, clean disk by diskpart, wait 5s and retry...");
            DSPT_CleanDisk(pDrvInfo->PhyDrive);

            Sleep(5000);

            Log("Now retry to install...");
            rc = InstallVentoy2PhyDrive(pDrvInfo, pCfg->PartStyle, TryId++);
        }
    }

    SetVentoyFsType(VTOY_FS_EXFAT);

out:
    Log("Ventoy_CLI_Install [%s]", rc == 0 ? "SUCCESS" : "FAILED");

    return rc;
}

static int Ventoy_CLI_Update(PHY_DRIVE_INFO* pDrvInfo, CLI_CFG* pCfg)
{
    int rc; 
    int TryId = 1;
    
    Log("Ventoy_CLI_Update start ...");

    if (pDrvInfo->FrontEfi)
    {
        if (!VentoyPrepareFrontUpdate(pDrvInfo))
        {
            rc = 1;
            goto out;
        }
        Log("Front EFI update target: PhysicalDrive%d Size:%llu bytes DataStartLBA:%llu SecureBoot:%d",
            pDrvInfo->PhyDrive, pDrvInfo->SizeInBytes, pDrvInfo->DataStartSector, g_SecureBoot);
    }

    rc = UpdateVentoy2PhyDrive(pDrvInfo, TryId++);
    if (rc && !pDrvInfo->FrontEfi)
    {
        Log("This time update failed, now wait and retry...");
        Sleep(4000);

        //Try2
        Log("Now retry to update...");
        rc = UpdateVentoy2PhyDrive(pDrvInfo, TryId++);
        if (rc)
        {
            //Try3
            Sleep(1000);
            Log("Now retry to update...");
            rc = UpdateVentoy2PhyDrive(pDrvInfo, TryId++);
            if (rc)
            {
                //Try4 is dangerous ...
                Sleep(3000);
                Log("Now retry to update...");
                rc = UpdateVentoy2PhyDrive(pDrvInfo, TryId++);
            }
        }
    }

out:
    Log("Ventoy_CLI_Update [%s]", rc == 0 ? "SUCCESS" : "FAILED");
    if (pDrvInfo->FrontEfi)
    {
        VentoyShowFrontEfiResult(pDrvInfo, rc == 0);
    }

    return rc;
}

void CLI_UpdatePercent(int Pos)
{
    int Len;
    FILE* File = NULL;
    CHAR szBuf[128];

    Len = (int)sprintf_s(szBuf, sizeof(szBuf), "%d", Pos * 100 / PT_FINISH);
    fopen_s(&File, VENTOY_CLI_PERCENT, "w+");
    if (File)
    {
        fwrite(szBuf, 1, Len, File);
        fwrite("\n", 1, 1, File);
        fclose(File);
    }
}

static void CLI_WriteDoneFile(int ret)
{
    FILE* File = NULL;

    fopen_s(&File, VENTOY_CLI_DONE, "w+");
    if (File)
    {
        if (ret == 0)
        {
            fwrite("0\n", 1, 2, File);
        }
        else
        {
            fwrite("1\n", 1, 2, File);
        }
        fclose(File);
    }
    else
    {
        Log("[ERROR] Cannot write CLI completion status: %s", VENTOY_CLI_DONE);
    }
}

BOOL CLI_InitStatus(void)
{
    const char* files[] = { VENTOY_CLI_PERCENT, VENTOY_CLI_DONE };
    int i;
    DWORD error;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        if (!DeleteFileA(files[i]))
        {
            error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            {
                Log("[ERROR] Cannot clear previous CLI status %s: %u", files[i], error);
                return FALSE;
            }
        }
    }
    return TRUE;
}

void CLI_FinishStatus(int ret)
{
    CLI_UpdatePercent(PT_FINISH);
    CLI_WriteDoneFile(ret);
}

PHY_DRIVE_INFO* CLI_PhyDrvInfo(void)
{
    return g_CLI_PhyDrvInfo;
}

/*
 * Ventoy2Disk.exe VTOYCLI { /I | /U } { /Drive:F: | /PhyDrive:1 } [options]
 * 
 */
int VentoyCLIMain(int argc, char** argv)
{
    int ret = 1;
    PHY_DRIVE_INFO* pDrvInfo = NULL;
    CLI_CFG CliCfg;

    g_CLI_PhyDrvInfo = pDrvInfo = (PHY_DRIVE_INFO*)malloc(sizeof(PHY_DRIVE_INFO));
    if (!pDrvInfo)
    {
        goto end;
    }
    memset(pDrvInfo, 0, sizeof(PHY_DRIVE_INFO));

    if (CLI_CheckParam(argc, argv, pDrvInfo, &CliCfg))
    {
        goto end;
    }

    //Check USB type for install
    if (CliCfg.op == 0 && CliCfg.USBCheck)
    {
        if (pDrvInfo->BusType != BusTypeUsb)
        {
            Log("[ERROR] PhyDrive %d is NOT USB type",  pDrvInfo->PhyDrive);
            goto end;
        }
    }

    if (CliCfg.op == 0)
    {
        if (CliCfg.NonDest)
        {
            ret = Ventoy_CLI_NonDestInstall(pDrvInfo, &CliCfg);
        }
        else
        {
            AlertSuppressInit();
            SetAlertPromptHookEnable(TRUE);
            ret = Ventoy_CLI_Install(pDrvInfo, &CliCfg);
        }
    }
    else
    {
        if (pDrvInfo->VentoyVersion[0] == 0)
        {
            Log("[ERROR] No Ventoy information detected in PhyDrive %d, so can not do update", pDrvInfo->PhyDrive);
            goto end;
        }

        ret = Ventoy_CLI_Update(pDrvInfo, &CliCfg);
    }

end:
    g_CLI_PhyDrvInfo = NULL;
    CHECK_FREE(pDrvInfo);

    CLI_FinishStatus(ret);

    return ret;
}
