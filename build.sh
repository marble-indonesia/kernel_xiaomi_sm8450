#!/bin/bash

SECONDS=0
LOG_FILE="Compile.log"
>"$LOG_FILE"

TELEGRAM_BOT_TOKEN=""
TELEGRAM_CHAT_ID=""

ANYKERNEL_REPO="https://github.com/pavelc4-playground/AnyKernel3.git"
ANYKERNEL_DIR="../AnyKernel3"
KERNEL_VERSION="5.10.xx"
KERNEL_NAME="Chandelier"

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

if git rev-parse --show-toplevel > /dev/null 2>&1; then
    KP_ROOT="$(git rev-parse --show-toplevel)"
else
    KP_ROOT="$SCRIPT_DIR"
fi

find_aosp_root() {
    local dir="$KP_ROOT"
    while [[ "$dir" != "/" ]]; do
        if [[ -d "$dir/.repo" ]]; then
            echo "$dir"
            return 0
        fi
        dir="$(dirname "$dir")"
    done
    return 1
}

if [ -f ".build.rc" ]; then
    source .build.rc
    echo "Loaded from .build.rc: SRC_ROOT=$SRC_ROOT"
else
    if SRC_ROOT="$(find_aosp_root)"; then
        echo "AOSP Source Root: $SRC_ROOT"
    else
        SRC_ROOT="$(realpath "$KP_ROOT/../..")"
        echo "Using fallback SRC_ROOT: $SRC_ROOT"
    fi
fi

TC_DIR="$SRC_ROOT/prebuilts-master/clang/host/linux-x86/clang-r584948b"
PREBUILTS_DIR="$SRC_ROOT/prebuilts/kernel-build-tools/linux-x86"
BRANCH="$(git branch --show-current)"
MODULES_REPO="sm8450-modules"
DT_REPO="sm8450-devicetrees"

DO_CLEAN=false
ONLY_CONFIG=false
ONLY_KERNEL=false
ONLY_DTB=false
ONLY_MODULES=false
MAKE_ZIP=false
UPLOAD_TG=false
TARGET=
DTB_WILDCARD="*"
DTBO_WILDCARD="*"

echo_i() { echo -e "\n\033[1;36m==> $1\033[0m\n"; }
echo_w() { echo -e "\033[1;33m⚠ $1\033[0m"; }
echo_e() { echo -e "\n\033[1;31m✗ $1\033[0m\n"; }
echo_s() { echo -e "\033[1;32m✓ $1\033[0m"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -c | --clean) DO_CLEAN=true ;;
        -o | --only-config) ONLY_CONFIG=true ;;
        -k | --only-kernel) ONLY_KERNEL=true ;;
        -d | --only-dtb) ONLY_DTB=true ;;
        -m | --only-modules) ONLY_MODULES=true ;;
        -z | --zip) MAKE_ZIP=true ;;
        -t | --telegram) UPLOAD_TG=true ;;
        --src-root) SRC_ROOT="$2"; shift ;;
        --tg-token) TELEGRAM_BOT_TOKEN="$2"; shift ;;
        --tg-chat) TELEGRAM_CHAT_ID="$2"; shift ;;
        -h | --help)
            echo "Usage: $0 [options] <target>"
            echo "Options:"
            echo "  -c, --clean         Clean build"
            echo "  -o, --only-config   Generate config only"
            echo "  -k, --only-kernel   Build kernel only"
            echo "  -d, --only-dtb      Build DTB only"
            echo "  -m, --only-modules  Build modules only"
            echo "  -z, --zip           Create AnyKernel3 zip"
            echo "  -t, --telegram      Upload to Telegram"
            echo "  --src-root PATH     Override SRC_ROOT"
            echo "  --tg-token TOKEN    Telegram bot token"
            echo "  --tg-chat ID        Telegram chat ID"
            exit 0
            ;;
        *) TARGET="$1" ;;
    esac
    shift
done

