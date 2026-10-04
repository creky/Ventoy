#!/bin/sh

VTOY_PATH=$PWD/..

# FRONT_EFI also rebuilds the normally prebuilt Linux helpers. The historical
# fallback toolchains and Windows PE helpers must be rebuilt on their own hosts.
if [ "$1" = "FRONT_EFI" ]; then
    for front_file in "$VTOY_PATH/INSTALL/ventoy/vtoyjump32.exe" \
        "$VTOY_PATH/INSTALL/ventoy/vtoyjump64.exe" \
        "$VTOY_PATH/VtoyTool/vtoytool/01/vtoytool_64" \
        "$VTOY_PATH/VtoyTool/vtoytool/02/vtoytool_64"; do
        if [ ! -f "$front_file" ] || ! grep -aFq VTOY_FRONT_EFI_V1 "$front_file"; then
            echo "FRONT_EFI requires a rebuilt $front_file"
            echo "Rebuild the Windows installers/PE helpers and VtoyTool 01/02 with their compatible toolchains first."
            exit 1
        fi
    done
    if [ ! -f "$VTOY_PATH/INSTALL/Ventoy2Disk.exe" ] || \
        ! grep -aFq VTOY_FRONT_EFI_SAFE_V2 "$VTOY_PATH/INSTALL/Ventoy2Disk.exe"; then
        echo "FRONT_EFI requires a rebuilt Windows installer with VTOY_FRONT_EFI_SAFE_V2."
        exit 1
    fi
    for front_file in "$VTOY_PATH/INSTALL"/Ventoy2Disk_*.exe; do
        [ -e "$front_file" ] || continue
        if ! grep -aFq VTOY_FRONT_EFI_SAFE_V2 "$front_file"; then
            echo "FRONT_EFI requires a rebuilt $front_file with VTOY_FRONT_EFI_SAFE_V2."
            exit 1
        fi
    done
    for front_file in "$VTOY_PATH/Plugson/vs/VentoyPlugson/Release/VentoyPlugson.exe" \
        "$VTOY_PATH/Plugson/vs/VentoyPlugson/x64/Release/VentoyPlugson_X64.exe"; do
        if [ ! -f "$front_file" ] || ! grep -aFq 'Selected volume does not match the Ventoy data partition' "$front_file"; then
            echo "FRONT_EFI requires the rebuilt Windows Plugson: $front_file"
            exit 1
        fi
    done
fi

cilog() {
    datestr=$(date +"%Y/%m/%d %H:%M:%S")
    echo "$datestr $*"
}

LOG=$VTOY_PATH/DOC/build.log
[ -f $LOG ] && rm -f $LOG

cd $VTOY_PATH/DOC
cilog "prepare_env ..."
sh prepare_env.sh

export PATH=$PATH:/opt/gcc-linaro-7.4.1-2019.02-x86_64_aarch64-linux-gnu/bin:/opt/aarch64--uclibc--stable-2020.08-1/bin:/opt/mips-loongson-gcc7.3-linux-gnu/2019.06-29/bin/:/opt/mips64el-linux-musl-gcc730/bin/

cilog "build grub2 ..."
cd $VTOY_PATH/GRUB2
sh buildgrub.sh >> $LOG 2>&1 || exit 1

cilog "build ipxe ..."
cd $VTOY_PATH/IPXE
sh buildipxe.sh >> $LOG 2>&1 || exit 1

cilog "build edk2 ..."
cd $VTOY_PATH/EDK2
sh buildedk.sh >> $LOG 2>&1 || exit 1

if [ "$1" = "FRONT_EFI" ]; then
    cilog "build front EFI VtoyTool ..."
    cd "$VTOY_PATH/VtoyTool" || exit 1
    bash -e build.sh >> "$LOG" 2>&1 || exit 1

    cilog "build front EFI vtoycli ..."
    cd "$VTOY_PATH/vtoycli/fat_io_lib" || exit 1
    bash -e buildlib.sh >> "$LOG" 2>&1 || exit 1
    cd "$VTOY_PATH/vtoycli" || exit 1
    bash -e build.sh >> "$LOG" 2>&1 || exit 1
    for front_arch in i386 x86_64 aarch64 mips64el; do
        front_file="$VTOY_PATH/INSTALL/tool/$front_arch/vtoycli"
        if [ ! -f "$front_file" ] || ! grep -aFq VTOY_FRONT_EFI_SAFE_V2 "$front_file"; then
            echo "FRONT_EFI requires a rebuilt $front_file with VTOY_FRONT_EFI_SAFE_V2."
            exit 1
        fi
    done

    cilog "build front EFI GTK and Qt interfaces ..."
    cd "$VTOY_PATH/LinuxGUI" || exit 1
    bash -e build_gtk.sh >> "$LOG" 2>&1 || exit 1
    bash -e build_qt.sh >> "$LOG" 2>&1 || exit 1
fi



#
# We almost rarely modifiy these code, so no need to build them everytime
# If you want to rebuild them, just uncomment them.
#

#cd $VTOY_PATH/VtoyTool
#sh build.sh || exit 1

#cd $VTOY_PATH/vtoycli/fat_io_lib
#sh buildlib.sh

#cd $VTOY_PATH/vtoycli
#sh build.sh || exit 1

#cd $VTOY_PATH/FUSEISO
#sh build_libfuse.sh
#sh build.sh


# cd $VTOY_PATH/ExFAT
# sh buidlibfuse.sh || exit 1
# sh buidexfat.sh || exit 1
# /bin/cp -a EXFAT/shared/mkexfatfs   $VTOY_PATH/INSTALL/tool/mkexfatfs_64
# /bin/cp -a EXFAT/shared/mount.exfat-fuse   $VTOY_PATH/INSTALL/tool/mount.exfat-fuse_64


# cd $VTOY_PATH/SQUASHFS/SRC
# sh build_lz4.sh
# sh build_lzma.sh
# sh build_lzo.sh
# sh build_zstd.sh

# cd $VTOY_PATH/SQUASHFS/squashfs-tools-4.4/squashfs-tools
# sh build.sh

# cd $VTOY_PATH/VBLADE/vblade-master
# sh build.sh

cd $VTOY_PATH/INSTALL

if [ "$1" = "CI" ]; then
    Ver=$(date +%m%d%H%M)
    sed "s/VENTOY_VERSION=.*/VENTOY_VERSION=\"$Ver\"/"  -i ./grub/grub.cfg
fi

cilog "packing ventoy-$Ver ..."
sh ventoy_pack.sh $1 >> $LOG 2>&1 || exit 1

echo -e '\n============== SUCCESS ==================\n'
