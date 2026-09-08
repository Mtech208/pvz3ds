cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED ENV{PS2DEV} OR NOT DEFINED ENV{PS2SDK})
	message(FATAL_ERROR "PS2DEV and PS2SDK must be defined by the PS2 preset")
endif()

file(TO_CMAKE_PATH "$ENV{PS2DEV}" PS2DEV_ROOT)
file(TO_CMAKE_PATH "$ENV{PS2SDK}" PS2SDK_ROOT)
set(PS2_EE_BIN "${PS2DEV_ROOT}/ee/bin")

include("${PS2SDK_ROOT}/ps2dev.cmake")

set(CMAKE_C_COMPILER "${PS2_EE_BIN}/mips64r5900el-ps2-elf-gcc.exe" CACHE FILEPATH "PS2 C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${PS2_EE_BIN}/mips64r5900el-ps2-elf-g++.exe" CACHE FILEPATH "PS2 C++ compiler" FORCE)
set(CMAKE_ASM_COMPILER "${PS2_EE_BIN}/mips64r5900el-ps2-elf-gcc.exe" CACHE FILEPATH "PS2 assembler" FORCE)
