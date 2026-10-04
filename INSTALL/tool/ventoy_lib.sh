#!/bin/sh

#Ventoy partition 32MB
VENTOY_PART_SIZE=33554432
VENTOY_PART_SIZE_MB=32
VENTOY_SECTOR_SIZE=512
VENTOY_SECTOR_NUM=65536

ventoy_false() {
    [ "1" = "2" ]
}

ventoy_true() {
    [ "1" = "1" ]
}


vtinfo() {
    echo -e "\033[32m$*\033[0m"
}

vtwarn() {
    echo -e "\033[33m$*\033[0m"
}


vterr() {
    echo -e "\033[31m$*\033[0m"
}

vtdebug() {
    echo "$*" >> ./log.txt
}

vtoy_gen_uuid() {
    if  uuid -F BIN > /dev/null 2>&1; then
        uuid -F BIN
    elif uuidgen -V > /dev/null 2>&1; then
        a=$(uuidgen | sed 's/-//g')
        echo -en "\x${a:0:2}\x${a:2:2}\x${a:4:2}\x${a:6:2}\x${a:8:2}\x${a:10:2}\x${a:12:2}\x${a:14:2}\x${a:16:2}\x${a:18:2}\x${a:20:2}\x${a:22:2}\x${a:24:2}\x${a:26:2}\x${a:28:2}\x${a:30:2}"        
    elif python -V > /dev/null 2>&1; then
        a=$(python -c 'import sys,uuid; sys.stdout.write(uuid.uuid4().hex)')
        echo -en "\x${a:0:2}\x${a:2:2}\x${a:4:2}\x${a:6:2}\x${a:8:2}\x${a:10:2}\x${a:12:2}\x${a:14:2}\x${a:16:2}\x${a:18:2}\x${a:20:2}\x${a:22:2}\x${a:24:2}\x${a:26:2}\x${a:28:2}\x${a:30:2}"
    elif [ -e /dev/urandom ]; then
        dd if=/dev/urandom bs=1 count=16 status=none
    else
        datestr=$(date +%N%N%N%N%N)
        a=${datestr:0:32}
        echo -en "\x${a:0:2}\x${a:2:2}\x${a:4:2}\x${a:6:2}\x${a:8:2}\x${a:10:2}\x${a:12:2}\x${a:14:2}\x${a:16:2}\x${a:18:2}\x${a:20:2}\x${a:22:2}\x${a:24:2}\x${a:26:2}\x${a:28:2}\x${a:30:2}"
    fi
}

check_tool_work_ok() {
    
    if echo 1 | hexdump > /dev/null; then
        vtdebug "hexdump test ok ..."
    else
        vtdebug "hexdump test fail ..."
        ventoy_false
        return
    fi
   
    if mkexfatfs -V > /dev/null; then        
        vtdebug "mkexfatfs test ok ..."
    else
        vtdebug "mkexfatfs test fail ..."
        ventoy_false
        return
    fi
    
    if vtoycli fat -T; then
        vtdebug "vtoycli fat test ok ..."
    else
        vtdebug "vtoycli fat test fail ..."
        ventoy_false
        return
    fi
    
    vtdebug "tool check success ..."
    ventoy_true
}


get_disk_part_name() {
    DISK=$1
    
    if echo $DISK | grep -q "/dev/loop"; then
        echo ${DISK}p${2}
    elif echo $DISK | grep -q "/dev/nvme[0-9][0-9]*n[0-9]"; then
        echo ${DISK}p${2}
    elif echo $DISK | grep -q "/dev/mmcblk[0-9]"; then
        echo ${DISK}p${2}
    elif echo $DISK | grep -q "/dev/nbd[0-9]"; then
        echo ${DISK}p${2}
    elif echo $DISK | grep -q "/dev/zd[0-9]"; then
        echo ${DISK}p${2}
    elif echo $DISK | grep -q "/dev/md[0-9]"; then
        echo ${DISK}p${2}
    else
        echo ${DISK}${2}
    fi
}

