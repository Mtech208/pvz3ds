#ifdef PS2_PLATFORM

#include "Ps2VramAllocator.h"

#include <gsKit.h>
#include <gsCore.h>

extern GSGLOBAL* gsGlobal;

// Texture VRAM allocator layered on top of gsKit's bump allocator.
//
// gsKit_vram_alloc() cannot release individual allocations. We therefore keep
// released texture ranges in an address-sorted free list. Allocations split
// larger free blocks and frees coalesce adjacent ranges, which avoids the old
// behaviour where a 64 KiB request consumed an entire multi-hundred-KiB hole.
// The texture backend may still rewind the whole texture arena as a final fallback.
#define PS2_MAX_VRAM_FREE 256

struct Ps2VramBlock
{
    uint32_t vram;
    uint32_t size;
};

static Ps2VramBlock s_vramFree[PS2_MAX_VRAM_FREE];
static int s_vramFreeCount = 0;
static uint32_t s_texVramBase = 0;
static inline uint32_t Ps2VramAlign(uint32_t size)
{
    // User buffers are allocated by gsKit on 256-byte VRAM boundaries.
    // Mirror that granularity in the recycle list or a free/reuse cycle can
    // lose the hidden padding and eventually return overlapping ranges.
    return (size + 255u) & ~255u;
}

static void Ps2VramCaptureBase()
{
    if (!s_texVramBase && gsGlobal)
        s_texVramBase = gsGlobal->TexturePointer
            ? gsGlobal->TexturePointer : gsGlobal->CurrentPointer;
}

static void Ps2VramRemoveBlock(int index)
{
    for (int i = index; i + 1 < s_vramFreeCount; ++i)
        s_vramFree[i] = s_vramFree[i + 1];
    --s_vramFreeCount;
}

uint32_t Ps2VramAlloc(uint32_t size)
{
    if (!gsGlobal || size == 0)
        return GSKIT_ALLOC_ERROR;

    size = Ps2VramAlign(size);
    Ps2VramCaptureBase();

    // Best-fit reduces fragmentation while the list remains small enough that
    // a linear scan is cheaper than maintaining a more complex structure.
    int best = -1;
    for (int i = 0; i < s_vramFreeCount; ++i)
    {
        if (s_vramFree[i].size >= size &&
            (best < 0 || s_vramFree[i].size < s_vramFree[best].size))
            best = i;
    }

    if (best >= 0)
    {
        const uint32_t vram = s_vramFree[best].vram;
        if (s_vramFree[best].size == size)
        {
            Ps2VramRemoveBlock(best);
        }
        else
        {
            s_vramFree[best].vram += size;
            s_vramFree[best].size -= size;
        }
        return vram;
    }

    return gsKit_vram_alloc(gsGlobal, size, GSKIT_ALLOC_USERBUFFER);
}

void Ps2VramFree(uint32_t vram, uint32_t size)
{
    // Callers must retire any queued GS work that references this range before
    // releasing it. Keeping synchronization out of this allocator lets several
    // LRU evictions share a single GS fence instead of stalling per block.
    if (vram == GSKIT_ALLOC_ERROR || size == 0)
        return;

    size = Ps2VramAlign(size);

    // Insert in address order so adjacent ranges can be coalesced in O(n).
    int pos = 0;
    while (pos < s_vramFreeCount && s_vramFree[pos].vram < vram)
        ++pos;

    // If the bookkeeping list is saturated, keeping a fragmented block would
    // be unsafe because it could never be reused. The full-arena fallback can
    // recover it later, so simply leave it unavailable for now.
    if (s_vramFreeCount >= PS2_MAX_VRAM_FREE)
        return;

    for (int i = s_vramFreeCount; i > pos; --i)
        s_vramFree[i] = s_vramFree[i - 1];
    s_vramFree[pos].vram = vram;
    s_vramFree[pos].size = size;
    ++s_vramFreeCount;

    // Merge with previous range.
    if (pos > 0)
    {
        Ps2VramBlock& prev = s_vramFree[pos - 1];
        Ps2VramBlock& cur = s_vramFree[pos];
        if (prev.vram + prev.size == cur.vram)
        {
            prev.size += cur.size;
            Ps2VramRemoveBlock(pos);
            --pos;
        }
    }

    // Merge with next range.
    if (pos + 1 < s_vramFreeCount)
    {
        Ps2VramBlock& cur = s_vramFree[pos];
        Ps2VramBlock& next = s_vramFree[pos + 1];
        if (cur.vram + cur.size == next.vram)
        {
            cur.size += next.size;
            Ps2VramRemoveBlock(pos + 1);
        }
    }
}

void Ps2VramEvictAll()
{
    s_vramFreeCount = 0;
    if (gsGlobal && s_texVramBase)
        gsGlobal->CurrentPointer = s_texVramBase;
}

extern "C" int ps2_dbg_vram_current_kb()
{
    return gsGlobal ? (int)(gsGlobal->CurrentPointer / 1024) : 0;
}

extern "C" int ps2_dbg_vram_free_blocks()
{
    return s_vramFreeCount;
}

extern "C" int ps2_dbg_vram_recycled_kb()
{
    uint32_t total = 0;
    for (int i = 0; i < s_vramFreeCount; ++i)
        total += s_vramFree[i].size;
    return (int)(total / 1024);
}

#endif
