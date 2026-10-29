#!/bin/bash
# Cross-compile the polaris userland for arm64 iOS 14.
#
# The board has no dyld and no shared cache, so the userland is built
# freestanding: no libSystem, no libc, just raw system calls (userland/
# polaris.h).  The result is a static arm64 Mach-O, which is exactly what the
# kernel's static exec path (patch 0041) can start.
#
#   output: out/userland/launchd.bin, out/userland/hello.bin
set -uo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${OUT:-${REPO}/out/userland}"
SRC="${SRC:-${REPO}/userland}"
TARGET=arm64-apple-mios14.0

mkdir -p "$OUT"

step() { printf '\n===== %s =====\n' "$*"; }

step "toolchain"
xcrun --sdk macosx --show-sdk-path
clang --version | head -2

# -nostdlib      no libSystem, no crt: the entry point is our own _start
# -fno-stack-protector  nothing to protect against without a libc
# -no_pie        the kernel computes the entry point from __TEXT + entryoff,
#                and it does not slide a non-PIE image
build_one() {
	local src="$1" out="$2"
	local flags=(
		-target "$TARGET"
		-O2
		-nostdlib
		-fno-stack-protector
		-fno-builtin
		-Wall
		-Wl,-no_pie
		-Wl,-e,_start
	)
	clang "${flags[@]}" -o "$out" "$src" || return 1
}

for prog in launchd hello; do
	step "compile ${prog}.c"
	if ! build_one "${SRC}/${prog}.c" "${OUT}/${prog}.bin"; then
		echo "FAILED to compile ${prog}.c"
		exit 1
	fi
	ls -l "${OUT}/${prog}.bin"
done

step "inspect"
for prog in launchd hello; do
	echo "--- ${prog}.bin"
	otool -hv "${OUT}/${prog}.bin" | tail -3
	otool -l "${OUT}/${prog}.bin" | grep -E "LC_MAIN|LC_UNIXTHREAD|LC_LOAD_DYLINKER|LC_SEGMENT_64|entryoff|vmaddr|fileoff|filesize" | head -30
done
