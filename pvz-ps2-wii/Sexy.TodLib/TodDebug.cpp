#include <time.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdexcept>

#ifdef PS2_PLATFORM
#include <malloc.h> // mallinfo() for heap diagnostics
#include <stdio.h>
// On PS2 every printf is a SIF fio RPC to the IOP (TTY/host write). The fio
// service is not thread-safe: the loading pthread and the main thread both
// log heavily during loads, and unserialized writes wedge the IOP on real
// hardware (works in emulators, which serialize SIF internally). Route the
// tracing here through the same global IO lock as the pak reader.
#include "Ps2IoLock.h"
#include "Ps2Trace.h" // Ps2BootStage: freeze forensics inside the log write
#include "Ps2BigArena.h" // arena usage in the HESITATE heap traces

// Mixer park handshake (Ps2SoundManager.cpp) — forward-declared to keep the
// PS2 sound header out of TodLib, same pattern as Reanimator/CreditScreen.
namespace Sexy { void Ps2MixerIoPauseBegin(); void Ps2MixerIoPauseEnd(); }

// SEXY_LOG_LEVEL is the single build-time logging switch on every platform.
// PS2 defaults it to off because each persisted line is an open/write/close
// round-trip on the save device so the last line survives a hard freeze.
#endif

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "TodDebug.h"
#include "TodCommon.h"
#include "misc/Debug.h"
#include "misc/SexyLog.h"
#include "../SexyAppFramework/SexyAppBase.h"

using namespace Sexy;

static char gLogFileName[512];
static char gDebugDataFolder[512];

#ifdef PS2_PLATFORM
// Holds every printf from boot until TodAssertInitForApp opens the single
// log.txt at <appdata>/userdata/ — sized for the whole boot+init transcript.
static char gEarlyLogBuffer[16384];
static size_t gEarlyLogBufferLen = 0;
static bool gEarlyLogOverflow = false;

static const size_t PS2_LOG_BUFFER_SIZE = 65536;
static char gPs2LogBuffer[PS2_LOG_BUFFER_SIZE];
static size_t gPs2LogBufferLen = 0;
static size_t gPs2LogDroppedBytes = 0;

static void TodBufferEarlyLog(const char* theMsg)
{
	if (theMsg == nullptr)
		return;

	size_t aLen = strlen(theMsg);
	size_t aRemaining = sizeof(gEarlyLogBuffer) - gEarlyLogBufferLen;
	size_t aCopyLen = aLen < aRemaining ? aLen : aRemaining;
	if (aCopyLen > 0)
	{
		memcpy(gEarlyLogBuffer + gEarlyLogBufferLen, theMsg, aCopyLen);
		gEarlyLogBufferLen += aCopyLen;
	}
	if (aCopyLen < aLen)
		gEarlyLogOverflow = true;
}
#endif

//0x514EA0
void TodErrorMessageBox(const char* theMessage, const char* theTitle)
{
#ifdef __SWITCH__
	ErrorApplicationConfig c;
	errorApplicationCreate(&c, theTitle, theMessage);
	errorApplicationShow(&c);
#else
	throw std::runtime_error("Error Box\n--" + std::string(theTitle) + "--\n" + theMessage);
#endif
}

void TodTraceMemory()
{
}

void* TodMalloc(int theSize)
{
	TOD_ASSERT(theSize > 0);
	return malloc(theSize);
}

void TodFree(void* theBlock)
{
	if (theBlock != nullptr)
	{
		free(theBlock);
	}
}

