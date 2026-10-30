# ios-rmx2117 — port of ios-sweet to the Realme Narzo 30 Pro 5G (RMX2117 / MT6853)

**Device: Realme Narzo 30 Pro 5G (RMX2117L1), Dimensity 800U (MT6853)**.
Verified via ADB: Android 12 (`Q.1546350_1ab2a`), SPL 2024-01-05, ~6GB RAM,
8 CPUs, **`sys.oem_unlock_allowed`=1 — unlockable.**

## Boot chain (MTK — much friendlier than Qualcomm/Samsung)

    BootROM -> preloader -> LK (has fastboot!) -> boot.img kernel
                             -> our shim -> XNU + ADT

- Unlock: realme **in-depth test** app → `fastboot flashing unlock`
  (wipes userdata — needs user approval)
- `fastboot boot` may allow unflashed test boots (LK usually supports it)
- `mtkclient` BROM/DL mode = recovery safety net

## Hardware facts — from the LIVE prebuilt DTB
(`research/rmt6853/dtbs/entry_0_0_id00000000.dtb`, extracted from
`android_device_realme_RMX2117`'s `mt6853.dtb`, itself a dt_table image)

| Item | Value |
|---|---|
| GIC | **GIC-600**: GICD `0x0c000000`+0x40000, GICR `0x0c040000`+0x200000 |
| arch_timer | PPI 13/14/11/10 → INTID 29/30/**27**/26; **13 MHz** (`0xc65d40`) |
| UFS | `ufshci@0x11270000` size 0x2300, **GIC_SPI 105**, `mediatek,ufshci` |
| CPUs | 6×A55 @~1.7GHz (`reg` 0x000–0x500) + 2×A76 @~2.17GHz (`0x600`,`0x700`) — **contiguous MPIDRs (core<<8)** |
| Touch | The recovery live DT contains the project `oplus,tp_noflash` node on SPI4 (IRQ GPIO14, reset GPIO33); SPI4 is `mediatek,mt6765-spi`, and I2C5 declares push-pull signaling for its MT6360 LDO. The XNU bring-up config now uses enhanced SPI CFG2 timing and the DT's I2C mode. The NT36672C ID is still unconfirmed; the previous log showed SPI timeouts and I2C NACKs. |
| Panel | `nt35595_fhd_dsi_cmd_truly_nt50358` 1080×2400 (k6853v1_64 atag) |
| bootargs | `console=ttyS0,921600n1` — MTK UART @ 921600 |

## Boot image (community-verified `android_device_realme_RMX2117`)

| Param | Value |
|---|---|
| header_version | **2** |
| base | `0x40078000` (kernel offset `0x8000`) |
| pagesize | 2048 |
| ramdisk offset | `0x07c08000` |
| tags/dtb offset | `0x0bc08000` |
| dtb | **baked into boot.img** (`BOARD_INCLUDE_DTB_IN_BOOTIMG`) |
| boot partition | `boot` (present in by-name, no A/B slot suffix) |

## Component mapping

| ios-sweet piece | File | state |
|---|---|---|
| Entry (EL1, DTB in x0) | `shim/start.S` | unchanged — LK arm64 boot protocol |
| MMU | `shim/mmu.c` | FB_PHYS 0x7e605000; 8GB device map covers GIC/FB/UFS |
| FB console | `shim/console.[ch]` | 0x7e605000, 1080×2400 |
| Payload loader | `shim/loader.c` | embedded `_end` + FDT initrd both work |
| UFS probe | `shim/ufs.c` | `0x11270000`, 13MHz timeout, MTK-glue caveat noted |
| GIC/PSCI/SMP | `shim/shim.c` | GIC-600 addrs, `i<<8` MPIDRs, IMEM/PSHOLD stubbed |
| Boot image | `shim/build.sh` | RMX2117 v2 offsets, embedded payload |
| ADT builder | `tools/mkadt.py` | MT6853 constants, real MPIDRs, 2-cluster topology |
| DTB dump | `tools/fdt_dump.py` | NEW — parses the live dtb |
| QEMU loop | `tools/qemu-rmx2117.py` | FB 0x7e605000 (virt RAM base matches!) |
| XNU patches | `xnu/patches` | copied from ios-sweet (85 files) + CI workflow; needs macOS build |
| ADT | `dtb/mt6853.adt` | BUILT — highmem dram 0x100000000+2G, real fb 0x77e20000/p4352 |

## Risks

- **Unlock wipes userdata** — that's a user decision; fastboot unlock is
  confirmed possible (`oem_unlock_allowed=1`).
- The UFS host has MTK glue registers — the JEDEC UTRL model applies, but
  power/clock may need MTK-specific enable (first probe will reveal).
- The LK framebuffer address `0x7e605000` is the *source-tree* default;
  the live one may differ (SELinux blocks reading `/chosen` props —
  we know it from the prebuilt dtb only if LK reuses it).
- MTK's `reset` path: no Qualcomm IMEM/PSHOLD — PSCI SYSTEM_RESET only.
