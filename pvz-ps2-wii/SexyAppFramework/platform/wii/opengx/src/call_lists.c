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

#include "call_lists.h"
#include "debug.h"
#include "efb.h"
#include "gpu_resources.h"
#include "opengx.h"
#include "stencil.h"
#include "utils.h"

#include <GL/gl.h>
#include <assert.h>
#include <stdint.h>
#include <malloc.h>
#include <ogc/system.h> /* SYS_GetArena*Size(), for report_heap() */
#include <stdarg.h>
#include <stdlib.h>

#define MAX_COMMANDS_PER_BUFFER 4
/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Upstream value: 1536.
 *
 * RenderGlobal reserves maxWorldRenderers * 3 display lists in a single
 * glGenLists() call: (400/16+1) * 8 * (400/16+1) * 3 == 16224. Anything below
 * that makes glGenLists return 0, which the engine treats as fatal
 * ("OpenGL could not allocate display lists") before the world ever loads.
 *
 * The table is cheap to grow: CallList is a single pointer, so this is 4 bytes
 * per entry -- 128 KB of BSS at 32768, against 6 KB at the upstream value.
 * Lists that are never recorded into cost nothing beyond that pointer.
 * ------------------------------------------------------------------------ */
#define MAX_CALL_LISTS 32768
/* MAX_GXLIST_SIZE (1 MB, "the glut teapot can take more than 300KB") is gone:
 * queue_draw_geometry() now sizes each record buffer from the draw it is about
 * to record. See the LOCAL MODIFICATION note there. */
#define CALL_LIST_START_ID 1

typedef struct
{
    CommandType type;
    union {
        GLuint gllist; // glCallList

        GLenum cap; // glEnable, glDisable

        GLenum mode; // glBegin, glFrontFace

        struct LightParams {
            uint16_t light;
            uint16_t pname;
            GLfloat params[4];
        } light;

        struct MaterialParams {
            uint16_t face;
            uint16_t pname;
            GLfloat params[4];
        } material;

        struct {
            GLenum sfactor;
            GLenum dfactor;
        } blend_func;

        struct BoundTexture {
            GLenum target;
            GLuint texture;
        } bound_texture;

        struct TexEnv {
            GLenum target;
            GLenum pname;
            GLint param;
        } tex_env;

        struct {
            GLfloat x;
            GLfloat y;
            GLfloat z;
        } xyz;

        struct {
            GLfloat angle;
            GLfloat x;
            GLfloat y;
            GLfloat z;
        } rotate;

        struct DrawGeometry {
            GLenum mode;
            /* LOCAL MODIFICATION (BetaPlusPlus Wii port): was uint16_t, which
             * silently truncated any recorded draw above 65535 vertices -- a
             * chunk section reaches that. The GX_Begin() calls below are split
             * at the u16 limit instead; see utils.h. */
            uint32_t count;
            union client_state cs;
            u32 list_size;
            /* LOCAL MODIFICATION (BetaPlusPlus Wii port): bytes actually held by
             * gxlist, which is NOT list_size whenever the shrink below failed and
             * the full-capacity buffer was kept. Tracked so the accounting in
             * _ogx_call_lists_memory() balances instead of drifting upward. */
            u32 alloc_size;
            /* LOCAL MODIFICATION (BetaPlusPlus Wii port): whether gxlist came from
             * (and must be returned to) the size-class pool below, rather than
             * being a one-off memalign(). */
            bool pooled;
            void *gxlist;
            struct AttribFormat {
                unsigned attribute : 5;
                /* Most of these only require 2-3 bits, but let's round it */
                unsigned inputmode : 3;
                unsigned comptype : 4;
                unsigned compsize : 4;
            } formats[4 + MAX_TEXTURE_UNITS]; /* 4: pos, norm, clr1 and clr2 */
            #define CALL_LIST_DRAW_FORMATS(fmt) (sizeof(fmt) / sizeof(fmt[0]))
        } draw_geometry;

        float color[4];

        float normal[3];

        float matrix[16];
    } c;
} Command;

typedef struct CommandBuffer
{
    Command commands[MAX_COMMANDS_PER_BUFFER];
    struct CommandBuffer *next;
} CommandBuffer;

typedef struct
{
    CommandBuffer *head;
} CallList;

static CallList call_lists[MAX_CALL_LISTS];
static GXColor s_current_color;
static float s_current_normal[3];
static bool s_last_draw_used_indexed_data = false;
static uint16_t s_last_draw_sync_token = 0;
static union client_state s_last_client_state;
static bool s_last_client_state_is_valid = false;

/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Cache de setup_draw_geometry entre draws consecutivos con mismo formato.
 *
 * NOTA (fix del GFX FIFO desync, ver historial): la primera versión de esta
 * cache comparaba un hash de 32 bits (multiplicativo, x31) de dg->formats en
 * vez del array completo. Con pocos bits de entropía por campo (attribute,
 * inputmode, comptype, compsize son todos bitfields chicos) una colisión de
 * hash entre dos formatos *distintos* es perfectamente posible, y en ese caso
 * setup_draw_geometry() no se volvía a llamar aunque el descriptor de vertice
 * realmente necesitado hubiera cambiado -- el GP terminaba parseando el
 * siguiente display list con el layout equivocado, exactamente el mismo
 * mecanismo de desync que el fix de CLR1 de mas arriba. dg->formats pesa
 * apenas ~48 bytes (4+MAX_TEXTURE_UNITS entradas de 4 bytes), asi que
 * comparar el array entero con memcmp() sale gratis frente al costo de mandar
 * un display list entero por FIFO, y elimina la colision como sospechoso.
 * ------------------------------------------------------------------------ */
static struct {
    struct AttribFormat formats[4 + MAX_TEXTURE_UNITS];
    bool valid;
} s_cached_setup = {0};

/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Throttle the out-of-memory diagnostics below.
 *
 * Recording is all-or-nothing per draw, so once the heap is full *every*
 * recorded draw fails, and this port records hundreds of them per world load
 * (one per chunk section, 169 for the sky dome alone). On this target
 * warning() is a formatted write that leaves the console over EXI and costs
 * milliseconds apiece, so reporting each failure turns a memory problem into
 * an unresponsive machine and buries the first -- and most informative --
 * failure under thousands of identical lines.
 *
 * Report the first few, then one in every 512, always with the running total
 * so the scale of the shortfall is still visible.
 * ------------------------------------------------------------------------ */
static unsigned s_record_oom_count = 0;

static bool should_report_oom(void)
{
    unsigned n = s_record_oom_count++;
    return n < 4 || (n % 512) == 0;
}

/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Say what the heap actually looks like when a record fails.
 *
 * "Out of memory" on its own cannot separate the three cases that matter, and
 * this port has already spent sessions on the wrong one:
 *
 *   in use high, free low     -> genuinely exhausted, cut what is allocated
 *   in use low,  free high    -> fragmentation, the request cannot be placed
 *   total small               -> the arena is not the one we think it is
 *
 * The last of those was real (the heap was MEM1-only, ~8 MB; see the
 * MALLOC_MEM2 note in src/wii/WiiEarlyInit.cpp), and no amount of staring at
 * "Out of memory" could have distinguished it from the first. Printed once per
 * reported failure, which should_report_oom() already keeps rare.
 *
 * mi.arena and mi.uordblks are deliberately NOT printed: they are fiction on
 * this target. With MALLOC_MEM2=1 libogc's _sbrk_r hands out the MEM1 leftovers
 * first and only switches to Arena2 once a request no longer fits, so the heap
 * is built from two regions ~230 MB apart in the address space. newlib's
 * malloc_extend_top() accounts for a discontiguous sbrk with
 * "sbrked_mem += brk - old_end", which folds that entire address hole into
 * sbrked_mem -- and mi.arena *is* sbrked_mem, with mi.uordblks derived from it
 * as (arena - fordblks). An earlier version of this function printed both, and
 * a 24+64 MB console duly reported "300106 KB total, 289221 KB used".
 *
 * What is left is measured by walking the bins, so it is real: fordblks/ordblks
 * give the free total and how many pieces it is in (their ratio is what
 * separates "exhausted" from "fragmented"), keepcost is the top chunk, and the
 * two arena sizes say whether sbrk can still grow the heap at all.
 * ------------------------------------------------------------------------ */
