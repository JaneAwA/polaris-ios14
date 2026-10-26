#!/bin/bash
#
# Build the EL1 shim that hands control to the XNU kernel on polaris.
#
# The shim is not part of the kernel: it runs as an Android boot-image payload,
# sets up the MMU, parses the Mach-O, installs the device tree and jumps to the
# kernel entry. It only needs a freestanding aarch64 compiler; there is no
# libc, no SDK and no XNU dependency.
#
#   ZIG=/path/to/zig scripts/build-shim.sh        # cross, from any host
#
# Output: out/Image
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$REPO/loader/shim
TOOLS=$REPO/loader/tools
OUT=$REPO/out
mkdir -p "$OUT"

PY=${PY:-python3}
command -v "$PY" >/dev/null 2>&1 || PY=python
"$PY" -c 'import sys' >/dev/null || { echo "error: no python" >&2; exit 1; }

if [ -n "${ZIG:-}" ] && [ -x "$ZIG" ]; then
	CC="$ZIG cc -target aarch64-freestanding"
elif command -v zig >/dev/null 2>&1; then
	CC="zig cc -target aarch64-freestanding"
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
	CC=aarch64-linux-gnu-gcc
elif command -v aarch64-unknown-linux-gnu-gcc >/dev/null 2>&1; then
	CC=aarch64-unknown-linux-gnu-gcc
else
	echo "error: no aarch64 cross compiler and no zig" >&2
	echo "       set ZIG=/path/to/zig" >&2
	exit 1
fi

CFLAGS="-O2 -g0 -ffreestanding -nostdlib -fno-builtin -fno-pic -mgeneral-regs-only \
	-mstrict-align -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -mno-outline-atomics -Wall -Wextra -DPOLARIS"

OBJS=()
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c libc.c; do
	o=$OUT/${f%.*}.o
	$CC $CFLAGS -c "$SHIM/$f" -o "$o"
	OBJS+=("$o")
done

# Every C object must be free of absolute relocations: the image is linked at 0
# but executes wherever ABL put it. start.o is excluded because the entry stub
# legitimately loads a literal address.
"$PY" "$TOOLS/elfcheck.py" abs64 "${OBJS[@]:1}"

$CC $CFLAGS -T "$SHIM/shim.ld" -Wl,--no-gc-sections -Wl,--build-id=none \
	"${OBJS[@]}" -o "$OUT/shim.elf"
"$PY" "$TOOLS/elfcheck.py" relocs "$OUT/shim.elf"
"$PY" "$TOOLS/elf2image.py" "$OUT/shim.elf" "$OUT/Image"

# .bss and the stacks are NOBITS; ABL copies kernel_size bytes, so the file has
# to be padded to _end or the shim starts with uninitialised memory.
size=$("$PY" "$TOOLS/elfcheck.py" symbol "$OUT/shim.elf" _end)
"$PY" -c "import os,sys; os.truncate(sys.argv[1], int(sys.argv[2]))" "$OUT/Image" "$size"
"$PY" "$TOOLS/elfcheck.py" image "$OUT/Image"
echo "shim: out/Image ($(wc -c < "$OUT/Image" | tr -d ' ') bytes)"