if [ -z "$TARGET" ]; then
    echo "Target (device) not specified!"
    echo "Example: $0 marble"
    echo "Example with zip: $0 -z marble"
    echo "Example with Telegram: $0 -z -t marble"
    exit 1
fi

KERNEL_DIR="$SRC_ROOT/device/xiaomi/$TARGET-kernel"

if [ ! -d "$KERNEL_DIR" ]; then
    echo_i "Setting up kernel output directory structure..."
    mkdir -p "$KERNEL_DIR" || {
        echo_e "Failed to create $KERNEL_DIR"
        exit 1
    }
    mkdir -p "$KERNEL_DIR/dtbs"
    mkdir -p "$KERNEL_DIR/vendor_ramdisk"
    mkdir -p "$KERNEL_DIR/vendor_dlkm"
    echo_s "Created kernel directory structure"
else
    mkdir -p "$KERNEL_DIR/dtbs"
    mkdir -p "$KERNEL_DIR/vendor_ramdisk"
    mkdir -p "$KERNEL_DIR/vendor_dlkm"
fi

KERNEL_COPY_TO="$KERNEL_DIR"
DTB_COPY_TO="$KERNEL_DIR/dtbs"
DTBO_COPY_TO="$DTB_COPY_TO/dtbo.img"
VBOOT_DIR="$KERNEL_DIR/vendor_ramdisk"
VDLKM_DIR="$KERNEL_DIR/vendor_dlkm"

DEFCONFIG="gki_defconfig"
DEFCONFIGS="vendor/waipio_GKI.config \
vendor/xiaomi_GKI.config \
vendor/debugfs.config \
vendor/addon.config"

MODULES_SRC="../$MODULES_REPO/qcom/opensource"
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

DTC_PATH="$PREBUILTS_DIR/bin/dtc"
UFDT_PATH="$PREBUILTS_DIR/bin/ufdt_apply_overlay"

if ! command -v python3 &> /dev/null; then
    echo_e "python3 not found! Install with: sudo apt install python3"
    exit 1
fi

if [ ! -f "$DTC_PATH" ]; then
    echo_w "Prebuilt DTC not found at $DTC_PATH"
    if command -v dtc &> /dev/null; then
        DTC_PATH="$(command -v dtc)"
        echo_s "Using system DTC: $DTC_PATH"
        dtc --version
    else
        echo_e "DTC not found! Install with: sudo apt install device-tree-compiler"
        exit 1
    fi
fi

if [ ! -f "$UFDT_PATH" ]; then
    echo_w "ufdt_apply_overlay not found, DTB overlay testing will be skipped"
    UFDT_PATH=""
fi

m() {
    local make_args=(
        -j$(nproc --all)
        O=out
        ARCH=arm64
        LLVM=1
        LLVM_IAS=1
        CC=$CC
        CXX=$CXX
        HOSTCC=$HOSTCC
        HOSTCXX=$HOSTCXX
        LD=$LD
        AR=$AR
        NM=$NM
        OBJCOPY=$OBJCOPY
        OBJDUMP=$OBJDUMP
        STRIP=$STRIP
        DTC_EXT="$DTC_PATH"
        TARGET_PRODUCT=$TARGET
    )

    if [ -n "$UFDT_PATH" ]; then
        make_args+=(DTC_OVERLAY_TEST_EXT="$UFDT_PATH")
    fi

    make "${make_args[@]}" "$@" 2> >(tee -a "$LOG_FILE") || exit $?
}

build_all() {
    echo_i "Building kernel, modules, and dtbs..."
    m Image modules dtbs
    echo_s "Kernel compiled successfully!"

    rm -rf out/modules
    m INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install
}