/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Account for the memory the recorded display lists hold.
 *
 * report_heap() says how much of the heap is gone; it cannot say to WHOM. On
 * this port the answer was strongly suspected to be these buffers -- Minecraft
 * records one GX display list per chunk section, and at SHORT render distance
 * RenderGlobal builds 9 x 8 x 9 = 648 of them -- but "strongly suspected" is
 * how the PS2 port lost sessions to the wrong hypothesis, so it is measured.
 *
 * Counted as currently-allocated bytes, so this is a live total and not a
 * high-water mark: it goes down when glNewList re-records a section into a
 * smaller list, or when glDeleteLists drops one.
 * ------------------------------------------------------------------------ */
static u32 s_list_bytes = 0;
static u32 s_list_count = 0;

/* Do not let recorded geometry consume the whole Wii heap.  Unlike a failed
 * malloc at the top level, this is recoverable: WorldRenderer keeps the
 * section dirty and tries it again after moving has released old sections.
 * The definition comes from cmake/wii.cmake so this remains a Wii policy, not
 * an upstream OpenGX behaviour change. */
#ifndef WII_GXLIST_MAX_BYTES
#define WII_GXLIST_MAX_BYTES (11u * 1024u * 1024u)
#endif
#ifndef WII_GXLIST_ARENA_BYTES
#define WII_GXLIST_ARENA_BYTES (12u * 1024u * 1024u)
#endif
static bool s_list_budget_blocked = false;

bool _ogx_call_lists_budget_blocked(void)
{
    return s_list_budget_blocked;
}

void _ogx_call_lists_memory(u32 *bytes, u32 *lists)
{
    if (bytes) *bytes = s_list_bytes;
    if (lists) *lists = s_list_count;
}

/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Count the two ways a recorded draw can miss the pool/arena and fall back
 * to a one-off memalign(), which is exactly the churn the pool exists to
 * avoid (see the "shatters the heap" comment below).
 *
 * These were the missing piece in diagnosing the cold-load slowdown: the RAM
 * report already showed gxlists= average size climbing and MEM2 sbrk
 * collapsing on a first load vs. a reload, but not *why* -- whether draws
 * were simply bigger, or whether the pool had stopped being able to serve
 * them. Now it says which:
 *
 *   bypass  -- the draw's buffer is above GXLIST_POOL_CEIL_SHIFT (256 KB) and
 *              was never eligible for pooling in the first place. A high
 *              count here means some chunk sections are recording unusually
 *              large lists (dense terrain, a big cave system), not a pool
 *              problem.
 *   overflow-- the draw's buffer fits a pool class, but neither that class's
 *              free list nor the arena's remaining budget could serve it, so
 *              it fell back to the general heap. A high count here, together
 *              with MEM2 sbrk shrinking, means WII_GXLIST_ARENA_BYTES is too
 *              small for the concurrent working set at the current render
 *              distance -- this is the case a cold first load hits, because
 *              nothing has been freed back into the pool yet to reuse.
 * ------------------------------------------------------------------------ */
static u32 s_bypass_bytes = 0;
static u32 s_bypass_count = 0;
static u32 s_overflow_bytes = 0;
static u32 s_overflow_count = 0;

void _ogx_call_lists_overflow_stats(u32 *bypassBytes, u32 *bypassCount,
                                     u32 *overflowBytes, u32 *overflowCount)
{
    if (bypassBytes)   *bypassBytes   = s_bypass_bytes;
    if (bypassCount)   *bypassCount   = s_bypass_count;
    if (overflowBytes) *overflowBytes = s_overflow_bytes;
    if (overflowCount) *overflowCount = s_overflow_count;
}

/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Pool of reusable GX display-list buffers, one free list per power-of-two
 * size class from 256 B to 256 KB.
 *
 * queue_draw_geometry() used to memalign() a differently-sized buffer for
 * every recorded draw and then, once GX_EndDispList() reported the true
 * size, memalign() a second, exactly-sized buffer and free() the first --
 * and every chunk-section rebuild repeats this at a new size as the section
 * changes. Over a play session that shatters the heap into thousands of
 * small, differently-sized free blocks (measured: 10884 KB free split across
 * 1715 blocks, mean 6.5 KB, with both sbrk arenas already exhausted -- full
 * AND fragmented at once). Rounding the allocation up to a size class and
 * recycling same-class blocks instead of returning them to the general
 * allocator converts that churn into reuse of a small, bounded set of block
 * sizes.
 *
 * list_size (the true GX_EndDispList() length used at replay, via
 * GX_CallDispList(dg->gxlist, dg->list_size)) already exists separately from
 * the buffer's allocation size, so a pooled buffer is never shrunk to fit --
 * the slack between the two is simply never touched at replay time.
 * Capacities above the largest class bypass the pool and keep the original
 * memalign/shrink/free behaviour, so one huge outlier list cannot force a
 * huge class or waste pool budget.
 *
 * Bounded by GXLIST_POOL_MAX_CACHED_BYTES: past that budget a released block
 * is freed for real instead of cached, so a shift in usage pattern (e.g. a
 * render-distance change) cannot pin memory forever. One global byte budget
 * rather than a per-class cap: with 11 classes, a per-class count cap large
 * enough to matter would total far more memory than the byte budget allows
 * if every class filled up at once, so the byte budget is the real limiter
 * either way.
 * ------------------------------------------------------------------------ */
#define GXLIST_POOL_FLOOR_SHIFT       8u  /* 256 B */
#define GXLIST_POOL_CEIL_SHIFT        18u /* 256 KB */
#define GXLIST_POOL_NUM_CLASSES       (GXLIST_POOL_CEIL_SHIFT - GXLIST_POOL_FLOOR_SHIFT + 1u)
/*
 * One megabyte only covered a few released terrain sections.  Crossing a
 * chunk boundary releases and rebuilds many more than that, so the overflow
 * kept returning differently-sized buffers to newlib and immediately asking
 * it for replacements.  That is exactly the allocation pattern behind a heap
 * with tens of MB free but no block larger than a few KB.  Four MB is bounded
 * (and remains visible in the Wii RAM report), but is enough to retain a full
 * handoff of the common 32--256 KB section buffers between renderer windows.
 */
#define GXLIST_POOL_MAX_CACHED_BYTES  (4u * 1024u * 1024u)

typedef struct GxListPoolBlock {
    struct GxListPoolBlock *next;
} GxListPoolBlock;

static GxListPoolBlock *s_pool_free[GXLIST_POOL_NUM_CLASSES];
static u32 s_pool_cached_bytes = 0;
static u32 s_pool_cached_blocks = 0;

/*
 * Display lists are the largest, most frequently replaced allocations in a
 * loaded world.  Giving them a contiguous reservation before assets and chunk
 * I/O start means their lifetime never punches holes in newlib's general heap.
 * The existing size-class lists remain the allocator inside this reservation;
 * a block returned from a deleted renderer is reused by the next renderer.
 */
static void *s_gxlist_arena = NULL;
static u32 s_gxlist_arena_used = 0;

void _ogx_call_lists_arena_memory(u32 *used_bytes, u32 *capacity_bytes)
{
    if (used_bytes) *used_bytes = s_gxlist_arena_used;
    if (capacity_bytes)
        *capacity_bytes = s_gxlist_arena != NULL ? WII_GXLIST_ARENA_BYTES : 0;
}

