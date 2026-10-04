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
        { L"Recovery backup: %S\r\nKeep this file and its .txt metadata on another disk. Check the original disk identity and SHA256 before recovery. Do not use a formatting install.", L"恢复备份：%S\r\n请将此文件及其 .txt 信息文件保存在另一块磁盘上。恢复前核对原磁盘身份及 SHA256，不要通过格式化安装来恢复。" },
        { L"Stopped before writing the target disk. Resolve the reason below, then check the disk again.", L"已在写入目标磁盘前停止。请处理以下原因，再重新检查磁盘。" },
        { L"The disk was modified and recovery is not verified. Keep the backup; do not retry a formatting install.", L"磁盘已有写入，尚未确认恢复。请保留备份，不要重试普通格式化安装。" },
        { L"The operation failed. The ranges where writes were attempted have been restored and read-back verified. This does not verify all disk data or the health of the disk.", L"操作失败；本次尝试写入的范围已恢复并回读校验。这不代表全盘数据已校验或磁盘健康。" },
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
        { L"Disk access or read-back verification failed. Follow the recovery status above and the diagnostics below. Do not continue installing before the cause is resolved.", L"磁盘读写或回读校验失败。请按上方恢复状态及下方诊断处理，原因未查清前不要继续安装。" },
        { L"Unable to start the operation. Close other applications and retry.", L"无法启动操作。请关闭其他应用后重试。" },
        { L"Ventoy is already installed. Use Update to keep existing files; do not use a formatting reinstall.", L"此磁盘已安装 Ventoy。请使用“升级”保留现有文件，不要重新格式化安装。" },
        { L"The actual data volume is not accessible. Reconnect the disk and check its volume status. Do not format it when Windows prompts; encrypted volumes cannot use this mode.", L"无法访问实际数据卷。请重新连接磁盘并检查卷状态。Windows 提示格式化时不要确认；加密卷不能使用此模式。" },
        { L"The running Windows system disk cannot use non-destructive installation. Select a separate data disk.", L"当前 Windows 系统所在磁盘不能执行无损安装，请选择独立的数据磁盘。" },
        { L"No free partition-table entry is available. This mode needs one unused entry; freeing file space will not help. Do not delete a partition just to retry.", L"分区表没有空闲表项；此模式需要一个空闲表项，删除文件不能解决。不要为了重试直接删除分区。" },
        { L"Extended/logical partitions, dynamic disks and Storage Spaces are not supported. Use a supported basic disk; do not force-convert this disk to retry.", L"不支持扩展/逻辑分区、动态磁盘或存储空间。请选择受支持的基本磁盘，不要为了重试强制转换当前磁盘。" },
        { L"The actual data partition is not a supported readable filesystem. Windows front EFI installation supports NTFS, exFAT, FAT and FAT32. ReFS, UDF and unidentified filesystems are not supported by this Windows path. Do not format the disk to retry.", L"实际数据分区不是受支持的可读文件系统。Windows 前置安装支持 NTFS、exFAT、FAT、FAT32；此 Windows 路径不支持 ReFS、UDF 及无法确认的文件系统。不要为了重试直接格式化。" },
        { L"The data partition is encrypted or its raw filesystem signature does not match Windows. Unlocking it in Windows does not make it readable by Ventoy at boot. Stop and use an unencrypted supported data partition.", L"数据分区已加密，或原始文件系统签名与 Windows 识别结果不一致。在 Windows 中解锁不代表 Ventoy 启动时可以读取。请停止操作，改用未加密且受支持的数据分区。" },
        { L"Recovery metadata could not be saved and verified. Check free space and permissions in the backup folder on the other disk. Keep any partial backup files; do not use them for recovery without verification.", L"恢复信息文件未能完整保存并校验。请检查另一块磁盘上备份目录的权限及剩余空间。保留已生成的文件，未经核验不要用于恢复。" },
        { L"Data partition starts at LBA %llu (%.2f MiB); its position will be preserved.", L"数据分区起点：LBA %llu（%.2f MiB），本次保持原位置。" },
        { L"Verified data volume: %S\r\nFilesystem: %S", L"已核实的数据卷：%S\r\n文件系统：%S" },
        { L"Original I/O failure", L"原始 I/O 故障" },
        { L"Recovery I/O failure", L"回滚 I/O 故障" },
        { L"Phase: %S   Operation: %S\r\nTarget: %S", L"阶段：%S   操作：%S\r\n目标：%S" },
        { L"Requested byte range (inclusive): %llu - %llu", L"请求字节范围（含首尾）：%llu - %llu" },
        { L"Recorded byte offset: %llu (no byte range recorded)", L"记录的字节偏移：%llu（未记录字节范围）" },
        { L"Requested 512-byte LBA range (inclusive): %llu - %llu", L"请求的 512 字节 LBA 范围（含首尾）：%llu - %llu" },
        { L"Requested: %lu bytes   Transferred: %lu bytes", L"请求：%lu 字节   实际传输：%lu 字节" },
        { L"System error: %lu (0x%08lX): %s", L"系统错误：%lu（0x%08lX）：%s" },
        { L"System error text is unavailable.", L"无法取得系统错误说明。" },
        { L"No system error code was reported; use the I/O result above.", L"系统未提供错误码，请以本次 I/O 结果为准。" },
        { L"Read-back contents did not match the expected data.", L"回读内容与预期数据不一致。" },
        { L"The operation stopped and further disk writes were stopped. Keep the backup. Prioritize recovering important files and use a healthy replacement disk. Do not blindly retry or skip bad sectors to continue installing.", L"操作已停止，已停止追加磁盘写入。请保留备份，优先救出重要数据并换用健康磁盘。不要盲目重试或通过跳过坏块继续安装。" },
        { L"This is a file I/O failure. The target disk's write/recovery state is shown above. Preserve existing backups and use a writable backup folder on another healthy disk.", L"这是文件 I/O 故障，目标磁盘的写入及恢复情况以上方状态为准。请保留已有备份，并更换到另一块健康磁盘上的可写备份目录。" },
        { L"An I/O or CRC error alone does not prove bad sectors; the connection, power supply, controller or media may be involved.", L"I/O 或 CRC 错误本身不能直接证明存在坏块，也可能涉及连接、供电、控制器或介质。" }
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
