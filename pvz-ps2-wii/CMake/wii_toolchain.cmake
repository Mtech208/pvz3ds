set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

if(NOT DEFINED ENV{DEVKITPRO})
	set(ENV{DEVKITPRO} "C:/devkitPro")
endif()
if(NOT DEFINED ENV{DEVKITPPC})
	set(ENV{DEVKITPPC} "$ENV{DEVKITPRO}/devkitPPC")
endif()

file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO)
file(TO_CMAKE_PATH "$ENV{DEVKITPPC}" DEVKITPPC)

set(CMAKE_C_COMPILER   "${DEVKITPPC}/bin/powerpc-eabi-gcc.exe")
set(CMAKE_CXX_COMPILER "${DEVKITPPC}/bin/powerpc-eabi-g++.exe")
set(CMAKE_ASM_COMPILER "${DEVKITPPC}/bin/powerpc-eabi-gcc.exe")
set(CMAKE_AR           "${DEVKITPPC}/bin/powerpc-eabi-gcc-ar.exe")
set(CMAKE_RANLIB       "${DEVKITPPC}/bin/powerpc-eabi-gcc-ranlib.exe")
set(CMAKE_STRIP        "${DEVKITPPC}/bin/powerpc-eabi-strip.exe")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH
	"${DEVKITPPC}"
	"${DEVKITPRO}/libogc"
	"${DEVKITPRO}/portlibs/ppc"
	"${DEVKITPRO}/portlibs/wii"
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(NINTENDO_WII TRUE CACHE BOOL "Build for Nintendo Wii" FORCE)

add_compile_definitions(GEKKO HW_RVL)
add_compile_options(-mrvl -mcpu=750 -meabi -mhard-float)
add_link_options(-mrvl -mcpu=750 -meabi -mhard-float)

include_directories(SYSTEM
	"${DEVKITPRO}/libogc/include"
	"${DEVKITPRO}/portlibs/ppc/include"
	"${DEVKITPRO}/portlibs/wii/include"
)
link_directories(
	"${DEVKITPRO}/libogc/lib/wii"
	"${DEVKITPRO}/portlibs/ppc/lib"
	"${DEVKITPRO}/portlibs/wii/lib"
)
