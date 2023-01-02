#!/bin/bash
#
# Compile script for Xiaomi 8450 kernel, dts and modules with AOSPA
# Copyright (C) 2024 Adithya R.

SECONDS=0
LOG_FILE="log.txt"
> "$LOG_FILE"

KP_ROOT="$(realpath ../..)"
SRC_ROOT="$HOME/pa"
TC_DIR="$KP_ROOT/prebuilts-master/clang/host/linux-x86/clang-20"
PREBUILTS_DIR="$KP_ROOT/prebuilts/kernel-build-tools/linux-x86"
DO_CLEAN=false
NO_LTO=false
ONLY_CONFIG=false
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
export CC=clang
export CXX=clang++
export HOSTCC=clang
export HOSTCXX=clang++
export LD=ld.lld
export AR=llvm-ar
export NM=llvm-nm
export OBJCOPY=llvm-objcopy
export OBJDUMP=llvm-objdump
export STRIP=llvm-strip

echo "Using clang: $(which clang)"
clang --version


function m() {
    make -j$(nproc --all) O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 \
        CC=$CC CXX=$CXX HOSTCC=$HOSTCC HOSTCXX=$HOSTCXX \
        LD=$LD AR=$AR NM=$NM OBJCOPY=$OBJCOPY OBJDUMP=$OBJDUMP STRIP=$STRIP \
        DTC_EXT="$PREBUILTS_DIR/bin/dtc" \
        DTC_OVERLAY_TEST_EXT="$PREBUILTS_DIR/bin/ufdt_apply_overlay" \
        TARGET_PRODUCT=$TARGET $@ 2> >(tee -a "$LOG_FILE") || exit $?
}

if $DO_CLEAN; then
    rm -rf out sm8450-modules
    echo "Cleaned output directories."
fi

echo -e "Generating config...\n"
mkdir -p out
m $DEFCONFIG
m ./scripts/kconfig/merge_config.sh $DEFCONFIGS vendor/${TARGET}_GKI.config
scripts/config --file out/.config --set-str LOCALVERSION "-VestiaZeta-Kernel.LTO-Edition"

if $NO_LTO; then
    scripts/config --file out/.config -d LTO_CLANG_FULL -e LTO_NONE --set-str LOCALVERSION "-Zetta.Ocen-Kernel-NoLTO"
    echo -e "\nDisabled LTO!"
fi

if $ONLY_CONFIG; then
    exit 0
fi

echo -e "\nBuilding kernel...\n"
m Image modules dtbs
rm -rf out/modules
m INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install

echo -e "\nBuilding techpack modules..."
for module in $MODULES; do
    echo -e "\nBuilding $module..."
    m -C $MODULES_SRC/$module M=$MODULES_SRC/$module KERNEL_SRC="$(pwd)" OUT_DIR="$(pwd)/out"
    m -C $MODULES_SRC/$module M=$MODULES_SRC/$module KERNEL_SRC="$(pwd)" OUT_DIR="$(pwd)/out" \
        INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install
done

echo -e "\nKernel compiled succesfully!\nMerging dtb's...\n"

