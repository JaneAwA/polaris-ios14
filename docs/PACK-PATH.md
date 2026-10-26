# From `kernel.release` to a bootable image

Everything in this file is derived from static reading of the two reference
trees (`src/ios-sweet`, `src/ios16_on_polaris_mix2s`) plus the contents of
`boot/boot-polaris-2301.1.img`. Nothing here has been executed on hardware.

## What is in the `boot` partition today

`boot/boot-polaris-2301.1.img` is an **Android boot image (v0, 2048-byte
pages)** whose kernel payload is a gzip member:

| field | value |
| --- | --- |
| magic | `ANDROID!` |
| kernel_size | 6 656 485 |
| kernel_addr | `0x10000000` |
| ramdisk_size | 1 (one page, empty) |
| page_size | 2048 |
| cmdline | empty |
| payload | gzip → **33 554 560 B (32 MiB) UEFI firmware volume** |

The decompressed volume identifies itself as an `edk2-msm` build
(`/home/strongtz/git/edk2-msm/workspace/Build/polaris/RELEASE_GCC5/AARCH64/
Silicon/Qualcomm/QcomPkg/PrePi/PrePi/DEBUG/...`). So the boot partition holds
**UEFI**, not a kernel — a kernel has to be handed to it from outside, which is
what the shim already does for the iOS 16 chain.

## The packing chain that the iOS 16 chain used

Three pure-Python tools plus one cross-compiled blob:

```
kernel.release ─┐
                ├─ mkadt.py ──► polaris.adt ──┐
                │                            ├─ mkpayload.py ──► payload.bin
                └────────────────────────────┘        │
                                                      ├─ shim/build.sh ──► boot.img
                            shim (aarch64, freestanding)┘
```

* `mkadt.py OUT.adt --polaris --ram 6` builds the minimal Apple Device Tree
  from constants in the file (GICD `0x17a00000`, GICR `0x17a60000`,
  framebuffer `0x9d400000`, UFS host `0x1d84000`, timebase 19.2 MHz,
  `VTIMER_INTID 19`, `PHYS_BASE 0xb0000000`). It needs **no** input beyond its
  own arguments, so it can run on Windows today.
* `mkpayload.py KERNEL ADT OUT --cmdline "..." [--ramdisk IMG]` walks the
  kernel's segment table, resolves the PIE fixups, and emits a `SWEETXNU`
  container: a `boot_args`-shaped header, the segment table, and
  `char cmdline[608]` at offset `0x48`.
* `shim/build.sh payload.bin` compiles `start.S shim.c console.c mmu.c fdt.c
  loader.c ufs.c libc.c` for `aarch64-freestanding` and wraps the result with
  `mkbootimg.py` into the image that `fastboot boot` accepts.

## Verified: the shim's `boot_args` fits 7195 exactly

`shim/loader.c:48-50` asserts three things about the structure it fills in.
All three hold against `pexpert/pexpert/arm64/boot.h` in this tree:

| assertion | 7195 field layout | result |
| --- | --- | --- |
| `offsetof(Video) == 40` | `Revision` 0, `Version` 2, pad 4, `virtBase` 8, `physBase` 16, `memSize` 24, `topOfKernelData` 32 | 40 ✓ |
| `offsetof(CommandLine) == 108` | `Video` is `6 × unsigned long` = 48 → 88, `machineType` 88, pad, `deviceTreeP` 96, `deviceTreeLength` 104 | 108 ✓ |
| `sizeof == 736` | 108 + `BOOT_LINE_LENGTH` 608 = 716, `bootFlags` +8, `memSizeActual` +8 | 736 ✓ |

`BOOT_LINE_LENGTH` is 608 in `arm64/boot.h:26`, the same value `mkpayload.py`
uses for the payload header. So neither the shim nor the payload container
needs a 7195-specific variant — the difference between the two generations is
entirely in what the kernel does with the arguments.

## Two risks that only the artifact can settle

### 1. `mkpayload.py` only understands chained fixups

`mkpayload.py:6-7` states the kernel "is linked as a PIE with
`LC_DYLD_CHAINED_FIXUPS`", and `apply_chained_fixups()` returns immediately
when `m['fixups']` is `None` — i.e. it silently leaves every pointer
**unrelocated** if the kernel uses the older `LC_DYLD_INFO_ONLY` binding
opcodes. It also asserts `imports_count == 0`.

xnu-8796 (Xcode 13+/14 toolchain) does emit chained fixups. xnu-7195 was built
by Apple with a 2020 toolchain; whether **our** link on a `macos-14` runner
emits them depends on what `LDFLAGS_KERNEL` passes to `ld64` and on the
toolchain default. This must be checked on the produced binary:

```sh
python - <<'PY'
import struct
b = open('kernel.release','rb').read()
ncmds = struct.unpack_from('<I', b, 16)[0]
off = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', b, off)
    if cmd in (0x80000022, 0x80000034):   # DYLD_INFO_ONLY, CHAINED_FIXUPS
        print(hex(cmd), 'at', off)
    off += size
PY
```

* `0x80000034` → `mkpayload.py` works as is.
* `0x80000022` → the fixup walker has to learn the rebase/bind opcode stream
  before any image can be produced. That is bounded work (the kernel is
  self-contained, so binds should reduce to the `__kmod__`-style absolute
  symbols), but it is not free.

### 2. The shim cannot be built on the current Windows host

There is no `zig`, no `aarch64-linux-gnu-gcc` and no `clang` in the workspace,
and the sandbox cannot install one. `shim/build.sh` therefore has to run in CI.
The runner has Xcode's clang, but `clang -target aarch64-none-elf` links with
`ld64`, which does not accept the GNU `shim.ld` script, so the practical route
is a pinned Zig tarball (`zig cc -target aarch64-freestanding`), which
`build.sh` already knows how to drive through `$ZIG`.

## Order of work once a kernel exists

1. Inspect the Mach-O for the fixup format (above); patch `mkpayload.py` only
   if it reports `LC_DYLD_INFO_ONLY`.
2. `mkadt.py --polaris --ram 6` — the 6 GB variant is the safe one for both
   installed sizes.
3. `mkpayload.py` with a cmdline that carries the two switches 0001 added:
   `sweet_ios_userland=1 unentitled_ios_sim_launch=1`, alongside the existing
   `debug=0x8 serial=3 keepsyms=1 cpus=4`.
4. Build the shim in CI (Zig), pack with `mkbootimg.py`, then `fastboot boot`
   the result — a temporary boot that writes nothing.
5. Only after the kernel reaches launchd do the root-volume work: iOS 14 has a
   single root volume and one dyld shared cache, so `mkiosroot.sh` from the
   16 chain needs the 14.8 `18H17` IPSW layout instead (`System/Library/
   Caches/com.apple.dyld/dyld_shared_cache_arm64e` etc.), and `MOCKFS` must be
   added to the kernel config — it is **not** in any `[ ... ]` list in
   `config/MASTER`.