build_techpack_modules() {
    echo_i "Building techpack modules..."
	if [ ! -d "$MODULES_SRC" ]; then
      echo_w "Module sources not found at $MODULES_SRC"
      echo_w "Skipping techpack modules..."
      return 0
    fi

    for module in $MODULES; do
	if [ ! -d "$MODULES_SRC/$module" ]; then
        echo_w "Module $module not found, skipping..."
        continue
    fi
        echo "Building $module..."
        m -C $MODULES_SRC/$module M=$MODULES_SRC/$module KERNEL_SRC="$(pwd)" OUT_DIR="$(pwd)/out"
        m -C $MODULES_SRC/$module M=$MODULES_SRC/$module KERNEL_SRC="$(pwd)" OUT_DIR="$(pwd)/out" \
            INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install
    done
    echo_s "Techpack modules build completed"
}

copy_kernel() {
    echo_i "Copying kernel..."
    cp out/arch/arm64/boot/Image $KERNEL_COPY_TO
    echo_s "Copied kernel to $KERNEL_COPY_TO"
}

process_dtbs() {
    echo_i "Merging dtbs..."
    rm -rf out/dtbs{,-base}
    mkdir -p out/dtbs out/dtbs-base

    mv out/arch/arm64/boot/dts/vendor/qcom/$DTB_WILDCARD.dtb \
       out/arch/arm64/boot/dts/vendor/qcom/$DTBO_WILDCARD.dtbo \
       out/dtbs-base 2>/dev/null || {
        echo_w "Some DTB files not found, continuing..."
    }

    rm -f out/arch/arm64/boot/dts/vendor/qcom/*.dtbo

    if [ -f "$SRC_ROOT/build/android/merge_dtbs.py" ]; then
        python3 "$SRC_ROOT/build/android/merge_dtbs.py" out/dtbs-base out/arch/arm64/boot/dts/vendor/qcom/ out/dtbs 2> >(tee -a "$LOG_FILE") || exit $?
    else
		echo_w "merge_dtbs.py not found at $SRC_ROOT/build/android/merge_dtbs.py"
        echo_w "Copying DTBs directly..."
        cp out/arch/arm64/boot/dts/vendor/qcom/*.dtb out/dtbs/ 2>/dev/null || true
    fi

    if [ -d "$DTB_COPY_TO" ]; then
        rm -f $DTB_COPY_TO/*.dtb
        cp out/dtbs/*.dtb $DTB_COPY_TO 2>/dev/null || echo_w "No DTB files to copy"
    else
        rm -f $DTB_COPY_TO
        cat out/dtbs/*.dtb >> $DTB_COPY_TO 2>/dev/null || echo_w "No DTB files"
    fi
    echo_s "Copied dtb(s) to $DTB_COPY_TO"

    if command -v mkdtboimg.py &> /dev/null; then
        if ls out/dtbs/*.dtbo 1> /dev/null 2>&1; then
            mkdtboimg.py create $DTBO_COPY_TO --page_size=4096 out/dtbs/*.dtbo 2> >(tee -a "$LOG_FILE") || {
                echo_w "Failed to create DTBO image"
            }
            echo_s "Generated dtbo.img to $DTBO_COPY_TO"
        else
            echo_w "No DTBO files found"
        fi
    else
        echo_w "mkdtboimg.py not found, skipping DTBO"
    fi
}

install_modules() {
    echo_i "Installing modules..."

    first_stage_modules="$(cat modules.list.msm.waipio 2>/dev/null || echo '')"
    second_stage_modules="$(cat modules.list.second_stage modules.list.second_stage.$TARGET 2>/dev/null || echo '')"
    vendor_dlkm_modules="$(cat modules.list.vendor_dlkm modules.list.vendor_dlkm.$TARGET 2>/dev/null || echo '')"
    modules_out="out/modules/lib/modules/$(ls -t out/modules/lib/modules/ | head -n1)"

    rm -rf "$VBOOT_DIR" "$VDLKM_DIR"
    mkdir -p "$VBOOT_DIR" "$VDLKM_DIR"

    for module in $first_stage_modules; do
        mod_path=$(find $modules_out -name "$module" -print -quit)
        [ -z "$mod_path" ] && continue
        cp $mod_path $VBOOT_DIR
        echo $module >> $VBOOT_DIR/modules.load
        echo $module >> $VBOOT_DIR/modules.load.recovery
    done

    for module in $second_stage_modules; do
        mod_path=$(find $modules_out -name "$module" -print -quit)
        [ -z "$mod_path" ] && continue
        cp $mod_path $VBOOT_DIR
        cp $mod_path $VDLKM_DIR
        echo $module >> $VBOOT_DIR/modules.load.recovery
        echo $module >> $VDLKM_DIR/modules.load
    done

    for module in $vendor_dlkm_modules; do
        mod_path=$(find $modules_out -name "$module" -print -quit)
        [ -z "$mod_path" ] && continue
        cp $mod_path $VDLKM_DIR
        echo $module >> $VDLKM_DIR/modules.load
    done

    for dest_dir in $VBOOT_DIR $VDLKM_DIR; do
        [ -f "modules.vendor_blocklist.msm.waipio" ] && cp modules.vendor_blocklist.msm.waipio $dest_dir/modules.blocklist
        cp $modules_out/modules.{alias,dep,softdep} $dest_dir 2>/dev/null || true
    done

    [ -f "$VBOOT_DIR/modules.dep" ] && sed -E -i 's#([^: ]*/)([^/]*\.ko)([:]?)([ ]|$)#/lib/modules/\2\3\4#g' "$VBOOT_DIR/modules.dep"
    [ -f "$VDLKM_DIR/modules.dep" ] && sed -E -i 's#([^: ]*/)([^/]*\.ko)([:]?)([ ]|$)#/vendor_dlkm/lib/modules/\2\3\4#g' "$VDLKM_DIR/modules.dep"

    echo_s "Modules installation completed"
}

