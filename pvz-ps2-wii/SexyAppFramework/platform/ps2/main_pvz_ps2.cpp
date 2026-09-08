#ifdef PS2_PLATFORM

#include <kernel.h>
#include <sifrpc.h>
#include <gsKit.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <malloc.h>
#include <pthread.h>
#include <exception>
#include <typeinfo>

#include "LawnApp.h"
#include "Resources.h"
#include "Sexy.TodLib/TodStringFile.h"
#include "Sexy.TodLib/TodDebug.h"
#include "Ps2PvzServices.h"
#include "Ps2Trace.h"
#include "Ps2BigArena.h"
#include "Ps2CrashHandler.h"

using namespace Sexy;

namespace Sexy { void Ps2MixerIoPauseBegin(); }

// Every uncaught C++ exception funnels here: TOD_ASSERT (TodErrorMessageBox
// throws on PS2), bad_alloc from a full heap, anything else. Without this the
// main thread dies alone in std::terminate while the mixer thread keeps
// playing music over a frozen frame, and log.txt says nothing. Instead: put
// the reason (plus heap use, to tell OOM from a full pool) in userdata/log.txt
// and end the whole process so the death is visible.
static void Ps2FatalDie(const char* theWhat)
{
    // Park the mixer before touching the filesystem: this append is fio from
    // this thread with the mixer streaming audsrv RPCs — the same SIF
    // collision the pause scopes exist for. No matching End; we never return.
    Sexy::Ps2MixerIoPauseBegin();

    struct mallinfo aHeap = mallinfo();
    char aLine[1280];
    snprintf(aLine, sizeof(aLine), "[FATAL] %s (heap in use: %d KB)\n",
             (theWhat != NULL && theWhat[0] != '\0') ? theWhat : "(sin mensaje)",
             aHeap.uordblks / 1024);
    TodForceAppendToDebugLog(aLine);

    Exit(1);
}

static void Ps2TerminateHandler()
{
    // Reached on a throw outside main's try/catch (another thread, or during
    // unwinding). Recover the message when there is an active exception.
    if (std::current_exception())
    {
        try { std::rethrow_exception(std::current_exception()); }
        catch (const std::exception& e) { Ps2FatalDie(e.what()); }
        catch (...) { Ps2FatalDie("std::terminate: excepcion de tipo desconocido"); }
    }
    Ps2FatalDie("std::terminate sin excepcion activa");
}

// Linked with -Wl,--wrap=__cxa_throw (CMakeLists). Logs every C++ throw
// BEFORE stack unwinding begins: unwinding walks the .eh_frame tables, and a
// broken table (the historic missing-.eh_frame link bug) kills the EE inside
// libgcc's add_fdes with nothing in the log. This line is written first, so
// even then log.txt names the exception type and the throw site (feed the ra
// value to mips64r5900el-ps2-elf-addr2line -f -e pvz_ps2_syms.elf).
extern "C" void __real___cxa_throw(void* thrownException, void* typeInfo, void (*destructor)(void*)) __attribute__((noreturn));
extern "C" void __wrap___cxa_throw(void* thrownException, void* typeInfo, void (*destructor)(void*))
{
    const std::type_info* aType = static_cast<const std::type_info*>(typeInfo);
    char aLine[256];
    snprintf(aLine, sizeof(aLine), "[THROW] tipo=%s ra=%p\n",
             (aType != NULL) ? aType->name() : "(desconocido)",
             __builtin_return_address(0));
    TodForceAppendToDebugLog(aLine);
    __real___cxa_throw(thrownException, typeInfo, destructor);
}

// ---------------------------------------------------------------------------
// Big-block arena (see Ps2BigArena.h). A trivial first-fit allocator over a
// region reserved in .bss: with at most a couple dozen live blocks, an
// implicit list with 64-byte headers is enough. free() only marks the block;
// the allocation walk coalesces adjacent free runs before testing the fit,
// which keeps Ps2BigFree cheap enough for any thread.
// ---------------------------------------------------------------------------
#define PS2_BIG_ALIGN 64u
// Header magic: a free() of an interior pointer or a buffer overrun lands on
// a header without it, and gets logged + contained instead of silently
// shredding a neighbour's payload.
#define PS2_BIG_MAGIC 0xB16B10C5u

