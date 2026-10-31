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
- Touch: generic live-DTB node is `goodix,touch`, but the RMX2117 project
  20611 overlay selects `oplus,tp_noflash` on SPI4 (IRQ GPIO14, reset GPIO33).
  The overlay is more specific for touch bring-up; confirm the NT36672C
  candidate driver with its chip-ID probe. Bootargs `console=ttyS0,921600n1`.

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

## 2026-10-25: Kernel-source DTS audit (RMX2117-Source/android_kernel_realme_RMX2117)

Full MT6853 kernel source pulled: `research/rmt6853/mt6853-src.dts`
(183KB SoC dts) + `research/rmt6853/k6853v1_64_6360.dts` (board overlay,
name matches `TARGET_BOOTLOADER_BOARD_NAME := k6853v1_64_6360` in both
community device trees). Confirms/corrects:

- **ADT highmem range is SAFE.** Every firmware `no-map` carveout in
  `reserved-memory` allocates inside `0x40000000-0x80000000` (ion, consys,
  wifi, adsp/scp/sspm/mcupm share). `zmc` CMA (0x35000000) and ssmr alloc
  at >=0xc0000000 but are Linux-side pools — dead under XNU. Nothing
  firmware-owned touches `0x1_00000000-0x1_80000000`. EMI MPU
  (`emimpu@0x10226000`, 32 regions) protects DRAM via `prot-base
  0x40000000`/`prot-size 0x4_00000000` windows, but the range is
  normal-world (Linux uses it) — NS access OK. EMI fault would surface as
  GIC_SPI 189.
- **`kick-reg 0x14013000` verified** = `dsi0` reg base (DSI_START +0x0C),
  IRQ SPI 269.
- **ufs**: `mediatek,ufshci` 0x11270000/0x2300 SPI105 confirmed;
  `vcc-supply = mt_pmic_vemc_ldo_reg` + `vcc-fixed-regulator` (always-on,
  no SW sequencing needed); 2 lanes; AH8 10ms. NEW: `ufs_mphy@0x11fa0000`
  (0xc000) — the PHY block if link re-init is ever needed (LK init
  persists today).
- **PSCI** `arm,psci-1.0` via smc; idle params present but unused by us.
- **Timer/GIC** — 13MHz, PPI13/14/11/10 -> INTID 29/30/**27**/26, GIC-600
  GICD 0x0c000000/GICR 0x0c040000 all confirmed; GIC node has a third reg
  = INTPOL 0x0c53d668 (not needed).
- **fb_base 0x7e605000 origin found**: the board overlay's `/chosen`
  carries `atag,videolfb-*` defaults — LK overwrites them at runtime
  (real fb 0x77e20000). Source `/memory` = EVB 1GB ending at the default
  fb carveout; real map comes from LK's atag patching.
- **Optional**: `apuart0` UART @ `0x11002000` (ttyS0 921600n1, 26MHz) —
  a serial early-console path if fbcon ever fails.
- TOPRGU watchdog base `0x10007000` (matches probe); PMIF/PMIC SPMI @
  0x10027000 for any future pmu/reset needs.

## 2026-10-25: Stage-mark colors + mkmarks fix

Colors seen on screen with `shim-boot.img` (payload-marked, flashed 01:55)
are the `tools/mkmarks.py` kernel stage marks (BGRA fills on OVL L1):
green m0 sfc entry, yellow m1 fb PTE build, magenta m2 pre-MMU, blue m3
vbar install, white m4 just before `msr sctlr_el1` (MMU on). Next marks
(m4a orange, m4b cyan, m5 pink, m6 cyan) are post-MMU.

- The flashed payload lacked the idx60 map fix (site entry+0x1f00
  unmodified): L1 at 0x78816000 is outside the mapped 32MB fb block, so
  post-MMU marks fault silently. Stopping at white is NOT proof of a hang.