get_disk_mounts() (
    devices="$1"
    entries=$(awk '{
        for (i=7; i<=NF; i++) if ($i == "-") { print $3 " " $5 " " $(i+2); break }
    }' /proc/self/mountinfo) || return 1
    while read -r mountdev encoded_path encoded_source; do
        [ -n "$mountdev" ] || continue
        case " $devices " in
            *" $mountdev "*) ;;
            *)
                # FUSE filesystems have a virtual mount device; resolve their block source too.
                source=$(printf '%bX' "$encoded_source")
                source=${source%X}
                [ -b "$source" ] || continue
                hexdev=$(stat -Lc '%t:%T' "$source") || return 1
                sourcedev=$(printf '%d:%d' "0x${hexdev%:*}" "0x${hexdev#*:}") || return 1
                case " $devices " in
                    *" $sourcedev "*) ;;
                    *) continue ;;
                esac
                ;;
        esac
        printf '%s %s\n' "$mountdev" "$encoded_path"
    done <<EOF
$entries
EOF
)

check_umount_disk() (
    for tool in stat readlink mountpoint; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            vterr "Required command '$tool' is missing. Install it with your Linux package manager, then retry. Unmounting has not started."
            return 1
        fi
    done
    if ! [ -b "$1" ]; then
        vterr "The target device $1 is unavailable. Stop and reconnect it before retrying."
        return 1
    fi
    hexdev=$(stat -Lc '%t:%T' "$1") || return 1
    devno=$(printf '%d:%d' "0x${hexdev%:*}" "0x${hexdev#*:}") || return 1
    blockpath=$(readlink -f "/sys/dev/block/$devno") || return 1
    devices=$(cat "$blockpath/dev") || return 1
    for partition in "$blockpath"/*/partition; do
        [ -f "$partition" ] || continue
        partdev=$(cat "${partition%/partition}/dev") || return 1
        devices="$devices $partdev"
    done

    while read -r swap_source swap_rest; do
        [ "$swap_source" != Filename ] || continue
        swap_source=$(printf '%bX' "$swap_source")
        swap_source=${swap_source%X}
        [ -b "$swap_source" ] || continue
        hexdev=$(stat -Lc '%t:%T' "$swap_source") || return 1
        swapdev=$(printf '%d:%d' "0x${hexdev%:*}" "0x${hexdev#*:}") || return 1
        case " $devices " in
            *" $swapdev "*) vterr "$swap_source is active swap. Run swapoff before retrying."; return 1 ;;
        esac
    done < /proc/swaps || return 1

    mounts=$(get_disk_mounts "$devices") || return 1
    # Unmount child paths before their parents; mountinfo escapes spaces and backslashes.
    mounts=$(printf '%s\n' "$mounts" | LC_ALL=C sort -rk2,2) || return 1
    while read -r mountdev encoded_path; do
        [ -n "$mountdev" ] || continue
        mtpnt=$(printf '%bX' "$encoded_path")
        mtpnt=${mtpnt%X}
        visible_dev=$(mountpoint -d "$mtpnt") || {
            vterr "The mount at $mtpnt changed. Check the target disk before retrying."
            return 1
        }
        if [ "$visible_dev" != "$mountdev" ]; then
            vterr "Another device covers $mtpnt. Unmount it manually before retrying."
            return 1
        fi
        vtdebug "Trying to umount $mtpnt ..."
        if ! umount "$mtpnt"; then
            vterr "Cannot unmount $mtpnt. Close applications using it, then retry."
            return 1
        fi
    done <<EOF
$mounts
EOF
    remaining=$(get_disk_mounts "$devices") || return 1
    if [ -n "$remaining" ]; then
        vterr 'The target disk still has mounted partitions. Disable automatic mounting and retry.'
        return 1
    fi
)

get_ventoy_version_from_cfg() {
    if grep -q 'set.*VENTOY_VERSION=' $1; then
        grep 'set.*VENTOY_VERSION=' $1 | awk -F'"' '{print $2}'
    else
        echo 'none'
    fi
}

get_disk_efi_part_number() {
    # Only the front layout needs the stricter new validator; keep legacy compatibility.
    first_part=$(get_disk_part_name "$1" 1)
    if [ "$(cat /sys/class/block/${first_part#/dev/}/size 2>/dev/null)" = "$VENTOY_SECTOR_NUM" ] &&
       [ "$(cat /sys/class/block/${first_part#/dev/}/start 2>/dev/null)" = 2048 ]; then
        layout_efi=$(vtoycli partresize -L "$1") || { echo 0; return 1; }
        echo "$layout_efi"
    else
        echo 2
    fi
}

is_disk_contains_ventoy() {
    DISK=$1    

    efi_number=$(get_disk_efi_part_number "$DISK") || return 1
    if [ "$efi_number" = 1 ]; then
        ventoy_true
        return
    fi
    
    PART1=$(get_disk_part_name $1 1)  
    PART2=$(get_disk_part_name $1 2)  
    
    if [ -e /sys/class/block/${PART2#/dev/}/size ]; then    
        SIZE=$(cat /sys/class/block/${PART2#/dev/}/size)
    else
        SIZE=0
    fi

    if ! [ -b $PART1 ]; then
        vtdebug "$PART1 not exist"
        ventoy_false
        return
    fi
    
    if ! [ -b $PART2 ]; then
        vtdebug "$PART2 not exist"
        ventoy_false
        return
    fi
    
    PART1_TYPE=$(dd if=$DISK bs=1 count=1 skip=450 status=none | hexdump -n1 -e  '1/1 "%02X"')
    PART2_TYPE=$(dd if=$DISK bs=1 count=1 skip=466 status=none | hexdump -n1 -e  '1/1 "%02X"')
    
    # if [ "$PART1_TYPE" != "EE" ]; then
        # if [ "$PART2_TYPE" != "EF" ]; then
            # vtdebug "part2 type is $PART2_TYPE not EF"
            # ventoy_false
            # return
        # fi
    # fi
    
    # PART1_TYPE=$(dd if=$DISK bs=1 count=1 skip=450 status=none | hexdump -n1 -e  '1/1 "%02X"')
    # if [ "$PART1_TYPE" != "07" ]; then
        # vtdebug "part1 type is $PART2_TYPE not 07"
        # ventoy_false
        # return
    # fi
    
    if [ -e /sys/class/block/${PART1#/dev/}/start ]; then
        PART1_START=$(cat /sys/class/block/${PART1#/dev/}/start)
    fi
    
    if [ "$PART1_START" != "2048" ]; then
        vtdebug "part1 start is $PART1_START not 2048"
        ventoy_false
        return
    fi 
    
    if [ "$VENTOY_SECTOR_NUM" != "$SIZE" ]; then
        vtdebug "part2 size is $SIZE not $VENTOY_SECTOR_NUM"
        ventoy_false
        return
    fi
    
    ventoy_true
}

check_disk_secure_boot() {
    if ! is_disk_contains_ventoy $1; then
        ventoy_false
        return
    fi
    
    PART2=$(get_disk_part_name "$1" "$(get_disk_efi_part_number "$1")")
    
    vtoycli fat -s $PART2
}

get_disk_ventoy_version() {

    if ! is_disk_contains_ventoy $1; then
        ventoy_false
        return
    fi
    
    PART2=$(get_disk_part_name "$1" "$(get_disk_efi_part_number "$1")")
    
    ParseVer=$(vtoycli fat $PART2)
    if [ $? -eq 0 ]; then
        vtdebug "Ventoy version in $PART2 is $ParseVer"
        echo $ParseVer
        ventoy_true
        return
    fi
    
    ventoy_false
}

wait_and_create_part() {
    vPART1=$1
    vPART2=$2
    echo 'Wait for partitions $vPART1 and $vPART2 ...'
    for i in 0 1 2 3 4 5 6 7 8 9; do
        if ls -l $vPART1 2>/dev/null | grep -q '^b'; then
            if ls -l $vPART2 2>/dev/null | grep -q '^b'; then
                break
            fi
        else
            echo "Wait for $vPART1 and $vPART2 ..."
            sleep 1
        fi
    done

    if ls -l $vPART1 2>/dev/null | grep -q '^b'; then
        echo "$vPART1 exist OK"
    else
        MajorMinor=$(sed "s/:/ /" /sys/class/block/${vPART1#/dev/}/dev)        
        echo "mknod -m 0660 $vPART1 b $MajorMinor ..."
        mknod -m 0660 $vPART1 b $MajorMinor
    fi
    
    if ls -l $vPART2 2>/dev/null | grep -q '^b'; then
        echo "$vPART2 exist OK"
    else
        MajorMinor=$(sed "s/:/ /" /sys/class/block/${vPART2#/dev/}/dev)        
        echo "mknod -m 0660 $vPART2 b $MajorMinor ..."
        mknod -m 0660 $vPART2 b $MajorMinor        
    fi

    if ls -l $vPART1 2>/dev/null | grep -q '^b'; then
        if ls -l $vPART2 2>/dev/null | grep -q '^b'; then
            echo "partition exist OK"
        fi
    else
        echo "[FAIL] $vPART1/$vPART2 does not exist"
        exit 1
    fi
}


format_ventoy_disk_mbr() {
    reserve_mb=$1
    DISK=$2
    PARTTOOL=$3
    
    PART1=$(get_disk_part_name $DISK 1)
    PART2=$(get_disk_part_name $DISK 2)
    
    sector_num=$(cat /sys/block/${DISK#/dev/}/size)
    
    part1_start_sector=2048 
    
    if [ $reserve_mb -gt 0 ]; then
        reserve_sector_num=$(expr $reserve_mb \* 2048)
        part1_end_sector=$(expr $sector_num - $reserve_sector_num - $VENTOY_SECTOR_NUM - 1)
    else
        part1_end_sector=$(expr $sector_num - $VENTOY_SECTOR_NUM - 1)
    fi
    
    part2_start_sector=$(expr $part1_end_sector + 1)
    
    modsector=$(expr $part2_start_sector % 8)
    if [ $modsector -gt 0 ]; then
        vtdebug "modsector:$modsector need to be aligned with 4KB"
        part1_end_sector=$(expr $part1_end_sector - $modsector)
        part2_start_sector=$(expr $part1_end_sector + 1)
    fi
    
    part2_end_sector=$(expr $part2_start_sector + $VENTOY_SECTOR_NUM - 1)

    export part2_start_sector

    vtdebug "part1_start_sector=$part1_start_sector  part1_end_sector=$part1_end_sector"
    vtdebug "part2_start_sector=$part2_start_sector  part2_end_sector=$part2_end_sector"

    if [ -e $PART1 ]; then
        echo "delete $PART1"
        rm -f $PART1
    fi

    if [ -e $PART2 ]; then
        echo "delete $PART2"
        rm -f $PART2
    fi

    echo ""
    echo "Create partitions on $DISK by $PARTTOOL in MBR style ..."
    
    if [ "$PARTTOOL" = "parted" ]; then
        vtdebug "format disk by parted ..."
        parted -a none --script $DISK \
            mklabel msdos \
            unit s \
            mkpart primary ntfs $part1_start_sector $part1_end_sector \
            mkpart primary fat16 $part2_start_sector $part2_end_sector \
            set 1 boot on \
            quit

        sync
        echo -en '\xEF' | dd of=$DISK conv=fsync bs=1 count=1 seek=466 > /dev/null 2>&1
    else
    vtdebug "format disk by fdisk ..."
    
fdisk $DISK >>./log.txt 2>&1 <<EOF
o
n
p
1
$part1_start_sector
$part1_end_sector
n
p
2
$part2_start_sector
$part2_end_sector
t
1
7
t
2
ef
a
1
w
EOF
    fi
   
    udevadm trigger --name-match=$DISK >/dev/null 2>&1
    partprobe >/dev/null 2>&1
    partx -u $DISK >/dev/null 2>&1
    sleep 3
    echo "Done"


    echo 'Wait for partitions ...'
    for i in 0 1 2 3 4 5 6 7 8 9; do
        if [ -b $PART1 -a -b $PART2 ]; then
            break
        else
            echo "Wait for $PART1/$PART2 ..."
            sleep 1
        fi
    done

    if ! [ -b $PART1 ]; then
        MajorMinor=$(sed "s/:/ /" /sys/class/block/${PART1#/dev/}/dev)        
        echo "mknod -m 0660 $PART1 b $MajorMinor ..."
        mknod -m 0660 $PART1 b $MajorMinor
    fi
    
    if ! [ -b $PART2 ]; then
        MajorMinor=$(sed "s/:/ /" /sys/class/block/${PART2#/dev/}/dev)        
        echo "mknod -m 0660 $PART2 b $MajorMinor ..."
        mknod -m 0660 $PART2 b $MajorMinor        
    fi

    if [ -b $PART1 -a -b $PART2 ]; then
        echo "partition exist OK"
    else
        echo "[FAIL] $PART1/$PART2 does not exist"
        exit 1
    fi

    echo "create efi fat fs $PART2 ..."
    for i in 0 1 2 3 4 5 6 7 8 9; do
        check_umount_disk "$PART2" || return 1

        if mkfs.vfat -F 16 -n VTOYEFI -s 1 $PART2; then
            echo 'success'
            break
        else
            echo "$? retry ..."
            sleep 2
        fi
    done
}


format_ventoy_disk_gpt() {
    reserve_mb=$1
    DISK=$2
    PARTTOOL=$3
    
    PART1=$(get_disk_part_name $DISK 1)
    PART2=$(get_disk_part_name $DISK 2)
    
    sector_num=$(cat /sys/block/${DISK#/dev/}/size)
    
    part1_start_sector=2048 
    
    if [ $reserve_mb -gt 0 ]; then
        reserve_sector_num=$(expr $reserve_mb \* 2048 + 33)
        part1_end_sector=$(expr $sector_num - $reserve_sector_num - $VENTOY_SECTOR_NUM - 1)
    else
        part1_end_sector=$(expr $sector_num - $VENTOY_SECTOR_NUM - 34)
    fi
    
    part2_start_sector=$(expr $part1_end_sector + 1)
    
    modsector=$(expr $part2_start_sector % 8)
    if [ $modsector -gt 0 ]; then
        vtdebug "modsector:$modsector need to be aligned with 4KB"
        part1_end_sector=$(expr $part1_end_sector - $modsector)
        part2_start_sector=$(expr $part1_end_sector + 1)
    fi
    
    part2_end_sector=$(expr $part2_start_sector + $VENTOY_SECTOR_NUM - 1)

    export part2_start_sector

    vtdebug "part1_start_sector=$part1_start_sector  part1_end_sector=$part1_end_sector"
    vtdebug "part2_start_sector=$part2_start_sector  part2_end_sector=$part2_end_sector"

    if [ -e $PART1 ]; then
        echo "delete $PART1"
        rm -f $PART1
    fi

    if [ -e $PART2 ]; then
        echo "delete $PART2"
        rm -f $PART2
    fi

    echo ""
    echo "Create partitions on $DISK by $PARTTOOL in GPT style ..."
    
    vtdebug "format disk by parted ..."
    
    if [ "$TOOLDIR" != "aarch64" ]; then
        vt_set_efi_type="set 2 msftdata on"
    fi    
    
    parted -a none --script $DISK \
        mklabel gpt \
        unit s \
        mkpart Ventoy ntfs $part1_start_sector $part1_end_sector \
        mkpart VTOYEFI fat16 $part2_start_sector $part2_end_sector \
        $vt_set_efi_type \
        quit

    sync
    
    vtoycli gpt -f $DISK
    sync

    udevadm trigger --name-match=$DISK >/dev/null 2>&1
    partprobe >/dev/null 2>&1
    partx -u $DISK >/dev/null 2>&1
    sleep 3
    echo "Done"

    echo 'Wait for partitions ...'
    for i in 0 1 2 3 4 5 6 7 8 9; do
        if [ -b $PART1 -a -b $PART2 ]; then
            break
        else
            echo "Wait for $PART1/$PART2 ..."
            sleep 1
        fi
    done

    if ! [ -b $PART1 ]; then
        MajorMinor=$(sed "s/:/ /" /sys/class/block/${PART1#/dev/}/dev)        
        echo "mknod -m 0660 $PART1 b $MajorMinor ..."
        mknod -m 0660 $PART1 b $MajorMinor
    fi
    
    if ! [ -b $PART2 ]; then
        MajorMinor=$(sed "s/:/ /" /sys/class/block/${PART2#/dev/}/dev)        
        echo "mknod -m 0660 $PART2 b $MajorMinor ..."
        mknod -m 0660 $PART2 b $MajorMinor        
    fi

    if [ -b $PART1 -a -b $PART2 ]; then
        echo "partition exist OK"
    else
        echo "[FAIL] $PART1/$PART2 does not exist"
        exit 1
    fi

    echo "create efi fat fs $PART2 ..."
    
    for i in 0 1 2 3 4 5 6 7 8 9; do
        check_umount_disk "$PART2" || return 1
        
        if mkfs.vfat -F 16 -n VTOYEFI -s 1 $PART2; then
            echo 'success'
            break
        else
            echo "$? retry ..."
            sleep 2
        fi
    done
}