struct Ps2BigBlock
{
    unsigned int mSize;   // payload bytes, multiple of PS2_BIG_ALIGN
    unsigned int mFree;
    unsigned int mMagic;
    unsigned char mPad[PS2_BIG_ALIGN - 3 * sizeof(unsigned int)];
};

static unsigned char sBigArena[PS2_BIG_ARENA_BYTES] __attribute__((aligned(64)));
static pthread_mutex_t sBigLock;
static pthread_once_t  sBigOnce = PTHREAD_ONCE_INIT;
static unsigned int    sBigUsed = 0;

static void Ps2BigInit(void)
{
    pthread_mutex_init(&sBigLock, NULL);
    Ps2BigBlock* aFirst = (Ps2BigBlock*)sBigArena;
    aFirst->mSize = PS2_BIG_ARENA_BYTES - sizeof(Ps2BigBlock);
    aFirst->mFree = 1;
    aFirst->mMagic = PS2_BIG_MAGIC;
}

static inline Ps2BigBlock* Ps2BigNext(Ps2BigBlock* theBlock)
{
    return (Ps2BigBlock*)((unsigned char*)theBlock + sizeof(Ps2BigBlock) + theBlock->mSize);
}

static inline bool Ps2BigInRange(const void* thePtr)
{
    return (const unsigned char*)thePtr >= sBigArena
        && (const unsigned char*)thePtr < sBigArena + PS2_BIG_ARENA_BYTES;
}

static inline unsigned int Ps2BigPooledSize(unsigned int theSize)
{
    unsigned int aSize = (theSize + PS2_BIG_ALIGN - 1) & ~(PS2_BIG_ALIGN - 1);
    if (aSize >= (192u << 10) && aSize <= (256u << 10)) return 256u << 10;
    if (aSize >= (384u << 10) && aSize <= (512u << 10)) return 512u << 10;
    if (aSize >= (768u << 10) && aSize <= (1u << 20)) return 1u << 20;
    return aSize;
}

static void Ps2BigCoalesceLocked(void)
{
    Ps2BigBlock* aBlock = (Ps2BigBlock*)sBigArena;
    while (Ps2BigInRange(aBlock) && aBlock->mMagic == PS2_BIG_MAGIC)
    {
        if (aBlock->mFree)
        {
            Ps2BigBlock* aNext = Ps2BigNext(aBlock);
            while (Ps2BigInRange(aNext) && aNext->mMagic == PS2_BIG_MAGIC && aNext->mFree)
            {
                aBlock->mSize += sizeof(Ps2BigBlock) + aNext->mSize;
                aNext = Ps2BigNext(aBlock);
            }
        }
        aBlock = Ps2BigNext(aBlock);
    }
}

extern "C" void* Ps2BigAlloc(unsigned int theSize)
{
    if (theSize == 0 || theSize > PS2_BIG_ARENA_BYTES - sizeof(Ps2BigBlock))
        return NULL;
    pthread_once(&sBigOnce, Ps2BigInit);
    unsigned int aNeed = Ps2BigPooledSize(theSize);

    void* aPtr = NULL;
    pthread_mutex_lock(&sBigLock);
    Ps2BigCoalesceLocked();
    Ps2BigBlock* aBest = NULL;
    Ps2BigBlock* aBlock = (Ps2BigBlock*)sBigArena;
    while (Ps2BigInRange(aBlock))
    {
        if (aBlock->mMagic != PS2_BIG_MAGIC)
        {
            char aLine[96];
            snprintf(aLine, sizeof(aLine), "[BIG] header corrupto en +%u — walk abortado\n",
                     (unsigned)((unsigned char*)aBlock - sBigArena));
            TodForceAppendToDebugLog(aLine);
            break;
        }
        if (aBlock->mFree && aBlock->mSize >= aNeed &&
            (aBest == NULL || aBlock->mSize < aBest->mSize))
        {
            aBest = aBlock;
        }
        aBlock = Ps2BigNext(aBlock);
    }

    if (aBest != NULL)
    {
        if (aBest->mSize >= aNeed + sizeof(Ps2BigBlock) + PS2_BIG_ALIGN)
        {
            Ps2BigBlock* aRest =
                (Ps2BigBlock*)((unsigned char*)aBest + sizeof(Ps2BigBlock) + aNeed);
            aRest->mSize = aBest->mSize - aNeed - sizeof(Ps2BigBlock);
            aRest->mFree = 1;
            aRest->mMagic = PS2_BIG_MAGIC;
            aBest->mSize = aNeed;
        }
        aBest->mFree = 0;
        sBigUsed += aBest->mSize;
        aPtr = (unsigned char*)aBest + sizeof(Ps2BigBlock);
    }
    pthread_mutex_unlock(&sBigLock);
    return aPtr;
}