void TodAssertFailed(const char* theCondition, const char* theFile, int theLine, const char* theMsg, ...)
{
	char aFormattedMsg[1024];
	va_list argList;
	va_start(argList, theMsg);
	int aCount = TodVsnprintf(aFormattedMsg, sizeof(aFormattedMsg), theMsg, argList);
	va_end(argList);

	if (aCount != 0) {
		if (aFormattedMsg[aCount - 1] != '\n')
		{
			if (aCount + 1 < 1024)
			{
				aFormattedMsg[aCount] = '\n';
				aFormattedMsg[aCount + 1] = '\0';
			}
			else
			{
				aFormattedMsg[aCount - 1] = '\n';
			}
		}
	}

	char aBuffer[1024];
	if (*theCondition != '\0')
	{
		TodSnprintf(aBuffer, sizeof(aBuffer), "\n%s(%d)\nassertion failed: '%s'\n%s\n", theFile, theLine, theCondition, aFormattedMsg);
	}
	else
	{
		TodSnprintf(aBuffer, sizeof(aBuffer), "\n%s(%d)\nassertion failed: %s\n", theFile, theLine, aFormattedMsg);
	}
	TodTrace("%s", aBuffer);

	TodErrorMessageBox(aBuffer, "Assertion failed");

	exit(0);
}

void TodLog(const char* theFormat, ...)
{
	char aButter[1024];
	va_list argList;
	va_start(argList, theFormat);
	int aCount = TodVsnprintf(aButter, sizeof(aButter), theFormat, argList);
	va_end(argList);

	if (aButter[aCount - 1] != '\n')
	{
		if (aCount + 1 < 1024)
		{
			aButter[aCount] = '\n';
			aButter[aCount + 1] = '\0';
		}
		else
		{
			aButter[aCount - 1] = '\n';
		}
	}

	TodLogString(aButter);
}

void TodLogString(const char* theMsg)
{
	// The per-platform branch that used to be here is gone: the sink knows how
	// to reach the disk on each target, and this only has to decide that the
	// message is ordinary per-line traffic rather than a fatal-path line.
	SexyLogWrite(theMsg);
}

#ifdef PS2_PLATFORM
extern "C" void TodInitDebugLogForAppData(const char* theAppDataFolder)
{
	if (theAppDataFolder == nullptr)
		theAppDataFolder = "";

	std::string aUserPath = std::string(theAppDataFolder) + "userdata/";
	MkDir(theAppDataFolder);
	MkDir(aUserPath);

	strcpy(gDebugDataFolder, aUserPath.c_str());
	strcpy(gLogFileName, gDebugDataFolder);
	strcpy(gLogFileName + strlen(gLogFileName), "log.txt");
	TOD_ASSERT(strlen(gLogFileName) < 512);

	Ps2IoLockAcquire();
	FILE* aFile = fopen(gLogFileName, "w");
	if (aFile != nullptr)
	{
		// Build stamp so a pulled stick proves at a glance the log is from the
		// binary just deployed, not a stale run.
		static const char kHeader[] =
			"[PS2 DEBUG] log.txt started from boot (build " __DATE__ " " __TIME__ ")\n";
		fwrite(kHeader, 1, sizeof(kHeader) - 1, aFile);
#if SEXY_LOG_LEVEL < 1
		static const char kDisabled[] =
			"[PS2 DEBUG] per-line logging disabled (SEXY_LOG_LEVEL=0); "
			"fatal errors are still written\n";
		fwrite(kDisabled, 1, sizeof(kDisabled) - 1, aFile);
#endif
		if (gEarlyLogBufferLen > 0)
		{
			static const char kBufferedHeader[] = "[PS2 DEBUG] early printf buffer:\n";
			fwrite(kBufferedHeader, 1, sizeof(kBufferedHeader) - 1, aFile);
			fwrite(gEarlyLogBuffer, 1, gEarlyLogBufferLen, aFile);
			if (gEarlyLogBuffer[gEarlyLogBufferLen - 1] != '\n')
				fwrite("\n", 1, 1, aFile);
		}
		if (gEarlyLogOverflow)
		{
			static const char kOverflow[] = "[PS2 DEBUG] early printf buffer overflow; old output was truncated.\n";
			fwrite(kOverflow, 1, sizeof(kOverflow) - 1, aFile);
		}
		fclose(aFile);
	}
	gEarlyLogBufferLen = 0;
	gEarlyLogOverflow = false;
	gPs2LogBufferLen = 0;
	gPs2LogDroppedBytes = 0;
	// fprintf is not link-wrapped like printf, so this reaches the real EE
	// TTY and PCSX2's console shows where the log landed. Only when running
	// from host: (PCSX2/ps2link) — on a real console the TTY fio RPC can
	// wedge the IOP while the USB stack is busy, freezing boot on a black
	// screen.
	if (strncmp(gLogFileName, "host:", 5) == 0)
		fprintf(stderr, "[PS2 DEBUG] log file '%s' (%s)\n",
			gLogFileName, (aFile != nullptr) ? "open" : "OPEN FAILED");
	Ps2IoLockRelease();
}

