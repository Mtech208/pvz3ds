#pragma once
#ifdef PS2_PLATFORM

// PS2 OpenGL compatibility-layer tuning. Keep backend switches here so game
// code never needs renderer-specific feature defines.
#ifndef PS2_GL_ENABLE_PSMT8
#define PS2_GL_ENABLE_PSMT8 1
#endif

// Bitmask: 1 = indexed source textures, 2 = auto-palettized RGBA textures.
#ifndef PS2_GL_ENABLE_PSMT4
#define PS2_GL_ENABLE_PSMT4 3
#endif

#ifndef PS2_GL_TEXTURE_GUARDS
#define PS2_GL_TEXTURE_GUARDS 1
#endif

#ifndef PS2_GL_TEXTURE_SHRINK_SHIFT
#define PS2_GL_TEXTURE_SHRINK_SHIFT 1
#endif

// Logical OpenGL texture limit. 512 preserves the current low-memory PvZ
// profile; a renderer with a different memory budget can raise this without
// changing the GL implementation.
#ifndef PS2_GL_MAX_TEXTURE_SIZE
#define PS2_GL_MAX_TEXTURE_SIZE 512
#endif

#ifndef PS2_GL_MAX_TEXTURES
#define PS2_GL_MAX_TEXTURES 4096
#endif

#ifndef PS2_GL_MAX_VBOS
#define PS2_GL_MAX_VBOS 2048
#endif

#ifndef PS2_GL_MAX_DISPLAY_LISTS
#define PS2_GL_MAX_DISPLAY_LISTS 4096
#endif

// Small direct-mapped post-transform cache used by the generic 3D path.
// Keep this a power of two; shared vertices in quads/strips/indexed meshes
// avoid repeating MVP, texcoord, lighting and fog work on the EE/VU0 path.
#ifndef PS2_GL_VERTEX_CACHE_SIZE
#define PS2_GL_VERTEX_CACHE_SIZE 128
#endif
#if PS2_GL_VERTEX_CACHE_SIZE > 0 && (PS2_GL_VERTEX_CACHE_SIZE & (PS2_GL_VERTEX_CACHE_SIZE - 1))
#error PS2_GL_VERTEX_CACHE_SIZE must be zero or a power of two
#endif

#ifndef PS2_GL_RENDER_STATS
#define PS2_GL_RENDER_STATS 0
#endif

// Use GIF REGLIST for generic PATH3 triangle streams. Textured vertices carry
// ST/RGBAQ/XYZ2 payloads and untextured vertices carry RGBAQ/XYZ2 without A+D
// address words. Keep a compile-time fallback for hardware/emulator parity tests.
#ifndef PS2_GL_USE_GIF_REGLIST
#define PS2_GL_USE_GIF_REGLIST 1
#endif

// VU0 macro mode is already usable through libvux and accelerates individual
// matrix/vector operations without changing the draw submission architecture.
#ifndef PS2_GL_USE_VU0_MACRO
#define PS2_GL_USE_VU0_MACRO 1
#endif

// Optional VU0 micro-mode backend. Macro mode remains the correctness path;
// micro mode is developed as a separate batched path because the EE cannot use
// VU0 macro instructions while a VU0 microprogram is running.
#ifndef PS2_GL_ENABLE_VU0_MICRO
#define PS2_GL_ENABLE_VU0_MICRO 0
#endif

// VU0 micro mode has a fixed VIF/MSCAL synchronization cost. Process enough
// vertices per invocation to amortize it while staying comfortably inside the
// 4 KiB VU0 data memory. Ps2GlTransform64.vsm requires this to remain 64.
#ifndef PS2_GL_VU0_MICRO_BATCH_VERTICES
#define PS2_GL_VU0_MICRO_BATCH_VERTICES 64
#endif

#ifndef PS2_GL_VU0_MICRO_MIN_VERTICES
#define PS2_GL_VU0_MICRO_MIN_VERTICES 64
#endif

// Clip-space results are cached in EE RAM for the duration of a draw so the
// normal clipper/primitive assembler can consume VU0 results without changing
// OpenGL semantics. The allocation grows lazily up to this limit.
#ifndef PS2_GL_VU0_MICRO_MAX_VERTICES
#define PS2_GL_VU0_MICRO_MAX_VERTICES 32768
#endif

// Defensive bound for the VIF0 idle wait after FLUSHE. A timeout disables the
// micro backend and falls back to the macro/scalar correctness renderer.
#ifndef PS2_GL_VU0_MICRO_SYNC_SPINS
#define PS2_GL_VU0_MICRO_SYNC_SPINS 1000000
#endif

// Optional parity check against the scalar EE matrix multiply. Useful while
// validating a new dvp-as/toolchain or emulator; keep off in release builds.
#ifndef PS2_GL_VU0_MICRO_VALIDATE
#define PS2_GL_VU0_MICRO_VALIDATE 1
#endif

// VU1 remains opt-in while the active batch backend is validated on hardware.
// The public GL API must behave identically with this set to 0 or 1.
#ifndef PS2_GL_ENABLE_VU1
#define PS2_GL_ENABLE_VU1 0
#endif

#ifndef PS2_GL_VU1_BATCH_VERTICES
#define PS2_GL_VU1_BATCH_VERTICES 64
#endif

// Small draws stay on the EE/VU0 path: VIF1 setup and DMA have a fixed cost.
#ifndef PS2_GL_VU1_MIN_VERTICES
#define PS2_GL_VU1_MIN_VERTICES 32
#endif

// VU1 Phase 2C keeps the 64-vertex clip-space compatibility backend and can
// optionally direct-submit fully accepted triangle draws through PATH1. The
// direct path performs MVP/perspective/viewport/depth/STQ/GIF packing in VU1.
#ifndef PS2_GL_VU1_MAX_VERTICES
#define PS2_GL_VU1_MAX_VERTICES 65536
#endif

#ifndef PS2_GL_VU1_SYNC_SPINS
#define PS2_GL_VU1_SYNC_SPINS 1000000
#endif

#ifndef PS2_GL_VU1_VALIDATE
#define PS2_GL_VU1_VALIDATE 1
#endif

#ifndef PS2_GL_VU1_PATH1
#define PS2_GL_VU1_PATH1 0
#endif

// Phase 2C uses 84 vertices (28 triangles) per direct batch. Each input
// vertex occupies three qwords (position, STQ seed, RGBA lanes), so 84 vertices
// consume 252 qwords and remain inside VIF UNPACK's 256-qword command limit.
// Keep this a multiple of three and update the VU1 data layout if it changes.
#ifndef PS2_GL_VU1_PATH1_BATCH_VERTICES
#define PS2_GL_VU1_PATH1_BATCH_VERTICES 84
#endif

// Base-level fallback for mipmapped MIN filters. True mipmap storage is not
// implemented yet, so make the approximation explicit and deterministic.
#ifndef PS2_GL_BASE_LEVEL_MIP_FALLBACK
#define PS2_GL_BASE_LEVEL_MIP_FALLBACK 1
#endif

#endif // PS2_PLATFORM
