// WiiHeap.cpp — see WiiHeap.h for what is reported and why the two mallinfo
// fields everyone reaches for first are deliberately absent.
#ifdef WII_PLATFORM

#include "wii/WiiHeap.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <malloc.h>

#include <gccore.h>

#include "wii/WiiEarlyInit.h"
#include "misc/GraphicsBlockArena.h"
#include "misc/BigBlockArena.h"
#include "SexyAppBase.h" // gSexyAppBase, for the allocation-failure purge rescue

// Defined in opengx/src/call_lists.c. Forward-declared rather than reached via
// that file's private header, which would pull opengx's internal GX state into
// game code for two integers.
extern "C" void _ogx_call_lists_memory(u32 *bytes, u32 *lists);

// Defined alongside it: bytes/blocks currently sitting in the size-class pool
// that recycles gxlist buffers across chunk-section rebuilds (see the "GX
// display-list buffer pool" section in call_lists.c). These are real heap
// bytes -- just not live GX lists -- so they must stay visible here or this
// report would undercount actual consumption.
extern "C" void _ogx_call_lists_pool_memory(u32 *cachedBytes, u32 *cachedBlocks);

// Defined alongside those: how many recorded draws missed the pool/arena and
// fell back to a one-off memalign(). See the comment on
// _ogx_call_lists_overflow_stats() in call_lists.h for what bypass vs.
// overflow each mean. Added while chasing the "first load of a session is
// slow, reload is smooth" report: the free/gxlists/pool figures already told
// us the arena was being spent for the first time on a cold load, but not
// whether it had actually run out. These counters say that directly instead
// of it being inferred from MEM2 sbrk shrinking.
extern "C" void _ogx_call_lists_overflow_stats(u32 *bypassBytes, u32 *bypassCount,
                                                u32 *overflowBytes, u32 *overflowCount);
extern "C" void _ogx_call_lists_arena_memory(u32 *usedBytes, u32 *capacityBytes);
extern "C" void wiiAudioMemoryStats(u32 *cachedBytes, u32 *residentBytes,
                                     u32 *pendingBuffers, u32 *rejectedSounds,
                                     u32 *streamStarves, u32 *streamAddBusy)
	__attribute__((weak));

namespace
{
// Static storage deliberately: this is read while rendering the OOM screen,
// precisely when allocating a std::string to format diagnostics might fail.
char s_badAllocLines[5][96] = {};
std::size_t s_lastBadAllocRequest = 0;
void *s_lastBadAllocCaller = nullptr;
u32 s_previousHeadroomKb = 0;
u32 s_lowHeadroomKb = 0xffffffffu;

void captureBadAllocLines()
{
	struct mallinfo mi = mallinfo();
	u32 listBytes = 0, listCount = 0;
	u32 poolBytes = 0, poolBlocks = 0;
	u32 arenaUsed = 0, arenaCapacity = 0;
	_ogx_call_lists_memory(&listBytes, &listCount);
	_ogx_call_lists_pool_memory(&poolBytes, &poolBlocks);
	_ogx_call_lists_arena_memory(&arenaUsed, &arenaCapacity);

	const u32 heapArenaLeft = (u32)SYS_GetArena2Size();
	const u32 headroomKb =
		((u32)mi.fordblks + heapArenaLeft) / 1024u;

	std::snprintf(s_badAllocLines[0], sizeof(s_badAllocLines[0]),
		"Wii heap at allocation failure:");
	std::snprintf(s_badAllocLines[1], sizeof(s_badAllocLines[1]),
		"headroom %uKB; heap free %uKB/%d blocks",
		headroomKb, (unsigned)mi.fordblks / 1024u, mi.ordblks);
	std::snprintf(s_badAllocLines[2], sizeof(s_badAllocLines[2]),
		"heap top %uKB; MEM2 left %uKB",
		(unsigned)mi.keepcost / 1024u, heapArenaLeft / 1024u);
	std::snprintf(s_badAllocLines[3], sizeof(s_badAllocLines[3]),
		"GX %uKB/%u; pool %uKB/%u; arena %u/%uKB",
		listBytes / 1024u, listCount, poolBytes / 1024u, poolBlocks,
		arenaUsed / 1024u, arenaCapacity / 1024u);
	if (s_lastBadAllocRequest != 0)
	{
		std::snprintf(s_badAllocLines[4], sizeof(s_badAllocLines[4]),
			"request %uKB (%u bytes), caller %p",
			(unsigned)(s_lastBadAllocRequest / 1024u),
			(unsigned)s_lastBadAllocRequest,
			s_lastBadAllocCaller);
	}
	else
	{
		std::snprintf(s_badAllocLines[4], sizeof(s_badAllocLines[4]),
			"failed C++ request: size unavailable");
	}
}
} // namespace