// Normal PS2 logging is buffered in EE RAM. Opening, writing and closing a
// FAT file for every line makes logging dominate frame time on real hardware.
// Forced writes still use one synchronous transaction so fatal diagnostics and
// periodic stress snapshots survive a hard freeze.
static void TodFlushPs2DebugLog(const char* theForcedMsg)
{
	if (gLogFileName[0] == '\0')
	{
		if (theForcedMsg != nullptr)
			TodBufferEarlyLog(theForcedMsg);
		return;
	}

	Sexy::Ps2MixerIoPauseBegin();
	Ps2IoLockAcquire();

	if (gPs2LogBufferLen == 0 && (theForcedMsg == nullptr || theForcedMsg[0] == '\0'))
	{
		Ps2IoLockRelease();
		Sexy::Ps2MixerIoPauseEnd();
		return;
	}

	Ps2BootStage(0, 0, 64);
	FILE* aFile = fopen(gLogFileName, "a");
	Ps2BootStage(64, 0, 64);
	if (aFile != nullptr)
	{
		if (gPs2LogDroppedBytes > 0)
		{
			char aDroppedLine[96];
			int aDroppedLen = snprintf(aDroppedLine, sizeof(aDroppedLine),
				"[PS2 DEBUG] buffered log dropped %u old bytes\n",
				(unsigned int)gPs2LogDroppedBytes);
			if (aDroppedLen > 0)
				fwrite(aDroppedLine, 1, (size_t)aDroppedLen, aFile);
		}
		if (gPs2LogBufferLen > 0)
			fwrite(gPs2LogBuffer, 1, gPs2LogBufferLen, aFile);
		if (theForcedMsg != nullptr && theForcedMsg[0] != '\0')
			fwrite(theForcedMsg, 1, strlen(theForcedMsg), aFile);
		Ps2BootStage(64, 64, 0);
		fclose(aFile);
		gPs2LogBufferLen = 0;
		gPs2LogDroppedBytes = 0;
	}
	Ps2BootStage(0, 64, 0);

	Ps2IoLockRelease();
	Sexy::Ps2MixerIoPauseEnd();
}

static void TodBufferPs2DebugLog(const char* theMsg)
{
	if (theMsg == nullptr || theMsg[0] == '\0')
		return;

	if (gLogFileName[0] == '\0')
	{
		TodBufferEarlyLog(theMsg);
		return;
	}

	const char* aSource = theMsg;
	size_t aLen = strlen(theMsg);
	size_t aPreDiscard = 0;
	if (aLen > PS2_LOG_BUFFER_SIZE)
	{
		aPreDiscard = aLen - PS2_LOG_BUFFER_SIZE;
		aSource += aPreDiscard;
		aLen = PS2_LOG_BUFFER_SIZE;
	}

	Ps2IoLockAcquire();
	gPs2LogDroppedBytes += aPreDiscard;
	if (gPs2LogBufferLen + aLen > PS2_LOG_BUFFER_SIZE)
	{
		size_t aDiscard = gPs2LogBufferLen + aLen - PS2_LOG_BUFFER_SIZE;
		if (aDiscard >= gPs2LogBufferLen)
		{
			gPs2LogDroppedBytes += gPs2LogBufferLen;
			gPs2LogBufferLen = 0;
		}
		else
		{
			memmove(gPs2LogBuffer, gPs2LogBuffer + aDiscard, gPs2LogBufferLen - aDiscard);
			gPs2LogBufferLen -= aDiscard;
			gPs2LogDroppedBytes += aDiscard;
		}
	}

	memcpy(gPs2LogBuffer + gPs2LogBufferLen, aSource, aLen);
	gPs2LogBufferLen += aLen;
	Ps2IoLockRelease();
}