void _ogx_call_lists_reserve_arena(void)
{
    if (s_gxlist_arena != NULL)
        return;
    s_gxlist_arena = memalign(32, WII_GXLIST_ARENA_BYTES);
    if (s_gxlist_arena == NULL) {
        warning("Could not reserve %uKB GX display-list arena; using heap fallback",
                WII_GXLIST_ARENA_BYTES / 1024u);
    }
}

static bool gxlist_from_arena(const void *ptr)
{
    if (s_gxlist_arena == NULL || ptr == NULL)
        return false;
    const uintptr_t p = (uintptr_t)ptr;
    const uintptr_t begin = (uintptr_t)s_gxlist_arena;
    return p >= begin && p < begin + WII_GXLIST_ARENA_BYTES;
}

/* Chunk meshes are cached while a world is active to avoid heap churn during
 * renderer rebuilds.  Once the world is gone, release the heap-backed part of
 * that cache.  The arena is shared with persistent UI lists, so its blocks are
 * kept and remain available for the next world. */
void _ogx_call_lists_trim_pool(void)
{
    u32 retained_bytes = 0;
    u32 retained_blocks = 0;

    for (unsigned int i = 0; i < GXLIST_POOL_NUM_CLASSES; ++i) {
        const u32 class_size = 1u << (GXLIST_POOL_FLOOR_SHIFT + i);
        GxListPoolBlock *kept = NULL;
        GxListPoolBlock *block = s_pool_free[i];

        while (block != NULL) {
            GxListPoolBlock *next = block->next;
            if (gxlist_from_arena(block)) {
                block->next = kept;
                kept = block;
                retained_bytes += class_size;
                retained_blocks++;
            } else {
                free(block);
            }
            block = next;
        }
        s_pool_free[i] = kept;
    }

    s_pool_cached_bytes = retained_bytes;
    s_pool_cached_blocks = retained_blocks;
}

void _ogx_call_lists_pool_memory(u32 *cached_bytes, u32 *cached_blocks)
{
    if (cached_bytes) *cached_bytes = s_pool_cached_bytes;
    if (cached_blocks) *cached_blocks = s_pool_cached_blocks;
}

/* Forward declaration: defined below, alongside gxlist_pool_round_up() which
 * it wraps. _ogx_call_lists_prewarm_pool() needs it and sits up here with
 * the rest of the pool's public accounting functions rather than down among
 * the acquire/release internals. */
static int gxlist_pool_class_index(u32 class_size);

/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Seed the size-class pool from the arena at boot, before any world has ever
 * loaded.
 *
 * Without this, the *first* world load of a session starts with every
 * s_pool_free[] list empty. That is not slow by itself -- gxlist_acquire()
 * still carves fresh space straight out of the arena, a cheap bump
 * allocation, not memalign()+free() -- but it means the arena's fixed
 * WII_GXLIST_ARENA_BYTES budget is being spent for the very first time while
 * gameplay is already running, exactly when a chunk-generation/relighting
 * burst is also asking for the most concurrent sections. Once that budget
 * is exhausted, every further request this session falls back to memalign()
 * on the general heap instead (see s_overflow_bytes/count above) -- and that
 * is the allocate/shrink/free churn that fragments the heap.
 *
 * A *second* load in the same session never pays this: exiting the first
 * world frees its sections back into the pool for free, so the second load
 * finds classes already stocked. This function gives the FIRST load that
 * same head start, by seeding the classes that dominate a real chunk
 * section's size in practice -- measured directly off userdata/log.txt's
 * gxlists= samples (a first, unwarmed session climbs from ~2 KB average
 * to 42-48 KB as sections round up to the 16/32/64 KB classes; a warmed
 * reload stays around 17-21 KB). Revisit this table if render distance or
 * terrain rendering changes shift that distribution.
 *
 * Deliberately modest: the total below is kept under
 * GXLIST_POOL_MAX_CACHED_BYTES (the same budget gxlist_release() enforces at
 * runtime) so this cannot itself starve a first load's own sections of
 * arena space -- it only removes the "nobody has freed anything yet"
 * penalty, it doesn't pre-claim the whole arena.
 *
 * Must run before anything else has touched the arena (s_gxlist_arena_used
 * == 0); call once, right after _ogx_call_lists_reserve_arena().
 * ------------------------------------------------------------------------ */
void _ogx_call_lists_prewarm_pool(void)
{
    if (s_gxlist_arena == NULL || s_gxlist_arena_used != 0)
        return; /* no arena, or already claimed by real use: leave it alone */

    static const struct { u32 class_shift; u32 count; } kSeedClasses[] = {
        { 14u, 24u }, /*  16 KB x 24 =  384 KB */
        { 15u, 48u }, /*  32 KB x 48 = 1536 KB */
        { 16u, 16u }, /*  64 KB x 16 = 1024 KB */
    };

    for (unsigned s = 0; s < sizeof(kSeedClasses) / sizeof(kSeedClasses[0]); s++)
    {
        const u32 class_size = 1u << kSeedClasses[s].class_shift;
        const int index = gxlist_pool_class_index(class_size);

        for (u32 n = 0; n < kSeedClasses[s].count; n++)
        {
            if (s_gxlist_arena_used + class_size > WII_GXLIST_ARENA_BYTES)
                return; /* arena budget reached; stop rather than overrun it */
            if (s_pool_cached_bytes + class_size > GXLIST_POOL_MAX_CACHED_BYTES)
                return; /* stay under the same cap gxlist_release() enforces */

            GxListPoolBlock *block =
                (GxListPoolBlock *)((unsigned char *)s_gxlist_arena + s_gxlist_arena_used);
            s_gxlist_arena_used += class_size;

            block->next = s_pool_free[index];
            s_pool_free[index] = block;
            s_pool_cached_bytes += class_size;
            s_pool_cached_blocks++;
        }
    }
}

/* Rounds capacity up to a pool size class. Returns 0 if capacity is above the
 * largest class -- the caller must then bypass the pool. */
static u32 gxlist_pool_round_up(u32 capacity)
{
    u32 class_size = 1u << GXLIST_POOL_FLOOR_SHIFT;
    while (class_size < capacity) {
        if (class_size == (1u << GXLIST_POOL_CEIL_SHIFT)) return 0;
        class_size <<= 1;
    }
    return class_size;
}

/* class_size must be a value gxlist_pool_round_up() actually returned. */
static int gxlist_pool_class_index(u32 class_size)
{
    int index = 0;
    while (class_size > (1u << GXLIST_POOL_FLOOR_SHIFT)) {
        class_size >>= 1;
        index++;
    }
    return index;
}

/* Allocates a 32-byte-aligned buffer of at least `capacity` bytes, from the
 * pool when possible. Sets *out_alloc_size to the buffer's real size and
 * *out_pooled to whether it must be returned via gxlist_release() rather than
 * plain free(). Returns NULL on OOM, exactly like memalign(). */
static void *gxlist_acquire(u32 capacity, u32 *out_alloc_size, bool *out_pooled)
{
    u32 class_size = gxlist_pool_round_up(capacity);
    u32 allocation_size = class_size ? class_size : capacity;

    if (s_list_bytes + allocation_size > WII_GXLIST_MAX_BYTES) {
        s_list_budget_blocked = true;
        *out_alloc_size = allocation_size;
        *out_pooled = (class_size != 0);
        return NULL;
    }

    if (class_size == 0) {
        /* Above the largest class: bypass the pool entirely, as today. */
        *out_alloc_size = capacity;
        *out_pooled = false;
        s_bypass_bytes += capacity;
        s_bypass_count++;
        return memalign(32, capacity);
    }

    int index = gxlist_pool_class_index(class_size);
    GxListPoolBlock *block = s_pool_free[index];
    if (block) {
        s_pool_free[index] = block->next;
        s_pool_cached_bytes -= class_size;
        s_pool_cached_blocks--;
    }

    *out_alloc_size = class_size;
    *out_pooled = true;
    if (block)
        return (void *)block;

    if (s_gxlist_arena != NULL &&
        s_gxlist_arena_used + class_size <= WII_GXLIST_ARENA_BYTES) {
        void *result = (unsigned char *)s_gxlist_arena + s_gxlist_arena_used;
        s_gxlist_arena_used += class_size;
        return result;
    }

    /* The fixed arena can be exhausted by an unusual scene.  Falling back is
     * still correct; the live-list budget above keeps this path bounded. */
    s_overflow_bytes += class_size;
    s_overflow_count++;
    return memalign(32, class_size);
}

