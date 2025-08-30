#!/bin/bash
#
# Compile script for Xiaomi 8450 kernel, dts and modules with AOSPA
# Copyright (C) 2024 Adithya R.
# Modified with build options by sineva

SECONDS=0
LOG_FILE="log.txt"
> "$LOG_FILE"

KP_ROOT="$(realpath ../..)"
SRC_ROOT="$HOME/pa"
TC_DIR="$KP_ROOT/prebuilts-master/clang/host/linux-x86/clang-21"
PREBUILTS_DIR="$KP_ROOT/prebuilts/kernel-build-tools/linux-x86"

DO_CLEAN=false
NO_LTO=false
ONLY_CONFIG=false
BUILD_KERNEL=false
BUILD_DT=false
BUILD_FULL=true
TARGET=
DTB_WILDCARD="*"
DTBO_WILDCARD="*"

while [ "${#}" -gt 0 ]; do
    case "${1}" in
        -c | --clean )
            DO_CLEAN=true
            ;;
        -n | --no-lto )
            NO_LTO=true
            ;;
        -o | --only-config )
            ONLY_CONFIG=true
            ;;
        --kernel-only )
            BUILD_KERNEL=true
            BUILD_DT=false
            BUILD_FULL=false
            ;;
        --dt-only )
            BUILD_KERNEL=true
            BUILD_DT=true
            BUILD_FULL=false
            ;;
        --full )
            BUILD_KERNEL=true
            BUILD_DT=true
            BUILD_FULL=true
            ;;
        * )
            TARGET="${1}"
            ;;
    esac
    shift
done

if [ -z "$TARGET" ]; then
    echo "Target (device) not specified!"
    exit 1
fi

if ! source .build.rc || [ -z "$SRC_ROOT" ]; then
    echo -e "Create a .build.rc file here and define\nSRC_ROOT=<path/to/aospa/source>"
    exit 1
fi

KERNEL_DIR="$SRC_ROOT/device/xiaomi/$TARGET-kernel"

if [ ! -d "$KERNEL_DIR" ]; then
    echo "$KERNEL_DIR does not exist!"
    exit 1
fi

KERNEL_COPY_TO="$KERNEL_DIR"
DTB_COPY_TO="$KERNEL_DIR/dtbs"
DTBO_COPY_TO="$DTB_COPY_TO/dtbo.img"
VBOOT_DIR="$KERNEL_DIR/vendor_ramdisk"
VDLKM_DIR="$KERNEL_DIR/vendor_dlkm"

DEFCONFIG="gki_defconfig"
DEFCONFIGS="vendor/waipio_GKI.config \
vendor/xiaomi_GKI.config \
vendor/addon.config \
vendor/debugfs.config"

MODULES_SRC="../sm8450-modules/qcom/opensource"
MODULES="mmrm-driver \
audio-kernel \
camera-kernel \
cvp-kernel \
dataipa/drivers/platform/msm \
datarmnet/core \
datarmnet-ext/aps \
datarmnet-ext/offload \
datarmnet-ext/shs \
datarmnet-ext/perf \
datarmnet-ext/perf_tether \
datarmnet-ext/sch \
datarmnet-ext/wlan \
display-drivers/msm \
eva-kernel \
video-driver \
wlan/qcacld-3.0/.qca6490"

case "$TARGET" in
    "marble" )
        DTB_WILDCARD="ukee"
        DTBO_WILDCARD="marble-sm7475-pm8008-overlay"
        ;;
    "cupid" )
        DTB_WILDCARD="waipio"
        DTBO_WILDCARD="cupid-sm8450-pm8008-overlay"
        ;;
esac

export PATH="$TC_DIR/bin:$PREBUILTS_DIR/bin:$PATH"
function m() {
    make -j$(nproc --all) O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 \
        DTC_EXT="$PREBUILTS_DIR/bin/dtc" \
        DTC_OVERLAY_TEST_EXT="$PREBUILTS_DIR/bin/ufdt_apply_overlay" \
        TARGET_PRODUCT=$TARGET $@ 2> >(tee -a "$LOG_FILE") || exit $?
}

$DO_CLEAN && (
    rm -rf out sm8450-modules
    echo "Cleaned output directories."
)

echo -e "Generating config...\n"
mkdir -p out
m $DEFCONFIG
m ./scripts/kconfig/merge_config.sh $DEFCONFIGS vendor/${TARGET}_GKI.config
scripts/config --file out/.config \
    --set-str LOCALVERSION "-Zetta-kernel.v1"
