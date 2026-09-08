#pragma once

#include <stddef.h>

#define WII_GRAPHICS_ARENA_BYTES (16u << 20)
#define WII_GRAPHICS_ARENA_MIN_ALLOC (32u << 10)

#ifdef __cplusplus
extern "C" {
#endif

void*  ConsoleGraphicsAlloc(size_t theSize);
void   ConsoleGraphicsFree(void* thePtr);
int    ConsoleGraphicsOwns(const void* thePtr);
size_t ConsoleGraphicsUsedBytes(void);
size_t ConsoleGraphicsLargestFreeBytes(void);
void   ConsoleGraphicsCompactFreeBlocks(void);

#ifdef __cplusplus
}
#endif
