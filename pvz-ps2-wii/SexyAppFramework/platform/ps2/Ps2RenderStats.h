#pragma once

#ifdef PS2_PLATFORM

// Renderer residency counters, kept in their own header so diagnostics callers
// do not have to pull the GS backend (and gsKit) into game code.
//
// Uploads and evictions reported per frame are what distinguish a texture
// working set that fits in VRAM from one that is re-uploaded every frame. The
// latter costs a GS fence per miss, because releasing a VRAM range means
// retiring the queued work that still references it.

extern "C" void ps2_dbg_texture_frame_reset(void);
extern "C" int ps2_dbg_texture_uploads_frame(void);
extern "C" int ps2_dbg_texture_evictions_frame(void);
extern "C" int ps2_dbg_texture_uploads_total(void);
extern "C" int ps2_dbg_texture_evictions_total(void);
extern "C" int ps2_dbg_texture_arena_rewinds(void);
extern "C" int ps2_dbg_texture_resident_count(void);
extern "C" int ps2_dbg_clut_shared_kb(void);
extern "C" int ps2_dbg_clut_unique_count(void);

// Queue-guard traffic. The guard is a corruption backstop: it firing means the
// gsKit oneshot pool is undersized for the frame, which is what put the main
// thread into gsKit's unbounded FINISH spin and froze the game on hardware.
// A healthy frame reports zero. Recoveries count wedged-GIF resets, which
// should never be anything but zero.
extern "C" void ps2_dbg_queue_frame_reset(void);
extern "C" int ps2_dbg_queue_flushes_frame(void);
extern "C" int ps2_dbg_queue_flushes_total(void);
extern "C" int ps2_dbg_gif_recoveries(void);

#endif
