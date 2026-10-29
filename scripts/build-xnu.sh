#!/bin/bash
# Build xnu-7195 (iOS 14.8 / Darwin 20.6 kernel generation) on a macOS runner.
#
# No third-party build helper is used: xnu-7195 predates the Kernel Debug Kit
# requirement (no KDKROOT), and the one closed archive it used to link against
# (libfirehose_kernel.a) is replaced by in-tree stubs, so a plain in-tree
# `make all` is the whole build.
#
# Environment:
#   XNU_TAG         git tag of apple-oss-distributions/xnu   (xnu-7195.141.2)
#   KERNEL_CONFIG   RELEASE | DEVELOPMENT | DEBUG            (RELEASE)
#   MACHINE_CONFIG  T8010 | T8020 | T8101 | ... | POLARIS    (T8010)
#   PATCH_SET       series | none                            (series)
#   SDK_MODE        host | macos11.3                         (host)
#   JOBS            parallel make jobs                       (hw.ncpu)
#   REPO            checkout of this repository              (script's parent)
set -uo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
XNU_TAG="${XNU_TAG:-xnu-7195.141.2}"
KERNEL_CONFIG="${KERNEL_CONFIG:-RELEASE}"
MACHINE_CONFIG="${MACHINE_CONFIG:-T8010}"
PATCH_SET="${PATCH_SET:-series}"
SDK_MODE="${SDK_MODE:-host}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 3)}"
SRC="${SRC:-${REPO}/xnu}"
LOG="${REPO}/build.log"

step() { printf '\n===== %s =====\n' "$*"; }
die()  { printf '\nFATAL: %s\n' "$*" | tee -a "$LOG"; exit 1; }

: > "$LOG"
exec > >(tee -a "$LOG") 2>&1

step "host toolchain"
sw_vers
xcodebuild -version || true
HOST_SDK="$(xcrun --sdk macosx --show-sdk-path)"
echo "host SDK: ${HOST_SDK}"
echo "host SDK version: $(xcrun --sdk macosx --show-sdk-version)"

SDKROOT_OVERRIDE=""
if [ "$SDK_MODE" = "macos11.3" ]; then
	step "fetch macOS 11.3 SDK (last SDK matching the xnu-7195 generation)"
	# sparse checkout of a single SDK out of a large mirror repository
	if [ ! -d "${REPO}/sdk/MacOSX11.3.sdk" ]; then
		git clone --depth 1 --filter=blob:none --sparse \
			https://github.com/phracker/MacOSX-SDKs "${REPO}/sdk" || die "SDK clone failed"
		git -C "${REPO}/sdk" sparse-checkout set MacOSX11.3.sdk || die "sparse-checkout failed"
	fi
	SDKROOT_OVERRIDE="${REPO}/sdk/MacOSX11.3.sdk"
	[ -d "${SDKROOT_OVERRIDE}/System/Library/Frameworks/Kernel.framework" ] \
		|| die "11.3 SDK has no Kernel.framework (needed for PrivateHeaders)"
	echo "SDKROOT override: ${SDKROOT_OVERRIDE}"
fi

step "clone ${XNU_TAG}"
rm -rf "$SRC"
git clone --depth 1 --branch "$XNU_TAG" \
	https://github.com/apple-oss-distributions/xnu.git "$SRC" || die "clone failed"
cd "$SRC" || die "cd $SRC"
echo "commit: $(git rev-parse HEAD)"
echo "files:  $(git ls-files | wc -l)"

if [ "$PATCH_SET" = "series" ]; then
	step "apply patch series"
	while IFS= read -r p; do
		case "$p" in ''|\#*) continue ;; esac
		f="${REPO}/patches/${p}"
		[ -f "$f" ] || die "missing patch ${p}"
		# A patch carrying CR bytes applies or not depending on the git version's
		# CRLF heuristic -- it passed locally and failed on the runner. Refuse it.
		if [ "$(LC_ALL=C tr -d '\r' < "$f" | wc -c)" != "$(wc -c < "$f")" ]; then
			die "patch ${p} contains CR bytes; regenerate it as a git diff on an LF tree"
		fi
		if ! git apply -p1 --check "$f"; then
			printf 'APPLY-CHECK FAILED: %s\n' "$p"
			die "patch does not apply to ${XNU_TAG}"
		fi
		git apply -p1 "$f" || die "apply failed: ${p}"
		printf 'ok  %s\n' "$p"
	done < "${REPO}/patches/series"
	git status --porcelain
else
	echo "patch set disabled (PATCH_SET=${PATCH_SET})"
fi

step "userland"
# The root filesystem the kernel carries is built around the compiled
# userland, so it cannot live in the patch series: compile it here and patch
# the result in afterwards.  If any of this fails the kernel still builds --
# the image from the patch series (the hand written stub) stays in place.
if bash "${REPO}/scripts/build-userland.sh"; then
	if python3 "${REPO}/tools/mkuserland.py" --xnu "$SRC" \
		--binaries "${REPO}/out/userland" --patch "${REPO}/out/userland.patch"; then
		if git -C "$SRC" apply -p1 "${REPO}/out/userland.patch"; then
			echo "ok  userland"
		else
			echo "WARNING: the userland patch did not apply; keeping the stub image"
			git -C "$SRC" checkout -- bsd/kern/bsd_init.c
		fi
	else
		echo "WARNING: could not build the userland image; keeping the stub image"
	fi
else
	echo "WARNING: the userland did not compile; keeping the stub image"
fi

