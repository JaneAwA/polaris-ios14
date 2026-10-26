# polaris-ios14 — iOS 14 kernel bring-up for the Xiaomi Mi MIX 2S

Xiaomi Mi MIX 2S (`polaris`, Snapdragon 845 / SDM845, 4×Cortex-A75 + 4×Cortex-A55,
Adreno 630) running the **iOS 14 kernel generation**.

This repository is not a fork of `ios-sweet` (that tree is `xnu-8796` = iOS 16) and
not a fork of `UniXNU` (that tree is `xnu-10063` = iOS 17). It starts from Apple's
own `xnu-7195.141.2` drop, which is the kernel shipped in **iOS 14.8 / macOS 11.6**
(Darwin 20.6), and ports the SWEET patch family forward/backward onto it.

## Why 7195 and not 8796

| iOS | Darwin | xnu | upstream tree |
|---|---|---|---|
| 14.8 | 20.6 | 7195.141.2 | this repository |
| 16.7 | 22.6 | 8796.141.3 | `ios-sweet`, `ios16_on_polaris_mix2s` |
| 17.7 | 23.6 | 10063.x | `UniXNU` |

The iOS 16 chain is the one that previously reached a visible screen on this
device. It cannot be reused for 14: the 4 KiB page support, AMFI/trust-cache
handling and the whole userspace bootstrap are version-specific.

## launchd: the actual failure, in source

`bsd/kern/kern_exec.c` (xnu-7195):

```c
:184   boolean_t unentitled_ios_sim_launch = FALSE;

:6503  #if DEBUG || DEVELOPMENT
:6504  #if XNU_TARGET_OS_OSX
:6505      PE_parse_boot_argn("unentitled_ios_sim_launch", ...);   /* not compiled in RELEASE */
```

```c
:6919  if (proc_platform(p) == PLATFORM_IOS) {
:6927  #if DEBUG || DEVELOPMENT
:6928      if (unentitled_ios_sim_launch) { no_sandbox_entitled = TRUE; }
:6935  #endif
:6936  if (!no_sandbox_entitled)
:6937      os_reason_create(OS_REASON_EXEC, EXEC_EXIT_REASON_WRONG_PLATFORM);
```

`EXEC_EXIT_REASON_WRONG_PLATFORM` is 14 (`bsd/sys/reason.h:249`), which is exactly
the `CS_KILLED ... namespace 9 code 0xe` that kills `launchd` on a RELEASE kernel.
A RELEASE build therefore *cannot* pass this gate no matter what boot arguments are
passed: the tunable is compiled out.

`patches/0001-sweet-ios14-launchd-platform-optin.patch` fixes that with two
independent switches and a fail-closed default:

* `unentitled_ios_sim_launch=1` is parsed in RELEASE as well;
* a second, new switch `sweet_ios_userland=1` must also be present;
* a stock macOS kernel that sets neither behaves bit-for-bit as before.

Boot arguments for this kernel: `unentitled_ios_sim_launch=1 sweet_ios_userland=1`.

## Status

| item | state |
|---|---|
| xnu-7195.141.2 configures, compiles and links on a modern runner | the whole tree compiled by run #8; run #9 reached the link (209 undefined symbols); run #10 turned the open pmap/asm on and found three missing non-Apple sources, fixed by 0010; run #11 verifying |
| launchd platform gate | patched (0001) |
| POLARIS machine config (SDM845, GICv3 Group 1, 16K pages) | written (0003/0004) |
| UFS / HFS+ root | not ported |
| IOMobileFramebuffer / IOSurface | not ported |
| IOHID (touch / keys) | not ported |
| userspace root image | not started; iOS 14 has no cryptex/preboot split, so the root is one HFS+ volume built offline |
| flashable image | **no** |

## What CI has actually proven

