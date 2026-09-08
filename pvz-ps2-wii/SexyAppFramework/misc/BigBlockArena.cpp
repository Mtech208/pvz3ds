#include "BigBlockArena.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>

#ifdef WII_PLATFORM
#include <malloc.h>
#endif
#ifdef PS2_PLATFORM
#include "platform/ps2/Ps2BigArena.h"
#endif

#ifdef WII_PLATFORM
namespace
{
const size_t kBigAlign = 64u;
const unsigned int kBigMagic = 0xB16B10C5u;
const int kBigFreeBinCount = 6;

struct BigBlock
{
	size_t mSize;
	unsigned int mFree;
	unsigned int mMagic;
	BigBlock* mFreeNext;
};

static unsigned char* sArena = nullptr;
static pthread_mutex_t sLock;
static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
static size_t sUsed = 0;
static BigBlock* sFreeBins[kBigFreeBinCount];

size_t AlignUp(size_t value)
{
	return (value + kBigAlign - 1) & ~(kBigAlign - 1);
}

size_t HeaderBytes()
{
	return AlignUp(sizeof(BigBlock));
}

size_t PooledRequestSize(size_t value)
{
	value = AlignUp(value);
	if (value >= (192u << 10) && value <= (256u << 10)) return 256u << 10;
	if (value >= (384u << 10) && value <= (512u << 10)) return 512u << 10;
	if (value >= (768u << 10) && value <= (1u << 20)) return 1u << 20;
	return value;
}

int FreeBinForSize(size_t size)
{
	if (size <= (128u << 10)) return 0;
	if (size <= (256u << 10)) return 1;
	if (size <= (512u << 10)) return 2;
	if (size <= (1u << 20)) return 3;
	if (size <= (2u << 20)) return 4;
	return 5;
}

void InitArena()
{
	pthread_mutex_init(&sLock, nullptr);
	sArena = static_cast<unsigned char*>(memalign(kBigAlign, CONSOLE_BIG_ARENA_BYTES));
	if (sArena == nullptr)
		return;
	std::memset(sArena, 0, CONSOLE_BIG_ARENA_BYTES);
	BigBlock* first = reinterpret_cast<BigBlock*>(sArena);
	first->mSize = CONSOLE_BIG_ARENA_BYTES - HeaderBytes();
	first->mFree = 1;
	first->mMagic = kBigMagic;
	first->mFreeNext = nullptr;
}

bool InRange(const void* ptr)
{
	const unsigned char* bytes = static_cast<const unsigned char*>(ptr);
	return sArena != nullptr && bytes >= sArena &&
		bytes < sArena + CONSOLE_BIG_ARENA_BYTES;
}

BigBlock* Next(BigBlock* block)
{
	return reinterpret_cast<BigBlock*>(
		reinterpret_cast<unsigned char*>(block) + HeaderBytes() + block->mSize);
}

void CoalesceLocked()
{
	for (BigBlock* block = reinterpret_cast<BigBlock*>(sArena);
		InRange(block) && block->mMagic == kBigMagic; block = Next(block))
	{
		if (!block->mFree)
			continue;
		BigBlock* next = Next(block);
		while (InRange(next) && next->mMagic == kBigMagic && next->mFree)
		{
			block->mSize += HeaderBytes() + next->mSize;
			next = Next(block);
		}
	}
}

void RebuildBinsLocked()
{
	std::memset(sFreeBins, 0, sizeof(sFreeBins));
	for (BigBlock* block = reinterpret_cast<BigBlock*>(sArena);
		InRange(block) && block->mMagic == kBigMagic; block = Next(block))
	{
		block->mFreeNext = nullptr;
		if (!block->mFree)
			continue;
		const int bin = FreeBinForSize(block->mSize);
		block->mFreeNext = sFreeBins[bin];
		sFreeBins[bin] = block;
	}
}

void NormalizeLocked()
{
	CoalesceLocked();
	RebuildBinsLocked();
}

BigBlock* FindBestFitLocked(size_t need)
{
	BigBlock* best = nullptr;
	for (int bin = FreeBinForSize(need); bin < kBigFreeBinCount; bin++)
	{
		for (BigBlock* block = sFreeBins[bin]; block != nullptr; block = block->mFreeNext)
		{
			if (block->mSize < need)
				continue;
			if (best == nullptr || block->mSize < best->mSize)
				best = block;
		}
		if (best != nullptr)
			break;
	}
	return best;
}
}
#endif

