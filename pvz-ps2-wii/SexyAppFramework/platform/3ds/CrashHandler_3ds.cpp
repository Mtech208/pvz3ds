#ifdef NINTENDO_3DS

// 3DS crash handling. There is no debugger on hardware, so the fatal path has
// to be self-describing: it writes a crash log to the same SD card the game
// data lives on, and shows the OS error screen.
//
// Three things are covered, matching what the PS2 handler does for its target:
//
//   1. SexyLogSinkWrite -- the logging sink (see SexyLog.h). On 3DS the shared
//      desktop/PS2 sink is a poor fit (it depends on gLogFileName and desktop
//      fopen semantics), so this audit-trails straight to sdmc:/.../log.txt.
//
//   2. Every C++ throw, via -Wl,--wrap=__cxa_throw. Logs the exception type and
//      the throw site (return address) BEFORE unwinding, so even a broken
//      .eh_frame table that dies inside libgcc leaves the reason in the log.
//
//   3. A std::terminate handler and, when libctru exposes it, a user exception
//      handler for data/instruction aborts, both funnelling register state to
//      the crash log before the built-in error screen takes over.

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <exception>
#include <typeinfo>

#include <sys/stat.h>

#include "misc/SexyLog.h"

// The game data dir doubles as the log dir: it always exists once the SD is
// mounted in MakeWindow, so a fatal line lands somewhere findable without
// trying to create directories from a broken context.
#define PvZ3DS_DATA_DIR    "sdmc:/3ds/PlantsvsZombies/"
#define PvZ3DS_LOG_PATH    PvZ3DS_DATA_DIR "log.txt"
#define PvZ3DS_CRASH_PATH  PvZ3DS_DATA_DIR "crash.log"

namespace
{

// A tiny append that never depends on the C++ runtime or the game allocator,
// in case the crash is an OOM or a corrupted heap. fopen/fwrite via newlib over
// devkitARM's sdmc support is about all that survives reliably.
void AppendToCrashLog(const char* theLine)
{
	if (theLine == NULL)
		return;

	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/PlantsvsZombies", 0777);

	FILE* aFile = fopen(PvZ3DS_LOG_PATH, "a");
	if (aFile != NULL)
	{
		fwrite(theLine, strlen(theLine), 1, aFile);
		fclose(aFile);
	}

	// Mirror into the crash log so the fatal reason is separate and obvious.
	aFile = fopen(PvZ3DS_CRASH_PATH, "a");
	if (aFile != NULL)
	{
		fwrite(theLine, strlen(theLine), 1, aFile);
		fclose(aFile);
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Logging sink (SexyLog.h seam).
// ---------------------------------------------------------------------------
extern "C" void SexyLogSinkWrite(const char* theLine)
{
	AppendToCrashLog(theLine);
}

// ---------------------------------------------------------------------------
// C++ throw capture.
// ---------------------------------------------------------------------------
extern "C" void __real___cxa_throw(void* thrownException, void* typeInfo, void (*destructor)(void*)) __attribute__((noreturn));
extern "C" void __wrap___cxa_throw(void* thrownException, void* typeInfo, void (*destructor)(void*))
{
	const std::type_info* aType = static_cast<const std::type_info*>(typeInfo);
	char aLine[256];
	snprintf(aLine, sizeof(aLine), "[THROW] tipo=%s ra=%p\n",
	         (aType != NULL) ? aType->name() : "(desconocido)",
	         __builtin_return_address(0));
	AppendToCrashLog(aLine);
	__real___cxa_throw(thrownException, typeInfo, destructor);
}

// ---------------------------------------------------------------------------
// std::terminate handler.
// ---------------------------------------------------------------------------
static void PvZ3dsTerminateHandler()
{
	if (std::current_exception())
	{
		try { std::rethrow_exception(std::current_exception()); }
		catch (const std::exception& e) { AppendToCrashLog(e.what()); AppendToCrashLog("\n"); }
		catch (...) { AppendToCrashLog("[FATAL] excepcion de tipo desconocido en terminate\n"); }
	}
	else
	{
		AppendToCrashLog("[FATAL] std::terminate sin excepcion activa\n");
	}

	svcBreak(USERBREAK_PANIC);
}

// ---------------------------------------------------------------------------
// User exception handler (data/instruction aborts). Modern libctru installs it
// per-thread through threadOnException() (thread.h); the callback is invoked
// on the faulting thread's TLS slots by the atomic handler stubs and receives
// the ERRF exception info plus a CpuRegisters dump (types.h/errf.h).
// ---------------------------------------------------------------------------

static void PvZ3dsExceptionHandler(ERRF_ExceptionInfo* theInfo, CpuRegisters* theRegs)
{
	static bool sCrashing = false;
	if (sCrashing)
	{
		svcBreak(USERBREAK_PANIC);
		return;
	}
	sCrashing = true;

	char aLine[512];
	const char* aType = "?";
	switch (theInfo->type)
	{
		case ERRF_EXCEPTION_PREFETCH_ABORT: aType = "prefetch_abort"; break;
		case ERRF_EXCEPTION_DATA_ABORT:     aType = "data_abort";     break;
		case ERRF_EXCEPTION_UNDEFINED:      aType = "undefined";      break;
		case ERRF_EXCEPTION_VFP:            aType = "vfp";            break;
	}
	snprintf(aLine, sizeof(aLine),
	         "[CRASH] exception type=%s fsr=%08lx far=%08lx\n"
	         "  cpsr=%08lx pc=%08lx lr=%08lx sp=%08lx\n"
	         "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx\n",
	         aType,
	         (unsigned long)theInfo->fsr, (unsigned long)theInfo->far,
	         (unsigned long)theRegs->cpsr, (unsigned long)theRegs->pc,
	         (unsigned long)theRegs->lr, (unsigned long)theRegs->sp,
	         (unsigned long)theRegs->r[0], (unsigned long)theRegs->r[1],
	         (unsigned long)theRegs->r[2], (unsigned long)theRegs->r[3],
	         (unsigned long)theRegs->r[12]);
	AppendToCrashLog(aLine);

	// Show the built-in error screen so the failure is visible on hardware.
	svcBreak(USERBREAK_PANIC);
}

// Installed from main() (or MakeWindow) once the SD is mounted. Returns nothing;
// a missing install is harmless, the OS error screen is the fallback.
extern "C" void PvZ3dsInstallCrashHandler()
{
	// std::terminate is process-wide: install exactly once.
	static bool sTerminateInstalled = false;
	if (!sTerminateInstalled)
	{
		sTerminateInstalled = true;
		std::set_terminate(PvZ3dsTerminateHandler);
	}

	// The ERRF user exception hook is stored in thread-local storage, so it has
	// to be re-installed on EVERY thread that could fault (the game's loading
	// thread especially) or a crash there falls back to the bare OS error
	// screen with no log. Run the handler on a stack we own so there is nothing
	// to allocate for the hook (a crash is often an OOM), reused per install.
	static u8 sHandlerStack[0x4000] __attribute__((aligned(8)));
	threadOnException(PvZ3dsExceptionHandler,
		(void*)((uintptr_t)sHandlerStack + sizeof(sHandlerStack)),
		WRITE_DATA_TO_FAULTING_STACK);
}

#endif // NINTENDO_3DS