/* Returns a buffer obtained from gxlist_acquire() (or NULL, a no-op). Caches
 * it for reuse when it was pooled and doing so stays under the byte budget;
 * otherwise frees it for real, exactly like the plain free() this replaces. */
static void gxlist_release(void *ptr, u32 alloc_size, bool pooled)
{
    if (!ptr) return;
    if (pooled && (gxlist_from_arena(ptr) ||
                   s_pool_cached_bytes + alloc_size <= GXLIST_POOL_MAX_CACHED_BYTES)) {
        int index = gxlist_pool_class_index(alloc_size);
        GxListPoolBlock *block = (GxListPoolBlock *)ptr;
        block->next = s_pool_free[index];
        s_pool_free[index] = block;
        s_pool_cached_bytes += alloc_size;
        s_pool_cached_blocks++;
        return;
    }
    /* An arena block is never individually freeable.  It should have taken the
     * branch above; keep this guard so a bookkeeping failure cannot pass an
     * interior pointer to free(). */
    if (!gxlist_from_arena(ptr))
        free(ptr);
}

static void report_heap(void)
{
    struct mallinfo mi = mallinfo();
    unsigned free_kb = (unsigned)mi.fordblks / 1024u;
    warning("  heap: %u KB free in %d blocks (avg %u KB, top chunk %u KB); "
            "sbrk MEM2 left %u KB; MEM1 non-heap %u KB",
            free_kb,
            mi.ordblks,
            mi.ordblks > 0 ? free_kb / (unsigned)mi.ordblks : 0u,
            (unsigned)mi.keepcost / 1024u,
            (unsigned)SYS_GetArena2Size() / 1024u,
            (unsigned)SYS_GetArena1Size() / 1024u);
}

#define BUFFER_IS_VALID(buffer) (((uint32_t)buffer) > 1)
#define LIST_IS_USED(index) BUFFER_IS_VALID(call_lists[index].head)
#define LIST_IS_RESERVED_OR_USED(index) (call_lists[index].head != NULL)
#define LIST_RESERVE(index) call_lists[index].head = (void*)1
#define LIST_UNRESERVE(index) call_lists[index].head = NULL

static inline int last_command(CommandBuffer **buffer)
{
    CommandBuffer *next;

    while (BUFFER_IS_VALID(*buffer)) {
        next = (*buffer)->next;
        if (!next) {
            for (int i = 0; i < MAX_COMMANDS_PER_BUFFER; i++) {
                Command *curr = &(*buffer)->commands[i];
                if (curr->type == COMMAND_NONE) {
                    return i - 1;
                }
            }
            return MAX_COMMANDS_PER_BUFFER - 1;
        }

        *buffer = next;
    }

    return -1;
}

static Command *new_command(CommandBuffer **head)
{
    int last_index = -1;
    CommandBuffer *last_buffer = NULL;

    if (BUFFER_IS_VALID(*head)) {
        last_buffer = *head;
        last_index = last_command(&last_buffer);
    }

    if (last_index < 0 || last_index == MAX_COMMANDS_PER_BUFFER - 1) {
        CommandBuffer *new_buffer = malloc(sizeof(CommandBuffer));
        if (!new_buffer) {
            if (should_report_oom()) {
                warning("Out of memory for a call-list buffer (%u failures "
                        "so far)", s_record_oom_count);
                report_heap();
            }
            return NULL;
        }

        for (int i = 0; i < MAX_COMMANDS_PER_BUFFER; i++) {
            new_buffer->commands[i].type = COMMAND_NONE;
        }
        new_buffer->next = NULL;

        if (last_buffer) {
            last_buffer->next = new_buffer;
        } else {
            *head = new_buffer;
        }
        return &new_buffer->commands[0];
    } else {
        return &last_buffer->commands[last_index + 1];
    }
}

static void setup_draw_geometry(struct DrawGeometry *dg,
                                bool uses_indexed_data)
{
    u8 vtxindex = GX_VTXFMT0;

    /* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) ----------------------
     * Read current_color before the branch below, not inside it.
     *
     * Upstream declares it uninitialised here and only assigns it under
     * "uses_indexed_data && s_last_draw_used_indexed_data", yet stores it into
     * s_current_color unconditionally further down -- so the first list-driven
     * draw that supplies no colour array (s_last_draw_used_indexed_data is
     * still false at that point) hands GX whatever was on the stack as the
     * vertex colour for every one of its vertices.
     * -------------------------------------------------------------------- */
    GXColor current_color = gxcol_new_fv(glparamstate.imm_mode.current_color);

    if (uses_indexed_data && s_last_draw_used_indexed_data) {
        bool data_changed = false;
        /* If the indexed data has changed, we need to wait until the previous
         * list has completed its execution, because changing the data under
         * its feet will cause rendering issues. */
        if (!dg->cs.color_enabled) {
            if (!gxcol_equal(current_color, s_current_color)) data_changed = true;
        }

        if (!dg->cs.normal_enabled) {
            if (memcmp(s_current_normal, glparamstate.imm_mode.current_normal,
                       sizeof(s_current_normal)) != 0)
                data_changed = true;
        }

        if (data_changed) {
            /* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------
             * Compare as a signed difference rather than with `<`.
             *
             * GX_GetDrawSync() returns a u16 that wraps, and _ogx_draw_sync_token
             * is restarted from 0 once per frame (ogx_prepare_swap_buffers).
             * A plain `<` against a token from a previous counter epoch waits
             * for a value the GP will never report, and because the CPU is
             * blocked here it can never issue the tokens that would get there
             * either -- an unrecoverable hang, which is what a world load
             * produced: the frame froze mid-render with the GP idle.
             *
             * The stale-epoch case is handled properly by
             * _ogx_call_lists_reset_sync() below; this makes the comparison
             * itself correct across the u16 wrap on top of that.
             * ------------------------------------------------------------ */
            while ((int16_t)(GX_GetDrawSync() - s_last_draw_sync_token) < 0)
                ;
        }
    }

    OgxDrawMode gxmode = _ogx_draw_mode(dg->mode);
    OgxDrawData draw_data = {
        gxmode,
        dg->count,
        /* The remaining fields are not used when drawing through lists */
    };
    /* Setup the same vertex attribute descriptions that were in place when the
     * list was created */
    GX_ClearVtxDesc();
    for (int i = 0; i < CALL_LIST_DRAW_FORMATS(dg->formats); i++) {
        if (dg->formats[i].inputmode == GX_NONE) continue;
        uint8_t attribute = dg->formats[i].attribute;
        GX_SetVtxDesc(attribute, dg->formats[i].inputmode);
        GX_SetVtxAttrFmt(GX_VTXFMT0, attribute,
                         dg->formats[i].comptype, dg->formats[i].compsize, 0);

        /* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) ------------------
         * Declare CLR1 whenever CLR0 comes from an array.
         *
         * queue_draw_geometry() writes the colour element *twice* per vertex,
         * to feed GX's two colour channels ("the color data is duplicated to
         * CLR0 and CLR1"). The indexed branch below declares both, so the
         * no-colour-array case matches. The array case did not: dg->formats[]
         * never holds a CLR1 entry -- count_attributes() emits one reader per
         * colour array, and the reader loop deliberately ignores CLR1 -- so
         * the descriptor promised the GP one colour per vertex while the list
         * carried two.
         *
         * Four surplus bytes per vertex is an immediate FIFO desync: the GP
         * runs off the end of each vertex and reads the next one's bytes as
         * commands. It only bites where a recorded draw supplies per-vertex
         * colours, which in this game means chunk meshes (block lighting) --
         * hence a menu that renders perfectly and a "GFX FIFO: unknown opcode"
         * the moment a world starts building sections.
         * ---------------------------------------------------------------- */
        if (attribute == GX_VA_CLR0) {
            GX_SetVtxDesc(GX_VA_CLR1, dg->formats[i].inputmode);
            GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1,
                             dg->formats[i].comptype, dg->formats[i].compsize, 0);
        }
    }

    if (!dg->cs.normal_enabled) {
        GX_SetVtxDesc(GX_VA_NRM, GX_INDEX8);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        GX_SetArray(GX_VA_NRM, s_current_normal, 12);
        floatcpy(s_current_normal, glparamstate.imm_mode.current_normal, 3);
        /* Not needed on Dolphin, but it is on a Wii */
        DCStoreRange(s_current_normal, 12);
    }
    if (!dg->cs.color_enabled) {
        GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
        GX_SetVtxDesc(GX_VA_CLR1, GX_INDEX8);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGB8, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGB8, 0);
        s_current_color = current_color;
        GX_SetArray(GX_VA_CLR0, &s_current_color, 4);
        GX_SetArray(GX_VA_CLR1, &s_current_color, 4);
        DCStoreRange(&s_current_color, 4);
    }

    /* It makes no sense to use a fixed texture coordinates for all vertices,
     * so we won't add them unless they are enabled. */

    GX_InvVtxCache();
}

