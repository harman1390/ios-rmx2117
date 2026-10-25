# ios-rmx2117 handoff notes

## Status

Port skeleton created from ios-sweet for the **Narzo 30 Pro 5G
(RMX2117, MT6853 / Dimensity 800U)**. The two earlier targets are parked:
`../ios-b2q/` (Flip 3 — bootloader locked) and the initial MT6785 guesses
were corrected by the live DTB.

## Device (confirmed via ADB)

- **RMX2117L1**, Android 12 realmeUI 3, SPL 2024-01-05, `MT6853V/TNZA`
- `ro.boot.flash.locked`=1, **`sys.oem_unlock_allowed`=1** → unlockable
- `ro.boot.verifiedbootstate`=green, 8 cores, ~5.5GB usable RAM
- UFS: `boot`/`dtbo`/`vbmeta`/`super`/`userdata`/`lk`/`preloader_*` all
  present in `/dev/block/by-name`
- SELinux blocks `/sys/firmware/devicetree` property reads as shell —
  we have the live DTB from the community repo instead
  (`research/rmt6853/dtbs/entry_0_0_id00000000.dtb`)

## Verified from the live DTB

- GIC-600: GICD `0x0c000000`+0x40000, GICR `0x0c040000`+0x200000
- Timer: 13MHz, INTID 27 (vtimer) / 30 (ptimer)
- UFS `ufshci@0x11270000` size 0x2300, GIC_SPI 105, mediatek,ufshci
- CPUs: contiguous MPIDRs `core<<8` (6×A55 + 2×A76, NOT the MT6785
  non-contiguous layout — the `cpu@100` node names misled)
- Panel NT35595 1080×2400; framebuffer `0x7e605000` (k6853v1_64 atag)
- Touch: `goodix,touch`; bootargs `console=ttyS0,921600n1`

## Boot image (community-verified)

`header_version 2`, `base 0x40078000`, `pagesize 2048`, kernel `+0x8000`,
dtb baked into boot.img (real DTB already copied to `dtb/mt6853.dtb`).

## Next

- **User decides on unlock** (wipes userdata) — in-depth test app +
  `fastboot flashing unlock`.
- Optionally `fastboot boot` a test shim image before flashing.
- XNU stage: copy xnu/patches from ios-sweet, audit for SM7150/MTK
  differences (13MHz timer, GIC-600, MTK UFS glue, contiguous MPIDRs).

## Audit fixes (2026-02, this tree)

- **`dtb/stock-boot.img` is NOT stock** — its kernel slot inflates to our
  shim image (contains the `SWEETXNU` literal). The boot partition was
  dumped *after* a shim was already flashed, so no real stock kernel
  exists in this repo. `stock-ramdisk.img`/`mt6853.dtb` are likely the
  real stock pieces (they came from the boot image we keep repacking).
  To get true stock: re-dump `boot` before flashing, or pull boot.img
  from the RMX2117 OTA/firmware package.
- `start.S`: dead `cmp x10,#8` after `mov w10,#2` meant `vbar_el2` was
  never hooked (EL2 exceptions -> uninitialized VBAR -> hang). Now
  re-reads CurrentEL, hooks EL2 vectors, and drops EL2->EL1h via eret
  (HCR.RW, CNTHCTL, CPTR/HSTR unmasked, SP_EL1 set). Same vbar_el2 hook
  in `secondary_entry`.
- `start.S`: missing `.text` before `_start` orphaned `_start`,
  `secondary_entry` and `vectors` inside `.entry2`. Fixed — `_start` now
  at 0x800, `vectors` at 0x1000; both entry points (0 and 0x80000) work.
- `fdt.c`: `in_chosen` was cleared by any child node of `/chosen`
  (clobbered state) and the token walk had no bound — now tracks
  `chosen_depth` and stops at `totalsize`.
- `shim.c`: framebuffer detection now also accepts the single u64
  `atag,videolfb-fb_base` property, not only the `_l`/`_h` u32 pair.
- `ufs.c`: **the beacon no longer writes a hard-coded LBA** (`0x6EF40`
  could have hit live data). GPT is parsed first; the mailbox block goes
  to `expdb`'s first block — MTK's crash-dump partition, meant to be
  overwritten. No `expdb` -> no UFS write at all. GPT field widths fixed.
- `build.bat`: `type >>` could corrupt binary payloads -> `copy /b`.
- `build.sh` now matches `.bat`: gzip'd kernel (LK requires it), stock
  ramdisk, stock cmdline, os_version/patch fields.
