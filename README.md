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
| xnu-7195.141.2 compiles on a modern runner | build_setup fixed (0002); full kernel build running in Actions |
| launchd platform gate | patched (0001) |
| POLARIS machine config (SDM845, GICv3 Group 1, 16K pages) | written (0003), compile being verified |
| UFS / HFS+ root | not ported |
| IOMobileFramebuffer / IOSurface | not ported |
| IOHID (touch / keys) | not ported |
| userspace root image | not started; iOS 14 has no cryptex/preboot split, so the root is one HFS+ volume built offline |
| flashable image | **no** |

## Patch series

Applied in the order listed in `patches/series`:

| patch | what it does | why |
|---|---|---|
| `0001-sweet-ios14-launchd-platform-optin` | parses `unentitled_ios_sim_launch` in RELEASE behind a second switch | without it launchd dies at `EXEC_EXIT_REASON_WRONG_PLATFORM` |
| `0002-sweet-build-symbol-aliasing-without-availability-pl` | `bsd/sys/make_symbol_aliasing.sh` falls back to a built-in release table | macOS 14.5 SDK dropped `usr/local/libexec/availability.pl`; `build_setup` died before any compile |
| `0003-sweet-polaris-machine-config` | `MACHINE_CONFIGS=POLARIS`: machine flags, `POLARIS.h`, board config, cache lines, GIC Group 1 setup, Group 1 IRQ routing in `sleh_irq` | this is what makes the kernel target SDM845 instead of an Apple SoC |

93 patches exist in the iOS 16 tree. Checked one by one against 7195.141.2 with
`git apply --check`: 13 apply unchanged, 80 need rewriting. This repository only
ever contains patches that have been verified against this exact tag.


## Build

Nothing needs to be installed locally: `.github/workflows/xnu.yml` builds on a
macOS runner and uploads `out/kernel.*` plus `build.log`.

Inputs: `xnu_tag`, `kernel_config`, `machine_config`, `patch_set` (`series` | `none`),
`sdk_mode` (`host` | `macos11.3`).

Local equivalent:

```sh
XNU_TAG=xnu-7195.141.2 MACHINE_CONFIG=T8010 PATCH_SET=series bash scripts/build-xnu.sh
```

## Do not flash anything from this repository yet

There is no device tree, no payload, no root image and no boot shim here. The only
thing this repository produces today is a kernel binary and a compile log.
