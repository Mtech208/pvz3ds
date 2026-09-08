#pragma once

// Heap tracing for the Wii port.
//
// This exists because "Out of memory" on its own cannot separate the three cases
// that matter, and this port has already spent sessions on the wrong one:
//
//   in use high, free low   -> genuinely exhausted; cut what is allocated
//   in use low,  free high  -> fragmentation; the request cannot be PLACED
//   total small             -> the arena is not the one we think it is
//
// The same reasoning (and the same numbers) as report_heap() in
// opengx/src/call_lists.c, which reports at the moment a display-list allocation
// fails. This one is driven from the game side instead, so the world-load
// milestones and the steady state can be compared against each other.
//
// What is printed, and what is deliberately NOT
// ---------------------------------------------
// MALLOC_MEM2=1 selects Arena2 as the one and only sbrk arena. Arena1 remains
// useful for explicit allocations, framebuffers and other libogc facilities,
// but it cannot satisfy malloc and must not be included in heap headroom.
//
// What is left is computed by walking the bins, so it is real:
//   free    total free bytes (fordblks)
//   blocks  how many pieces that free space is in (ordblks)
//   top     the top chunk (keepcost)
//   arena2  what sbrk can still claim for malloc
//   arena1  shown separately as non-heap memory
//
// free/blocks is the ratio that separates "exhausted" from "fragmented": a large
// free total split across hundreds of small blocks with a small top chunk is
// fragmentation, and cutting allocations further will not help.

// Print one tagged heap line. Tag is padded so a log reads as a column.
void wiiHeapReport(const char *tag);

// Snapshot the heap at the point a C++ allocation failed.  The OOM handler
// subsequently releases the world, so this has to happen before cleanup if the
// in-game error screen is to show useful numbers rather than an empty heap.
void wiiHeapCaptureBadAlloc();

// One of the short diagnostic lines captured by wiiHeapCaptureBadAlloc().
// Returns an empty string when no allocation failure has been captured yet.
const char *wiiHeapLastBadAllocLine(unsigned index);
