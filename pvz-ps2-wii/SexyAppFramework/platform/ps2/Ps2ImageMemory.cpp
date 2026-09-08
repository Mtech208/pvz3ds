#ifdef PS2_PLATFORM

#include "Ps2ImageMemory.h"

#include <malloc.h>
#include <new>
#include <cstdio>

#include "SexyAppBase.h"
#include "Ps2BigArena.h"

namespace Sexy
{

long Ps2ImageUsedHeapBytes()
{
    const struct mallinfo aHeap = mallinfo();
    return (long)aHeap.uordblks;
}

bool Ps2PrepareImageDecode(long thePeakBytes)
{
    if (thePeakBytes <= 0 || gSexyAppBase == nullptr)
        return true;

    // Large decode buffers can come from the reserved arena instead of the
    // general EE heap. Only apply the heap ceiling when the arena cannot hold
    // the entire transient peak.
    const bool aFitsArena =
        thePeakBytes >= (long)PS2_BIG_ALLOC_THRESHOLD &&
        (long)(PS2_BIG_ARENA_BYTES - Ps2BigUsedBytes()) >= thePeakBytes;
    if (aFitsArena)
        return true;

    const long aHeapGuardLine = (25L << 20) - (long)PS2_BIG_ARENA_BYTES;
    if (Ps2ImageUsedHeapBytes() + thePeakBytes <= aHeapGuardLine)
        return true;

    gSexyAppBase->PurgeLazyImageBits(true);
    if (Ps2ImageUsedHeapBytes() + thePeakBytes <= aHeapGuardLine)
        return true;

    gSexyAppBase->PurgeLazyImages();
    return Ps2ImageUsedHeapBytes() + thePeakBytes <= aHeapGuardLine;
}

uint32_t* Ps2AllocImageBits(int theCount)
{
    uint32_t* aBits = new (std::nothrow) uint32_t[theCount];
    if (aBits == nullptr && gSexyAppBase != nullptr)
    {
        printf("[IMG] alloc %d bytes failed (used=%.2fMB), purging bits\n",
            (int)(theCount * sizeof(uint32_t)),
            (double)Ps2ImageUsedHeapBytes() / (1024.0 * 1024.0));
        gSexyAppBase->PurgeLazyImageBits(true);
        aBits = new (std::nothrow) uint32_t[theCount];
        if (aBits == nullptr)
        {
            printf("[IMG] alloc still failing, purging textures too\n");
            gSexyAppBase->PurgeLazyImages();
            aBits = new (std::nothrow) uint32_t[theCount];
        }
    }
    return aBits;
}

unsigned char* Ps2AllocImageBytes(int theCount)
{
    unsigned char* aBytes = new (std::nothrow) unsigned char[theCount];
    if (aBytes == nullptr && gSexyAppBase != nullptr)
    {
        printf("[IMG] alloc %d bytes failed (used=%.2fMB), purging bits\n",
            theCount, (double)Ps2ImageUsedHeapBytes() / (1024.0 * 1024.0));
        gSexyAppBase->PurgeLazyImageBits(true);
        aBytes = new (std::nothrow) unsigned char[theCount];
        if (aBytes == nullptr)
        {
            printf("[IMG] alloc still failing, purging textures too\n");
            gSexyAppBase->PurgeLazyImages();
            aBytes = new (std::nothrow) unsigned char[theCount];
        }
    }
    return aBytes;
}

} // namespace Sexy

#endif // PS2_PLATFORM