extern "C" int Ps2BigOwns(const void* thePtr)
{
    return thePtr != NULL && Ps2BigInRange(thePtr);
}

extern "C" void Ps2BigFree(void* thePtr)
{
    if (thePtr == NULL)
        return;

    pthread_once(&sBigOnce, Ps2BigInit);
    bool aValidBlock = false;
    bool aDoubleFree = false;
    pthread_mutex_lock(&sBigLock);
    Ps2BigBlock* aBlock = (Ps2BigBlock*)sBigArena;
    while (Ps2BigInRange(aBlock) && aBlock->mMagic == PS2_BIG_MAGIC)
    {
        void* aPayload = (unsigned char*)aBlock + sizeof(Ps2BigBlock);
        if (aPayload == thePtr)
        {
            aValidBlock = true;
            aDoubleFree = aBlock->mFree != 0;
            if (!aDoubleFree)
            {
                aBlock->mFree = 1;
                sBigUsed -= aBlock->mSize;
                Ps2BigCoalesceLocked();
            }
            break;
        }

        uintptr_t aNext = (uintptr_t)aBlock + sizeof(Ps2BigBlock) + aBlock->mSize;
        uintptr_t aArenaEnd = (uintptr_t)sBigArena + PS2_BIG_ARENA_BYTES;
        if (aNext <= (uintptr_t)aBlock || aNext >= aArenaEnd)
            break;
        aBlock = (Ps2BigBlock*)aNext;
    }
    pthread_mutex_unlock(&sBigLock);

    if (!aValidBlock || aDoubleFree)
    {
        char aLine[112];
        snprintf(aLine, sizeof(aLine), "[BIG] invalid free ptr=%p%s\n",
            thePtr, aDoubleFree ? " (double free)" : "");
        TodForceAppendToDebugLog(aLine);
    }
}

extern "C" unsigned int Ps2BigUsedBytes(void)
{
    return sBigUsed;
}

extern "C" unsigned int Ps2BigLargestFreeBytes(void)
{
    pthread_once(&sBigOnce, Ps2BigInit);
    unsigned int aLargest = 0;
    pthread_mutex_lock(&sBigLock);
    Ps2BigCoalesceLocked();
    Ps2BigBlock* aBlock = (Ps2BigBlock*)sBigArena;
    while (Ps2BigInRange(aBlock) && aBlock->mMagic == PS2_BIG_MAGIC)
    {
        if (aBlock->mFree && aBlock->mSize > aLargest)
            aLargest = aBlock->mSize;
        aBlock = Ps2BigNext(aBlock);
    }
    pthread_mutex_unlock(&sBigLock);
    return aLargest;
}

// Linked with -Wl,--wrap=free (CMakeLists): arena pointers must never reach
// newlib's free. operator delete/delete[] are malloc-based in GCC and funnel
// into free, so this one wrap covers delete of arena-backed new[] buffers,
// the codebase's mixed new[]/free() spots, and plain C callers alike. The
// range test is lock-free and constant, so the 99% non-arena case costs one
// compare.
extern "C" void __real_free(void* thePtr);
extern "C" void __wrap_free(void* thePtr)
{
    if (Ps2BigOwns(thePtr))
    {
        Ps2BigFree(thePtr);
        return;
    }
    __real_free(thePtr);
}

// Linked with -Wl,--wrap=_Znwj / -Wl,--wrap=_Znaj (CMakeLists): every plain
// (throwing) operator new/new[] in the game funnels here. Two jobs:
//  - name the caller: a failed allocation logs its size and return address
//    (feed the ra to addr2line against pvz_ps2_syms.elf), which the [THROW]
//    line alone cannot do — its ra only ever points inside operator new.
//  - rescue it: shed purgeable image bits and retry before letting the real
//    operator throw. The engine's own hot paths (lazy decode, atlases) already
//    do this dance locally; this catches every allocation that does not.
// GCC's default operator new/delete are malloc/free, so handing out malloc
// memory here is ABI-clean (array cookies are the caller's business), and the
// nothrow variants call through these wrappers too, keeping their semantics.
extern "C" void* __real__Znwj(unsigned int theSize);
extern "C" void* __real__Znaj(unsigned int theSize);