$NO_LTO && (
    scripts/config --file out/.config \
        -d LTO_CLANG_FULL -e LTO_NONE \
    --set-str LOCALVERSION "-Zetta-kernel.v1"
    echo -e "\nDisabled LTO!"
)

$ONLY_CONFIG && exit
if $BUILD_KERNEL || $BUILD_DT || $BUILD_FULL; then
    echo -e "\nBuilding kernel...\n"
    m Image
fi

if $BUILD_DT || $BUILD_FULL; then
    echo -e "\nBuilding dtbs...\n"
    m dtbs
    echo -e "\nMerging dtbs...\n"
    rm -rf out/dtbs{,-base}
    mkdir out/dtbs{,-base}
    mv  out/arch/arm64/boot/dts/vendor/qcom/$DTB_WILDCARD.dtb \
        out/arch/arm64/boot/dts/vendor/qcom/$DTBO_WILDCARD.dtbo \
        out/dtbs-base
    rm -f out/arch/arm64/boot/dts/vendor/qcom/*.dtbo
    ../../build/android/merge_dtbs.py out/dtbs-base out/arch/arm64/boot/dts/vendor/qcom/ out/dtbs \
        2> >(tee -a "$LOG_FILE") || exit $?
    mkdtboimg.py create $DTBO_COPY_TO --page_size=4096 out/dtbs/*.dtbo \
        2> >(tee -a "$LOG_FILE") || exit $?
    echo "Generated dtbo.img to $DTBO_COPY_TO".
fi

if $BUILD_FULL; then
    echo -e "\nBuilding modules...\n"
    m modules
    rm -rf out/modules
    m INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install

    echo -e "\nBuilding techpack modules..."
    for module in $MODULES; do
        echo -e "\nBuilding $module..."
        m -C $MODULES_SRC/$module M=$MODULES_SRC/$module KERNEL_SRC="$(pwd)" OUT_DIR="$(pwd)/out"
        m -C $MODULES_SRC/$module M=$MODULES_SRC/$module KERNEL_SRC="$(pwd)" OUT_DIR="$(pwd)/out" \
            INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install
    done

    echo -e "\nCopying modules..."
    first_stage_modules="$(cat modules.list.msm.waipio)"
    second_stage_modules="$(cat modules.list.second_stage modules.list.second_stage.$TARGET)"
    vendor_dlkm_modules="$(cat modules.list.vendor_dlkm modules.list.vendor_dlkm.$TARGET)"
    modules_out="out/modules/lib/modules/$(ls -t out/modules/lib/modules/ | head -n1)"

    rm -rf $VBOOT_DIR && mkdir -p $VBOOT_DIR
    rm -rf $VDLKM_DIR && mkdir -p $VDLKM_DIR

    echo -e "\nCopying first stage modules..."
    for module in $first_stage_modules; do
        mod_path=$(find $modules_out -name "$module" -print -quit)
        [ -z "$mod_path" ] && echo "Could not locate $module, skipping!" && continue
        cp $mod_path $VBOOT_DIR
        echo $module >> $VBOOT_DIR/modules.load
        echo $module >> $VBOOT_DIR/modules.load.recovery
    done

    echo -e "\nCopying second stage modules..."
    for module in $second_stage_modules; do
        mod_path=$(find $modules_out -name "$module" -print -quit)
        [ -z "$mod_path" ] && echo "Could not locate $module, skipping!" && continue
        cp $mod_path $VBOOT_DIR
        cp $mod_path $VDLKM_DIR
        echo $module >> $VBOOT_DIR/modules.load.recovery
        echo $module >> $VDLKM_DIR/modules.load
    done

    echo -e "\nCopying vendor_dlkm modules..."
    for module in $vendor_dlkm_modules; do
        mod_path=$(find $modules_out -name "$module" -print -quit)
        [ -z "$mod_path" ] && echo "Could not locate $module, skipping!" && continue
        cp $mod_path $VDLKM_DIR
        echo $module >> $VDLKM_DIR/modules.load
    done

    for dest_dir in $VBOOT_DIR $VDLKM_DIR; do
        cp modules.vendor_blocklist.msm.waipio $dest_dir/modules.blocklist
        cp $modules_out/modules.{alias,dep,softdep} $dest_dir
    done

    sed -E -i 's|([^: ]*/)([^/]*\.ko)([:]?)([ ]\|$)|/lib/modules/\2\3\4|g' $VBOOT_DIR/modules.dep
    sed -E -i 's|([^: ]*/)([^/]*\.ko)([:]?)([ ]\|$)|/vendor_dlkm/lib/modules/\2\3\4|g' $VDLKM_DIR/modules.dep
fi

echo -e "\nCompleted in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !"
