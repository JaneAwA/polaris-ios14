# What is missing between "a kernel binary" and "the phone runs iOS 14"

This file exists so the next session does not have to rediscover the chain. Every
number below was measured in this repository or in the iOS 16 tree
(`luuu20309/ios16_on_polaris_mix2s`), not guessed.

## Where the work currently stands

1. `xnu-7195.141.2` builds on a `macos-14` runner (run #1, after 0002).
2. `MACHINE_CONFIGS=POLARIS` compiles the kernel for SDM845 (0003, in CI).
3. Nothing has been flashed. There is no payload, no device tree and no root.

## The boot chain, in order

| # | component | state | notes |
|---|---|---|---|
| 1 | UEFI firmware on the phone | ready | `edk2-porting/edk2-msm` release `2301.1`, `boot-polaris.img`, 6,662,144 B, sha256 `c30e40…9225`. Already downloaded to `unixnu/boot/`. |
| 2 | payload packer | reusable | `tools/mkpayload.py` (kernel + ADT + optional ramdisk → payload, with `--max-payload-size` check against the ramdisk→DTB gap). |
| 3 | boot shim | **layout-compatible** | The shim asserts `offsetof(boot_args, Video) == 40`, `offsetof(CommandLine) == 108`, `sizeof(boot_args) == 736`. xnu-7195's `pexpert/pexpert/arm64/boot.h` produces exactly 736 / 40 / 108 with `BOOT_LINE_LENGTH == 608`, so the iOS 16 shim can carry a 7195 kernel unchanged. Only the kernel entry convention matters, and that is `x0 = boot_args` PA with MMU and caches off, identical in both trees. |
| 4 | ADT (Apple Device Tree) | reusable | The ADT is consumed by `pexpert`, whose parser is unchanged between 7195 and 8796. The polaris ADT from the iOS 16 tree (`tools/polaris.adt`) is the starting point; the CPU nodes must stay a contiguous `cpus=1..8` prefix. |
| 5 | kernel | **being built here** | 7195 sources + patches 0001-0003. |
| 6 | root volume | not started | iOS 14.8 IPSW for `iPhone10,3` (18H17) is on disk at `G:/mix2siOS/iPhone10,3,iPhone10,6_14.8_18H17_Restore.ipsw` (4.9 GB). iOS 14 has no cryptex/AppOS/preboot split, so the root is one volume; the iOS 16 chain packed its root as HFS+ from an extracted tree, and the same `tools/mkiosroot-macos.sh` approach applies with a 7195-era dyld shared cache. |
| 7 | `launchd` | patched, untested | see 0001. The boot-args are `unentitled_ios_sim_launch=1 sweet_ios_userland=1`. |

## Why the kernel alone cannot reach SpringBoard yet

The 8796 chain needed 93 patches. Against 7195.141.2, 13 apply unchanged and
80 need rewriting. The missing ones are not cosmetic:

* machine/board bring-up: `0006` (open-source asm pmap for a non-Apple SoC),
  `0007`-`0009` (stub Apple-internal headers and closed-library gaps, drop
  Apple-SoC-only exports) — these are what make a macOS-config kernel link
  without Apple's private objects;
* root filesystem: `0021`/`0024` mockfs, `0028` UFS root, `0030`/`0031` writes,
  `0034` DMA above 4 GB, `0026` HFS — without these the kernel panics at
  `Failed to mount root device`;
* display: `0003` framebuffer console, `0046` IOSurface/IOMobileFramebuffer,
  `0056`/`0068` present path;
* input: `0058` HID event services, `0060` touch/keys, `0062` power/battery;
* platform identity: `0059` iPhone identity, `0061` iPhone X display size.

Each is written against 8796 line numbers. Porting one is a small, verifiable
job: apply, compile in CI, fix the rejected hunks. That is why this repository
exists with a CI pipeline instead of a pile of untested patch files.

## Order of work that produces visible progress fastest

1. green `POLARIS` kernel (this milestone);
2. `0006`-`0009` so the kernel links with `MACHINE_CONFIGS=POLARIS` instead of
   relying on the macOS default machine;
3. mockfs + UFS + HFS so the kernel can mount something and print
   `rootvp` progress instead of panicking;
4. payload + ADT with a mockfs ramdisk, boot on hardware, collect the panic;
5. only then the real iOS 14 root volume and `launchd`.
