// gx_wii.cpp — Wii display integration for opengx.
//
// This file used to be a ~1500-line hand-written OpenGL 1.1 implementation over
// GX. It got the port as far as the main menu -- which proved the whole rest of
// the stack (asset loading, matrices, texture upload, framebuffer flip) was
// sound -- and was then replaced by opengx, which implements the same surface
// more completely. See the note in cmake/wii.cmake for the reasoning; the short
// version is display lists: GL specifies that a list records state changes as
// well as geometry, and only opengx does both.
//
// What is left here is the role opengx calls the "display integration library",
// the part SDL or GLUT plays on a desktop. opengx renders; this file decides
// what it renders into and when the result reaches the TV.

#ifdef WII_PLATFORM

#include "wii/gx_wii.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <gccore.h>
#include <malloc.h>

#include <GL/gl.h>
#include <opengx.h>

#include "wii/WiiEarlyInit.h"

// cmake/wii.cmake always defines this; the fallback exists so that compiling
// this file on its own does not silently pick the opposite of the documented
// default. Missing-and-therefore-zero would turn the deflicker filter OFF, which
// is a visible regression on an interlaced TV rather than a harmless one.
#ifndef WII_DEFLICKER
#define WII_DEFLICKER 1
#endif

#ifndef WII_TEXTURE_RGB5A3
#define WII_TEXTURE_RGB5A3 0
#endif

// opengx's log mask, defined in its debug.c. Declared here rather than by
// including the library's internal debug.h, which is not part of its public
// surface; a global variable is not name-mangled, so the C definition links
// against this declaration.
extern "C" unsigned int _ogx_log_mask;

namespace
{

// The GX command FIFO. 256 KB is the usual homebrew size: big enough that the
// CPU rarely stalls waiting for the GP to drain it, small enough not to matter
// against MEM1. Must be 32-byte aligned and uncached.
constexpr unsigned int kFifoSize = 256 * 1024;

void       *g_fifo        = nullptr;
GXRModeObj *g_rmode       = nullptr;
void       *g_xfb[2]      = { nullptr, nullptr };
int         g_activeFb    = 0;
bool        g_widescreen  = false;
bool        g_initialised = false;

} // namespace

int wiigl_width()  { return g_rmode ? g_rmode->fbWidth   : 640; }
int wiigl_height() { return g_rmode ? g_rmode->efbHeight : 480; }

void wiigl_flush_cache(const void *data, unsigned int bytes)
{
	if (data && bytes)
		DCFlushRange(const_cast<void *>(data), bytes);
}

void wiigl_init(bool widescreen)
{
	if (g_initialised) return;
	g_widescreen = widescreen;

	// Video is already up: WiiEarlyInit.cpp brings it and the text console
	// online before main() so early failures are readable. Adopt its render mode
	// and framebuffer as buffer 0 rather than starting over -- calling
	// VIDEO_Init twice and allocating a third framebuffer would waste ~600 KB of
	// MEM1 purely so the diagnostic console could exist.
	g_rmode  = wiiGetRenderMode();
	g_xfb[0] = wiiGetEarlyFramebuffer();

	// Two external framebuffers so the VI can scan one while GX copies into the
	// other; flipping between them is what makes the image tear-free.
	g_xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g_rmode));

	// --- GX ---
	g_fifo = memalign(32, kFifoSize);
	std::memset(g_fifo, 0, kFifoSize);
	GX_Init(g_fifo, kFifoSize);

	GXColor background = { 0, 0, 0, 255 };
	GX_SetCopyClear(background, GX_MAX_Z24);

	GX_SetViewport(0.0f, 0.0f, g_rmode->fbWidth, g_rmode->efbHeight, 0.0f, 1.0f);
	GX_SetDispCopyYScale((f32)g_rmode->xfbHeight / (f32)g_rmode->efbHeight);
	GX_SetScissor(0, 0, g_rmode->fbWidth, g_rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, g_rmode->fbWidth, g_rmode->efbHeight);
	GX_SetDispCopyDst(g_rmode->fbWidth, g_rmode->xfbHeight);
	// The vertical (deflicker) filter runs during every EFB->XFB copy, so it is
	// per-frame copy bandwidth, and it blurs vertically by design.
	//
	// It is ON by default because it earns its cost on the hardware this port
	// targets: a 480i TV flickers badly on high-contrast horizontal edges, and
	// Minecraft's terrain is nothing but high-contrast horizontal edges. Turning
	// it off is a real win in sharpness and a small one in copy bandwidth, but
	// only on a progressive display -- build with -DWII_DEFLICKER=OFF to try it,
	// and judge it on the TV it will actually run on, not in Dolphin.
