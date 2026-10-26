#!/bin/bash
#
# Pack the kernel into a fastboot-bootable Android boot image for polaris.
#
#   scripts/pack-boot.sh KERNEL STOCK_BOOT_IMG SHIM_IMAGE OUTPUT.img
#
# Inputs:
#   KERNEL          out/kernel.release.polaris from the xnu workflow
#   STOCK_BOOT_IMG  any stock polaris boot.img (MIUI/Lineage). Only its appended
#                   DTB tail is used: 65 board variants, so ABL keeps picking
#                   the matching one and our device tree stays untouched.
#   SHIM_IMAGE      out/Image from the shim workflow
#
# Everything here is Python plus gzip: no compiler, no SDK.
set -euo pipefail

if [ $# -ne 4 ]; then
	sed -n '2,15p' "$0" >&2
	exit 2
fi

REPO=$(cd "$(dirname "$0")/.." && pwd)
TOOLS=$REPO/loader/tools
KERNEL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
STOCK=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
SHIM=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
OUTPUT=$4

PY=${PY:-python3}
command -v "$PY" >/dev/null 2>&1 || PY=python
"$PY" -c 'import sys' >/dev/null || { echo "error: no python" >&2; exit 1; }

WORK=$REPO/out/pack
rm -rf "$WORK"
mkdir -p "$WORK"

# ABL loads the ramdisk slot at 0x1000000 and the stock DTB sits at 0x1dea000,
# so anything above this overlaps the device tree and the boot silently dies.
LIMIT=$((0x1dea000 - 0x1000000))

echo "== ADT (iPhone10,3 identity + SDM845 topology) =="
"$PY" "$TOOLS/mkadt.py" "$WORK/polaris.adt" --polaris --ram 6

echo "== payload =="
"$PY" "$TOOLS/mkpayload.py" "$KERNEL" "$WORK/polaris.adt" "$WORK/payload.bin" \
	--check-polaris-cpus
SIZE=$(wc -c < "$WORK/payload.bin" | tr -d ' ')
echo "payload: ${SIZE} bytes (limit ${LIMIT})"
if [ "$SIZE" -ge "$LIMIT" ]; then
	echo "error: payload does not fit between 0x1000000 and the DTB" >&2
	exit 1
fi

echo "== DTB tail from the stock boot image =="
"$PY" "$TOOLS/polaris_dtb.py" --from-boot "$STOCK" "$WORK/polaris-dtb.bin"

echo "== boot image =="
gzip -9 -n -c "$SHIM" > "$WORK/kernel.bin"
cat "$WORK/polaris-dtb.bin" >> "$WORK/kernel.bin"
"$PY" "$TOOLS/mkbootimg.py" -o "$OUTPUT" \
	--kernel "$WORK/kernel.bin" --ramdisk "$WORK/payload.bin" \
	--header_version 1 --pagesize 4096 --base 0 \
	--kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1dea000 \
	--os_version 0x200001a2 \
	--cmdline 'console=ttyMSM0,115200n8 androidboot.hardware=qcom androidboot.console=ttyMSM0 msm_rtb.filter=0x237 ehci-hcd.park=3 service_locator.enable=1 swiotlb=2048 androidboot.configfs=true loop.max_part=7 androidboot.usbcontroller=a600000.dwc3 androidboot.boot_devices=soc/1d84000.ufshc cgroup_disable=pressure androidboot.selinux=permissive'

echo
echo "built $OUTPUT ($(wc -c < "$OUTPUT" | tr -d ' ') bytes)"
echo "flash test without touching the phone's storage:"
echo "  fastboot boot $(basename "$OUTPUT")"