static void execute_draw_geometry_list(struct DrawGeometry *dg)
{
    bool uses_indexed_data = !dg->cs.normal_enabled || !dg->cs.color_enabled;

    /* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) ----------------------
     * Cache de setup_draw_geometry entre draws consecutivos con mismo formato.
     * --------------------------------------------------------------------- */
    if (!uses_indexed_data) {
        /* setup_draw_geometry() (branch !uses_indexed_data) only depends on
         * dg->formats -- dg->cs.normal_enabled/color_enabled are both true
         * whenever we're in this branch, so they add no distinguishing power
         * and don't need to be part of the comparison. */
        bool same_setup = s_cached_setup.valid &&
            memcmp(s_cached_setup.formats, dg->formats,
                   sizeof(dg->formats)) == 0;

        if (!same_setup) {
            setup_draw_geometry(dg, uses_indexed_data);
            memcpy(s_cached_setup.formats, dg->formats, sizeof(dg->formats));
            s_cached_setup.valid = true;
        }
    } else {
        if (!s_last_client_state_is_valid ||
            s_last_client_state.as_int != dg->cs.as_int) {
            setup_draw_geometry(dg, uses_indexed_data);
            s_last_client_state = dg->cs;
            s_last_client_state_is_valid = true;
        }
        s_cached_setup.valid = false;
    }
    /* --------------------------------------------------------------------- */

    GX_CallDispList(dg->gxlist, dg->list_size);

    if (uses_indexed_data) {
        s_last_draw_sync_token = send_draw_sync_token();
        s_last_draw_used_indexed_data = true;
    } else {
        s_last_draw_used_indexed_data = false;
    }
}
static void flat_draw_geometry(void *cb_data)
{
    struct DrawGeometry *dg = cb_data;
    execute_draw_geometry_list(dg);
}

static void run_draw_geometry(struct DrawGeometry *dg)
{
    union client_state cs;

    /* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) ----------------------
     * queue_draw_geometry() can now decline to record a draw (allocation
     * failure, or a list that did not fit). Both leave an empty entry, and
     * everything below -- starting with the mode patch, which dereferences
     * gxlist -- assumes a real one. */
    if (!dg->gxlist || dg->list_size == 0)
        return;

    /* Update the drawing mode on the list. This required peeping into
     * GX_Begin() code. */
    OgxDrawMode gxmode = _ogx_draw_mode(dg->mode);
    u8 *fifo_ptr = dg->gxlist;
    u8 mode_opcode = gxmode.mode | (GX_VTXFMT0 & 0x7);
    if (*fifo_ptr != mode_opcode) {
        /* Before altering the list, we need to make sure that it's not in use
         * by the GP.
         * TODO: find a better criterium, to minimize waits */
        GX_DrawDone();
        *fifo_ptr = mode_opcode;
        DCStoreRange(fifo_ptr, 32); // min size is 32
    }

    _ogx_efb_set_content_type(OGX_EFB_SCENE);

    _ogx_gpu_resources_push();
    cs = glparamstate.cs;
    glparamstate.cs = dg->cs;
    _ogx_update_matrices();
    _ogx_apply_state();
    _ogx_setup_render_stages();
    glparamstate.cs = cs;

    execute_draw_geometry_list(dg);
    _ogx_gpu_resources_pop();

    glparamstate.draw_count++;

    if (glparamstate.stencil.enabled) {
        s_last_client_state_is_valid = false;
        s_cached_setup.valid = false;
        _ogx_gpu_resources_push();
        _ogx_stencil_draw(flat_draw_geometry, dg);
        _ogx_gpu_resources_pop();
        s_last_client_state_is_valid = false;
        s_cached_setup.valid = false;
    }
}

static void run_command(Command *cmd)
{
    switch (cmd->type) {
    case COMMAND_DRAW_ARRAYS:
        run_draw_geometry(&cmd->c.draw_geometry);
        break;
    case COMMAND_DRAW_ELEMENTS:
        run_draw_geometry(&cmd->c.draw_geometry);
        break;
    case COMMAND_CALL_LIST:
        glCallList(cmd->c.gllist);
        break;
    case COMMAND_ENABLE:
        glEnable(cmd->c.cap);
        break;
    case COMMAND_DISABLE:
        glDisable(cmd->c.cap);
        break;
    case COMMAND_LIGHT:
        glLightfv(cmd->c.light.light, cmd->c.light.pname, cmd->c.light.params);
        break;
    case COMMAND_MATERIAL:
        glMaterialfv(cmd->c.material.face, cmd->c.material.pname,
                     cmd->c.material.params);
        break;
    case COMMAND_BLEND_FUNC:
        glBlendFunc(cmd->c.blend_func.sfactor, cmd->c.blend_func.dfactor);
        break;
    case COMMAND_BIND_TEXTURE:
        glBindTexture(cmd->c.bound_texture.target,
                      cmd->c.bound_texture.texture);
        break;
    case COMMAND_TEX_ENV:
        glTexEnvi(cmd->c.tex_env.target,
                  cmd->c.tex_env.pname,
                  cmd->c.tex_env.param);
        break;
    case COMMAND_LOAD_IDENTITY:
        glLoadIdentity();
        break;
    case COMMAND_PUSH_MATRIX:
        glPushMatrix();
        break;
    case COMMAND_POP_MATRIX:
        glPopMatrix();
        break;
    case COMMAND_MULT_MATRIX:
        glMultMatrixf(cmd->c.matrix);
        break;
    case COMMAND_TRANSLATE:
        glTranslatef(cmd->c.xyz.x, cmd->c.xyz.y, cmd->c.xyz.z);
        break;
    case COMMAND_ROTATE:
        glRotatef(cmd->c.rotate.angle,
                  cmd->c.rotate.x, cmd->c.rotate.y, cmd->c.rotate.z);
        break;
    case COMMAND_SCALE:
        glScalef(cmd->c.xyz.x, cmd->c.xyz.y, cmd->c.xyz.z);
        break;
    case COMMAND_FRONT_FACE:
        glFrontFace(cmd->c.mode);
        break;
    case COMMAND_COLOR:
        glColor4fv(cmd->c.color);
        break;
    case COMMAND_NORMAL:
        glNormal3fv(cmd->c.normal);
        break;
    }
}

