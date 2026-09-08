#pragma once

#ifdef PS2_PLATFORM

#define PS2_GS_TEXTURE_SHRINK_SHIFT 1
#define PS2_GS_MAX_TEXTURE_SIZE 512
#define PS2_GS_MAX_RESIDENT_TEXTURES 4096
#define PS2_GS_SPRITE_BATCH_MAX_SPRITES 256
#define PS2_GS_BATCH_MAX_PRIMITIVES PS2_GS_SPRITE_BATCH_MAX_SPRITES
#define PS2_GS_COMMAND_MAX_PRIMITIVES 1024
#define PS2_GS_COMMAND_MAX_RUNS 512
#define PS2_GS_COMMAND_MAX_REGISTERS (PS2_GS_COMMAND_MAX_PRIMITIVES * 10)
#define PS2_GS_QUEUE_GUARD_BYTES 8192u

// Defensive bound for every GS FINISH wait the game controls. gsKit's own wait
// is a bare CSR spin with no timeout, so a GIF that never raises FINISH hangs
// the main thread forever -- silently, with audio still playing, and only on
// real hardware. Anything the game calls waits at most this many spins and then
// resets the GIF instead. Sized like PS2_GL_VU1_SYNC_SPINS: far longer than any
// legitimate batch, short enough that a wedge is a dropped frame, not a freeze.
#define PS2_GS_FINISH_SPINS 1000000u

#endif