- Post-mortem channels after a run: DRAM mailbox at
  **`0x40080000 + offset(mbox)`** (currently `0x40080a70`; recompute with
  `nm out/shim.elf | findstr mbox` after any rebuild) and the expdb
  block. `probe_cycle.ps1` peeks both.

## Real firmware (D:\rmx2117_fw\files, dumped into dtb/)

Firmware zip is the **A11** build (`dest_mtk_11`, SPL 2022-03-29); the
device runs A12/SPL 2024-01-05 — image format is identical, os_version
in our image stays `12.0.0/2024-01-05`.

New files: `real-stock-boot.img` (byte copy), `real-stock-kernel`
(inflated, 36.5MB, `ARMd`/`text_offset 0x80000`, no SWEETXNU — real
Linux), `real-stock-ramdisk.img` (identical to `stock-ramdisk.img`),
`real-stock-dtb.dtb` (dtb section, 0x40-byte `d7b7ab1e` MTK header +
FDT), `real-stock-inner.dtb` (bare FDT).

Stock boot header decoded: `hdr v2`, `page 2048`,
`kernel_addr 0x40080000`, `ramdisk_addr 0x47c80000`,
`dtb_addr 0x4bc80000`, cmdline `bootopt=64S3,32N2,64N2
buildvariant=user`, kernel is **gzip** → our build params are all
byte-identical; LK decompresses the kernel (gzip/bootimage strings in
lk.img).

- **`lk.img` emits `atag,videolfb-fb_base_h`/`_l` + `atag,videolfb-vramSize`**
  into the runtime DTB → the split-u32 fb lookup in shim.c is the right
  path; the baked dtb lacking these props is normal.
- **`scatter.txt` confirms `expdb` at `0x14a08000`** (~20MB, ends at frp
  0x15e08000) → GPT-discovered mailbox write target exists on this
  device. Old hard-coded LBA `0x6ef40` ≈ `logo` partition start
  (0x6ef00000) +0x40000 — was likely harmless but wrong-named.
- Both build scripts now bake `real-stock-dtb.dtb`; `mt6853.dtb`
  (community) kept for reference.

## Unverified assumptions still open

- Whether LK passes the DTB pointer in x0 at all on this chain — the
  mailbox's dtb word (mbox[4]) and `MB(13)` will answer it.
- Whether LK honors `text_offset`/`kernel_addr` from the header or jumps
  to a fixed offset — both 0 and 0x80000 entries are wired.
- `FB_BASE 0x7e605000` fallback is a guess; if the screen stays dark,
  the mailbox + expdb are the signal channel.
- Bootloader lock state on the user's device is unknown — if still
  locked, LK will reject the unsigned image before `_start` runs.


## Session 2026-10-20 (UFS probe findings)
- pl.write32/read32 work in patched-preloader mode; TOPRGU spare regs 0x100070E0/0x1001A0F0 are NOT writable (locked/no decode). DRAM mbox (0x40080a70) is zeroed on the BROM re-entry path -> dead post-mortem channel. SRAM 0x110000 window is BROM workspace, clobbered on re-entry.
- WDT arm works: mode 0x31+restart fired ~60s later (session drop). wdt regs R/W verified live.
- UFS host live in BROM session: HCS=0x10f, HCE=1, list running at SRAM 0x103800. Manual SRAM-descriptor TUR cleared UTRLDBR (command consumed) but OCS stayed 0x0f, no resp UPIU.
- Stopping the running UTRL list from DA kills the session instantly (DA owns the host).
- Shim state machine proven by reset cadence: WDT-kill at _start works; latest 'never resets' = reached wdt_off() at end of test_ufs -> full probe ran, all stage()/beacon writes errored at SCSI level (no SHSTG in expdb).
- Bug fixed, UNFLASHED: struct ucd is 1040B so prdt[16] overlapped SRAM_DATA at 0x110800 -> PRDT/data corrupted each other. New layout UTRD 0x110000 / UCD 0x110400(0x600) / data 0x110a00 / ents 0x112a00. shim-boot.bin built w/ fix + staged WDT arm (start 0x2000, shim_main 0x4000, test_ufs 0x8000).
- Open: whether OCS=0xf is INVALID_PRDT or MTK-vendor code; whether shim SCSI would succeed post-fix; real goal = boot XNU, not UFS per se.

## 2026-10-20: Display + UFS fully working (on-device verified)

**Display**: Panel is DSI *command-mode* (`nt35595_fhd_dsi_cmd_truly`). Framebuffer writes need a DSI_START kick (`0x1401300C`: write 0 then 1) or nothing reaches the panel. Two live OVL layers: L0 `0x77e20000` pitch 4352, L1 (logo, on top) `0x78816000` pitch 4320  console paints both, plus a magenta band in `_start` as earliest proof-of-life. Cyan screen = reached `test_ufs`.