// The standard allocator only reports `bad_alloc`, not the requested size.
// On Wii that makes a fragmented 900 KB request look identical to a corrupt
// save asking for 20 MB.  Keep the last throwing request in static storage so
// the existing OOM screen can identify which case happened without allocating
// while the heap is already under pressure.
void *operator new(std::size_t size)
{
	const std::size_t request = size == 0 ? 1 : size;

	if (void *result = std::malloc(request))
		return result;

	// Purge rescue, mirroring the PS2 new-wrap in main_pvz_ps2.cpp.
	//
	// The lazy-image scheme depends on PurgeLazyImageBits running when memory
	// runs out, and on Wii nothing ever called it: the only trigger was
	// IsHeapUnderPressure(), which measures TOTAL free bytes. A fragmented heap
	// reads as healthy by that measure right up to the failure -- the run that
	// produced this comment threw on a 512 KB request with 24.6 MB free spread
	// over 5280 blocks. Failure itself is the one unambiguous signal available,
	// so drive the purge from here and retry before giving up.
	//
	// Logging is safe in this path: the wrappers in misc/SexyLog.cpp format into
	// stack buffers and never allocate.
	//
	// Not thread-safe on purpose: a race just skips one purge attempt, whereas a
	// lock here could deadlock against a purge that allocates.
	static bool sInRescue = false;
	if (!sInRescue && Sexy::gSexyAppBase != nullptr)
	{
		sInRescue = true;
		const int aPurged = Sexy::gSexyAppBase->PurgeLazyImageBits(true);

		// Retry the graphics arena first. What the purge just freed is image
		// bits, and on Wii those are allocated from the arena (MemoryImage.cpp),
		// so that is where the room actually reappears -- retrying only malloc
		// would walk past it. Letting a non-graphics block land there is fine on
		// this path: operator delete below routes every pointer by range, and
		// parking one block in a 16MB reserve beats dying with the general heap
		// holding megabytes it cannot place.
		void *rescued = ConsoleGraphicsAlloc(request);
		if (rescued == nullptr)
			rescued = std::malloc(request);
		sInRescue = false;

		// Bounded: a rescue that keeps succeeding would otherwise write a line
		// per allocation, and this file is one of the few that must stay usable
		// when everything else is failing.
		static int sNumReported = 0;
		if (sNumReported < 16)
		{
			sNumReported++;
			wiiLog("[WII][RAM] alloc %uKB failed, purged %d image bits -> %s\n",
			       (unsigned)(request / 1024u), aPurged,
			       rescued != nullptr ? "rescued" : "still failing");
		}

		if (rescued != nullptr)
			return rescued;
	}

	for (;;)
	{
		if (void *result = std::malloc(request))
			return result;

		std::new_handler handler = std::get_new_handler();
		if (handler == nullptr)
		{
			s_lastBadAllocRequest = request;
			s_lastBadAllocCaller =
				__builtin_extract_return_addr(__builtin_return_address(0));
			throw std::bad_alloc();
		}
		handler();
	}
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void *ptr) noexcept
{
	if (ConsoleGraphicsOwns(ptr))
		ConsoleGraphicsFree(ptr);
	else if (ConsoleBigBlockOwns(ptr))
		ConsoleBigBlockFree(ptr);
	else
		std::free(ptr);
}

void operator delete[](void *ptr) noexcept { ::operator delete(ptr); }
void operator delete(void *ptr, std::size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void *ptr, std::size_t) noexcept { ::operator delete(ptr); }