extern "C" void* ConsoleBigBlockAlloc(size_t theSize)
{
#ifdef PS2_PLATFORM
	return Ps2BigAlloc((unsigned int)theSize);
#elif defined(WII_PLATFORM)
	if (theSize == 0 || theSize > CONSOLE_BIG_ARENA_BYTES - HeaderBytes())
		return nullptr;
	pthread_once(&sOnce, InitArena);
	if (sArena == nullptr)
		return nullptr;

	const size_t need = PooledRequestSize(theSize);
	void* result = nullptr;
	pthread_mutex_lock(&sLock);
	NormalizeLocked();
	BigBlock* block = FindBestFitLocked(need);
	if (block != nullptr)
	{
		if (block->mSize >= need + HeaderBytes() + kBigAlign)
		{
			BigBlock* rest = reinterpret_cast<BigBlock*>(
				reinterpret_cast<unsigned char*>(block) + HeaderBytes() + need);
			rest->mSize = block->mSize - need - HeaderBytes();
			rest->mFree = 1;
			rest->mMagic = kBigMagic;
			rest->mFreeNext = nullptr;
			block->mSize = need;
		}
		block->mFree = 0;
		block->mFreeNext = nullptr;
		sUsed += block->mSize;
		result = reinterpret_cast<unsigned char*>(block) + HeaderBytes();
		RebuildBinsLocked();
	}
	pthread_mutex_unlock(&sLock);
	return result;
#else
	(void)theSize;
	return nullptr;
#endif
}

extern "C" int ConsoleBigBlockOwns(const void* thePtr)
{
#ifdef PS2_PLATFORM
	return Ps2BigOwns(thePtr);
#elif defined(WII_PLATFORM)
	// Ownership checks run from global operator delete. Do not reserve the
	// large arena merely because an unrelated heap object is being freed.
	return thePtr != nullptr && InRange(thePtr);
#else
	(void)thePtr;
	return 0;
#endif
}

extern "C" void ConsoleBigBlockFree(void* thePtr)
{
#ifdef PS2_PLATFORM
	Ps2BigFree(thePtr);
#elif defined(WII_PLATFORM)
	if (!ConsoleBigBlockOwns(thePtr))
		return;
	pthread_mutex_lock(&sLock);
	BigBlock* block = reinterpret_cast<BigBlock*>(
		static_cast<unsigned char*>(thePtr) - HeaderBytes());
	if (block->mMagic == kBigMagic && !block->mFree)
	{
		block->mFree = 1;
		sUsed -= block->mSize;
		NormalizeLocked();
	}
	pthread_mutex_unlock(&sLock);
#else
	(void)thePtr;
#endif
}

extern "C" size_t ConsoleBigBlockUsedBytes(void)
{
#ifdef PS2_PLATFORM
	return Ps2BigUsedBytes();
#elif defined(WII_PLATFORM)
	return sUsed;
#else
	return 0;
#endif
}

extern "C" size_t ConsoleBigBlockLargestFreeBytes(void)
{
#ifdef PS2_PLATFORM
	return Ps2BigLargestFreeBytes();
#elif defined(WII_PLATFORM)
	pthread_once(&sOnce, InitArena);
	if (sArena == nullptr)
		return 0;
	size_t largest = 0;
	pthread_mutex_lock(&sLock);
	NormalizeLocked();
	for (int bin = 0; bin < kBigFreeBinCount; bin++)
		for (BigBlock* block = sFreeBins[bin]; block != nullptr; block = block->mFreeNext)
			if (block->mSize > largest) largest = block->mSize;
	pthread_mutex_unlock(&sLock);
	return largest;
#else
	return 0;
#endif
}

extern "C" void* ConsoleScratchAlloc(size_t theSize)
{
	return ConsoleBigBlockAlloc(theSize);
}

extern "C" void ConsoleScratchFree(void* thePtr)
{
	ConsoleBigBlockFree(thePtr);
}

extern "C" int ConsoleScratchOwns(const void* thePtr)
{
	return ConsoleBigBlockOwns(thePtr);
}

ConsoleScratchBuffer::ConsoleScratchBuffer(size_t theSize)
	: mData(theSize != 0 ? ConsoleScratchAlloc(theSize) : nullptr),
	  mSize(theSize), mArenaBacked(mData != nullptr)
{
	if (mData == nullptr && theSize != 0)
		mData = ::operator new[](theSize, std::nothrow);
}

ConsoleScratchBuffer::~ConsoleScratchBuffer()
{
	if (mArenaBacked)
		ConsoleScratchFree(mData);
	else
		::operator delete[](mData);
}
