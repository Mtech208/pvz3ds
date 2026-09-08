# Nintendo 3DS toolchain. Delegate to devkitPro's official toolchain file
# when it is present (it is installed with devkitARM and provides the ctr_*
# / dkp_* macros the CMakeLists uses), otherwise fall back to a minimal
# manual setup so a fresh checkout can still configure.
#
# The environment is normally injected by CMakePresets.json (3ds-release);
# the defaults here are only a safety net for manual command-line configs.

set(ENV{DEVKITPRO} "/c/devkitPro")
set(ENV{DEVKITARM} "/c/devkitPro/devkitARM")
set(DEVKITPRO "/c/devkitPro")
set(DEVKITARM "/c/devkitPro/devkitARM")

set(DEVKITPRO_TOOLCHAIN "${DEVKITPRO}/cmake/3ds.cmake")
if(EXISTS "${DEVKITPRO_TOOLCHAIN}")
	# devkitPro's 3ds.cmake sets CMAKE_SYSTEM_NAME, picks the ARM compilers,
	# adds the devkitpro cmake module dir to the module path (which provides
	# ctr_add_shader_library / dkp_add_embedded_binary_library /
	# ctr_generate_smdh / ctr_create_3dsx), and defines NINTENDO_3DS.
	include("${DEVKITPRO_TOOLCHAIN}")
else()
	message(WARNING
		"devkitPro toolchain not found at ${DEVKITPRO_TOOLCHAIN}. "
		"Using minimal manual 3DS toolchain setup. Expected GNU tools in "
		"${DEVKITARM}/bin and portlibs under ${DEVKITPRO}/portlibs/armv6k.")
	set(CMAKE_SYSTEM_NAME Generic)
	set(CMAKE_SYSTEM_PROCESSOR armv6k)

	set(CMAKE_C_COMPILER   "${DEVKITARM}/bin/arm-none-eabi-gcc.exe")
	set(CMAKE_CXX_COMPILER "${DEVKITARM}/bin/arm-none-eabi-g++.exe")
	set(CMAKE_ASM_COMPILER "${DEVKITARM}/bin/arm-none-eabi-gcc.exe")
	set(CMAKE_AR           "${DEVKITARM}/bin/arm-none-eabi-gcc-ar.exe")
	set(CMAKE_RANLIB       "${DEVKITARM}/bin/arm-none-eabi-gcc-ranlib.exe")
	set(CMAKE_STRIP        "${DEVKITARM}/bin/arm-none-eabi-strip.exe")

	set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
	set(CMAKE_FIND_ROOT_PATH
		"${DEVKITARM}"
		"${DEVKITPRO}/libctru"
		"${DEVKITPRO}/portlibs/armv6k"
	)
	set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
	set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
	set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

	set(NINTENDO_3DS TRUE CACHE BOOL "Build for Nintendo 3DS" FORCE)
	add_compile_options(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp)
	add_link_options(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp)

	include_directories(SYSTEM
		"${DEVKITARM}/arm-none-eabi/include"
		"${DEVKITPRO}/libctru/include"
		"${DEVKITPRO}/portlibs/armv6k/include"
	)
	link_directories(
		"${DEVKITPRO}/libctru/lib"
		"${DEVKITPRO}/portlibs/armv6k/lib"
	)
endif()