| run | config | outcome |
|---|---|---|
| #1 | T8010, 0001 | `build_setup` died: macOS 14.5 SDK has no `usr/local/libexec/availability.pl` (0002) |
| #2 | T8010, 0001-0002 | got past setup, then `-mcpu=hurricane` is not a clang 15 CPU (a stock Apple machine config problem, not ours) |
| #3 | POLARIS, 0001-0003 | 756x `#error Incompatible CPU type configured` (the FSR macro chain in `osfmk/arm/proc_reg.h` did not know POLARIS) + `cpu_signal_handler` undeclared (0004) |
| #4 | POLARIS, 0001-0004 | whole kernel compiled except `libkern/os/log.c`, `bsd/kern/subr_log.c`, `libkern/c++/OSKext.cpp`: `<os/firehose_buffer_private.h>` is closed and no KDK is installed on the runner (0005) |
| #5 | POLARIS, 0001-0005 | cancelled: that revision of 0005 still left `FIREHOSE_BUFFER_KERNEL_CHUNK_COUNT` (used by `log.c:702`) undefined and redefined `FIREHOSE_CHUNK_SIZE` to a value chunk helpers do not use. Fixed before pushing, see below |
| #6 | POLARIS, 0001-0005 (corrected) | cancelled before it reached the link: `LD_KERNEL_ARCHIVES` still carried `-lfirehose_kernel`, and `libfirehose_kernel.a` exists only inside a KDK. Fixed by 0006 |
| #7 | POLARIS, 0001-0006 | died in the patch check, not the build: 0006 had been produced with plain `diff -u` on a CRLF checkout, so a mixed-CR patch passed on the authoring machine and failed on the runner. Regenerated as a `git diff` on an LF tree; the script now refuses patches containing CR bytes |
| #8 | POLARIS, 0001-0006 (LF) | patches accepted, ~11 min of real compilation, then **exactly one** genuine error: `bsd/kern/subr_log.c:77 'os/firehose_buffer_private.h' file not found`. The header exists in the tree, but bsd compiles against `BUILD/obj/EXPORT_HDRS/libkern/`, and 0005 never added the file to `libkern/os/Makefile`'s `EXPORT_MI_LIST`. Fixed by 0007. The other 1776 `error:` lines were `xcrun -find ctfconvert/ctfmerge/ctf_insert/scan-build` probes failing on every sub-make, since Xcode 15 dropped those tools; the build script now seeds them and passes `DO_CTFMERGE=0` |
| #9 | POLARIS, 0001-0007 | the header is gone, the whole tree compiles, and the link ran for the first time: 209 undefined symbols, no compile errors. Two causes, both fixed by 0008/0009 (see below) |
| #10 | POLARIS, 0001-0009 | the open pmap/asm now actually compile, and immediately exposed three gaps, all of them "Apple never ships this source": `amcc_rorgn.c` is not in the drop at all, `start.s` includes a generated `arm64/tunables/tunables.s`, and `pmap.c:15137` uses a CDHash and lock that only the closed pmap defines. Fixed by 0010 |
| #11 | POLARIS, 0001-0010 | running |

### The 209 undefined symbols, and why they are not a porting bug

The link failure of run #9 looked catastrophic and turned out to be three
independent facts about building xnu off an Apple platform.

**1. The open arm64 pmap and asm were never compiled (192 of the 209 symbols).**
`osfmk/conf/files.arm64` does not list `osfmk/arm/pmap.c` as `standard`. It lists
it as

```
osfmk/arm/pmap.c		optional nos_arm_pmap
```

and the same trick hides `osfmk/arm/pcb.c`, `osfmk/arm64/locore.s`, `start.s`,
`cswitch.s`, `machine_routines_asm.s`, `pinst.s`, `gxf_exceptions.s` and
`machine_routines_apple.c` behind `nos_arm_asm`. Apple's normal builds link the
closed-source equivalents instead, so the option is only ever set by ports that
must use the sources -- `config/MASTER.arm64.bcm2837` (the Raspberry Pi 3 port)
sets exactly this option, and it is the only machine file in the tree that does:

```
#  ARM_EXTRAS_BASE = [ nos_arm_pmap nos_arm_asm ]
```