extern "C" void TodAppendToDebugLog(const char* theMsg)
{
	SexyLogWrite(theMsg);
}

extern "C" void TodForceAppendToDebugLog(const char* theMsg)
{
	TodFlushPs2DebugLog(theMsg);
}

// The printf/puts/putchar link wraps used to live here. They are in
// SexyLog.cpp now, shared with the Wii, which needed exactly the same trick for
// exactly the same reason: its stdout stops being read once GX owns the
// framebuffer, so an unwrapped printf is a message written into a void.
#endif

// ---------------------------------------------------------------------------
// The platform sink for everything except the Wii, which defines its own in
// WiiEarlyInit.cpp. Both PS2 and desktop end up writing to gLogFileName, so
// they share this one definition and differ only in how carefully they do it.
// ---------------------------------------------------------------------------
#if !defined(WII_PLATFORM) && !defined(NINTENDO_3DS)
extern "C" void SexyLogSinkWrite(const char* theLine)
{
	if (theLine == nullptr)
		return;

#ifdef PS2_PLATFORM
	TodBufferPs2DebugLog(theLine);
#else
	// Desktop: the same append TodLogString has always done. stderr on failure
	// rather than silence, because on a desktop somebody is watching a console.
	if (gLogFileName[0] == '\0')
		return;

	FILE* aFile = fopen(gLogFileName, "a");
	if (aFile == nullptr)
	{
		fprintf(stderr, __S("Failed to open log file '%s'\n"), gLogFileName);
		return;
	}

	if (fwrite(theLine, strlen(theLine), 1, aFile) != 1)
		fprintf(stderr, __S("Failed to write to log file\n"));

	fclose(aFile);
#endif
}
#endif // !WII_PLATFORM && !NINTENDO_3DS

void TodTrace(const char* theFormat, ...)
{
	char aButter[1024];
	va_list argList;
	va_start(argList, theFormat);
	int aCount = TodVsnprintf(aButter, sizeof(aButter), theFormat, argList);
	va_end(argList);

	if (aButter[aCount - 1] != '\n')
	{
		if (aCount + 1 < 1024)
		{
			aButter[aCount] = '\n';
			aButter[aCount + 1] = '\0';
		}
		else
		{
			aButter[aCount - 1] = '\n';
		}
	}

	printf("%s", aButter);
}

void TodHesitationTrace(const char* theFormat, ...)
{
#if defined(PS2_PLATFORM) && SEXY_LOG_LEVEL >= 2
	// PS2 loading diagnostics: print each hesitation point with current heap
	// usage so a stall shows exactly which step it reached (the original build
	// leaves this empty). Also report the last delta so growth is visible.
	static long sLastUsed = 0;
	struct mallinfo mi = mallinfo();
	long aUsed = (long)mi.uordblks;
	long aFree = (long)mi.fordblks;

	char aBuffer[512];
	va_list argList;
	va_start(argList, theFormat);
	TodVsnprintf(aBuffer, sizeof(aBuffer), theFormat, argList);
	va_end(argList);

	// free = fordblks: holes INSIDE the malloc arena, not sbrk headroom. A
	// failed medium alloc with several MB "free" here means no hole was big
	// enough — that is fragmentation, and the arena figure shows how much of
	// the big-block reserve is carrying the load.
	printf("[HESITATE used=%.2fMB (%+.2fMB) free=%.2fMB arena=%uKB/%uKB] %s\n",
		(double)aUsed / (1024.0 * 1024.0),
		(double)(aUsed - sLastUsed) / (1024.0 * 1024.0),
		(double)aFree / (1024.0 * 1024.0),
		Ps2BigUsedBytes() >> 10, PS2_BIG_ARENA_BYTES >> 10,
		aBuffer);
	sLastUsed = aUsed;
#else
	(void)theFormat;
#endif
}

