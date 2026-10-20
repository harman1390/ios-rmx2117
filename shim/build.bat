@echo off
rem Windows port of build.sh — RMX2117 (MT6853) shim boot image.
rem Toolchain: Arm GNU Toolchain (aarch64-none-elf) in ..\..\tools_pkg\arm-gcc
rem mkbootimg: ..\tools\mkbootimg.py (pure-python, no AOSP dep)
rem Usage: build.bat [payload_file]   (payload appended after _end)
setlocal
cd /d "%~dp0"
set TC=..\..\tools_pkg\arm-gcc\bin
set CC=%TC%\aarch64-none-elf-gcc.exe
set OBJCOPY=%TC%\aarch64-none-elf-objcopy.exe
set READELF=%TC%\aarch64-none-elf-readelf.exe
set NM=%TC%\aarch64-none-elf-nm.exe
set CFLAGS=-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra
if not defined HDRVER set HDRVER=2
if not exist out mkdir out
for %%f in (start.S shim.c console.c mmu.c fdt.c loader.c ufs.c) do (
	%CC% %CFLAGS% -c %%f -o out\%%~nf.o || exit /b 1
)
rem No absolute relocations allowed in C objects (image is not relocated).
set ABS=0
for %%f in (shim console mmu fdt loader ufs) do (
	for /f %%c in ('%READELF% -r out\%%f.o ^| findstr /c:"ABS64" ^| find /c /v ""') do set /a ABS+=%%c
)
if not "%ABS%"=="0" ( echo error: %ABS% absolute relocations in C objects >&2 & exit /b 1 )
%CC% %CFLAGS% -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N ^
	out\start.o out\shim.o out\console.o out\mmu.o out\fdt.o out\loader.o out\ufs.o -o out\shim.elf || exit /b 1
%OBJCOPY% -O binary out\shim.elf out\Image || exit /b 1
rem Pad to _end so .bss/stack space exists in the copied image.
for /f %%s in ('%NM% out\shim.elf ^| findstr /c:" _end"') do set END=%%s
python -c "import sys; open('out\\Image','ab').truncate(int('%END%',16))" || exit /b 1
if not "%~1"=="" (
	copy /b out\Image+"%~1" out\Image.cat >nul
	move /y out\Image.cat out\Image >nul
)
rem LK expects a gzip'd kernel (stock boot.img kernel is Image.gz): it
rem inflates to kernel_addr 0x40080000 and jumps to image base.
python -c "import gzip; d=open('out\\Image','rb').read(); open('out\\Image.gz','wb').write(gzip.compress(d, compresslevel=9, mtime=0))" || exit /b 1
python ..\tools\mkbootimg.py --header_version %HDRVER% --kernel out\Image.gz --pagesize 2048 ^
	--base 0x40078000 --kernel_offset 0x8000 --ramdisk_offset 0x07c08000 ^
	--second_offset 0xbff88000 --tags_offset 0x0bc08000 ^
	--dtb ..\dtb\real-stock-dtb.dtb --dtb_offset 0x0bc08000 ^
	--ramdisk ..\dtb\stock-ramdisk.img ^
	--cmdline "bootopt=64S3,32N2,64N2 buildvariant=user" --os_version 12.0.0 --os_patch_level 2024-01-05 -o out\shim-boot.img || exit /b 1
echo.
echo Built out\shim-boot.img — flash to 'boot' via mtkclient or fastboot boot.
dir out\Image out\shim-boot.img
