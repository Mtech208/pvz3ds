/*****************************************************************************
Copyright (c) 2011  David Guillen Fandos (david@davidgf.net)
Copyright (c) 2024  Alberto Mardegan (mardy@users.sourceforge.net)
All rights reserved.

Attention! Contains pieces of code from others such as Mesa and GRRLib

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of copyright holders nor the names of its
   contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL COPYRIGHT HOLDERS OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/

#ifndef OPENGX_CALL_LISTS_H
#define OPENGX_CALL_LISTS_H

#include "state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Except when specified otherwise, the enum name matches the GL function
 * (e.g., COMMAND_ENABLE == glEnable)
 */
typedef enum {
    COMMAND_NONE, /* The command entry is unused, it means we reached the end
                     of the call list */
    COMMAND_DRAW_ARRAYS,
    COMMAND_DRAW_ELEMENTS,
    COMMAND_CALL_LIST,
    COMMAND_ENABLE,
    COMMAND_DISABLE,
    COMMAND_LIGHT,
    COMMAND_MATERIAL,
    COMMAND_BLEND_FUNC,
    COMMAND_BIND_TEXTURE,
    COMMAND_TEX_ENV,
    COMMAND_LOAD_IDENTITY,
    COMMAND_PUSH_MATRIX,
    COMMAND_POP_MATRIX,
    COMMAND_MULT_MATRIX,
    COMMAND_TRANSLATE,
    COMMAND_ROTATE,
    COMMAND_SCALE,
    COMMAND_FRONT_FACE,
    COMMAND_COLOR,
    COMMAND_NORMAL,
} CommandType;

#define HANDLE_CALL_LIST(operation, ...) \
    if (glparamstate.current_call_list.index >= 0 && \
        glparamstate.current_call_list.execution_depth == 0) { \
        bool proceed = _ogx_call_list_append(COMMAND_##operation, ##__VA_ARGS__); \
        if (!proceed) return; \
    }

bool _ogx_call_list_append(CommandType op, ...);

/* LOCAL MODIFICATION (BetaPlusPlus Wii port): clear the per-frame draw-sync
 * bookkeeping. Call wherever the draw-sync token counter is restarted. */
void _ogx_call_lists_reset_sync(void);

/* Reserve the Wii-only GX display-list arena while the general heap is still
 * contiguous.  Safe to call more than once. */
void _ogx_call_lists_reserve_arena(void);

/* Seed the size-class pool from the arena so the first world load of a
 * session finds it already stocked, instead of only a reload benefiting from
 * sections a previous world freed. Call once, right after
 * _ogx_call_lists_reserve_arena() and before any world/list activity. */
void _ogx_call_lists_prewarm_pool(void);

/* Bytes/count of recorded draws that missed the pool: bypassBytes/Count is
 * lists above the pool's largest size class (never eligible for pooling);
 * overflowBytes/Count is lists that fit a class but found neither a free
 * pooled block nor arena room, and fell back to a one-off memalign(). Either
 * counter climbing during play, together with sbrk eating into MEM2, is the
 * signature of the arena budget being too small for the current working set. */
void _ogx_call_lists_overflow_stats(u32 *bypassBytes, u32 *bypassCount,
                                     u32 *overflowBytes, u32 *overflowCount);

/* Bytes permanently claimed from the fixed GX-list arena, and its capacity.
 * Used includes both live lists and cached blocks because arena allocations are
 * intentionally never returned to the general heap. */
void _ogx_call_lists_arena_memory(u32 *usedBytes, u32 *capacityBytes);

/* Release heap-backed cached lists after a world unload.  Arena blocks remain:
 * that fixed allocation also owns persistent GUI/font display lists. */
void _ogx_call_lists_trim_pool(void);

/* Drop the recorded commands for a list while retaining its GL name.  This is
 * intentionally different from glDeleteLists(): WorldRenderer owns a fixed
 * name range for its lifetime, so returning the name to glGenLists() would let
 * an unrelated renderer collide with it. */
void _ogx_call_list_clear_contents(unsigned int list);

#ifdef __cplusplus
} // extern C
#endif

#endif /* OPENGX_CALL_LISTS_H */
