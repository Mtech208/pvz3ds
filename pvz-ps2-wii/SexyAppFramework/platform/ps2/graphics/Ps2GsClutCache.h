#pragma once

#ifdef PS2_PLATFORM

#include <stdint.h>

// Shared CLUT residency.
//
// Every indexed texture used to own a private CLUT in VRAM: 1 KiB for a T8
// palette regardless of how small the texture itself is. Sprites drawn from the
// same source art usually palettize to identical CLUTs, so textures now look up
// a resident CLUT by content and reference it instead of uploading a duplicate.
// Besides reclaiming VRAM this keeps CBP stable across consecutive draws, which
// is what lets the GS skip redundant CLUT loads.
//
// Entries are reference counted. Lookups compare the full palette contents, so
// a hash collision cannot alias two different palettes onto one allocation.

uint64_t Ps2GsClutHash(const uint32_t* clut, int entryCount);

// Takes a reference to a resident CLUT holding this palette. Returns false when
// the palette is not resident and the caller must upload its own.
bool Ps2GsClutCacheAcquire(const uint32_t* clut, int entryCount, uint64_t hash, uint32_t* outVram);

// Publishes a freshly uploaded CLUT for later sharing. Returns false when the
// table is saturated, in which case the caller keeps private ownership.
bool Ps2GsClutCacheInsert(const uint32_t* clut, int entryCount, uint64_t hash,
	uint32_t vram, uint32_t vramSize);

// Drops one reference. Returns true when the caller must release the VRAM
// range, which is also the answer for an address the table never tracked.
bool Ps2GsClutCacheRelease(uint32_t vram);

// Forgets every entry without touching VRAM. Callers use this after rewinding
// the whole texture arena, which invalidates all recorded addresses at once.
void Ps2GsClutCacheReset(void);

int Ps2GsClutCacheCount(void);
int Ps2GsClutCacheSharedBytes(void);

#endif