#if WII_DEFLICKER
	GX_SetCopyFilter(g_rmode->aa, g_rmode->sample_pattern, GX_TRUE, g_rmode->vfilter);
#else
	GX_SetCopyFilter(g_rmode->aa, g_rmode->sample_pattern, GX_FALSE, g_rmode->vfilter);
#endif
	GX_SetFieldMode(g_rmode->field_rendering,
	                ((g_rmode->viHeight == 2 * g_rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

	// 24-bit Z, no alpha in the EFB. This one line is what makes every PS2 depth
	// workaround unnecessary: 24 bits against the GS's 15 signed bits is roughly
	// 500x the depth resolution.
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	// Copy into buffer 1, NOT buffer 0, and leave the display where it is.
	//
	// The EFB has never been written at this point, so this copy moves
	// uninitialised garbage -- aiming it at buffer 0 would splatter that over
	// the boot log the user is still reading (it showed up as a magenta screen
	// with the pre-GX lines missing). GX_TRUE means "clear after copying", so
	// the real purpose of the call is to leave the EFB clean for the first
	// genuine frame; where the garbage lands is irrelevant as long as it is not
	// on screen. The console stays visible until wiigl_end_frame flips to buffer
	// 1 with actual rendered content.
	GX_CopyDisp(g_xfb[1], GX_TRUE);

	// Hand over to opengx now that there is a GX context for it to build on.
	ogx_initialize();

	// The GX display-list arena is deliberately NOT reserved here.
	//
	// _ogx_call_lists_reserve_arena() + _ogx_call_lists_prewarm_pool() are a
	// local addition opengx carries from the Wii Minecraft port this backend
	// was adapted from; their comments still talk about chunk sections, world
	// loads and render distance. There, geometry was recorded into display
	// lists per terrain section, and pre-reserving 12 MB (WII_GXLIST_ARENA_BYTES)
	// kept that churn from shattering the heap.
	//
	// PvZ records no display lists at all -- there is not a single glNewList or
	// glCallList call outside opengx itself -- so every byte of that arena was
	// reserved at boot and never used, against a ~53 MB heap that LOW_MEMORY
	// builds are already fighting for. A NULL arena is a supported state in
	// call_lists.c (it warns and falls back to the heap), so the pooling path
	// stays correct if display lists ever do get used.

	// Turn opengx's own diagnostics on. It normally reads OPENGX_DEBUG from the
	// environment in _ogx_log_init(), which ogx_initialize() calls -- setting that
	// variable first produced no output at all, so rather than keep guessing at
	// why newlib's environment is not being consulted, write the mask directly.
	// It has to happen *after* ogx_initialize(), which would otherwise reset it.
	//
	// Bit 0 is "warning", which fires on things like a texture unit being skipped
	// for missing coordinates -- exactly the class of failure a white quad would
	// be. Bit 1 ("call-lists") is deliberately left clear: the font builds
	// hundreds of lists and would bury everything else.
	//
	// Level 2 and not 1, by the same rule as every other runtime diagnostic.
	// OpenGX rate-limits each warning call site and routes it through wiiLog, so
	// repeated state failures remain visible in the SD log without becoming a
	// per-primitive firehose.
	_ogx_log_mask = (SEXY_LOG_LEVEL >= 2) ? (1u << 0) : 0u;
	ogx_enable_double_buffering(1);

	g_initialised = true;

	wiiLog("[WII][CFG] log=%d ogxWarnings=%d deflicker=%d texture=%s\n",
	       SEXY_LOG_LEVEL, _ogx_log_mask != 0 ? 1 : 0, WII_DEFLICKER,
	       WII_TEXTURE_RGB5A3 ? "RGB5A3" : "RGBA8");

	// aa is logged because it constrains the pixel format: GX only supports
	// anti-aliasing with GX_PF_RGB565_Z16, and we ask for GX_PF_RGB8_Z24 above.
	// If a render mode ever comes back with aa=1 the two disagree and the copy
	// out of the EFB produces wrong colours -- worth ruling out before chasing a
	// tint as a texture-format problem.
	wiiLog("GX+opengx ready: %dx%d (efb %d), %s, aa=%d vf=%d\n",
	       g_rmode->fbWidth, g_rmode->xfbHeight, g_rmode->efbHeight,
	       g_widescreen ? "16:9" : "4:3",
	       (int)g_rmode->aa, (int)g_rmode->vfilter[2]);

	// Last thing in the function, and after that final line rather than before
	// it: the console is still the only thing on screen at this point -- the
	// first rendered frame does not reach the TV until wiigl_end_frame flips to
	// buffer 1 -- so the bring-up summary is still worth painting. From the next
	// caller onwards the console and the renderer share buffer 0, and wiiLog has
	// to stop drawing into it. Every other channel keeps working.
	wiiLogEndBootPhase();
}

void wiigl_begin_frame()
{
	// Nothing to do: opengx tracks its own per-frame state, and the EFB was
	// cleared by the copy that ended the previous frame.
}

void wiigl_end_frame()
{
	if (!g_initialised) return;

	// One-shot bring-up markers. GX_DrawDone() and VIDEO_WaitVSync() both block
	// on hardware finishing; if the first frame never reaches "presented", this
	// says which of the two swallowed it.
	static int s_firstSwapStage = 0;
	if (s_firstSwapStage == 0)
	{
		s_firstSwapStage = 1;
		wiiLog("[WII][SWAP] first end_frame enter\n");
	}

	// opengx may have put the EFB into a special mode (accumulation buffer,
	// selection rendering); this restores it and tells us whether presenting is
	// even meaningful this frame. A negative result means "do not swap".
	if (ogx_prepare_swap_buffers() < 0)
		return;

	g_activeFb ^= 1;

	// Order matters, and it used to be the other way round: GX_DrawDone() then
	// GX_CopyDisp(). That is wrong in both directions.
	//
	// Waiting BEFORE the copy buys nothing. GX_CopyDisp is itself a GP command
	// and executes after the drawing commands already in the FIFO, so the
	// ordering it was protecting is free -- all the wait did was park the CPU
	// until the GP had drained, every frame, with no work overlapping it.
	//
	// Not waiting AFTER the copy is the actual bug. VIDEO_SetNextFramebuffer
	// hands the XFB to the video interface while the GP may still be copying
	// into it. The copy is fast enough that it almost always wins the race
	// before the next retrace, which is exactly what makes this the kind of
	// defect that never shows up in Dolphin and shows up on a TV as an
	// intermittent torn or garbled band across the top of the frame.
	//
	// Copy, then wait for the copy, then present -- the sequence in every libogc
	// example.
	GX_CopyDisp(g_xfb[g_activeFb], GX_TRUE);
	GX_DrawDone();
	if (s_firstSwapStage == 1)
	{
		s_firstSwapStage = 2;
		wiiLog("[WII][SWAP] first GX_DrawDone returned\n");
	}

	VIDEO_SetNextFramebuffer(g_xfb[g_activeFb]);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	if (s_firstSwapStage == 2)
	{
		s_firstSwapStage = 3;
		wiiLog("[WII][SWAP] first frame presented\n");
	}
}

void wiigl_shutdown()
{
	if (!g_initialised) return;
	GX_DrawDone();
	GX_AbortFrame();
	g_initialised = false;
}

#endif // WII_PLATFORM
