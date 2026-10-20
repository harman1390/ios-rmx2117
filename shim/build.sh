#!/bin/bash
# Build the RMX2117 (Narzo 30 Pro / MT6853 Dimensity 800U) shim boot image.
#
# MTK boot chain: preloader -> LK (little kernel) -> boot image kernel.
# LK implements fastboot, so we can TEST without flashing: fastboot boot
# out/shim-boot.img after unlocking (realme in-depth test +
# fastboot flashing unlock -- wipes userdata, needs user approval).
#
# Boot image params are the community-verified RMX2117 values
# (android_device_realme_RMX2117 BoardConfig.mk):
#   header_version 2, base 0x40078000, pagesize 2048, dtb in bootimg.
#
# The xnu payload is appended to the kernel Image itself (loader.c finds
# it at _end); pass it as $1. LK also delivers a real ramdisk via the
# FDT /chosen initrd, so either path works.
HDRVER=${HDRVER:-2}
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Payload (tools/mkpayload.py) is appended after _end; loader.c probes for
# the SWEETXNU magic there before trying the FDT initrd path.
if [ -n "${1:-}" ]; then
	cat "$1" >> out/Image
fi
# LK expects a gzip'd kernel (stock boot.img kernel is Image.gz): it
# inflates to kernel_addr 0x40080000 and jumps to image base/text_offset.
python3 -c "import gzip; d=open('out/Image','rb').read(); open('out/Image.gz','wb').write(gzip.compress(d, compresslevel=9, mtime=0))"
# MT6853 boot image: the exact RMX2117 params from the community
# BoardConfig.mk (header 2, base 0x40078000, dtb baked into the image).
# DTB: copy the phone's own from ../research/rmt6853/dtbs/ as
# ../dtb/mt6853.dtb (BOARD_INCLUDE_DTB_IN_BOOTIMG := true).
python3 ../tools/mkbootimg.py --header_version "$HDRVER" --kernel out/Image.gz --pagesize 2048 \
	--base 0x40078000 --kernel_offset 0x8000 --ramdisk_offset 0x07c08000 \
	--second_offset 0xbff88000 --tags_offset 0x0bc08000 \
	--dtb ../dtb/real-stock-dtb.dtb --dtb_offset 0x0bc08000 \
	--ramdisk ../dtb/stock-ramdisk.img \
	--cmdline "bootopt=64S3,32N2,64N2 buildvariant=user" --os_version 12.0.0 --os_patch_level 2024-01-05 -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