setup_anykernel() {
    echo_i "Setting up AnyKernel3..."

    if [ ! -d "$ANYKERNEL_DIR" ]; then
        echo "Cloning AnyKernel3 repository..."
        git clone --depth=1 "$ANYKERNEL_REPO" "$ANYKERNEL_DIR" || {
            echo_e "Failed to clone AnyKernel3"
            return 1
        }
    else
        echo "AnyKernel3 already exists, pulling updates..."
        git -C "$ANYKERNEL_DIR" pull || echo_w "Failed to update, using existing"
    fi

    rm -rf "$ANYKERNEL_DIR"/*.zip
    rm -rf "$ANYKERNEL_DIR"/Image*
    rm -rf "$ANYKERNEL_DIR"/dtb*
    rm -rf "$ANYKERNEL_DIR"/modules

    echo_s "AnyKernel3 ready"
}

create_zip() {
    echo_i "Creating AnyKernel3 flashable zip..."
    setup_anykernel || return 1

    cp "$KERNEL_COPY_TO/Image" "$ANYKERNEL_DIR/" || {
        echo_e "Failed to copy kernel Image"
        return 1
    }


    if [ -d "$DTB_COPY_TO" ]; then
        cp -r "$DTB_COPY_TO" "$ANYKERNEL_DIR/" || echo_w "Failed to copy DTBs"
    fi


    if [ -f "$DTBO_COPY_TO" ]; then
        cp "$DTBO_COPY_TO" "$ANYKERNEL_DIR/" || echo_w "Failed to copy DTBO"
    fi


    if [ -d "$VDLKM_DIR" ] && [ "$(ls -A $VDLKM_DIR)" ]; then
        mkdir -p "$ANYKERNEL_DIR/modules/vendor_dlkm"
        cp -r "$VDLKM_DIR"/* "$ANYKERNEL_DIR/modules/vendor_dlkm/" || echo_w "Failed to copy modules"
    fi


    local date_str="$(date +%Y%m%d)"
    local time_str="$(date +%H%M)"
    ZIP_NAME="${KERNEL_NAME}-${KERNEL_VERSION}-${date_str}-${TARGET}.zip"
    ZIP_PATH="$ANYKERNEL_DIR/$ZIP_NAME"


    cd "$ANYKERNEL_DIR" || return 1
    echo "Zipping $ZIP_NAME..."
    zip -r9 "$ZIP_NAME" * -x .git README.md .gitignore *.zip 2>&1 | tee -a "$KP_ROOT/$LOG_FILE"
    cd - > /dev/null

    if [ -f "$ZIP_PATH" ]; then
        echo_s "Flashable zip created: $ZIP_PATH"
        return 0
    else
        echo_e "Failed to create zip"
        return 1
    fi
}

send_to_telegram() {
    local file="$1"

    if [ -z "$TELEGRAM_BOT_TOKEN" ] || [ -z "$TELEGRAM_CHAT_ID" ]; then
        echo_e "Telegram bot token or chat ID not configured!"
        return 1
    fi

    if [ ! -f "$file" ]; then
        echo_e "File not found: $file"
        return 1
    fi

    echo_i "Uploading to Telegram..."

    local md5=$(md5sum "$file" | awk '{print $1}')
    local sha256=$(sha256sum "$file" | awk '{print $1}')
    local filename=$(basename "$file")
    local filesize=$(du -h "$file" | awk '{print $1}')
    local hours=$((SECONDS / 3600))
    local minutes=$(((SECONDS % 3600) / 60))
    local secs=$((SECONDS % 60))
    local build_time=$(printf "%dh:%02dm:%02ds" $hours $minutes $secs)
    local kernel_ver=$(grep "Linux/arm64" out/.config 2>/dev/null | head -1 | awk '{print $3}' || echo "Unknown")
    local compiler=$($CC --version | head -1 || echo "Clang")
    local caption="<b>KERNEL BUILD SUCCESS!</b>
	${KERNEL_NAME} Kernel compiled successfully!

	<b>Device:</b> ${TARGET}
	<b>Kernel Version:</b> ${kernel_ver}
	<b>Compiler:</b> ${compiler}
	<b>Build Time:</b> ${build_time}
	<b>File Name:</b> ${filename}
	<b>File Size:</b> ${filesize}

	<b>Checksums:</b>
	<b>MD5:</b> <code>${md5}</code>
	<b>SHA256:</b> <code>${sha256}</code>"

    local response=$(curl -s -X POST \
        "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendDocument" \
        -F "chat_id=${TELEGRAM_CHAT_ID}" \
        -F "document=@${file}" \
        -F "caption=${caption}" \
        -F "parse_mode=HTML")

    if echo "$response" | grep -q '"ok":true'; then
        echo_s "Successfully uploaded to Telegram!"
        return 0
    else
        echo_e "Failed to upload to Telegram"
        echo "Response: $response"
        return 1
    fi
}

$DO_CLEAN && {
    rm -rf out $MODULES_REPO
    echo_s "Cleaned output directories"
}

mkdir -p out

echo_i "Generating config..."
m $DEFCONFIG
m ./scripts/kconfig/merge_config.sh $DEFCONFIGS vendor/${TARGET}_GKI.config
scripts/config --file out/.config -d LOCALVERSION_AUTO

$ONLY_CONFIG && exit

if $ONLY_KERNEL; then
    m Image
    copy_kernel
elif $ONLY_DTB; then
    m dtbs
    process_dtbs
elif $ONLY_MODULES; then
    m modules
    build_techpack_modules
    install_modules
else
    build_all
    build_techpack_modules
    copy_kernel
    process_dtbs
    install_modules
fi

if $MAKE_ZIP; then
    create_zip || echo_e "Failed to create zip"

    if $UPLOAD_TG && [ -f "$ZIP_PATH" ]; then
        send_to_telegram "$ZIP_PATH"
    fi
fi

echo_i "Completed in $((SECONDS / 60))m $((SECONDS % 60))s!"