void TodTraceAndLog(const char* theFormat, ...)
{
	char aButter[1024];
	va_list argList;
	va_start(argList, theFormat);
	int aCount = TodVsnprintf(aButter, sizeof(aButter), theFormat, argList);
	va_end(argList);

	if (aButter[aCount - 1] != '\n')
	{
		if (aCount + 1 < 1024)
		{
			aButter[aCount] = '\n';
			aButter[aCount + 1] = '\0';
		}
		else
		{
			aButter[aCount - 1] = '\n';
		}
	}

	#ifdef PS2_PLATFORM
	printf("%s", aButter);
	#elif defined(WII_PLATFORM)
	// PrintF and TodLogString share the same Wii sink. Emitting through both
	// would duplicate every TodTraceAndLog line after PrintF was routed through
	// the level-gated logger.
	TodLogString(aButter);
	#else
	printf("%s", aButter);
	TodLogString(aButter);
	#endif
}

void TodTraceWithoutSpamming(const char* theFormat, ...)
{
	static uint64_t gLastTraceTime = 0LL;
	uint64_t aTime = time(NULL);
	if (aTime < gLastTraceTime)
	{
		return;
	}

	gLastTraceTime = aTime;
	char aButter[1024];
	va_list argList;
	va_start(argList, theFormat);
	int aCount = TodVsnprintf(aButter, sizeof(aButter), theFormat, argList);
	va_end(argList);

	if (aButter[aCount - 1] != '\n')
	{
		if (aCount + 1 < 1024)
		{
			aButter[aCount] = '\n';
			aButter[aCount + 1] = '\0';
		}
		else
		{
			aButter[aCount - 1] = '\n';
		}
	}

#ifdef PS2_PLATFORM
	Ps2IoLockAcquire();
	printf("%s", aButter);
	Ps2IoLockRelease();
#else
	printf("%s", aButter);
#endif
}

void TodAssertInitForApp()
{
#ifdef PS2_PLATFORM
	// SexyAppBase::Init re-points the app data folder at <cwd>/ on non-CD boots,
	// overriding any boot-time save prefix from Ps2PvzServices. A log opened
	// at boot can therefore sit somewhere else entirely (e.g. inside mc0:
	// while saves land on host/USB). Reopen it at the effective location
	// whenever the path changed; TodInitDebugLogForAppData closes the old
	// handle itself.
	if ((GetAppDataFolder() + "userdata/log.txt") != gLogFileName)
	{
		TodInitDebugLogForAppData(GetAppDataFolder().c_str());
	}
	else
	{
		MkDir(GetAppDataFolder());
		MkDir(GetAppDataFolder() + "userdata");
	}
	strcpy(gDebugDataFolder, (GetAppDataFolder() + "userdata/").c_str());
	strcpy(gLogFileName, gDebugDataFolder);
	strcpy(gLogFileName + strlen(gLogFileName), "log.txt");
	TOD_ASSERT(strlen(gLogFileName) < 512);
#else
	MkDir(GetAppDataFolder());
	MkDir(GetAppDataFolder() + "userdata");
	std::string aRelativeUserPath = GetAppDataFolder() + "userdata/";
	strcpy(gDebugDataFolder, aRelativeUserPath.c_str());
	strcpy(gLogFileName, gDebugDataFolder);
	strcpy(gLogFileName + strlen(gLogFileName), "log.txt");
	TOD_ASSERT(strlen(gLogFileName) < 512);
#endif

	TodLog("Started %d\n", (uint64_t)time(NULL));
}
