#include "GraphicsBlockArena.h"

#ifdef WII_PLATFORM

#include <malloc.h>
#include <pthread.h>
#include <string.h>

namespace
{
const size_t kAlign = 64u;
const unsigned kMagic = 0x47524146u;
const int kFreeBinCount = 7;

struct Block
{
	size_t size;
	unsigned free;
	unsigned magic;
	Block* freeNext;
};

static unsigned char* sArena;
static pthread_mutex_t sLock;
static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
static size_t sUsed;
static Block* sFreeBins[kFreeBinCount];

size_t Align(size_t value) { return (value + kAlign - 1) & ~(kAlign - 1); }
size_t Header() { return Align(sizeof(Block)); }
bool InRange(const void* ptr)
{
	const unsigned char* p = static_cast<const unsigned char*>(ptr);
	return sArena != NULL && p >= sArena && p < sArena + WII_GRAPHICS_ARENA_BYTES;
}
Block* Next(Block* block)
{
	return reinterpret_cast<Block*>(reinterpret_cast<unsigned char*>(block) + Header() + block->size);
}
int FreeBinForSize(size_t size)
{
	if (size <= (32u << 10)) return 0;
	if (size <= (64u << 10)) return 1;
	if (size <= (128u << 10)) return 2;
	if (size <= (256u << 10)) return 3;
	if (size <= (512u << 10)) return 4;
	if (size <= (1u << 20)) return 5;
	return 6;
}
void CoalesceFreeBlocksLocked()
{
	for (Block* block = reinterpret_cast<Block*>(sArena); InRange(block) && block->magic == kMagic; block = Next(block))
	{
		if (!block->free)
			continue;

		Block* next = Next(block);
		while (InRange(next) && next->magic == kMagic && next->free)
		{
			block->size += Header() + next->size;
			next = Next(block);
		}
	}
}
void RebuildFreeBinsLocked()
{
	memset(sFreeBins, 0, sizeof(sFreeBins));
	for (Block* block = reinterpret_cast<Block*>(sArena); InRange(block) && block->magic == kMagic; block = Next(block))
	{
		block->freeNext = NULL;
		if (!block->free)
			continue;
		const int bin = FreeBinForSize(block->size);
		block->freeNext = sFreeBins[bin];
		sFreeBins[bin] = block;
	}
}
void NormalizeFreeSpaceLocked()
{
	CoalesceFreeBlocksLocked();
	RebuildFreeBinsLocked();
}
Block* FindBestFitLocked(size_t need)
{
	Block* best = NULL;
	const int firstBin = FreeBinForSize(need);
	for (int bin = firstBin; bin < kFreeBinCount; bin++)
	{
		for (Block* block = sFreeBins[bin]; block != NULL; block = block->freeNext)
		{
			if (block->size < need)
				continue;
			if (best == NULL || block->size < best->size)
				best = block;
		}
		if (best != NULL)
			break;
	}
	return best;
}
void Init()
{
	pthread_mutex_init(&sLock, NULL);
	sArena = static_cast<unsigned char*>(memalign(kAlign, WII_GRAPHICS_ARENA_BYTES));
	if (sArena == NULL) return;
	memset(sArena, 0, WII_GRAPHICS_ARENA_BYTES);
	Block* first = reinterpret_cast<Block*>(sArena);
	first->size = WII_GRAPHICS_ARENA_BYTES - Header();
	first->free = 1;
	first->magic = kMagic;
	first->freeNext = NULL;
	RebuildFreeBinsLocked();
}
}

extern "C" void* ConsoleGraphicsAlloc(size_t size)
{
	if (size < WII_GRAPHICS_ARENA_MIN_ALLOC || size > WII_GRAPHICS_ARENA_BYTES - Header()) return NULL;
	pthread_once(&sOnce, Init);
	if (sArena == NULL) return NULL;
	const size_t need = Align(size);
	void* result = NULL;
	pthread_mutex_lock(&sLock);
	NormalizeFreeSpaceLocked();
	Block* block = FindBestFitLocked(need);
	if (block != NULL)
	{
		if (block->size >= need + Header() + kAlign)
		{
			Block* rest = reinterpret_cast<Block*>(reinterpret_cast<unsigned char*>(block) + Header() + need);
			rest->size = block->size - need - Header();
			rest->free = 1;
			rest->magic = kMagic;
			rest->freeNext = NULL;
			block->size = need;
		}
		block->free = 0;
		block->freeNext = NULL;
		sUsed += block->size;
		result = reinterpret_cast<unsigned char*>(block) + Header();
		RebuildFreeBinsLocked();
	}
	pthread_mutex_unlock(&sLock);
	return result;
}

extern "C" int ConsoleGraphicsOwns(const void* ptr)
{
	return ptr != NULL && InRange(ptr);
}

extern "C" void ConsoleGraphicsFree(void* ptr)
{
	if (!ConsoleGraphicsOwns(ptr)) return;
	pthread_mutex_lock(&sLock);
	Block* block = reinterpret_cast<Block*>(static_cast<unsigned char*>(ptr) - Header());
	if (block->magic == kMagic && !block->free)
	{
		block->free = 1;
		sUsed -= block->size;
		NormalizeFreeSpaceLocked();
	}
	pthread_mutex_unlock(&sLock);
}

extern "C" void ConsoleGraphicsCompactFreeBlocks(void)
{
	pthread_once(&sOnce, Init);
	if (sArena == NULL) return;
	pthread_mutex_lock(&sLock);
	NormalizeFreeSpaceLocked();
	pthread_mutex_unlock(&sLock);
}

extern "C" size_t ConsoleGraphicsUsedBytes(void) { return sUsed; }

extern "C" size_t ConsoleGraphicsLargestFreeBytes(void)
{
	pthread_once(&sOnce, Init);
	if (sArena == NULL) return 0;
	size_t largest = 0;
	pthread_mutex_lock(&sLock);
	NormalizeFreeSpaceLocked();
	for (int bin = 0; bin < kFreeBinCount; bin++)
		for (Block* block = sFreeBins[bin]; block != NULL; block = block->freeNext)
			if (block->size > largest) largest = block->size;
	pthread_mutex_unlock(&sLock);
	return largest;
}

#else
extern "C" void* ConsoleGraphicsAlloc(size_t) { return NULL; }
extern "C" void ConsoleGraphicsFree(void*) {}
extern "C" int ConsoleGraphicsOwns(const void*) { return 0; }
extern "C" size_t ConsoleGraphicsUsedBytes(void) { return 0; }
extern "C" size_t ConsoleGraphicsLargestFreeBytes(void) { return 0; }
extern "C" void ConsoleGraphicsCompactFreeBlocks(void) {}
#endif
