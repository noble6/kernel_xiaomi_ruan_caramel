# caramel

Custom kernel for the Redmi Pad Pro 5G / POCO Pad 5G (ruan), built on
[kernel_xiaomi_ruan](https://github.com/noble6/kernel_xiaomi_ruan) (Xiaomi's
ruan-u-oss, Linux 5.10.198 GKI).

## What's in it

| | |
|---|---|
| Root | Official [KernelSU](https://github.com/tiann/KernelSU) (built in), version 32656 |
| Root hiding | [SUSFS](https://gitlab.com/simonpunk/susfs4ksu) v2.3.0, all features enabled |
| Network | TCP BBR as the default congestion control |
| Version | `5.10.198-caramel` |

KernelSU is pinned to a main-branch commit (SUSFS targets main, not a
release), which is newer than the latest manager release. Use the manager
built from the same commit, from KernelSU's CI: the `manager` artifact of
the "Build Manager" run for 0ff54fab
(https://github.com/tiann/KernelSU/actions/runs/37125049434),
`KernelSU_v3.3.0-55-g0ff54fab_32656-release.apk`. An older manager
reports a version/uapi mismatch. For SUSFS settings, install the
susfs4ksu module from the SUSFS repo.

## Compatibility

caramel replaces only the kernel in `boot`. The ROM's vendor modules
(`vendor_boot`, `vendor_dlkm`), dtb and dtbo stay the stock OS3.0 ones, so
caramel must keep the KMI: every symbol CRC the stock modules import must
match. For that reason:

- It is built with AOSP clang-r416183b, the compiler of the GKI builds.
- `CONFIG_HZ` stays 250; the modules have it compiled in.
- `CONFIG_LOCALVERSION_AUTO` stays on: `CONFIG_MODULE_SCMVERSION` depends on
  it and changes `struct module`, i.e. `module_layout` for every module.
- No change may alter a structure or an exported function that the
  modules use. Check `vmlinux.symvers` against the stock modules' imports
  after every change.

## Flashing

Flash the zip in recovery (`adb sideload`) on a ROM for ruan. To go back,
flash the ROM's `boot.img` again (or dirty flash the ROM).

## Building

```
caramel/build.sh /path/to/clang-r416183b
```

The zip lands in `out/`. The configuration is `gki_defconfig` +
`vendor/ruan_rom.config` + `vendor/caramel.config`.

## Updating KernelSU / SUSFS

SUSFS's KernelSU patch targets official KernelSU's main branch, not a
release. To update:

1. Take the newest `gki-android12-5.10` commit of susfs4ksu and find the
   KernelSU commit its `kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch`
   applies to cleanly.
2. Replace `KernelSU/kernel` and `KernelSU/uapi` with that commit's, apply
   the patch, and keep the `KSU_VERSION` fallback in `KernelSU/kernel/Kbuild`.
3. Write `30000 + git rev-list --count <commit>` to `KernelSU/VERSION`.
4. Update `fs/susfs.c` and `include/linux/susfs*.h` and re-apply the kernel
   patch's changes.
5. Build, then check the symbol CRCs as above.