typedef int (*IndexCallback)(int i, void *index_data);

static int draw_array_index_cb(int i, void *index_data)
{
    int first = *(int*)index_data;
    return i + first;
}

typedef struct {
    GLenum type;
    const GLvoid *indices;
} DrawElementsIndexData;

static int draw_elements_index_cb(int i, void *index_data)
{
    const DrawElementsIndexData *id = (DrawElementsIndexData*)index_data;
    return read_index(id->indices, id->type, i);
}

static void queue_draw_geometry(struct DrawGeometry *dg,
                                GLenum mode, GLsizei count,
                                IndexCallback index_cb,
                                void *index_data)
{
    /* When executing a display list containing glDrawElements() or
     * glDrawArrays() all the attributes that were not enabled at the time of
     * the list creation should be taken from the current active attribute
     * (color, normals and texture coordinates). Since we are not be able to
     * modify a GX list to add more attributes, we'll add them now as indexed
     * attributes: this will allow us to set the value of the indexed attribute
     * at the time when the list is executed. */
    dg->mode = mode;
    dg->cs = glparamstate.cs;
    OgxDrawMode gxmode = _ogx_draw_mode(mode);
    dg->count = count + gxmode.loop;
    dg->gxlist = NULL;
    dg->list_size = 0;
    dg->pooled = false;

    if (glparamstate.dirty.bits.dirty_attributes)
        _ogx_update_vertex_array_readers(gxmode);

    OgxArrayReader *vertex_reader = NULL;
    OgxArrayReader *normal_reader = NULL;
    OgxArrayReader *color_reader = NULL;
    OgxArrayReader *texcoord_reader[MAX_TEXTURE_UNITS] = { NULL };

    /* Get the GX formats used right now */
    OgxArrayReader *reader = NULL;
    int format_index = 0;
    memset(dg->formats, 0, sizeof(dg->formats));
    while (reader = _ogx_array_reader_next(reader)) {
        uint8_t attribute, inputmode, size, type;
        _ogx_array_reader_get_format(reader, &attribute, &inputmode,
                                     &type, &size);
        dg->formats[format_index].attribute = attribute;
        dg->formats[format_index].inputmode = inputmode;
        dg->formats[format_index].comptype = type;
        dg->formats[format_index].compsize = size;
        format_index++;

        if (attribute == GX_VA_POS) {
            vertex_reader = reader;
        } else if (attribute == GX_VA_NRM) {
            normal_reader = reader;
        } else if (attribute == GX_VA_CLR0) {
            /* Ignore CLR1, since, if present, it's identical */
            color_reader = reader;
        } else if (attribute >= GX_VA_TEX0 &&
                   attribute < GX_VA_TEX0 + MAX_TEXTURE_UNITS) {
            texcoord_reader[attribute - GX_VA_TEX0] = reader;
        }
    }

    /* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) ----------------------
     * Size the record buffer from the draw, and survive a failed allocation.
     *
     * Upstream allocated a flat MAX_GXLIST_SIZE (1 MB) here, unconditionally
     * and unchecked, for every recorded draw. Two problems on this target:
     *
     *  - A failed memalign() was never noticed. GX_BeginDispList(NULL, 1MB)
     *    points the CPU FIFO at address 0, so the graphics commands land on
     *    MEM1's low memory and the GP is later handed a list that is not one.
     *    The symptom is Dolphin's "GFX FIFO: unknown opcode", raised well after
     *    the allocation that caused it -- and 1 MB a draw is exactly the kind
     *    of request that starts failing on a 24 MB console once a world loads.
     *  - 1 MB to record a four-vertex glyph quad is also what made the font
     *    cost 288 of these.
     *
     * The bound below is deliberately generous (widest GX encoding for each
     * attribute actually in use) and is then rounded up with one spare 32-byte
     * block, so a list can never *exactly* fill its buffer -- libogc documents
     * that case as returning a garbage size from GX_EndDispList(), which would
     * hand GX_CallDispList() a length running far past the allocation.
     * -------------------------------------------------------------------- */
    int batch_max = _ogx_max_batch_vertices(gxmode.mode);
    if (dg->count > (u32)batch_max &&
        _ogx_vertices_per_primitive(gxmode.mode) == 0) {
        warning("Cannot split a %u-vertex strip/fan/loop; clamping to %d",
                (unsigned)dg->count, batch_max);
        dg->count = (u32)batch_max;
    }

    {
        /* Per vertex, in the order the loop below writes: position (<= 4
         * components x F32), normal (F32 XYZ, or 1 byte when indexed), CLR0 +
         * CLR1 (RGBA8, or 1 byte each when indexed), and one 2 x F32 texture
         * coordinate per enabled unit. */
        u32 bytes_per_vertex = 16;
        bytes_per_vertex += normal_reader ? 12 : 1;
        bytes_per_vertex += color_reader ? 8 : 2;
        for (int tex = 0; tex < MAX_TEXTURE_UNITS; tex++)
            if (texcoord_reader[tex]) bytes_per_vertex += 8;

        /* 3 bytes of GX_Begin() opcode and vertex count per batch. */
        u32 batches = (dg->count + (u32)batch_max - 1) / (u32)batch_max;
        u32 capacity = 3 * (batches ? batches : 1) +
                       (u32)dg->count * bytes_per_vertex;
        /* Round to 32 and add two more blocks. libogc requires the buffer to be
         * "at least 63 bytes larger than the maximum expected amount of data"
         * because of how the write-gather pipe is flushed, so the slack has to
         * be 64..95 here and not the 32..63 a single spare block would give.
         * The margin also keeps a list from ever exactly filling its buffer,
         * which GX_EndDispList() documents as returning a garbage size. */
        capacity = ((capacity + 31) & ~31u) + 64;

        dg->gxlist = gxlist_acquire(capacity, &dg->alloc_size, &dg->pooled);
        if (dg->gxlist) {
            s_list_bytes += dg->alloc_size;
            s_list_count++;
        }
        if (!dg->gxlist) {
            set_error(GL_OUT_OF_MEMORY);
            if (should_report_oom()) {
                warning("GX display-list allocation rejected (%u bytes, %u "
                        "vertices; lists=%u/%u KB); skipping the draw (%u failures so far)",
                        dg->alloc_size, (unsigned)dg->count,
                        s_list_bytes / 1024u, WII_GXLIST_MAX_BYTES / 1024u,
                        s_record_oom_count);
                report_heap();
            }
            return;
        }
        DCInvalidateRange(dg->gxlist, capacity);
        GX_BeginDispList(dg->gxlist, capacity);
    }

    /* Note that the drawing mode set here will be overwritten when executing the list */

    /* LOCAL MODIFICATION (BetaPlusPlus Wii port): one GX_Begin() per u16-sized
     * batch. run_draw_geometry() patches only the leading primitive opcode of a
     * recorded list, which stays correct here because every batch of one list
     * shares its mode; it would only matter if glPolygonMode() changed between
     * recording and replay. */
    for (u32 done = 0; done < dg->count; ) {
        u32 batch = dg->count - done;
        if (batch > (u32)batch_max) batch = (u32)batch_max;
        GX_Begin(gxmode.mode, GX_VTXFMT0, batch);
        for (u32 v = 0; v < batch; v++) {
            int index = index_cb((done + v) % count, index_data);
            _ogx_array_reader_process_element(vertex_reader, index);

            if (normal_reader) {
                _ogx_array_reader_process_element(normal_reader, index);
            } else {
                GX_Normal1x8(0);
            }

            /* The color data is duplicated to CLR0 and CLR1 */
            if (color_reader) {
                _ogx_array_reader_process_element(color_reader, index);
                _ogx_array_reader_process_element(color_reader, index);
            } else {
                GX_Color1x8(0);
                GX_Color1x8(0);
            }

            for (int tex = 0; tex < MAX_TEXTURE_UNITS; tex++) {
                if (texcoord_reader[tex]) {
                    _ogx_array_reader_process_element(texcoord_reader[tex], index);
                }
            }
        }
        GX_End();
        done += batch;
    }

    u32 size = GX_EndDispList();
    /* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) ----------------------
     * 1. Dropped an unconditional fprintf(stderr, "Created draw list %u\n").
     *    This port records thousands of lists (one per chunk section, 288 for
     *    the font), and a formatted write per list is pure overhead on a
     *    729 MHz CPU -- and noise that buries the port's own diagnostics.
     *
     * 2. Shrink with memalign+memcpy instead of realloc().
     *    realloc() was wrong here in two ways that only bite on hardware.
     *    GX_CallDispList() requires a 32-byte-aligned buffer -- that is why the
     *    list was allocated with memalign(32, ...) -- but realloc() only
     *    promises max_align_t (8 bytes here), so a block it chooses to move may
     *    come back misaligned and the GP then reads the list from the wrong
     *    address. And when it does move, the copy is made by the CPU, leaving
     *    the list sitting dirty in the data cache that the GP does not snoop.
     *    Taking realloc's return value (rather than upstream's assert, which
     *    compiles out under NDEBUG and left dg->gxlist dangling) fixed the
     *    lifetime but neither of those.
     *
     *    Shrinking in place is the common case on newlib -- dlmalloc splits the
     *    block and hands the same pointer back -- so this path normally costs
     *    one memcpy and keeps the guarantees.
     *
     * 3. Honour the documented overflow return. GX_EndDispList() answers 0 when
     *    the list did not fit its buffer; the recorded bytes are then a wrapped
     *    ring, not a list. Passing that on would eventually reach
     *    GX_CallDispList(), so drop the buffer and record nothing instead. With
     *    the capacity computed above this should be unreachable, which is
     *    exactly why it is worth saying out loud if it ever happens.
     * -------------------------------------------------------------------- */
    if (size == 0) {
        warning("Display list overflowed its buffer (%u vertices); "
                "skipping the draw", (unsigned)dg->count);
        s_list_bytes -= dg->alloc_size;
        s_list_count--;
        gxlist_release(dg->gxlist, dg->alloc_size, dg->pooled);
        dg->gxlist = NULL;
        dg->alloc_size = 0;
        dg->pooled = false;
        dg->list_size = 0;
        return;
    }

    /* Pooled buffers are never shrunk: list_size (below) already tells
     * GX_CallDispList() the true replay length, so the slack between it and
     * a pool size class is simply never touched at replay time, and shrinking
     * would defeat the pool's same-size-class reuse. Bypass allocations (see
     * gxlist_acquire()) keep the original shrink-to-exact-size behaviour. */
    if (!dg->pooled) {
        void *shrunk = memalign(32, size);
        if (shrunk) {
            memcpy(shrunk, dg->gxlist, size);
            DCFlushRange(shrunk, size);
            free(dg->gxlist);
            dg->gxlist = shrunk;
            s_list_bytes -= dg->alloc_size;
            s_list_bytes += size;
            dg->alloc_size = size;
        }
        /* If the shrink failed we simply keep the full-size buffer: it is
         * already aligned and already holds the list, so this wastes memory
         * but stays correct. */
    }
    dg->list_size = size;
}