rm -rf out/dtbs{,-base}
mkdir -p out/dtbs out/dtbs-base
mv out/arch/arm64/boot/dts/vendor/qcom/$DTB_WILDCARD.dtb out/arch/arm64/boot/dts/vendor/qcom/$DTBO_WILDCARD.dtbo out/dtbs-base
rm -f out/arch/arm64/boot/dts/vendor/qcom/*.dtbo
../../build/android/merge_dtbs.py out/dtbs-base out/arch/arm64/boot/dts/vendor/qcom/ out/dtbs 2> >(tee -a "$LOG_FILE") || exit $?

mkdir -p "$KERNEL_COPY_TO"
mkdir -p "$DTB_COPY_TO"

echo -e "\nCopying files...\n"
cp out/arch/arm64/boot/Image $KERNEL_COPY_TO
echo "Copied kernel to $KERNEL_COPY_TO."

if [ -d "$DTB_COPY_TO" ]; then
    rm -f $DTB_COPY_TO/*.dtb
    cp out/dtbs/*.dtb $DTB_COPY_TO
else
    rm -f $DTB_COPY_TO
    cat out/dtbs/*.dtb >> $DTB_COPY_TO
fi
echo "Copied dtb(s) to $DTB_COPY_TO."

if ! mkdtboimg.py create $DTBO_COPY_TO --page_size=4096 out/dtbs/*.dtbo 2> >(tee -a "$LOG_FILE"); then
    echo "ERROR: Failed to create DTBO image"
    exit 1
fi
echo "Generated dtbo.img to $DTBO_COPY_TO"

echo -e "\n=== MODULE DEBUGGING ==="
echo "Checking module list files..."

for file in "modules.list.msm.waipio" "modules.list.second_stage" "modules.list.second_stage.$TARGET" "modules.list.vendor_dlkm" "modules.list.vendor_dlkm.$TARGET"; do
    if [ -f "$file" ]; then
        echo "✓ $file exists ($(wc -l < "$file") lines)"
    else
        echo "✗ $file MISSING!"
    fi
done

first_stage_modules="$(cat modules.list.msm.waipio 2>/dev/null || echo "")"
second_stage_modules="$(cat modules.list.second_stage modules.list.second_stage.$TARGET 2>/dev/null || echo "")"
vendor_dlkm_modules="$(cat modules.list.vendor_dlkm modules.list.vendor_dlkm.$TARGET 2>/dev/null || echo "")"
modules_out="out/modules/lib/modules/$(ls -t out/modules/lib/modules/ | head -n1)"

echo "First stage modules count: $(echo "$first_stage_modules" | wc -w)"
echo "Second stage modules count: $(echo "$second_stage_modules" | wc -w)"
echo "Vendor DLKM modules count: $(echo "$vendor_dlkm_modules" | wc -w)"
echo "Modules output path: $modules_out"

if [ -d "$modules_out" ]; then
    echo "✓ Modules directory exists"
    echo "Available .ko files: $(find $modules_out -name '*.ko' | wc -l)"
    echo "Sample modules:"
    find $modules_out -name '*.ko' | head -5
else
    echo "✗ MODULES DIRECTORY NOT FOUND!"
    echo "Available directories in out/modules/lib/modules/:"
    ls -la out/modules/lib/modules/ 2>/dev/null || echo "No modules directory exists"
    exit 1
fi

echo -e "\nCreating module directories..."
rm -rf "$VBOOT_DIR" "$VDLKM_DIR"
if ! mkdir -p "$VBOOT_DIR" "$VDLKM_DIR"; then
    echo "ERROR: Failed to create module directories"
    exit 1
fi

if [ -d "$VBOOT_DIR" ] && [ -d "$VDLKM_DIR" ]; then
    echo "✓ Successfully created:"
    echo "  - $VBOOT_DIR"
    echo "  - $VDLKM_DIR"
else
    echo "ERROR: Module directories were not created!"
    exit 1
fi


echo -e "\nCopying first stage modules..."
first_stage_count=0
for module in $first_stage_modules; do
    mod_path=$(find $modules_out -name "$module" -print -quit)
    if [ -z "$mod_path" ]; then
        echo "Could not locate $module, skipping!"
        continue
    fi
    cp $mod_path $VBOOT_DIR
    echo $module >> $VBOOT_DIR/modules.load
    echo $module >> $VBOOT_DIR/modules.load.recovery
    echo "✓ Copied first stage: $module"
    ((first_stage_count++))
done

echo -e "\nCopying second stage modules..."
second_stage_count=0
for module in $second_stage_modules; do
    mod_path=$(find $modules_out -name "$module" -print -quit)
    if [ -z "$mod_path" ]; then
        echo "Could not locate $module, skipping!"
        continue
    fi
    cp $mod_path $VBOOT_DIR
    cp $mod_path $VDLKM_DIR
    echo $module >> $VBOOT_DIR/modules.load.recovery
    echo $module >> $VDLKM_DIR/modules.load
    echo "✓ Copied second stage: $module"
    ((second_stage_count++))
done

echo -e "\nCopying vendor_dlkm modules..."
vendor_dlkm_count=0
for module in $vendor_dlkm_modules; do
    mod_path=$(find $modules_out -name "$module" -print -quit)
    if [ -z "$mod_path" ]; then
        echo "Could not locate $module, skipping!"
        continue
    fi
    cp $mod_path $VDLKM_DIR
    echo $module >> $VDLKM_DIR/modules.load
    echo "✓ Copied vendor_dlkm: $module"
    ((vendor_dlkm_count++))
done

for dest_dir in $VBOOT_DIR $VDLKM_DIR; do
    if [ -f "modules.vendor_blocklist.msm.waipio" ]; then
        cp modules.vendor_blocklist.msm.waipio $dest_dir/modules.blocklist
    fi
    cp $modules_out/modules.{alias,dep,softdep} $dest_dir 2>/dev/null || true
done

if [ -f "$VBOOT_DIR/modules.dep" ]; then
   sed -E -i 's|([^: ]*/)([^/]*\.ko)([:]?)([ ]|$)|/lib/modules/\2\3\4|g' "$VBOOT_DIR/modules.dep"
fi

if [ -f "$VDLKM_DIR/modules.dep" ]; then
    sed -E -i 's|([^: ]*/)([^/]*\.ko)([:]?)([ ]|$)|/vendor_dlkm/lib/modules/\2\3\4|g' "$VDLKM_DIR/modules.dep"
fi


echo -e "\n=== BUILD SUMMARY ==="
echo "Target: $TARGET"
echo "Kernel: $(ls -lh $KERNEL_COPY_TO/Image 2>/dev/null | awk '{print $5}' || echo 'NOT FOUND')"
echo "DTBs: $(ls $DTB_COPY_TO/*.dtb 2>/dev/null | wc -l || echo '0') files"
echo "DTBO: $(ls -lh $DTBO_COPY_TO 2>/dev/null | awk '{print $5}' || echo 'NOT FOUND')"
echo "Vendor Ramdisk: $(ls $VBOOT_DIR/*.ko 2>/dev/null | wc -l || echo '0') modules"
echo "Vendor DLKM: $(ls $VDLKM_DIR/*.ko 2>/dev/null | wc -l || echo '0') modules"
echo "Modules copied: first_stage=$first_stage_count, second_stage=$second_stage_count, vendor_dlkm=$vendor_dlkm_count"

if [ -d "$VBOOT_DIR" ] && [ -d "$VDLKM_DIR" ]; then
    echo "✅ All directories created successfully!"
else
    echo "❌ Some module directories are missing!"
fi

echo -e "\nCompleted in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s)!"