**UFS**: Host was alive all along (`hce=1`, `hcs=0x10F`); LK's list sat at DRAM `0x4C59E400`. Takeover with our SRAM list @ `0x110000` works  the host reads/writes our descriptors (OCS goes `0xF`->`0` = real completion). The earlier "all-0xFF data" mystery: **LUN0 is a 4 MiB boot LU** (`lastlba=0x3FF`), not user flash. Scan LUNs -> **LUN2 = 0xEE57FF x 4K blocks (~60 GiB) holds the GPT**. Partition table reads perfectly; `expdb` = LBA `0x14A08` (matches prior DA math). `UFS BEACON rc=0`: mailbox snapshot written + stage breadcrumbs at `expdb+1..3`. Persistent post-mortem channel confirmed.

Remaining: `ufs_log_write()` can now stream the real log; next milestone = actual chainload payload work.

## 2026-10-20 (2): Milestone-2 probe results  ALL GREEN (device-verified)

- **EL**: boots at EL2, `_start` drops to EL1h correctly. midr=0x412FD050 = Cortex-A55 boot core.
- **ID_AA64MMFR0 = 0x0000000000101122**: TGran16=2 -> **16K granule supported** (XNU hard requirement OK); TGran4=0 (4K also fine).
- **PSCI v1.1 / SMCCC v1.1**: CPU_ON, CPU_OFF, SYSTEM_RESET all supported (feat=0).
- **SMP**: all 7 secondaries CPU_ON at mpidr 0x8100[1-7]00, ran secondary_main, CPU_OFF clean.
- **GIC-600**: GICD @0x0c000000 already enabled by LK (0x12); GICR found @0x0c040000 waker=0. **VTIMER PPI27 delivered 30 irqs/300ms; SGI1 delivered 10**  NS interrupt path fully works.
- **Loader**: FDT initrd present (0x47CB0000-0x47D5C272 = stock ramdisk); correctly rejects non-SWEETXNU.
- **WDT caveat**: TOPRGU rearm fired mid-probe when armed too short -> disarm after UFS is live; a hang now freezes the last screen (more useful than reset).
- **aee caveat**: MTK aee owns `expdb` and overwrites block0 on abnormal resets  mailbox moved to `opporeserve6` (OEM-reserved).
- **XNU readiness**: 16K granule OK, PSCI SMP OK, NS GIC OK, UFS r/w OK, display OK. Missing: the SWEETXNU payload itself (xnu/patches not ported to MT6853 yet).

## 2026-10-20 (3): Stage 2  payload pipeline prepared

- `xnu/patches/` copied from ios-sweet (85 files, SWEET series). `.github/workflows/xnu.yml` copied: builds xnu-8796 (macOS 13.5 / iOS 16 era) via blacktop/darwin-xnu-build on macos-14  needs the repo pushed to GitHub or a local macOS run of darwin-xnu-build.
- `dtb/mt6853.adt` built (9956B, `--highmem`: dram-base 0x100000000, +2GB in the upper bank, clear of LK/fb/shim). fbcon node corrected to the **verified** fb: 0x77e20000, pitch 4352.
- Missing input: `research/ipsw-x/d22ap.adt` (real D22AP /product node for MobileGestalt)  extract from an iPhone X IPSW: `remotezip.py URL Firmware/all_flash/DeviceTree.d22ap.im4p out/` then `ipsw img4 im4p extract`.
- Patches are Samsung-b2q heritage; most are SoC-agnostic XNU workarounds. MT6853-relevant deltas: MTK UFSHCI is plain JEDEC (simpler than QCom), DSI cmd-mode panel, GIC-600, PSCI SMP  all verified working on-device this session.

## 2026-10-20 (4): Chainload VERIFIED on-device (green screen)

`tools/mkstubpayload.py` packs a flat stub + `dtb/mt6853.adt` into a SWEETXNU payload (entry 0x100004000, dram 0x100000000+2G highmem). `payload_stub.S` paints both OVL layers green + DSI_START kick. Build: `build.bat out\stub-payload.bin`. Result: **green screen on-device**  loader copy/ADP/boot_args/jump all work.

`research/ipsw-x/d22ap.adt` extracted from iPhone10,3 16.7.16 (20H392) IPSW (bvx2/lzfse via pip `lzfse`); mt6853.adt rebuilt WITH real /product node (12640B).

**Only missing input for real XNU boot: the Mach-O kernel** (macOS CI workflow in place, or darwin-xnu-build on a Mac).