static void queue_draw_arrays(struct DrawGeometry *dg,
                              GLenum mode, GLint first, GLsizei count)
{
    queue_draw_geometry(dg, mode, count,
                        draw_array_index_cb, &first);
}

static void queue_draw_elements(struct DrawGeometry *dg,
                                GLenum mode, GLsizei count, GLenum type,
                                const GLvoid *indices)
{
    DrawElementsIndexData id = { type, indices };
    queue_draw_geometry(dg, mode, count,
                        draw_elements_index_cb, &id);
}

static void destroy_buffer(CommandBuffer *buffer)
{
    if (buffer->next) {
        destroy_buffer(buffer->next);
    }

    for (int i =0; i < MAX_COMMANDS_PER_BUFFER; i++) {
        Command *command = &buffer->commands[i];
        if (command->type == COMMAND_NONE) break;

        /* Free the memory for those commands who allocated it */
        if (command->type == COMMAND_DRAW_ELEMENTS ||
            command->type == COMMAND_DRAW_ARRAYS) {
            if (command->c.draw_geometry.gxlist) {
                s_list_bytes -= command->c.draw_geometry.alloc_size;
                s_list_count--;
                /* A section was discarded/recycled, so a previously deferred
                 * section may now fit.  Do not clear this on an arbitrary
                 * frame: it is the freeing event that makes retry useful. */
                s_list_budget_blocked = false;
            }
            gxlist_release(command->c.draw_geometry.gxlist,
                           command->c.draw_geometry.alloc_size,
                           command->c.draw_geometry.pooled);
        }
    }
    free(buffer);
}

static void destroy_list(int index)
{
    CallList *list = &call_lists[index];
    if (!LIST_IS_RESERVED_OR_USED(index)) return;

    if (BUFFER_IS_VALID(list->head)) {
        destroy_buffer(list->head);
    }
    list->head = NULL;
}

void _ogx_call_list_clear_contents(unsigned int list)
{
    if (list < CALL_LIST_START_ID)
        return;
    int index = (int)list - CALL_LIST_START_ID;
    if (index < 0 || index >= MAX_CALL_LISTS)
        return;

    CallList *entry = &call_lists[index];
    if (BUFFER_IS_VALID(entry->head))
        destroy_buffer(entry->head);

    /* Keep the name reserved.  RenderGlobal allocates the complete range once
     * and WorldRenderer reuses its two entries as it slides over the world. */
    entry->head = (void *)1;
}

/* This function returns true if the caller's code needs to be executed now,
 * false if it can immediately return with no further action.
 *
 * For operations that we store in GX lists we do return true, because we want
 * the caller to perform the operation as usual (the only difference will be
 * that GX will not really execute it, but store it in the GX display list).
 */