- `mkmarks.py` was syntactically broken (missing loop header, placeholder
  map-fix). Fixed: hand-encoded stub at entry+4 stores fb desc+32MB at
  L2[idx+1] (x10/x12/x13 are dead after the site), then replays and
  branches back. Verified by objdump.
- New image: `shim/out/shim-boot-marks2.img` (same shim, payload-marked2,
  cmdline + sweetlog). Original `shim-boot.img` untouched.
- If still white-only: read the log with
  `python mtk.py r opporeserve6 dump.bin` (SHIMLOG/SHSTG markers).
- No iOS root image exists in the repo yet (`tools/mkiosroot.sh` is the
  recipe; needs OS.dmg etc.).
- **On-device result (marks2):** user sees green, yellow, magenta, blue,
  white, purple, cyan, red, cyan = m0..m6 ALL reached. MMU is on and the
  kernel entered `vstart` (C). Next signals: `sweet_early_mark` stage bars
  1-20 (patches 0010/0012/0083) and the fbcon text after `serial_init`.
- **Screenshot after marks2:** shim text + 9 bars, last = cyan m6, nothing
  after. m6 site is `arm_init` entry (VSTART_OFF == `_arm_init`), so the
  kernel is likely running but INVISIBLE: `sweet_early.c` (0x77e20000,
  p4352) and the ADT fbcon node both draw to OVL **L0**, which sits under
  the opaque **L1** (0x78816000, p4320) the shim leaves up. Only L1 writes
  are visible. Also `sweet_early_relocate` sets `early_fb = base` without
  the +0x20000 fb offset inside the 0x77e00000 block.
- `tools/mkstage.py` (new): overwrites `_sweet_early_mark`
  (0xfffffe0007484fec, from out5 symtab) with a painter: orange bar on L1
  at row 1600+35*stage + DSI kick, stages <10 only (later ones run after
  TTBR0/V=P is gone). Pipeline: `mkmarks.py` -> `mkstage.py`.
  Image: `shim/out/shim-boot-marks3.bin`. Highest orange bar = last
  `arm_init` stage reached.
- **marks3 result:** 9 orange bars = stages 1-9 all reached (arm_init up
  to `sweet_early_relocate`, i.e. right before `set_mmu_ttb(invalid)`).
  Stage 10+ ("TTBR0 off") was not drawable, so the next stop is unknown.
- **marks4** (`shim/out/shim-boot-marks4.bin`): `mkstage.py` now also
  retargets the inlined `sweet_early_map_final` descriptor
  (`arm_vm_init`+0xb1c, `movk x8,#0x77e0` -> `#0x7800`) so the kernel VA
  alias maps the 0x78000000 block containing L1 (+0x816000), and stages
  10-20 paint through `early_dsi`-derived VA. Bars 10-20 at rows
  1600+35*stage. Highest bar = last stage reached.
- **marks4 result:** all 20 stage bars drawn -> `arm_init` reaches
  "kprintf startup complete"; no more marks exist after that, so the next
  blocker is unseen (kernel_bootstrap / IOKit / ufsroot mount / panic).
- **marks5** (`shim/out/shim-boot-marks5.bin`): `tools/mkfbl1.py` patches
  the ADT `sweet-fbcon` node (reg 0x78816000/0x9c4000, stride 4320, height
  2368) so kprintf text lands on visible L1. Kernel text should now show.
  Persistent log (boot-arg `sweetlog`) goes to the tail of the ufsroot
  partition; read with `tools/sweet-log-pull.sh` equivalent via mtkclient.
- **marks5 result:** kernel panic visible: "Unable to find driver for
  this platform: D22AP" (`IOPlatformExpert.cpp:2318`). Root cause:
  patch 0016's `SweetPlatformExpert` matches `IONameMatch 'xiaomi,sweet'`,
  but `mkadt.py` root `compatible` (D22AP, iPhone10,3, realme,RMX2117,
  mediatek,mt6853, AppleARM) no longer contained it, so only
  `IOPanicPlatform`-class fallbacks remained. Fix: `xiaomi,sweet` added
  after `realme,RMX2117` (D22AP stays compatible[0] for hw.target).