## 2026-10-21: Entry-band panic + fix

- Panic on-device: `ESR=0x96000021` = data abort, DFSC `0b100001` = **alignment fault**, stage `load`. `ELR=0x10148C4C4` = `ldr x16, .La` in `entryband.S` stub _A; `FAR=0x10148C4CC` = the `.La` literal at band+0x44 — 4-aligned only. With the MMU off, `ldr` literal requires natural alignment regardless of SCTLR.A. Fixed: `.balign 8` before `.La`/`.Lb`/`.Lc` (verified via objdump: literals now at band +0x48/+0xc0/+0x130; stub entry offsets 0x74/0xe8 unchanged).
- **Source drift**: the band inside `out/xnu-payload.bin` is a NEWER variant (two fill loops + `mrs x9,vbar_el1` store to 0x1014c8c7f8 per stub, third stub returns via direct `b`) — not generated from the tree's `entryband.S`. Its literals are already aligned (band +0x70, +0x108); scanned the whole band, no remaining misaligned `ldr` literal. If the band source is regenerated, port the alignment fix + the new instrumentation into `entryband.S`.
- `out/shim-boot.img` rebuilt 01:48 with the aligned-band payload (`xnu-payload.bin` == tail of `out/Image`, verified).

## 2026-10-24: Post-mortem audit of the "frozen boot" image

Root cause found  the kernel was blind, not dead:

- **Patch 0082 was EMPTY** (wiped by "LF normalize 0082" in 97c008b). It
  carried `fbcon_kick()`: DSI_START toggle after every fbcon character via
  the `kick-reg` ADT prop. out4 built without it -> after `serial_init`
  registers fbcon and `sweet_early_disable()` runs (arm_init.c:546), every
  kprintf/panic writes to the OVL fb but never reaches the cmd-mode panel.
  Screen freezes on the last early mark; indistinguishable from a hang.
  Restored from git history (hash-verified), committed.
- **0084 (new): UFS LUN scan.** `sweet_ufs.c` hard-coded `su.lun = 0`; on
  MT6853 LUN0/1 are 4 MiB boot LUs and the GPT is on LUN2 -> `rd=ufsroot0`
  could never find "userdata". Now scans LUNs 0-7 (still LUN0-first, so
  sweet-compatible). Both patches verified with `git apply` against the
  CI buildtree source.
- Verified OK in shipped payload: entry band has DSI kicks + aligned
  literals + vbar stores; early console path complete (0083 fb 0x77e20000,
  early_dsi 0x14013000, two-block VA alias); ADT has kick-reg=0x14013000,
  sweet-ufs reg=0x11270000, gicd/gicr, 13MHz, vtimer-intid 27, MPIDR i<<8;
  vtimer INTID read from DT (SWEET.h 19 is only a fallback); persistent
  log target is GPT-relative tail of the ufsroot partition (no fixed LBA).
- Landmines (all boot-arg gated, add to cmdline): sweetkeys=0,
  sweettouch=0, sweetusb=0  those drivers poke Qualcomm MMIO
  (SPMI/GENI/RPMh 0x18220000) that does not exist on MT6853.
- Next test cmdline suggestion:
  `debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress wdt=-1 sweetkeys=0 sweettouch=0 sweetusb=0 rd=ufsroot0`

## 2026-10-25: HFS root mount fixes (patch 0087)

- `rootrw=1` vs `ufsrw=1`: 0031 cleared MNT_RDONLY on rootrw=1, but the UFS
  driver (0030) refuses write opens unless ufsrw=1; the documented cmdline
  had only rootrw=1, so the read-write HFS root mount failed (EROFS on the
  device open / journal + volume-header writes). 0087 checks
  DKIOCISWRITABLE and mounts read-only with a warning when the device is
  not writable; BRINGUP cmdline now carries `rootrw=1 ufsrw=1`.
- sweetlog overwrote the HFS partition alternate volume header (last 1 KiB
  of the partition, rewritten by HFS on rw mount/flush) and vice versa.
  Log area now ends one 4 KiB block before the partition end;
  sweet-log-pull.sh reads from the new offset.
- LUN scan: unit attention is per LU, and an EIO from a boot/absent LU
  aborted the scan before LUN2. 0087 drains UA with TUR on every LU and
  skips LUs that return EIO.
- Flashing: `mtk.py w userdata` writes bytes as-is; flashing the
  img2simg output put an Android sparse header where HFS expects its
  volume header (offset 1024). Flash the raw .img with mtk.py; .simg is
  for `fastboot flash` only.