bool _ogx_call_list_append(CommandType op, ...)
{
    CallList *list = &call_lists[glparamstate.current_call_list.index];
    CommandBuffer *buffer = list->head;
    Command *command;
    va_list ap;
    int count;

    debug(OGX_LOG_CALL_LISTS, "Adding command %d to list %d",
          op, glparamstate.current_call_list.index);

    command = new_command(&list->head);
    /* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) ----------------------
     * new_command() returns NULL when the CommandBuffer malloc fails, and
     * upstream stored through it regardless. That is a write to address 0 --
     * and it only ever happens once the heap is already exhausted, which is
     * exactly the state this port reaches while a world builds, so it turned a
     * recoverable "we could not record this draw" into memory corruption.
     *
     * Drop the command instead. Returning must_execute keeps the documented
     * contract of this function, so a GL_COMPILE_AND_EXECUTE list still draws
     * even though nothing was recorded for a later replay.
     * -------------------------------------------------------------------- */
    if (!command) {
        return glparamstate.current_call_list.must_execute;
    }
    command->type = op;
    va_start(ap, op);
    switch (op) {
    case COMMAND_CALL_LIST:
        command->c.gllist = va_arg(ap, GLuint);
        break;
    case COMMAND_DRAW_ARRAYS:
        {
            GLenum mode = va_arg(ap, GLenum);
            GLint first = va_arg(ap, GLint);
            GLsizei count = va_arg(ap, GLsizei);
            queue_draw_arrays(&command->c.draw_geometry,
                              mode, first, count);
        }
        break;
    case COMMAND_DRAW_ELEMENTS:
        {
            GLenum mode = va_arg(ap, GLenum);
            GLsizei count = va_arg(ap, GLsizei);
            GLenum type = va_arg(ap, GLenum);
            const GLvoid *indices = va_arg(ap, GLvoid *);
            queue_draw_elements(&command->c.draw_geometry,
                                mode, count, type, indices);
        }
        break;
    case COMMAND_ENABLE:
    case COMMAND_DISABLE:
        command->c.cap = va_arg(ap, GLenum);
        break;
    case COMMAND_LIGHT:
        command->c.light.light = va_arg(ap, GLenum);
        command->c.light.pname = va_arg(ap, GLenum);
        switch (command->c.light.pname) {
        case GL_CONSTANT_ATTENUATION:
        case GL_LINEAR_ATTENUATION:
        case GL_QUADRATIC_ATTENUATION:
        case GL_SPOT_CUTOFF:
        case GL_SPOT_EXPONENT:
            count = 1;
            break;
        case GL_SPOT_DIRECTION:
            count = 3;
            break;
        case GL_POSITION:
        case GL_DIFFUSE:
        case GL_AMBIENT:
        case GL_SPECULAR:
            count = 4;
            break;
        }
        floatcpy(command->c.light.params, va_arg(ap, GLfloat *), count);
        break;
    case COMMAND_MATERIAL:
        command->c.material.face = va_arg(ap, GLenum);
        command->c.material.pname = va_arg(ap, GLenum);
        switch (command->c.material.pname) {
        case GL_SHININESS:
            count = 1;
            break;
        default:
            count = 4;
            break;
        }
        floatcpy(command->c.material.params, va_arg(ap, GLfloat *), count);
        break;
    case COMMAND_BLEND_FUNC:
        command->c.blend_func.sfactor = va_arg(ap, GLenum);
        command->c.blend_func.dfactor = va_arg(ap, GLenum);
        break;
    case COMMAND_BIND_TEXTURE:
        command->c.bound_texture.target = va_arg(ap, GLenum);
        command->c.bound_texture.texture = va_arg(ap, GLuint);
        break;
    case COMMAND_TEX_ENV:
        command->c.tex_env.target = va_arg(ap, GLenum);
        command->c.tex_env.pname = va_arg(ap, GLenum);
        command->c.tex_env.param = va_arg(ap, GLint);
        break;
    case COMMAND_MULT_MATRIX:
        floatcpy(command->c.matrix, va_arg(ap, GLfloat *), 16);
        break;
    case COMMAND_TRANSLATE:
    case COMMAND_SCALE:
        command->c.xyz.x = va_arg(ap, double);
        command->c.xyz.y = va_arg(ap, double);
        command->c.xyz.z = va_arg(ap, double);
        break;
    case COMMAND_ROTATE:
        command->c.rotate.angle = va_arg(ap, double);
        command->c.rotate.x = va_arg(ap, double);
        command->c.rotate.y = va_arg(ap, double);
        command->c.rotate.z = va_arg(ap, double);
        break;
    case COMMAND_FRONT_FACE:
        command->c.mode = va_arg(ap, GLenum);
        break;
    case COMMAND_COLOR:
        floatcpy(command->c.color, va_arg(ap, GLfloat *), 4);
        break;
    case COMMAND_NORMAL:
        floatcpy(command->c.normal, va_arg(ap, GLfloat *), 3);
        break;
    }
    va_end(ap);
    return glparamstate.current_call_list.must_execute;
}

/* --- LOCAL MODIFICATION (BetaPlusPlus Wii port) --------------------------
 * Drop the "GP is still reading my indexed data" bookkeeping.
 *
 * setup_draw_geometry() records the last draw-sync token it issued so a later
 * draw can wait before overwriting s_current_color / s_current_normal, which
 * the GP reads by pointer. ogx_prepare_swap_buffers() restarts the token
 * counter at 0 every frame, but nothing told these statics -- so the first
 * indexed list draw of a frame could sit waiting for a token from the previous
 * epoch that will never be reported again, with the CPU blocked and therefore
 * unable to issue it. That is a hard hang.
 *
 * Called from ogx_prepare_swap_buffers(), immediately before the frame's
 * GX_DrawDone(): once that returns nothing is in flight, so there is genuinely
 * nothing left to wait for.
 * ------------------------------------------------------------------------ */
void _ogx_call_lists_reset_sync(void)
{
    s_last_draw_sync_token = 0;
    s_last_draw_used_indexed_data = false;
    s_last_client_state_is_valid = false;
}

GLboolean glIsList(GLuint list)
{
    if (list < CALL_LIST_START_ID) {
        return GL_FALSE;
    }

    list -= CALL_LIST_START_ID;
    if (list >= MAX_CALL_LISTS) {
        return GL_FALSE;
    }

    return LIST_IS_USED(list);
}

void glDeleteLists(GLuint list, GLsizei range)
{
    if (glparamstate.current_call_list.index != -1) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if (range < 0) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    for (int i = 0; i < range; i++) {
        int index = list - CALL_LIST_START_ID;
        if (index < 0 || index >= MAX_CALL_LISTS) {
            /* Note that OpenGL does not specify an error in this case */
            break;
        }

        destroy_list(index);
    }
}

GLuint glGenLists(GLsizei range)
{
    int remaining = range;

    for (int i = 0; i < MAX_CALL_LISTS && remaining > 0; i++) {
        if (!LIST_IS_RESERVED_OR_USED(i)) {
            remaining--;
            if (remaining == 0) {
                /* We found a contiguous range available. Reserve them*/
                int first = i - range + 1;
                for (int j = first; j < first + range; j++)
                    LIST_RESERVE(j);
                return first + CALL_LIST_START_ID;
            }
        } else {
            remaining = range;
        }
    }

    if (remaining > 0) {
        warning("Could not allocate %d display lists", remaining);
        set_error(GL_OUT_OF_MEMORY);
    }

    return 0;
}

void glNewList(GLuint list, GLenum mode)
{
    if (list < CALL_LIST_START_ID) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    list -= CALL_LIST_START_ID;
    if (list >= MAX_CALL_LISTS) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    if (glparamstate.current_call_list.index != -1) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    glparamstate.current_call_list.index = list;
    glparamstate.current_call_list.must_execute = (mode == GL_COMPILE_AND_EXECUTE);
    glparamstate.current_call_list.execution_depth = 0;
    if (LIST_IS_USED(list)) {
        destroy_list(list);
    }
    LIST_RESERVE(list);
}

void glEndList(void)
{
    if (glparamstate.current_call_list.index < 0) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    GLuint list = glparamstate.current_call_list.index + CALL_LIST_START_ID;
    glparamstate.current_call_list.index = -1;
    glparamstate.current_call_list.execution_depth = 0;
}

void glCallList(GLuint id)
{
    if (id < CALL_LIST_START_ID ||
        id - CALL_LIST_START_ID >= MAX_CALL_LISTS) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    HANDLE_CALL_LIST(CALL_LIST, id);

    debug(OGX_LOG_CALL_LISTS, "Calling list %d", id - CALL_LIST_START_ID);

    bool must_decrement = false;
    if (glparamstate.current_call_list.index >= 0) {
        /* We don't want to expand the call list and put its command inside the
         * list currently building */
        glparamstate.current_call_list.execution_depth++;
        must_decrement = true;
    } else {
        /* Inicio de glCallList nivel 0 — invalidar cache por seguridad */
        s_cached_setup.valid = false;
    }

    CallList *list = &call_lists[id - CALL_LIST_START_ID];
    for (CommandBuffer *buffer = list->head;
         BUFFER_IS_VALID(buffer);
         buffer = buffer->next) {
        for (int i =0; i < MAX_COMMANDS_PER_BUFFER; i++) {
            Command *command = &buffer->commands[i];
            if (command->type == COMMAND_NONE) goto done;

            run_command(command);
        }
    }

done:
    /* Until we find a reliable mechanism to ensure that the client state has
     * been preserved, avoid reusing it across different lists. */
    s_last_client_state_is_valid = false;
    s_cached_setup.valid = false;

    if (must_decrement) {
        glparamstate.current_call_list.execution_depth--;
    }
}

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists)
{
    foreach(n, type, lists, glCallList);
}
