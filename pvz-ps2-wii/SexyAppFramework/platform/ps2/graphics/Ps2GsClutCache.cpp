#ifdef PS2_PLATFORM

#include "Ps2GsClutCache.h"

#include <string.h>

// Bounded so a pathological palette count cannot grow EE usage without limit.
#define PS2_GS_CLUT_CACHE_MAX_ENTRIES 64
#define PS2_GS_CLUT_CACHE_MAX_PALETTE 256

// Palette copies live in .bss, not on the heap, and that is deliberate.
//
// Inserts run inside the mid-draw texture upload path and the reset runs from
// Ps2GsTextureEvictAll, which is reached from the purge that operator new
// triggers when an allocation fails. Calling malloc or free from there reenters
// the allocator while it is recovering from its own failure, which is the shape
// of the confirmed "TLB in mallinfo during full purge" crash. 64 slots of 256
// words costs a fixed 64 KiB and keeps this path allocation-free.
struct Ps2GsClutCacheEntry
{
	uint64_t mHash;
	uint32_t mVram;
	uint32_t mVramSize;
	int mEntryCount;
	int mRefCount;
};

static Ps2GsClutCacheEntry sEntries[PS2_GS_CLUT_CACHE_MAX_ENTRIES];
static uint32_t sPalettes[PS2_GS_CLUT_CACHE_MAX_ENTRIES][PS2_GS_CLUT_CACHE_MAX_PALETTE];
static int sEntryCount = 0;

uint64_t Ps2GsClutHash(const uint32_t* clut, int entryCount)
{
	if (!clut || entryCount <= 0)
		return 0;

	// FNV-1a over the palette words. The exact comparison in Acquire is what
	// guarantees correctness; this only has to spread well enough to keep the
	// linear scan short.
	uint64_t hash = 1469598103934665603ULL;
	for (int i = 0; i < entryCount; ++i)
	{
		const uint32_t word = clut[i];
		for (int byte = 0; byte < 4; ++byte)
		{
			hash ^= (uint64_t)((word >> (byte * 8)) & 0xFF);
			hash *= 1099511628211ULL;
		}
	}
	return hash;
}

static int Ps2GsClutCacheFind(const uint32_t* clut, int entryCount, uint64_t hash)
{
	for (int i = 0; i < sEntryCount; ++i)
	{
		const Ps2GsClutCacheEntry& entry = sEntries[i];
		if (entry.mHash != hash || entry.mEntryCount != entryCount)
			continue;
		if (memcmp(sPalettes[i], clut, (size_t)entryCount * sizeof(uint32_t)) == 0)
			return i;
	}
	return -1;
}

static void Ps2GsClutCacheRemove(int index)
{
	// Entries are kept dense by moving the last one down, so its palette has to
	// follow it into the vacated slot.
	--sEntryCount;
	if (index != sEntryCount)
	{
		sEntries[index] = sEntries[sEntryCount];
		memcpy(sPalettes[index], sPalettes[sEntryCount], sizeof(sPalettes[index]));
	}
	memset(&sEntries[sEntryCount], 0, sizeof(sEntries[sEntryCount]));
}

bool Ps2GsClutCacheAcquire(const uint32_t* clut, int entryCount, uint64_t hash, uint32_t* outVram)
{
	if (!clut || entryCount <= 0 || !outVram)
		return false;

	const int index = Ps2GsClutCacheFind(clut, entryCount, hash);
	if (index < 0)
		return false;

	sEntries[index].mRefCount++;
	*outVram = sEntries[index].mVram;
	return true;
}

bool Ps2GsClutCacheInsert(const uint32_t* clut, int entryCount, uint64_t hash,
	uint32_t vram, uint32_t vramSize)
{
	if (!clut || entryCount <= 0 || entryCount > PS2_GS_CLUT_CACHE_MAX_PALETTE ||
		sEntryCount >= PS2_GS_CLUT_CACHE_MAX_ENTRIES)
		return false;

	const int slot = sEntryCount;
	memcpy(sPalettes[slot], clut, (size_t)entryCount * sizeof(uint32_t));

	Ps2GsClutCacheEntry& entry = sEntries[sEntryCount++];
	entry.mHash = hash;
	entry.mVram = vram;
	entry.mVramSize = vramSize;
	entry.mEntryCount = entryCount;
	entry.mRefCount = 1;
	return true;
}

bool Ps2GsClutCacheRelease(uint32_t vram)
{
	for (int i = 0; i < sEntryCount; ++i)
	{
		if (sEntries[i].mVram != vram)
			continue;
		if (--sEntries[i].mRefCount > 0)
			return false;
		Ps2GsClutCacheRemove(i);
		return true;
	}
	return true;
}

void Ps2GsClutCacheReset(void)
{
	// Runs from the full-purge path, so it must not touch the allocator. The
	// palette store is left as-is; sEntryCount is what makes a slot live.
	memset(sEntries, 0, sizeof(sEntries));
	sEntryCount = 0;
}

int Ps2GsClutCacheCount(void)
{
	return sEntryCount;
}

int Ps2GsClutCacheSharedBytes(void)
{
	// VRAM that duplicate palettes would have consumed without sharing.
	int bytes = 0;
	for (int i = 0; i < sEntryCount; ++i)
		bytes += (sEntries[i].mRefCount - 1) * (int)sEntries[i].mVramSize;
	return bytes;
}

#endif
