#!/bin/bash
# Cross-compile the polaris userland for arm64 iOS 14.
#
# The board has no dyld and no shared cache, so the userland is built
# freestanding: no libSystem, no libc, just raw system calls (userland/
# polaris.h).  The result is a static arm64 Mach-O, which is exactly what the
# kernel's static exec path (patch 0041) can start.
#
#   output: out/userland/launchd.bin, out/userland/hello.bin
#
# The runner has the macOS SDK only, so the first attempt targets iOS and
# falls back to macOS: the kernel does not look at the platform in
# LC_BUILD_VERSION, only at the CPU type and the load commands.
set -uo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${OUT:-${REPO}/out/userland}"
SRC="${SRC:-${REPO}/userland}"
TOOLS="${REPO}/tools"

mkdir -p "$OUT"

step() { printf '\n===== %s =====\n' "$*"; }

step "toolchain"
xcrun --sdk macosx --show-sdk-path
clang --version | head -2
for l in ld64.lld ld.lld; do
	command -v "$l" >/dev/null 2>&1 && echo "have $l: $(command -v $l)"
done

# -nostdlib          no libSystem, no crt: the entry point is our own
# -fno-stack-protector  nothing to protect against without a libc
# -fno-builtin        no libc builtins either
# -Wl,-e,_polaris_start  the entry point.  The Mach-O symbol of the C
#                       function polaris_start is _polaris_start: the linker
#                       wants the name with the underscore the object
#                       format adds, so asking for _start would look for a C
#                       function called "start" and fail.
COMMON=(-O2 -nostdlib -fno-stack-protector -fno-builtin -Wall)

VARIANTS=(
	"-target arm64-apple-macos11 -Wl,-static"
	"-target arm64-apple-macos11"
	"-target arm64-apple-macos11 -Wl,-platform_version,macos,11.0,11.0"
	"-target arm64-apple-macos13"
	"-target arm64-apple-mios14.0 -Wl,-platform_version,ios,14.0,14.0"
)

build_one() {
	local src="$1" out="$2"
	local v

	for v in "${VARIANTS[@]}"; do
		# shellcheck disable=SC2086
		echo "--- trying: clang ${COMMON[*]} ${v} -Wl,-e,_polaris_start -c $src"
		# shellcheck disable=SC2086
		if ! clang "${COMMON[@]}" ${v} -Wl,-e,_polaris_start -c -o "${out}.o" "$src"; then
			continue
		fi
		echo "--- object symbols:"
	nm -a "${out}.o" 2>/dev/null | head -20 || true
	echo "--- object file:"
		python3 "${TOOLS}/macho_info.py" "${out}.o"
		# shellcheck disable=SC2086
		if clang "${COMMON[@]}" ${v} -Wl,-e,_polaris_start -o "$out" "${out}.o"; then
			echo "--- linked with: ${v}"
			python3 "${TOOLS}/macho_info.py" "$out"
			# There is no dyld on this board, so the program must not
			# need one: every symbol has to be defined in the file.
			undef="$(nm -u "$out" 2>/dev/null | grep -v dyld_stub_binder || true)"
			if [ -n "$undef" ]; then
				echo "--- undefined symbols, the program would need dyld:"
				echo "$undef"
				continue
			fi
			return 0
		fi
		echo "--- link failed with: ${v}"
	done
	return 1
}

for prog in launchd hello; do
	step "compile ${prog}.c"
	if ! build_one "${SRC}/${prog}.c" "${OUT}/${prog}.bin"; then
		echo "FAILED to compile ${prog}.c"
		exit 1
	fi
	ls -l "${OUT}/${prog}.bin"
done

step "done"
