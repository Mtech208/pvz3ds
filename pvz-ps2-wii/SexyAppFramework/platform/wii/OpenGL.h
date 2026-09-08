#pragma once

#if defined(WII_PLATFORM)
// GL comes from opengx (vendored under src/wii/opengx); gx_wii.h only carries
// the display-integration hooks that own video, the framebuffers and the flip.
#include <GL/gl.h>
#include "wii/gx_wii.h"
#else
#include <glad/glad.h>
#include <SDL.h>
#endif

#ifndef GL_RESCALE_NORMAL
#define GL_RESCALE_NORMAL 0x803A
#endif

// Constants only. These may already exist -- opengx's gl.h pulls in glext.h,
// which defines them -- so they are guarded, but the *functions* below must not
// be: guarding them on the same condition silently dropped the console stubs on
// Wii and left every occlusion-query call unresolved.
#ifndef GL_SAMPLES_PASSED_ARB
#define GL_SAMPLES_PASSED_ARB           0x8914
#endif
#ifndef GL_QUERY_RESULT_ARB
#define GL_QUERY_RESULT_ARB             0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_ARB
#define GL_QUERY_RESULT_AVAILABLE_ARB   0x8867
#endif

#if !defined(WII_PLATFORM)
// SDL_GL_GetProcAddress-based runtime loaders (desktop only).
inline void *getProcEither(const char *arbName, const char *coreName)
{
	void *f = SDL_GL_GetProcAddress(arbName);
	if (!f) f = SDL_GL_GetProcAddress(coreName);
	return f;
}

inline void glGenQueriesARB(GLsizei count, GLuint *queries)
{
	using Function = void (APIENTRYP)(GLsizei, GLuint *);
	static Function function = reinterpret_cast<Function>(getProcEither("glGenQueriesARB", "glGenQueries"));
	if (function) function(count, queries);
	else for (GLsizei i = 0; i < count; ++i) queries[i] = 0;
}

inline void glDeleteQueriesARB(GLsizei count, const GLuint *queries)
{
	using Function = void (APIENTRYP)(GLsizei, const GLuint *);
	static Function function = reinterpret_cast<Function>(getProcEither("glDeleteQueriesARB", "glDeleteQueries"));
	if (function) function(count, queries);
}

inline void glBeginQueryARB(GLenum target, GLuint query)
{
	using Function = void (APIENTRYP)(GLenum, GLuint);
	static Function function = reinterpret_cast<Function>(getProcEither("glBeginQueryARB", "glBeginQuery"));
	if (function) function(target, query);
}

inline void glEndQueryARB(GLenum target)
{
	using Function = void (APIENTRYP)(GLenum);
	static Function function = reinterpret_cast<Function>(getProcEither("glEndQueryARB", "glEndQuery"));
	if (function) function(target);
}

inline void glGetQueryObjectuivARB(GLuint query, GLenum parameter, GLuint *value)
{
	using Function = void (APIENTRYP)(GLuint, GLenum, GLuint *);
	static Function function = reinterpret_cast<Function>(getProcEither("glGetQueryObjectuivARB", "glGetQueryObjectuiv"));
	if (function) function(query, parameter, value);
	// Fail-OPEN: sin soporte de query -> disponible y "paso 1 sample" (visible),
	// nunca 0 (que marcaria todo como ocluido y borraria el mundo).
	else *value = 1u;
}

#else
// Wii has no hardware occlusion query support through opengx. Stubs that
// report always visible so unsupported queries cannot cull the scene.
inline void glGenQueriesARB(GLsizei n, GLuint *q)
{
	for (GLsizei i = 0; i < n; ++i) q[i] = 0;
}
inline void glDeleteQueriesARB(GLsizei, const GLuint *)                        {}
inline void glBeginQueryARB(GLenum, GLuint)                                    {}
inline void glEndQueryARB(GLenum)                                              {}
inline void glGetQueryObjectuivARB(GLuint, GLenum, GLuint *v) { if (v) *v = 1u; }
#endif // desktop vs console