void wiiHeapReport(const char *tag)
{
	struct mallinfo mi = mallinfo();

	const unsigned freeKb = (unsigned)mi.fordblks / 1024u;
	const unsigned topKb  = (unsigned)mi.keepcost / 1024u;
	const unsigned a1Kb   = (unsigned)SYS_GetArena1Size() / 1024u;
	const unsigned a2Kb   = (unsigned)SYS_GetArena2Size() / 1024u;

	// Who is holding the heap. Minecraft records one GX display list per chunk
	// section, so at SHORT render distance this is 648 buffers of 20-200 KB and
	// is by far the largest single consumer -- but the point of printing it is
	// that "by far the largest" stops being a hypothesis. avg = bytes/lists is
	// the figure that says what raising the render distance would cost.
	u32 listBytes = 0, listCount = 0;
	_ogx_call_lists_memory(&listBytes, &listCount);

	u32 poolBytes = 0, poolBlocks = 0;
	_ogx_call_lists_pool_memory(&poolBytes, &poolBlocks);

	u32 bypassBytes = 0, bypassCount = 0, overflowBytes = 0, overflowCount = 0;
	_ogx_call_lists_overflow_stats(&bypassBytes, &bypassCount,
	                                &overflowBytes, &overflowCount);

	u32 arenaUsed = 0, arenaCapacity = 0;
	_ogx_call_lists_arena_memory(&arenaUsed, &arenaCapacity);
	const u32 graphicsArenaUsed = (u32)ConsoleGraphicsUsedBytes();
	const u32 graphicsArenaLargest = (u32)ConsoleGraphicsLargestFreeBytes();
	u32 audioCached = 0, audioResident = 0;
	u32 audioPending = 0, audioRejected = 0;
	u32 audioStarves = 0, audioAddBusy = 0;
	if (wiiAudioMemoryStats != nullptr)
		wiiAudioMemoryStats(&audioCached, &audioResident,
		                    &audioPending, &audioRejected,
		                    &audioStarves, &audioAddBusy);

	const u32 headroomKb = freeKb + a2Kb;
	const int headroomDeltaKb = s_previousHeadroomKb == 0
		? 0
		: (int)headroomKb - (int)s_previousHeadroomKb;
	s_previousHeadroomKb = headroomKb;
	if (headroomKb < s_lowHeadroomKb)
		s_lowHeadroomKb = headroomKb;

	// Routed through wiiLog rather than printf so this lands in all three
	// channels the port already uses -- screen console, SYS_Report (Dolphin log /
	// USB Gecko) and savedata/userdata/log.txt. The file is the one that
	// survives a hang, which is exactly the case being diagnosed.
	//
	// ovf/byp are cumulative since boot, not since this checkpoint -- read them
	// as deltas between two consecutive lines, the same way the rest of this
	// report is read. ovf climbing during a session means the arena ran out and
	// spilled onto the general heap (raise WII_GXLIST_ARENA_BYTES or lower
	// render distance); byp climbing means individual sections are recording
	// lists above the pool's 256 KB ceiling (a pool/arena change won't help
	// that one, the geometry itself is the issue).
	wiiLog("[WII][RAM] %-24s headroom=%uKB delta=%+dKB low=%uKB | "
	       "heapFree=%uKB/%d top=%uKB "
	       "sbrk: MEM2=%uKB MEM1(nonheap)=%uKB | gxlists=%uKB in %u (avg %uKB) "
	       "pool=%uKB/%u arena=%u/%uKB ovf=%uKB/%u byp=%uKB/%u | "
	       "gfxArena=%uKB largest=%uKB | audioCache=%uKB resident=%uKB pending=%u rejected=%u starve=%u busy=%u\n",
	       tag, headroomKb, headroomDeltaKb, s_lowHeadroomKb,
	       freeKb, mi.ordblks, topKb, a2Kb, a1Kb,
	       listBytes / 1024u, listCount,
	       listCount ? (listBytes / 1024u) / listCount : 0u,
	       poolBytes / 1024u, poolBlocks,
	       arenaUsed / 1024u, arenaCapacity / 1024u,
	       overflowBytes / 1024u, overflowCount,
	       bypassBytes / 1024u, bypassCount,
	       graphicsArenaUsed / 1024u, graphicsArenaLargest / 1024u,
	       audioCached / 1024u, audioResident / 1024u,
	       audioPending, audioRejected, audioStarves, audioAddBusy);
}

void wiiHeapCaptureBadAlloc()
{
	captureBadAllocLines();
	wiiLog("[WII][RAM] bad_alloc C++ request=%u bytes (%uKB) caller=%p\n",
	       (unsigned)s_lastBadAllocRequest,
	       (unsigned)(s_lastBadAllocRequest / 1024u),
	       s_lastBadAllocCaller);
	wiiHeapReport("bad_alloc pre-cleanup");
}

const char *wiiHeapLastBadAllocLine(unsigned index)
{
	return index < 5 ? s_badAllocLines[index] : "";
}

#endif // WII_PLATFORM