static void* Ps2NewWithRescue(unsigned int theSize, void* (*theReal)(unsigned int), void* theRa)
{
    // Big blocks go to the reserved arena first: they never compete with the
    // general heap's small-object confetti, so a purged background/definition
    // always leaves a hole the next one can reuse (the practical answer to
    // heap fragmentation on a flat sbrk arena). Arena full -> general heap.
    if (theSize >= PS2_BIG_ALLOC_THRESHOLD)
    {
        void* aBigPtr = Ps2BigAlloc(theSize);
        if (aBigPtr != NULL)
            return aBigPtr;
    }

    void* aPtr = malloc(theSize);
    if (aPtr != NULL)
        return aPtr;

    // Not thread-safe on purpose: a race just skips one purge attempt, while
    // a lock here could deadlock a purge that allocates.
    static bool sInRescue = false;
    char aLine[160];
    if (!sInRescue && Sexy::gSexyAppBase != NULL)
    {
        sInRescue = true;
        snprintf(aLine, sizeof(aLine),
                 "[NEW] %u bytes FAILED ra=%p (arena %uKB) — purgando bits lazy\n",
                 theSize, theRa, Ps2BigUsedBytes() >> 10);
        TodForceAppendToDebugLog(aLine);
        Sexy::gSexyAppBase->PurgeLazyImageBits(true);
        // The purge frees arena-backed image bits too, so retry there first —
        // and accept medium blocks (>= the rescue floor) into the arena even
        // below the routing threshold: a general heap that cannot serve 244KB
        // with 8.4MB of holes (the CLOUD5 sanding expansion) is pure confetti,
        // and parking one medium block in the arena beats dying.
        if (theSize >= PS2_BIG_RESCUE_FLOOR)
            aPtr = Ps2BigAlloc(theSize);
        if (aPtr == NULL)
            aPtr = malloc(theSize);
        sInRescue = false;
        if (aPtr != NULL)
        {
            snprintf(aLine, sizeof(aLine), "[NEW] rescatado: %u bytes tras purga\n", theSize);
            TodForceAppendToDebugLog(aLine);
            return aPtr;
        }
    }
    // Still failing: let the real operator run its new_handler loop and throw
    // bad_alloc (logged by the __cxa_throw wrap, caught by main's catch-all).
    return theReal(theSize);
}

extern "C" void* __wrap__Znwj(unsigned int theSize)
{
    return Ps2NewWithRescue(theSize, __real__Znwj, __builtin_return_address(0));
}
extern "C" void* __wrap__Znaj(unsigned int theSize)
{
    return Ps2NewWithRescue(theSize, __real__Znaj, __builtin_return_address(0));
}

// Definition of the gsGlobal pointer shared by the native GS renderer, Window.cpp and Input.cpp.
GSGLOBAL* gsGlobal = nullptr;

// Function-pointer globals declared extern in LawnApp.h; defined here for PS2
// (on PC they live in main.cpp which is excluded from this build).
bool (*gAppCloseRequest)()        = nullptr;
bool (*gAppHasUsedCheatKeys)()    = nullptr;
SexyString (*gGetCurrentLevelName)() = nullptr;

