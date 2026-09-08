@echo off

cd /d "%~dp0"

"C:\devkitPro\devkitPPC\bin\powerpc-eabi-strip.exe" --strip-all -o "build\wii-release\boot.elf" "build\wii-release\pvz_wii.elf"
