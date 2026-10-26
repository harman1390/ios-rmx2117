# RMX2117 (Narzo 30 Pro / MT6853) bring-up — adapted from ios-sweet

Device-verified 2026-10-20: shim display (DSI cmd-mode kick), UFS r/w,
PSCI v1.1 SMP (8 cores), GIC-600 NS interrupts, 16K granule, chainload
(SWEETXNU stub -> green screen). expdb is aee-owned; our UFS mailbox is
**opporeserve6**.

## 0. Back up first (mtkclient, BROM mode)

    python mtk.py r expdb,boot,dtbo,vbmeta,persist,nvram,nvdata,proinfo,protect1,protect2,md1img boot_bak/,dtbo_bak/,vbmeta_bak/,persist_bak/,nvram_bak/,nvdata_bak/,proinfo_bak/,protect1_bak/,protect2_bak/,md1img_bak/

Realme in-depth-test app -> `fastboot flashing unlock` **wipes userdata**
— the calibration partitions above are the insurance. Power+VolUp+VolDn =
BROM; mtkclient works even soft-bricked.

## 1. Kernel (macOS or GitHub CI)

    # macOS (Xcode 15.x required, NOT 16):
    brew install jq gum bash cmake ninja
    git clone https://github.com/blacktop/darwin-xnu-build dxb && cd dxb
    git checkout 663906528d1cd5ea8fde7de6f16de73db9511549
    cp /path/xnu/patches/*.patch /path/xnu/patches/*.sh dxb/patches/
    sed -i '' 's/arm64e" VALID_ARCHS="x86_64 arm64e"/arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' build.sh
    printf '#!/bin/sh\nexit 0\n' | sudo tee /usr/local/bin/xcodes && sudo chmod +x $_
    sudo xcode-select -s /Applications/Xcode_15.x.app/Contents/Developer
    MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=SWEET MAKEFLAGS=-k bash build.sh
    # -> dxb/fakeroot/System/Library/Kernels/kernel.development.*

or push ios-rmx2117 to GitHub and run the `xnu` workflow.

## 2. iOS root image (Linux; apfs-fuse + hfsprogs)

    tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
    sudo fsck.hfsplus -fy ios16-root.img
    sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
    sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
    sudo umount /mnt/iosroot; img2simg ios16-root.img ios16-root.simg   # fastboot only

## 3. Boot image (Windows OK — aarch64-none-elf in tools_pkg)

    python tools/mkadt.py dtb/mt6853.adt --highmem       # already built
    python tools/mkpayload.py KERNEL dtb/mt6853.adt shim/out/payload.bin \
      --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 wdt=-1 rootrw=1 ufsrw=1'
    cd shim; build.bat out\payload.bin   # -> out/shim-boot.img

MT6853 deltas vs the sweet cmdline: `cpus=8` (2xA76+6xA55, all verified),
`rd=ufsroot0` (GPT "userdata", found by the LUN scan; `rootrw=1` needs
`ufsrw=1` or the root falls back to read-only, patch 0087), DSI cmd-mode panel needs a
kick hook in the kernel fb path (shim precedent: DSI_START 0->1 @
0x1401300C), MTK WDT at 0x10007000 (not Qualcomm IMEM).

## 4. Flash

    python mtk.py w userdata ios16-root.img    # raw image: mtk.py does not unsparse
    # or, if unlocked: fastboot flash userdata ios16-root.simg
    python mtk.py w boot shim\out\shim-boot.img
    # or: fastboot boot shim\out\shim-boot.img

## 5. Logs

Kernel persistent log: TBD partition (opporeserve6 has room; or tail of
userdata like sweet). Shim mailbox already lands in opporeserve6:
`mtk.py r opporeserve6 dump.bin` -> SHIMUFS/SHIMLOG/SHSTG markers.

## Hardware facts (verified on-device)

- Boot: LK -> EL2 -> shim drops to EL1h; midr=A55 cpu0. MPIDRs 0x8100[0-7]00.
- Panel: nt35595 DSI cmd-mode 1080x2400. OVL L0 fb 0x77e20000 pitch 4352,
  L1 (logo) 0x78816000 pitch 4320. Kick: DSI_START(0x1401300C) 0->1.
- UFS @0x11270000: LU0/1=4MB boot LUs, LU2=user flash (GPT). Opporeserve6
  LBA=0x4F500. expdb=aee-owned LBA 0x14A08 (512B-sector view).
- GIC-600 @0x0c000000/+0x40000, GICR 0x0c040000/+0x200000, PPI27 vtimer.
- Timer 13MHz. PSCI v1.1/SMCCC v1.1 via smc. WDT TOPRGU 0x10007000.
- DRAM: kernel space = 0x100000000 + 2GB (mkadt --highmem).
