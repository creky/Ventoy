/******************************************************************************
 * Language.c
 *
 * Copyright (c) 2020, longpanda <admin@ventoy.net>
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
#include <versionhelpers.h>
#include "Ventoy2Disk.h"
#include "Language.h"

const TCHAR * GetString(enum STR_ID ID)
{
    static const WCHAR *front[][2] = {
        { L"Non-destructive Install (EFI first)", L"无损安装（EFI 前置）" },
        { L"VTOYEFI will become partition 1 (32 MiB). Existing data offsets and sizes stay unchanged; partition numbers change. Back up important files before proceeding. Secure Boot support will be OFF; disable firmware Secure Boot to start Ventoy. Continue?", L"VTOYEFI 将成为第一分区（32 MiB）。原数据分区的位置和大小不变，分区编号会改变。请先备份重要文件。本次不启用 Secure Boot，启动 Ventoy 时须关闭固件 Secure Boot。继续？" },
        { L"Physical disk %d: %S %S\r\nCapacity: %.2f GiB   Drive letters: %S", L"物理磁盘 %d：%S %S\r\n容量：%.2f GiB   盘符：%S" },
        { L"The first partition starts at %.2f MiB; at least 33 MiB is required (64 MiB recommended). Free space inside a filesystem does not count. The installer does not move partitions.", L"首个分区起点为 %.2f MiB；至少需要 33 MiB（建议预留 64 MiB）。文件系统内部的剩余空间不计入。安装器不会移动原分区。" },
        { L"Update this disk using the existing front EFI layout? Data partition offsets and sizes will stay unchanged. Secure Boot support will be OFF; disable firmware Secure Boot to start Ventoy.", L"确定升级此磁盘的前置 EFI 布局？数据分区起点和大小保持不变。本次不启用 Secure Boot，启动 Ventoy 时须关闭固件 Secure Boot。" },
        { L"Recovery backup: %S\r\nKeep this file on another disk. Do not use a normal/formatting install to recover.", L"恢复备份：%S\r\n请保存在另一块磁盘上。不要通过普通安装或格式化来恢复。" },
        { L"Stopped before writing the target disk. Resolve the reason below, then check the disk again.", L"已在写入目标磁盘前停止。请处理以下原因，再重新检查磁盘。" },
        { L"The disk was modified and recovery is not verified. Keep the backup; do not retry a formatting install.", L"磁盘已有写入，尚未确认恢复。请保留备份，不要重试普通格式化安装。" },
        { L"The operation failed. The affected regions were restored and read-back verification passed. Resolve the reason below before retrying.", L"操作失败，受影响区域已恢复，且回读校验通过。请处理以下原因后再重试。" },
        { L"The operation failed and recovery could not be verified. Keep the disk and backup unchanged; inspect the log and recover from the matching backup before using the disk.", L"操作失败，未能确认恢复成功。请保持磁盘和备份不变，查看日志并核对备份后恢复，再使用此磁盘。" },
        { L"Front EFI operation completed and written regions were verified. Data partition offsets and sizes were preserved. Reconnect the disk before use. Firmware Secure Boot must be disabled.", L"前置 EFI 操作完成，写入区域校验通过，数据分区起点和大小已保留。请重新连接磁盘后使用，并关闭固件 Secure Boot。" },
        { L"See log.txt (or cli_log.txt) for details.", L"详细信息请查看 log.txt（命令行模式为 cli_log.txt）。" },
        { L"Only 512-byte logical sectors are supported (including 512e). 4Kn disks cannot use this mode.", L"此模式仅支持 512 字节逻辑扇区（包括 512e），不支持 4Kn 磁盘。" },
        { L"The partition table is invalid or unsupported. Check the main/backup GPT tables, partition overlaps and partition order in a partition tool. Do not choose Format.", L"分区表无效或布局不受支持。请用分区工具检查 GPT 主备表、分区重叠和排列顺序，不要选择格式化。" },
        { L"There is not enough unallocated space before the partitions. Use a partition tool to reserve 64 MiB at the disk front, then rescan. Do not shrink only the end of the partition.", L"分区前未分配空间不足。请先用分区工具在磁盘前部预留 64 MiB，再重新扫描；只缩小分区尾部不能满足要求。" },
        { L"The package is incomplete, outdated or failed verification. Extract a complete rebuilt front EFI package to another disk; do not mix files from different releases.", L"安装包不完整、版本过旧或校验失败。请将完整重建的前置 EFI 安装包解压到另一块磁盘，不要混用不同版本文件。" },
        { L"Run the extracted installer from another physical disk, in a writable local folder. The recovery backup cannot be stored on the target disk.", L"请从另一块物理磁盘的本地可写目录运行已解压的安装器；恢复备份不能保存在目标磁盘上。" },
        { L"Unable to safely identify a volume. Close disk tools, disconnect unrelated virtual disks if necessary, refresh the device list, and try again.", L"无法安全确认卷归属。请关闭磁盘工具，必要时断开无关虚拟磁盘，再刷新设备列表重试。" },
        { L"The disk is in use. Close Explorer windows, applications and disk tools using it, then retry. Do not force a dismount or format.", L"磁盘正在被占用。请关闭使用它的资源管理器窗口、应用和分区工具后重试；不要强制卸载或格式化。" },
        { L"The disk identity or partition table changed. Stop other disk tools, refresh the device list, then select and confirm the intended disk again.", L"磁盘身份或分区表已变化。请停止其他分区工具，刷新设备列表，重新选择并确认目标磁盘。" },
        { L"Cannot save a complete recovery backup. Check folder permissions and free space (at least 34 MiB for the backup), then use a writable folder on another physical disk.", L"无法保存完整恢复备份。请检查目录权限和可用空间（备份至少需要 34 MiB），并使用另一块物理磁盘上的可写目录。" },
        { L"Not enough memory to prepare the boot files. Close other applications and retry.", L"内存不足，无法准备启动文件。请关闭其他应用后重试。" },
        { L"Boot files could not be completely read, decompressed or prepared. Extract the complete verified package again; do not reuse partial files.", L"启动文件未能完整读取、解压或准备。请重新解压完整且校验通过的安装包，不要复用残缺文件。" },
        { L"Disk access or read-back verification failed. Check the connection and power supply; follow the recovery status above before retrying.", L"磁盘读写或回读校验失败。请检查连接和供电；重试前先按上方恢复状态处理。" },
        { L"Unable to start the operation. Close other applications and retry.", L"无法启动操作。请关闭其他应用后重试。" },
        { L"Ventoy is already installed. Use Update to keep existing files; do not use a formatting reinstall.", L"此磁盘已安装 Ventoy。请使用“升级”保留现有文件，不要重新格式化安装。" },
        { L"No accessible data drive was found. Check the drive letter and unlock the volume if necessary. Do not format it when Windows prompts.", L"未找到可访问的数据盘符。请检查盘符，必要时先解锁卷；Windows 提示格式化时不要确认。" },
        { L"The running Windows system disk cannot use non-destructive installation. Select a separate data disk.", L"当前 Windows 系统所在磁盘不能执行无损安装，请选择独立的数据磁盘。" },
        { L"No free partition-table entry is available. This mode needs one unused entry; freeing file space will not help. Do not delete a partition just to retry.", L"分区表没有空闲表项；此模式需要一个空闲表项，删除文件不能解决。不要为了重试直接删除分区。" },
        { L"Extended/logical partitions, dynamic disks and Storage Spaces are not supported. Use a supported basic disk; do not force-convert this disk to retry.", L"不支持扩展/逻辑分区、动态磁盘或存储空间。请选择受支持的基本磁盘，不要为了重试强制转换当前磁盘。" },
        { L"Data partition starts at LBA %llu (%.2f MiB); its position will be preserved.", L"数据分区起点：LBA %llu（%.2f MiB），本次保持原位置。" }
    };
    BOOL chinese;

    if (ID < 0 || ID >= STR_ID_MAX) return L"";
    if (g_cur_lang_data && g_cur_lang_data->MsgString[ID][0] &&
        wcscmp(g_cur_lang_data->MsgString[ID], L"#"))
        return g_cur_lang_data->MsgString[ID];
    if (ID >= STR_MENU_FRONT_EFI)
    {
        chinese = g_cur_lang_data ? wcsstr(g_cur_lang_data->Name, L"Chinese") != NULL :
            PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
        return front[ID - STR_MENU_FRONT_EFI][chinese ? 1 : 0];
    }
    return L"";
}
