#!/bin/bash
# Build xnu-7195 (iOS 14.8 / Darwin 20.6 kernel generation) on a macOS runner.
#
# No third-party build helper is used: xnu-7195 predates the Kernel Debug Kit
# requirement (no KDKROOT, no libfirehose), so a plain in-tree `make install`
# is the whole build.
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

step "make TARGET_CONFIGS=\"${KERNEL_CONFIG} ARM64 ${MACHINE_CONFIG}\" -j${JOBS}"
MAKE_ARGS=(
	install -j"${JOBS}" -k
	"SDKROOT=macosx"
	"TARGET_CONFIGS=${KERNEL_CONFIG} ARM64 ${MACHINE_CONFIG}"
	"BUILD_WERROR=0" "BUILD_LTO=0" "LOGCOLORS=n" "CONCISE=0"
)
if [ -n "$SDKROOT_OVERRIDE" ]; then
	MAKE_ARGS+=( "SDKROOT=${SDKROOT_OVERRIDE}" "ALT_SDKROOT=${SDKROOT_OVERRIDE}" )
fi
make "${MAKE_ARGS[@]}"
rc=$?

step "result"
echo "make exit: ${rc}"
echo "error lines: $(grep -c 'error:' "$LOG" || true)"
echo "---- first 40 errors ----"
grep -n 'error:' "$LOG" | head -40 || true
echo "---- kernels produced ----"
find "${SRC}/BUILD" -name 'kernel.*' -type f 2>/dev/null | while read -r k; do
	printf '%s  %s bytes\n' "$k" "$(stat -f%z "$k")"
	shasum -a 256 "$k"
done
mkdir -p "${REPO}/out"
find "${SRC}/BUILD" -name 'kernel.*' -type f -exec cp {} "${REPO}/out/" \; 2>/dev/null
cp "$LOG" "${REPO}/out/build.log" 2>/dev/null
exit "$rc"
