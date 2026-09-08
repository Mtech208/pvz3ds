// gx_wii.h — Wii display integration for opengx.
//
// OpenGL itself now comes from opengx (src/wii/opengx, vendored); this file no
// longer declares a single GL entry point. What remains is the role opengx's own
// documentation calls the "display integration library" -- the part SDL or GLUT
// would play on a desktop:
//
//   * own VIDEO_Init, the render mode and the two external framebuffers
//   * own GX_Init, the command FIFO and the EFB->XFB copy configuration
//   * call ogx_initialize() once that is up
//   * drive the frame flip, asking ogx_prepare_swap_buffers() first
//
// This split is why replacing the hand-written GL backend cost one file rather
// than a rewrite: everything above was always integration, not translation.
//
// GL declarations come from <GL/gl.h>, which src/pc/OpenGL.h includes for this
// platform.

#pragma once
#ifdef WII_PLATFORM

#include <cstddef>

#include <GL/gl.h>

// --- VBO entry points ---------------------------------------------------------
// opengx implements these in src/vbo.c, but its gl.h is a GL 1.1-era header and
// declares none of them; the ARB-suffixed aliases the engine actually calls are
// declared only in glext.h, behind GL_GLEXT_PROTOTYPES, and are never
// implemented at all. So: declare the real ones, and forward the ARB spellings.
//
// Note that Tessellator::tryVBO is false on this port -- vertex data is fed to
// GX straight from main memory, where a VBO would only add a copy -- so this is
// a link-time requirement rather than a code path that ever runs.
extern "C" {
void glGenBuffers(GLsizei n, GLuint *buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, std::ptrdiff_t size, const void *data, GLenum usage);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
}

#ifndef GL_ARRAY_BUFFER_ARB
#define GL_ARRAY_BUFFER_ARB 0x8892
#endif
#ifndef GL_STREAM_DRAW_ARB
#define GL_STREAM_DRAW_ARB  0x88E0
#endif

inline void glGenBuffersARB(GLsizei n, GLuint *ids)   { glGenBuffers(n, ids); }
inline void glBindBufferARB(GLenum target, GLuint id) { glBindBuffer(target, id); }
inline void glBufferDataARB(GLenum target, GLsizei size, const void *data, GLenum usage)
{
	glBufferData(target, static_cast<std::ptrdiff_t>(size), data, usage);
}

// gx_wii.cpp owns the whole video pipeline, not just GX: VIDEO_Init, the two
// external framebuffers and the flip all live there, because ending a frame is
// one indivisible sequence (ogx_prepare_swap_buffers -> GX_DrawDone ->
// GX_CopyDisp -> VIDEO_SetNextFramebuffer -> VIDEO_Flush -> VIDEO_WaitVSync)
// and splitting it across files invites tearing. Display_wii.cpp is a thin
// lwjgl-shaped wrapper over these.
void wiigl_init(bool widescreen);
int  wiigl_width();
int  wiigl_height();

void wiigl_begin_frame();
void wiigl_end_frame();
void wiigl_shutdown();

// GX reads vertex data, display lists and textures straight out of main memory
// and never snoops the CPU's data cache. Every buffer the CPU writes and the GPU
// then reads has to be flushed first, or the GPU sees stale bytes -- the classic
// symptom is geometry that flickers or renders as garbage on hardware while
// looking perfect in Dolphin, which is more forgiving about this.
//
// opengx handles this for everything it allocates itself; this stays exported
// for port code that hands GX its own buffers.
void wiigl_flush_cache(const void* data, unsigned int bytes);

#endif // WII_PLATFORM