- **marks6** (`shim/out/shim-boot-marks6.bin`): ADT regenerated
  (`dtb/mt6853.adt`, 12692B), payload rebuilt with `mkpayload.py` from the
  out5 kernel (validated: only header ADT size + tail padding differ from
  the old payload.bin), then mkmarks -> mkstage -> mkfbl1.
- **marks6 result:** no panic; screen freezes after "SWEET: platform
  expert registered" (fbcon wraps to the top; newest line sits just above
  the blank cursor gap). Root cause: `SweetCPU` (0018/0065) and the display
  driver match `qcom,kryo470` / `sweet,display` (verified as strings in the
  payload's IOKit match tables), but the ADT cpu nodes only had
  `arm,cortex-a55/a76`+`ARM,v8` and disp0 only `rmx2117,display`. No CPU
  driver -> `gIOAllCPUInitializedKey` never published -> IOKit waits
  forever with no output.
- **marks7** (`shim/out/shim-boot-marks7.bin`): `mkadt.py` cpu compatible
  += `qcom,kryo470`, disp0 compatible += `sweet,display`; ADT 12804B
  (`dtb/mt6853.adt` updated); payload rebuilt + mkmarks/mkstage/mkfbl1.
  Expect next: `SWEET CPU: boot CPU 0 registered...` and then SMP / UFS
  root (`SWEET UFS:`) lines. cmdline still `cpus=1`.
- **marks7 result (photo):** SweetCPU/IOKit now proceed; kernel reaches
  BSD root mount: `BSD root: ufsroot0, major 1, minor 0` ->
  `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : 1020` ->
  `hfs_mount returned error=22` -> `cannot mount root, errno = 19`,
  `mount(N) failed` looping. UFS read path works; `userdata` still holds
  Android data, NOT an HFS+ iOS root. **Next hard requirement: build +
  flash the iOS root image** (ios-sweet recipe): IPSW iPhone10,3 16.7.16
  (20H392) -> OS.dmg/SystemOS.dmg/AppOS.dmg -> `tools/mkiosroot.sh`
  (Linux: apfs-fuse, hfsprogs, rsync, sudo loop mount) -> sweet-root-tweaks.py
  -> flash to `userdata`. Also adopt ios-sweet's boot args for userland:
  `unentitled_ios_sim_launch=1 ignition_level=1 ufsrw=1 rootrw=1
  launchd_unsecure_cache=1 wdt_test=no_user_checkin -enable_kprintf_spam`
  (current cmdline lacks them). Nothing in this workspace holds an
  IPSW/DMG/root image.
- **Root image prep (2026-10-25):** IPSW iPhone10,3 16.7.16 20H392 (URL
  from api.ipsw.me). `tools/remotezip.py` now forces curl `-4` (it hung
  before). Downloaded + CRC-verified into `C:\ios-root`: OS =
  `087-86698-044.dmg` (4.7GB), SystemOS = `087-86593-047.dmg` (2.7GB),
  AppOS = `087-86715-047.dmg` (10MB) (mapping from BuildManifest.plist,
  d22ap Erase install). WSL features enabled but WSL2 will not start:
  virtualization is off in firmware and/or a reboot is pending. Next:
  enable VT-x/SVM in BIOS, reboot, `wsl --install -d Ubuntu`, check
  `zgrep HFSPLUS /proc/config.gz` (stock WSL kernel may lack it), run
  `tools/mkiosroot.sh`.
- TODO (needs CI kernel rebuild): point `sweet_early.c` and the ADT fbcon
  node (mkadt.py) at L1 0x78816000/p4320 (map the 0x78000000 block in
  start.s/`sweet_early_map_final`), or have the shim disable OVL L1.

## 2026-10-25 (2): iOS root image built in WSL2

- **WSL kernel**: stock WSL2 has no hfsplus; built linux-msft-wsl-6.18.54.1
  in `~/k` (config has CONFIG_HFSPLUS_FS=y, FUSE, DM_VERITY, case NLS).
  Installed: `C:\wsl-kernel\bzImage` + `C:\Users\Harman\.wslconfig`
  `[wsl2] kernel=C:\\wsl-kernel\\bzImage` (double backslashes REQUIRED —
  `\w` is a bad escape and the whole file is ignored). Verify:
  `grep hfsplus /proc/filesystems` + `uname -r` ends in `+`.
- **apfs-fuse** built in WSL (`~/apfs-fuse`, installed /usr/local/bin).
  Needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` and `#include <cstdint>`
  in ApfsLib/PList.h for GCC 15 (`tools/build-apfs-fuse.sh` does all).
- **mkiosroot run notes** (tools/mkiosroot.sh, updated):
  - `install -o 272` FAILS on Ubuntu coreutils (numeric UID not in
    passwd) — the script now uses `install -m` + `chown`. That was the
    silent end-of-run death.
  - apfs-fuse+DrvFs throws transient EIO under load; rsync retries
    (10x, incremental) added to copy().
  - Long runs must be a systemd unit (`tools/run-mkiosroot-wsl.sh
    resume`) — `wsl -e` session children die with the session.
  - `MKIOSROOT_RESUME=1` continues a killed run on the existing img.
  - **CRLF**: run the script through `tr -d '\r'` or bash eats
    `set -euo pipefail`.
- **Image**: `C:\ios-root\ios16-root-20H392-rmx.img` (16G sparse, ~8G
  used) sha256 0de05179... (+ .sha256 file). Built from the three DMGs;
  all copies + cryptex hardlinks + dyld-cache links done;
  sweet-root-tweaks applied (launchd, containers, migration/setup/
  springboard prefs, VirtualAudio off, StartRecorderServer off).
  mobileactivationd data ark SKIPPED: system container only exists
  post-first-boot; activation now relies on the /chosen security-status
  properties (re-seed via tweaks rerun if activation misbehaves).
- **Boot image**: `shim/out/shim-boot-rmx1.img` = out5 kernel +
  dtb/mt6853.adt + cmdline `debug=0xc serial=3 cpus=1 keepsyms=1
  -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1
  ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1
  launchd_unsecure_cache=1 wdt=-1 sweetkeys=0 sweettouch=0 sweetusb=0
  sweetlog=1` (payload shim/out/payload-rmx1.bin).
- **Flash**: root img -> `userdata` (mtkclient `python mtk.py w userdata
  C:\ios-root\ios16-root-20H392-rmx.bin`, works locked; or
  `fastboot flash userdata` if unlocked — ERASES Android data).
  Boot img -> `boot` (`mtk.py w boot C:\ios-root\shim-boot-rmx1.bin`
  or fastboot). Both ship as .bin in C:\ios-root (the .img->.bin rename
  is cosmetic; sha256 unchanged). Persistent kernel log lands at the
  tail of the ~60G userdata partition, past the 16G image — survives
  the flash.

## 2026-10-25 (3): finish-iosroot-tail + fsck clean

- `sweet-root-tweaks.py` was NOT idempotent in `audio()`: a prior partial
  `shutil.move` left VirtualAudio.plugin in both HAL/ and HAL.disabled/,
  and the rerun died on "Destination path already exists". Now deletes the
  leftover source when the dst copy exists.
- `fsck.hfsplus -fy` reported 32 "Illegal name" and "could not be
  repaired". Root causes (all in WorkflowKit DefaultShortcuts l10n):
  - ~30 sibling pairs differing only in Unicode normalization (apfs-fuse
    name vs linux-normalized copy). fsck's rename collides with the
    existing sibling -> unrepairable. Fix: `tools/fix-hfs-names.py`
    deletes the precomposed dup / renames to Apple's canonical form.
  - 5 names containing ':' (stored as '/' in the catalog).
    `tools/fix-colon-names.py` renamed ':' -> '-'.
  - 2 Thai names (th.lproj): catalog held hard-linked dup records,
    decomposed `U+0E4D U+0E32` + canonical `U+0E33`, same inode. Driver
    composes on read so userland sees one name twice; removed one record
    per dir with `rm` (tools/del-thai-dups2.sh) and fsck repaired the rest.
  - Result: `fsck.hfsplus -fy` -> "The volume iOSRoot appears to be OK."
- Boot img `shim/out/shim-boot-rmx1.img` + `C:\ios-root\*.bin` verified:
  payload cmdline at payload+0x48 = full userland args (rd=ufsroot0,
  ignition_level=1, ufsrw/rootrw, launchd_unsecure_cache, sweet* gates).
- **mkiosroot.sh should gain a post-copy pass of fix-hfs-names.py +
  fix-colon-names.py** so future images are born fsck-clean.

## 2026-10-25 (4): resume fixes, fsck -n false positive, image re-export

- Resume (`MKIOSROOT_RESUME=1`) died at two non-idempotent steps:
  - dyld cache `ln` -> "Already exists": now `ln -f`.
  - keybag `printf | install -m 0600 /dev/stdin KB`: when KB already exists
    on hfsplus, install prints "install: No such file or directory" and
    writes NOTHING (reproduced). Now `tee` + chmod (mkiosroot.sh and
    finish-iosroot-tail.sh).
- **`fsck.hfsplus -n` (540.1-Linux) fails EVERY journaled volume**: even a
  fresh `mkfs.hfsplus -J` 64M image -> "invalid primary VHB at 2 result 35"
  (EDEADLK reading block 2 after its journal check) -> "Volume header needs
  minor repair". `-y` on the same image: OK; non-journaled `-n`: OK.
  Ignore it; mkiosroot.sh now verifies with a second `-y` pass that must say
  "appears to be OK".
- `C:\ios-root\ios16-root-20H392-rmx.bin` of 16:14 IST was copied while
  the WSL image was still mounted rw at /tmp/finish-root (header writeCount
  8 vs 9 in the live image) -> re-exported from the unmounted, fsck-clean
  WSL image with sha256 check (`/root/export-img.sh`, log
  `/root/export-img.log`), `.sha256` rewritten.
- Running WSL commands from PowerShell: inline `bash -c '...$VAR...'` gets
  mangled (empty vars, broken regex quoting). Put the commands in a script
  under `\\wsl.localhost\Ubuntu\root\` and run `wsl -u root -- bash /root/X.sh`.

## 2026-10-26: GUI OVL direct scanout (patch 0088) + DSI kick for present()
- `xnu/patches/0088-rmx2117-ovl-direct-scanout.patch`, three pieces:
  - `AppleCLCD::kick()` toggles DSI_START (dsi0+0x0C, mapped via ml_io_map)
    at the end of present() and once per power-off fade step. The GUI
    present path never kicked the cmd-mode panel before -- only the early
    console and fbcon paths did, and those die with their consoles.
    `sweetfb_kick=0` disables.
  - Direct scanout (`sweetfb_direct=1` default, `=0` -> pure CPU copy):
    start() maps disp_ovl0_2l @ 0x14006000, logs and sanity-checks
    EN/SRC_CON/L1_ADDR/L1_PITCH, and proceeds only if LK left layer 1
    scanning exactly fPhys @ fRowBytes (expect `L1 addr 0x78816000 pitch
    0x10e0`). present() then detects the single-enabled-layer case
    (CoreAnimation's composited frame), clips the same way the copy path
    does, and when the result is a full-bleed 1080x2400 4-byte-pixel rect
    in a physically-contiguous surface it writes OVL L1_ADDR (+0x0f60) and
    L1_PITCH (+0x0064) instead of doing the ~10 MB memcpy. The scan rect is
    cleaned for DMA with flush_dcache64, and the scanned surface is held
    one extra present (the OVL may still be DMA-ing the previous frame).
    Anything else falls through to the old copy path; misses are counted
    and the first few logged.
  - SweetSurface::create tries kIOMemoryPhysicallyContiguous for surfaces
    >= 8 MB (fullscreen-class), falling back to ordinary pages.
  - Power-off fade points L1 back at fPhys first so the fade stays visible.
- MT6853 OVL offsets cross-checked with vendor `mtk_disp_ovl.c`: L_ADDR(1)
  = 0x0f60 (0x0f40 + 0x20*n, mt6853), L_PITCH(1) = 0x0064 (low 16 bits;
  pitch[19:16] lives in L_PITCH_MSB @ +0x40 -- untouched, surfaces are
  limited to pitch <= 0xffff), EN @ +0x0c, SRC_CON @ +0x2c.
- NOT validated on device yet: assumes LK programs OVL in immediate-commit
  (no CMDQ/shadow relay that would need re-arming per frame). The boot log
  line `SWEET FB: ovl0_2l en ... src_con ... L1 addr 0x78816000` tells us
  what LK actually left; if the check fails the code disables direct and
  stays on the copy path.
- `git apply --check` clean vs the post-0087 source; local extracted
  buildtree advanced to post-0088. No Apple-fork clang on this box, so
  compile check is deferred to the CI xnu.yml run (it picks up the patch
  automatically).

## 2026-10-26 (2): 60 Hz presents + direct-scanout fallback fix (patch 0089)
- `xnu/patches/0089-rmx2117-ovl-scanout-60hz.patch` (SweetFramebuffer only):
  - `sweetfb_hz` default 10 -> 60: `fFlushTicks` becomes 1, so a dirty
    frame is presented every vsync tick (timer is 16667 us). The 10 Hz
    default from patch 0056 is what capped the GUI at 10 fps.
  - BUG FIX in 0088: a frame that fell back to the CPU-copy path left OVL
    L1 pointing at the last direct-scan surface -- the copied pixels in
    fPhys would never reach the panel until another direct frame ran.
    present() now restores L1_ADDR=fPhys/L1_PITCH before copying whenever
    fScan[0] is live, with the same one-extra-frame release rule.
  - PITCH_MSB (+0x0040+0x20*n, bits[3:0]=pitch[19:16]) readback is added
    to the start() sanity check and cleared whenever we program L_PITCH,
    so a stale nonzero MSB can't corrupt the stride.
  - Stats: `fStatDirect`/`fStatCopy` count which path each present took;
    vsyncFired logs them every ~5 s while frames flow (plus fDirectMiss).
    This is the on-device proof that 60 fps is real and which path wins.
- ovl0_2l is a TWO-LAYER block (vendor `mtk_ovl_layer_num(OVL0_2L) == 2`):
  L0 is the boot/panic console fb, L1 is the GUI layer. Multi-layer OVL
  compositing is not possible on this pipe; single-surface zero-copy
  scanout is the full extent of the display offload this hardware path
  allows. (ovl0 @ 0x14005000 has 4 layers but is not in the LK-configured
  pipe; repathing through mutex/CMDQ is out of scope.)
- True GPU acceleration (Mali G57) is not feasible: iOS GPU services are
  AGX-only. "Graphical acceleration" here = display-engine offload: the
  OVL DMA scans the CA surface directly at 60 Hz instead of a ~10 MB
  memcpy per frame (622 MB/s saved) plus a DSI_START kick per frame.

## 2026-10-29: input/startup fixes + GPU/CONN probes (patches 0060 rev, 0090, 0091)

**Priority order used** (user-impact x tractability):
1. startup delay + touch (boot-visible, SPI path verifiable)
2. vol-up (daily-driver usability)
3. GPU probe (documented milestone)
4. Wi-Fi/BT probe (foundation only -- iOS has no stack)
5. SIM/modem (document only -- huge, needs CommCenter)

**Fixes landed in patch 0060:**
- sweettouchdelay default 15 s -> **0** (boot arg still overrides; the wait
  is now conditional). This was the visible 'delay' -- an explicit sleep in
  	ouch_thread, not hardware bring-up time.
- Vol-up now polls **two** candidate sources: GPIO1 DIN *and* the MT6359
  PMIC HOMEKEY deb bit (TOPSTATUS  x2a bit  x8, verified against
  upstream mtk-pmic-keys.c mt6359_regs). Each line must be observed
  released once before presses count ('armed'), so a permanently-low wrong
  pad can never emit a fake press. Rationale: the stock DTB wires
  mt-pmic interrupts pwrkey/pwrkey_r/homekey/homekey_r and this phone
  has no physical home button -- realme commonly maps homekey to vol-up.
  Boot log line SWEET KEYS: volup=GPIO1+HOMEKEY voldn=GPIO80 plus
  KEYVU src=N prints reveal which line actually fires on device.

**Vendor-verified register facts (checked against kernel_xiaomi_mt6768 /
unplugged-kernel mt6853 sources, NOT guessed):**
- SPI4 gate = IFRAO4 bit 25, clr  xa8 / sta  xac at  x10001000
  (GATE_IFRAO4(CLK_IFRAO_SPI4,"ifrao_spi4","spi_ck",25)).
- SPI4 pins GPIO12/13/97/98 all func 2 (mt6853-pinfunc.h).
- GPIO1 IES/PU/PD = iocfg_br( x11d40000)  x70/ xb0/ x90 bit 12 --
  matches mt6853_pin_{ies,pu,pd}_range pin-field tables exactly; the
  existing driver writes were already correct, so if vol-up still fails it
  is a wiring/source issue, not register math.
- SPI4 clock index in the stock DTB decodes to CLK_IFRAO_SPI4 = 73.
- MFG PWR_CON: MFG0  x308 MFG1  x30c MFG2  x310 MFG3  x314
  MFG5  x31c (SPM  x10006000); sta masks b2/b3/b4/b5/b7; SRAM_PDN b8,
  ACK b12 *inside* each PWR_CON (not a separate PROT_ACK reg).
- MFG1 bus-prot release order (vendor spm_mtcmos_ctrl_mfg1_bus_prot):
  SRAM_PDN=0 -> way_en infracfg_pdn(0x10215000)+0x4c=0x1800 ->
  PROTEN_CLR(0x2a4)=0x600000 -> PROTEN_2_CLR(0x718)=0x80 ->
  PROTEN_1_CLR(0x2ac)=0x200000 -> PROTEN_2_CLR(0x718)=0x60.
- CONN_PWR_CON  x304, sta bit1; CONN bus-prot release order:
  PROTEN_CLR=b14, PROTEN_1_CLR=b10, PROTEN_CLR=b13|b18.

**New patches:**
-  090-sweet-gpu-m1.patch: sweet_gpu.c -- MFG0..5 power-on, MFG1
  bus-prot release, mfgcfg BG3D+external CG, read GPU_ID/SHADER/L2/TILER
  PRESENT+READY regs, kbase soft-reset. Uses the Valhall JM regmap from
  GPU-BRINGUP.md section 5 (PWRON at  x180/ x1a0, not the older
  0x84/0x88 layout). Gated sweetgpu=1 default OFF.
-  091-sweet-conn-m0.patch: sweet_conn.c -- CONN MTCMOS on, bus-prot
  release, read CONNSYS_HW_IP_VER at  x18002000+0x110. Gated
  sweetconn=1 default OFF.

**Feasibility calls (documented, not started):**
- Wi-Fi/BT: MT6635 conninfra probe only. iOS has no IO80211Family or
  IOBluetoothFamily driver that can talk to a MediaTek MAC/firmware pair;
  a functional stack is a separate multi-quarter project (custom
  IOEthernet/HCI plus userland). The M0 probe proves power+regs.
- SIM/modem: mddriver (CLDMA|CCIF|DPMAIF) + MD1_PWR_CON  x300 exist in
  the platform, but booting MD3 needs the modem firmware binary + CCIF
  handshake + SCP co-processor bring-up, and iOS has no CommCenter to
  consume it. Document-only; not attempted.
- GUI 'GPU acceleration': remains OVL direct-scanout (patch 0088/0089).
  sweetgpu=1 only probes the Mali block; iOS GPU services are AGX-only
  so there is no userland to feed it even if M1 passes.

**Still unknown (needs device log):**
- Which of GPIO1 vs HOMEKEY actually toggles for vol-up (driver now covers
  both; KEYVU src= log reveals it).
- Whether the touch IC answers with an NVT id at all vs a Himax
  HX83112F-style part (existing log prints the 6 raw id bytes).
- The shipped image cmdline previously carried sweetkeys=0 sweettouch=0
  sweetusb=0 kill switches (mkpayload --cmdline); a new image must drop
  those for any of the input drivers to run.

## 2026-10-31: Touch NVT protocol audit vs vendor source (bugs fixed)

Checked the real oplus driver from the MT6853 kernel source
(`drivers/input/touchscreen/oplus_touchscreen/Novatek/NT36672C_noflash/nvt_drivers_nt36672c_noflash.c`
+ `util_interface/touch_interfaces.[ch]`). Two protocol bugs in sweet_touch.c
made every transfer wrong:

- **`nvt_write` sent READ commands.** Vendor `CTP_SPI_WRITE` applies
  `SPI_WRITE_MASK(buf[0]) = off|0x80`; we sent `off&0x7f`. Every write
  (SWRST, ENG_RST, BOOT_RDY, CRC banks, all 256-byte fw pages) was
  silently dropped -- fw download could never have worked.
- **`nvt_read` missed the dummy byte.** Vendor clocks `len+1` bytes:
  offset, then the chip emits ONE dummy MISO byte, then data
  (`memcpy(buf+1, rbuf+2, len-1)`). We clocked `n` bytes and took
  `rx[1]`, so all reads were shifted one byte and short by one --
  chip-id never matched ("no NT36672C id" -> thread exits).
- **hw_cs (a4fcd0c) can't frame >32B transfers.** The engine deasserts
  CSB at every ACT boundary; 256B fw writes and 80B point reads split
  into separate CS windows. Reverted to bit-banged GPIO98 for the whole
  message (vendor packets would need packet_loop+PAUSE to use hw CSB).
- **GPIO43 is a hold-high line**, not a spare reset: stock
  `ctp_gt9896s_reset_active` pinctrl drives it output-high (output-low in
  suspend). Now driven high at init; still pulsed as reset fallback.
- **vtp_power_on() is wrong-project hardware.** The 20611 touch node
  wires NO supply -- incell TDDI is powered via the panel bias LK left
  on. `vdd_2v8->mt_pmic_vtp_ldo_reg` exists only on 20630/20637 nodes.
  The i2c5 block also never ungated `imp_iic_wrap_s` CLK_IMPS_AP_CLOCK_RO_I2C5
  (@0x11d04000 CG sta e00/clr e04 bit0) or the I2C_SEL mux -- MMIO to a
  gated controller risks a hang/abort, and it programmed 1.8V where
  vdd_2v8 defaults to 3.1V. Now gated behind `sweetvtp=1` (default off),
  ungates imp clock first, writes 0xba (3.1V flat) instead of 0x3d.
- MT6360 LDO regs themselves were right (EN_CTRL2 0x11 bit0x40,
  CTRL3 0x15; from mt6360_ldo_regmap.c). MT6360 PMU i2c addr is 0x34;
  the 0x64 subpmic_ldo addr is unverified.
- dtb_6 of dtbo.img confirmed the node: `oplus,tp_noflash` on spi4,
  irq GPIO14 falling, reset-gpio GPIO33 (pinctrl companion GPIO43),
  spi-max-frequency 9.6MHz, mode0. Recovery maps project 20686 -> dir
  20611. Panels offered: nt36672c (JDI/TM), hx83112f, ili7807s -- so if
  chip-id still fails the unit may have the Himax/Ilitek TDDI, which
  needs a different driver.