`MASTER.arm64` already references `ARM_EXTRAS_BASE` inside `KERNEL_BASE` but
never defines it. 0008 adds `config/MASTER.arm64.polaris`, selected by doconf as
`MASTER.<cpu>.<soc>`, which defines it.

**2. `___chkstk_darwin` came from libcc_kext.a.** clang calls it from any
function whose frame exceeds a page, with the size in `x9`; the kernel variants
are in `sleh.o`, `exception.o`, `processor_core.o`, `video_console.o` and a few
more. Nothing in the open tree implements it, and `libcc_kext.a` ships only in a
KDK -- which is also why 0006 had to drop that archive. 0009 adds a 15-instruction
probe loop in `osfmk/arm64/chkstk_darwin.s` that preserves `x9` and clobbers only
`x10`/`x11`, so it satisfies callers under either convention.

**3. Two `__llvm_profile_*_internal` and five IOMMU KPIs.** `OSKextGrabPgoDataLocked()`
calls the profiling runtime unconditionally; the source is in-tree at
`bsd/pgo/profile_runtime.c` but tagged `optional pgo`, so 0008 adds `pgo` to
`BSD_RELEASE` in the polaris MASTER. `config/Private.arm64.exports` demands five
symbols that only Apple's closed IOMMU drivers define (`_sart_get_desc`,
`_t8020dart_get_desc`, `_t8020dart_vo_tte`, `_uat_get_desc`,
`_nvme_ppl_get_desc`); 0009 removes them from the export list, since a kernel
that does not implement a KPI should not promise it.

### What the closed header has to supply, determined by static audit

Instead of one CI round-trip per missing identifier, the four translation units that
consume `<os/firehose_buffer_private.h>` (`libkern/os/log.c`, `bsd/kern/subr_log.c`,
`libkern/c++/OSKext.cpp` and, through `<os/trace_internal.h>`, `libkern/os/internal.c`)
were audited against every open firehose header in the tree. Result:

* every `firehose_*` type and every `firehose_tracepoint_flags_*` constant is already
  open — `_firehose_tracepoint_flags_activity_t` is generated by
  `OS_OPTIONS(_firehose_tracepoint_flags_activity, uint16_t, ...)` in
  `libkern/firehose/firehose_types_private.h:186`, so `trace_internal.h` compiles on its
  own and `internal.c` was never broken;
* the only genuinely closed pieces are the five entry points (`__firehose_buffer_create`,
  `__firehose_buffer_tracepoint_reserve`, `__firehose_buffer_tracepoint_flush`,
  `__firehose_merge_updates`, `__firehose_kernel_configuration_valid`) and two geometry
  macros: `FIREHOSE_BUFFER_KERNEL_CHUNK_COUNT` plus the `_DEFAULT_CHUNK_COUNT` /
  `_DEFAULT_IO_PAGES` pair used by `oslog_init()`.

The chunk size is **not** one of them: `firehose/chunk_private.h:30` already defines
`FIREHOSE_CHUNK_SIZE` as 4096, and the ring descriptors in `subr_log.c` are built from
that value. The stub therefore inherits it instead of defining its own.


## Patch series

Applied in the order listed in `patches/series`:

| patch | what it does | why |
|---|---|---|
| `0001-sweet-ios14-launchd-platform-optin` | parses `unentitled_ios_sim_launch` in RELEASE behind a second switch | without it launchd dies at `EXEC_EXIT_REASON_WRONG_PLATFORM` |
| `0002-sweet-build-symbol-aliasing-without-availability-pl` | `bsd/sys/make_symbol_aliasing.sh` falls back to a built-in release table | macOS 14.5 SDK dropped `usr/local/libexec/availability.pl`; `build_setup` died before any compile |
| `0003-sweet-polaris-machine-config` | `MACHINE_CONFIGS=POLARIS`: machine flags, `POLARIS.h`, board config, cache lines, GIC Group 1 setup, Group 1 IRQ routing in `sleh_irq` | this is what makes the kernel target SDM845 instead of an Apple SoC |
| `0004-sweet-polaris-irq-declarations` | adds POLARIS to the FSR fault-class macro chain, includes `<arm/machine_cpu.h>` in `sleh.c` | without it: `#error Incompatible CPU type configured`, and `cpu_signal_handler` has no prototype |
| `0005-sweet-firehose-buffer-stub` | `libkern/os/firehose_buffer_private.h` (five prototypes, `FIREHOSE_BUFFER_KERNEL_CHUNK_COUNT`, the two `oslog_init` defaults) plus five inert entry points in `subr_log.c` | the header is closed source in every xnu drop; without it three TUs cannot compile and there is no KDK on the runner |
| `0006-sweet-link-without-kdk-firehose-archive` | `LD_KERNEL_ARCHIVES` drops `-lfirehose_kernel` unless the SDK actually ships `libfirehose_kernel.a` | the archive lives only in a KDK, and with the five entry points implemented in-tree it is not needed; without this, `ld` fails *after* a full 25-minute compile |
| `0007-sweet-export-firehose-header` | adds `firehose_buffer_private.h` to `EXPORT_MI_LIST` in `libkern/os/Makefile` | `bsd` compiles with `-I BUILD/obj/EXPORT_HDRS/libkern`, not the source tree, so a header that only exists in `libkern/os/` is invisible to `bsd/kern/subr_log.c` |
| `0008-sweet-polaris-machine-config-open-arm64` | `config/MASTER.arm64.polaris`: defines `ARM_EXTRAS_BASE = [ nos_arm_pmap nos_arm_asm ]` and adds `pgo` to `BSD_RELEASE` | without the first attribute the open pmap/asm are not compiled at all (192 undefined symbols); without the second `bsd/pgo/profile_runtime.c` is not compiled and the two `__llvm_profile_*_internal` calls from `OSKext.cpp` are unresolved |
| `0009-sweet-replace-cc-kext-symbols` | new `osfmk/arm64/chkstk_darwin.s` plus its `files.arm64` entry, and five IOMMU KPIs removed from `config/Private.arm64.exports` | `___chkstk_darwin` lives in `libcc_kext.a` (KDK only) and is called from kernel prologues; the five KPIs are defined only by Apple's closed IOMMU drivers, so demanding them in the export list guarantees a link failure |
| `0010-sweet-open-pmap-residuals` | drops the `amcc_rorgn.c` entry from `files.arm64`, adds `osfmk/arm64/tunables/tunables.s` with an empty `APPLY_TUNABLES` assembler macro, and defines the compilation-service CDHash plus its simple lock in `osfmk/arm/pmap.c` | the three things the open arm64 port has never needed to compile before: `amcc_rorgn.c` is absent from the drop (only its header ships, and it is KTRR/CTRR-only), `start.s` line 35 includes a per-SoC generated file, and the CDHash globals live in Apple's closed pmap. Every caller of `rorgn_*` is already inside `#if KERNEL_INTEGRITY_KTRR || KERNEL_INTEGRITY_CTRR`, which POLARIS does not define |

93 patches exist in the iOS 16 tree. Checked one by one against 7195.141.2 with
`git apply --check`: 13 apply unchanged, 80 need rewriting. This repository only
ever contains patches that have been verified against this exact tag.


## Build

Nothing needs to be installed locally: `.github/workflows/xnu.yml` builds on a
macOS runner and uploads `out/kernel.*` plus `build.log`.

The target is `make all` rather than `make install`: `do_build_all:: do_build_kernel`
is what produces `BUILD/obj/kernel.release`, and the install half would additionally
try to write `/System/Library/Kernels` through `DSTROOT`, which SIP blocks.

Inputs: `xnu_tag`, `kernel_config`, `machine_config`, `patch_set` (`series` | `none`),
`sdk_mode` (`host` | `macos11.3`).

Local equivalent:

```sh
XNU_TAG=xnu-7195.141.2 MACHINE_CONFIG=POLARIS PATCH_SET=series bash scripts/build-xnu.sh
```

## Do not flash anything from this repository yet

There is no device tree, no payload, no root image and no boot shim here. The only
thing this repository produces today is a kernel binary and a compile log.