step "closed kernel headers"
# xnu-7195 includes <os/firehose_buffer_private.h>, which is not part of the
# public macOS SDK: Apple's own builds get it from the internal SDK. The runner
# image ships Kernel Debug Kits, whose PrivateHeaders carry the same family of
# headers. Report what is available, and install the exact missing header into
# the tree so the include chain stays intact (nothing is committed).
ls -d /Library/Developer/KDKs/* 2>/dev/null || echo "no KDK installed"
for name in firehose_buffer_private.h; do
	if [ -e "libkern/os/$name" ]; then
		echo "libkern/os/$name already provided by the patch series"
		continue
	fi
	hit=""
	for root in /Library/Developer/KDKs /Applications/Xcode*.app; do
		[ -e "$root" ] || continue
		hit=$(find "$root" -name "$name" -path '*PrivateHeaders*' 2>/dev/null | head -1)
		[ -n "$hit" ] && break
	done
	if [ -n "$hit" ]; then
		cp "$hit" "libkern/os/$name"
		echo "installed libkern/os/$name  <-  $hit"
	else
		echo "MISSING $name: no copy in any KDK or Xcode tree"
	fi
done

step "make TARGET_CONFIGS=\"${KERNEL_CONFIG} ARM64 ${MACHINE_CONFIG}\" -j${JOBS}"
# `all`, not `install`: do_build_all:: do_build_kernel produces
# BUILD/obj/kernel.release, while the install half would also try to write
# /System/Library/Kernels through DSTROOT and fail under SIP for no benefit.
#
# Xcode 15 dropped ctfconvert/ctfmerge/ctf_insert/scan-build. MakeInc.cmd probes
# each of them with `$(shell xcrun -sdk ... -find X)` guarded by `ifeq ($(X),)`,
# and because the probe yields an empty string that guard is true on every
# sub-make re-entry -- run #8 emitted 1776 copies of the same four messages.
# Seeding the variables non-empty short-circuits the probes; DO_CTFMERGE=0 is
# still required, because with it set the link recipe would run
# `ctfmerge ... -o $@` and `true` would silently leave the kernel file unwritten.
export CTFCONVERT=true CTFMERGE=true CTFINSERT=true SCAN_BUILD=true
MAKE_ARGS=(
	all -j"${JOBS}" -k
	"SDKROOT=macosx"
	"TARGET_CONFIGS=${KERNEL_CONFIG} ARM64 ${MACHINE_CONFIG}"
	"BUILD_WERROR=0" "BUILD_LTO=0" "LOGCOLORS=n" "CONCISE=0"
	"DO_CTFMERGE=0"
)

# `LD_KERNEL_LIBS = -lcc_kext` is unconditional in MakeInc.def:784. Apple ships
# that archive either in the KDK (KernelSupport) or inside an internal SDK, and
# a missing -l is a hard ld error, so probe before spending the build: if no
# copy is reachable, drop the flag and let the link report whatever symbols were
# actually needed. Anything printed here is worth reading in the log.
step "kernel link prerequisites"
CC_KEXT=""
for cand in \
	"$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)/usr/lib/libcc_kext.a" \
	"$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)/usr/local/lib/kernel/libcc_kext.a" \
	/Library/Developer/KDKs/*/System/Library/KernelSupport/libcc_kext.a
do
	if [ -e "$cand" ]; then CC_KEXT="$cand"; break; fi
done
if [ -n "$CC_KEXT" ]; then
	echo "libcc_kext.a: ${CC_KEXT}"
else
	echo "libcc_kext.a: NOT FOUND -- dropping -lcc_kext from the link"
	MAKE_ARGS+=( "LD_KERNEL_LIBS=" )
fi
ls /Library/Developer/KDKs/*/System/Library/KernelSupport/*.a 2>/dev/null | head -10 || echo "no KDK KernelSupport archives"

if [ -n "$SDKROOT_OVERRIDE" ]; then
	MAKE_ARGS+=( "SDKROOT=${SDKROOT_OVERRIDE}" "ALT_SDKROOT=${SDKROOT_OVERRIDE}" )
fi
make "${MAKE_ARGS[@]}"
rc=$?

step "result"
echo "make exit: ${rc}"
# The ctf/scan-build probes print "xcrun: error:" on every makefile include, so
# a raw `grep -c error:` reports thousands of false positives. Drop them.
REAL_ERR_RE='error:|fatal error|undefined reference|library not found|duplicate symbol'
NOISE_RE='ctfconvert|ctfmerge|ctf_insert|scan-build|unable to find utility|xcodebuild -sdk'
echo "compiler errors (excluding missing Xcode tools): $(grep -E "$REAL_ERR_RE" "$LOG" | grep -vE "$NOISE_RE" | wc -l || true)"
echo "---- first 40 real compiler errors ----"
grep -E "$REAL_ERR_RE" "$LOG" | grep -vE "$NOISE_RE" | head -40 || true
# linker failures do not contain "error:", so they need their own pass
echo "---- linker diagnostics ----"
grep -nE '^ld: |Undefined symbols|library not found|-lfirehose|duplicate symbol' "$LOG" | head -60 || true
echo "---- kernels produced ----"
find "${SRC}/BUILD" -name 'kernel.*' -type f 2>/dev/null | while read -r k; do
	printf '%s  %s bytes\n' "$k" "$(stat -f%z "$k")"
	shasum -a 256 "$k"
done
mkdir -p "${REPO}/out"
# Skip the 82 MB object archive: it is only useful for local relinking and it
# dominates the artifact size.
find "${SRC}/BUILD" -name 'kernel.*' -type f ! -name '*.a' -exec cp {} "${REPO}/out/" \; 2>/dev/null
cp "$LOG" "${REPO}/out/build.log" 2>/dev/null
if ls "${REPO}/out"/kernel.release.* >/dev/null 2>&1; then
	echo "KERNEL BUILT: $(cd "${REPO}/out" && ls kernel.release.* | tr '\n' ' ')"
else
	echo "NO KERNEL IN out/ -- see build.log"
fi
exit "$rc"
