#!/bin/sh

front_efi_package_check() {
    if [ "$(vtoycli partresize --front-efi-api)" != VTOY_FRONT_EFI_SAFE_V3 ]; then
        vterr 'The disk helper is outdated. Extract a complete rebuilt front EFI package; do not mix old tools with new scripts.'
        return 1
    fi
    FRONT_MANIFEST=./ventoy/front-efi.sha256
    if ! [ -f "$FRONT_MANIFEST" ] || ! command -v sha256sum >/dev/null 2>&1; then
        vterr 'Front EFI requires a rebuilt package with ventoy/front-efi.sha256 and sha256sum.'
        return 1
    fi
    if ! awk '
        NF != 2 || length($1) != 64 || $1 ~ /[^0-9a-fA-F]/ { bad=1 }
        $2 != "boot/boot.img" && $2 != "boot/core.img.xz" && $2 != "ventoy/ventoy.disk.img.xz" { bad=1 }
        { seen[$2]++; count++ }
        END { exit (bad || count != 3 || seen["boot/boot.img"] != 1 || seen["boot/core.img.xz"] != 1 || seen["ventoy/ventoy.disk.img.xz"] != 1) }
    ' "$FRONT_MANIFEST" || ! sha256sum -c "$FRONT_MANIFEST"; then
        vterr 'Front EFI package verification failed. Rebuild all required boot and runtime components.'
        return 1
    fi
}

front_efi_write() (
    set -e
    umask 077
    front_mode=$1
    workdir=
    phase=prepare
    trap '
        rc=$?
        if [ "$rc" != 0 ]; then
            if [ "$phase" = prepare ]; then
                vterr "Preparation failed; the target disk was NOT written."
                [ -z "$workdir" ] || vterr "Preparation files: $workdir"
                vterr "Stop and check the named device or file. For target-disk I/O/disconnection errors, rescue its data first; for backup/package errors, use known-good storage. Do not repeatedly install or format."
            elif [ "$rc" != 1 ] && [ "$rc" != 3 ]; then
                vterr "The transaction was interrupted; the disk may have been written. Stop and check before retrying."
                vterr "Recovery directory: $workdir (backup.ready marks a completed backup). Keep this folder; do not format the disk."
            fi
        fi
    ' EXIT
    front_efi_package_check
    if [ "$front_mode" = install ]; then
        vtoycli partresize -C "$DISK"
        data_part=$(get_disk_part_name "$DISK" 1)
        data_fs=$(blkid -o value -s TYPE "$data_part") || {
            vterr "Cannot identify the filesystem on $data_part. Reconnect the disk and check it in your partition tool."
            exit 1
        }
        case "$data_fs" in
            ntfs|exfat|vfat|ext2|ext3|ext4|xfs|udf) ;;
            *) vterr "Unsupported data filesystem: $data_fs"; exit 1 ;;
        esac
    else
        [ "$(vtoycli partresize -L "$DISK")" = 1 ] || {
            vterr 'The existing front EFI layout cannot be validated. Stop and inspect the disk before retrying.'
            exit 1
        }
    fi
    for holder in /sys/class/block/${DISK#/dev/}/holders/* /sys/class/block/${DISK#/dev/}/${DISK#/dev/}*/holders/*; do
        if [ -e "$holder" ]; then
            vterr 'The target disk belongs to an active device mapper or RAID device. Stop that device before retrying.'
            exit 1
        fi
    done
    workdir=$(mktemp -d "$(pwd -P)/ventoy-front-backup.XXXXXX")
    vtoycli partresize -Q "$DISK" "$workdir"
    model=$(cat /sys/class/block/${DISK#/dev/}/device/model 2>/dev/null || printf unknown)
    vtinfo "Target: $DISK  Model: $model"
    vtinfo "Operation: $front_mode; EFI: partition 1, 1-33 MiB; data: partition 2, original location."
    vtinfo "Recovery directory: $workdir"
    vtwarn 'Use a working directory on another disk. Secure Boot must be disabled.'
    if ! read -r -p 'Confirm this disk and continue? (y/n) ' Answer; then
        vtinfo "Cancelled; disk unchanged. Preparation files: $workdir"
        exit 0
    fi
    case "$Answer" in y|Y) ;; *) vtinfo "Cancelled; disk unchanged. Preparation files: $workdir"; exit 0 ;; esac

    cp ./boot/boot.img "$workdir/boot.img"
    xzcat ./boot/core.img.xz > "$workdir/core.img"
    xzcat ./ventoy/ventoy.disk.img.xz > "$workdir/efi.img"
    [ "$(wc -c < "$workdir/boot.img")" -eq 512 ]
    [ "$(wc -c < "$workdir/core.img")" -eq 1048064 ]
    [ "$(wc -c < "$workdir/efi.img")" -eq 33554432 ]
    vtoycli partresize -s "$workdir/efi.img" 0
    check_umount_disk "$DISK"
    phase=transaction
    vtoycli partresize -W "$DISK" "$front_mode" "$workdir"
)