// blank.tga is not included in some pak builds; write a minimal one to the host FS so
// the resource manager can find it at "images/blank".
static void EnsureBlankTga()
{
    // On a CD/DVD boot the current directory is the read-only disc, so this
    // fallback cannot (and need not) write: the shipped main.pak carries
    // images/blank.tga and the pak layer serves it directly. Skip the doomed
    // open/write pair to avoid a spurious warning.
    if (Ps2GetResourcePrefix()[0] != '\0')
        return;

    FILE* f = fopen("images/blank.tga", "rb");
    if (f) { fclose(f); return; }

    mkdir("images", 0755);   // create directory if missing (ignore errors)

    f = fopen("images/blank.tga", "wb");
    if (!f)
    {
        printf("[PvZ PS2] Warning: could not create images/blank.tga\n");
        return;
    }

    // Minimal uncompressed 32-bpp RGBA TGA: 4x4 white pixels.
    // 4x4 avoids potential issues with 1x1 textures on some PS2 texture units.
    static const unsigned char kHeader[18] = {
        0,    // ID length
        0,    // color map type: none
        2,    // image type: uncompressed true-color
        0,0,0,0,0,  // color map spec
        0,0,  // X origin (LE)
        0,0,  // Y origin (LE)
        4,0,  // width = 4 (LE)
        4,0,  // height = 4 (LE)
        32,   // pixel depth: 32 bpp
        0x28  // image descriptor: 8 alpha bits, top-left origin
    };
    fwrite(kHeader, 1, sizeof(kHeader), f);

    // 4*4 = 16 white BGRA pixels
    for (int i = 0; i < 16; ++i)
    {
        static const unsigned char kWhite[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        fwrite(kWhite, 1, 4, f);
    }
    fclose(f);
    printf("[PvZ PS2] Created images/blank.tga\n");
}

int main(int argc, char** argv)
{
    // argv[0] is the ELF boot path; its device prefix (cdrom0:/mass:/host:)
    // tells the services layer how we were launched so it can pick the asset
    // read root and save location. Guard against a launcher passing no args.
    const char* aArgv0 = (argc > 0 && argv != NULL && argv[0] != NULL) ? argv[0] : "";

    // Boot tracer: compiled out unless PS2_TRACE_ENABLE=1 (Ps2Trace.h). When
    // enabled, each point repaints the GS border colour so a black-screen hang
    // shows how far startup got — boot/init are dim, loading is bright.
    Ps2Trace(PS2_TRACE_BOOT_MAIN);

    std::set_terminate(Ps2TerminateHandler);

    // Before anything else can fault: set_terminate above only catches C++
    // exceptions, and a bad pointer never becomes one. This claims the EE's
    // memory-fault vectors so the faulting PC reaches the screen instead of
    // leaving a frozen picture with no information.
    Ps2CrashHandlerInstall();

    // The kernel hands the ELF's main thread priority 0 — the highest on the
    // EE, above the audio mixer thread (20, Ps2SoundManager). A CPU-bound
    // gameplay frame at priority 0 never yields, so the mixer crawls to ~1
    // chunk/s, the SPU2 ring underruns, and playback sticks on a frozen buzz
    // (mixer health log: "STALLED" cycling spots 3/4/5 with pause=0). Drop
    // below the mixer so it can preempt us; it only needs ~1ms per 46ms
    // chunk. This call originally lived in startup_ps2.c and was lost when
    // that file was removed in the logging refactor — do not lose it again.
    ChangeThreadPriority(GetThreadId(), 32);

    SifInitRpc(0);
    Ps2Trace(PS2_TRACE_BOOT_SIF);
    printf("[PvZ PS2] main() entered\n");

    // IOP modules (memory card, USB mass storage, cdfs, audsrv) + save location
    // + asset read root.
    Ps2PvzInitServices(aArgv0);
    if (Ps2GetSavePrefix()[0] != '\0')
        Sexy::SetAppDataFolder(Ps2GetSavePrefix());
    Ps2Trace(PS2_TRACE_BOOT_IOP);

    EnsureBlankTga();
    Ps2Trace(PS2_TRACE_BOOT_BLANKTGA);

    TodStringListSetColors(gLawnStringFormats, gLawnStringFormatCount);
    gGetCurrentLevelName    = LawnGetCurrentLevelName;
    gAppCloseRequest        = LawnGetCloseRequest;
    gAppHasUsedCheatKeys    = LawnHasUsedCheatKeys;
    gExtractResourcesByName = Sexy::ExtractResourcesByName;

    try
    {
        gLawnApp = new LawnApp();
        Ps2Trace(PS2_TRACE_BOOT_LAWNAPP);
        gLawnApp->Init();
        Ps2Trace(PS2_TRACE_INIT_DONE);
        gLawnApp->Start();
        gLawnApp->Shutdown();
        delete gLawnApp;
    }
    catch (const std::exception& e)
    {
        Ps2FatalDie(e.what());
    }
    catch (...)
    {
        Ps2FatalDie("excepcion no capturada de tipo desconocido");
    }

    return 0;
}

#endif // PS2_PLATFORM
