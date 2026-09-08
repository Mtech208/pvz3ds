#pragma once

#include <stddef.h>

#if defined(PS2_PLATFORM)
#define CONSOLE_BIG_ARENA_BYTES (4u << 20)
#elif defined(WII_PLATFORM)
#define CONSOLE_BIG_ARENA_BYTES (8u << 20)
#else
#define CONSOLE_BIG_ARENA_BYTES 0u
#endif

#ifdef __cplusplus
extern "C" {
#endif

void* ConsoleBigBlockAlloc(size_t theSize);
void  ConsoleBigBlockFree(void* thePtr);
int   ConsoleBigBlockOwns(const void* thePtr);
size_t ConsoleBigBlockUsedBytes(void);
size_t ConsoleBigBlockLargestFreeBytes(void);
void* ConsoleScratchAlloc(size_t theSize);
void  ConsoleScratchFree(void* thePtr);
int   ConsoleScratchOwns(const void* thePtr);

#ifdef __cplusplus
}

// One ownership-safe entry point for transient console staging buffers.
// It uses the fragmentation-proof arena when the request fits and falls back
// to a normal allocation elsewhere (or when the arena is temporarily busy).
class ConsoleScratchBuffer
{
public:
	explicit ConsoleScratchBuffer(size_t theSize);
	~ConsoleScratchBuffer();

	void* Data() const { return mData; }
	size_t Size() const { return mSize; }
	bool IsArenaBacked() const { return mArenaBacked; }
	explicit operator bool() const { return mData != nullptr; }

private:
	ConsoleScratchBuffer(const ConsoleScratchBuffer&);
	ConsoleScratchBuffer& operator=(const ConsoleScratchBuffer&);

	void* mData;
	size_t mSize;
	bool mArenaBacked;
};
#endif
