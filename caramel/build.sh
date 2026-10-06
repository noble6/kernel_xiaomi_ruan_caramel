#!/bin/bash
# Build caramel and package it as an AnyKernel3 zip.
#
#   caramel/build.sh [clang dir]
#
# Needs AOSP clang-r416183b, the compiler of the GKI builds; another
# compiler changes the symbol CRCs and the stock vendor modules no longer
# load. Default: prebuilts/clang/kernel/linux-x86/clang-r416183b of a ROM
# tree, or $CLANG_DIR.
set -euo pipefail

KDIR=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$KDIR/out}
CLANG_DIR=${1:-${CLANG_DIR:-}}
AK3_REPO=https://github.com/osm0sis/AnyKernel3
AK3_REV=020dfec

if [ -z "$CLANG_DIR" ] || [ ! -x "$CLANG_DIR/bin/clang" ]; then
	echo "usage: $0 <path to clang-r416183b>" >&2
	exit 1
fi
"$CLANG_DIR/bin/clang" --version | grep -q "r416183b" || {
	echo "error: $CLANG_DIR is not clang-r416183b" >&2
	exit 1
}

export PATH="$CLANG_DIR/bin:$PATH"
MAKE=(make -C "$KDIR" O="$OUT" ARCH=arm64 LLVM=1 LLVM_IAS=1
	CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)")

"${MAKE[@]}" gki_defconfig
"$KDIR/scripts/kconfig/merge_config.sh" -m -O "$OUT" "$OUT/.config" \
	"$KDIR/arch/arm64/configs/vendor/ruan_rom.config" \
	"$KDIR/arch/arm64/configs/vendor/caramel.config"
"${MAKE[@]}" olddefconfig
"${MAKE[@]}" Image

RELEASE=$(cat "$OUT/include/config/kernel.release")
AK3="$OUT/anykernel3"
rm -rf "$AK3"
git clone -q "$AK3_REPO" "$AK3"
git -C "$AK3" checkout -q "$AK3_REV"
rm -rf "$AK3"/.git* "$AK3"/README.md "$AK3"/modules "$AK3"/patch "$AK3"/ramdisk
cp "$KDIR/caramel/anykernel.sh" "$AK3/anykernel.sh"
cp "$KDIR/caramel/banner" "$AK3/banner"
cp "$OUT/arch/arm64/boot/Image" "$AK3/Image"

ZIP="$OUT/caramel-$RELEASE-$(date +%Y%m%d).zip"
rm -f "$ZIP"
(cd "$AK3" && zip -qr9 "$ZIP" . -x '.*')
echo "$ZIP"
