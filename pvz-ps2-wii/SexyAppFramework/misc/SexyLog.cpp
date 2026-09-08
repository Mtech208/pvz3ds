#include "misc/SexyLog.h"

#include <stdarg.h>
#include <stdio.h>

// Shared half of the logging system: the level gate, the formatting, and the
// printf capture. Everything platform-specific is behind SexyLogSinkWrite,
// which lives in WiiEarlyInit.cpp (Wii) and TodDebug.cpp (PS2 and desktop).
//
// One buffer size for every path, matching what TodLog/TodTrace/wiiLog each
// used independently before. Lines longer than this were already being
// truncated by all three; keeping the limit identical means no message that
// fits today starts getting cut.
#define SEXY_LOG_LINE_MAX 1024

namespace
{

// Formats and hands off. Split out because the gated and forced entry points
// differ only in whether they check the level -- not in how they build a line.
void SexyLogFormatAndSink(const char *theFormat, va_list theArgs)
{
	char aLine[SEXY_LOG_LINE_MAX];
	const int aCount = vsnprintf(aLine, sizeof(aLine), theFormat, theArgs);
	if (aCount <= 0)
		return;

	SexyLogSinkWrite(aLine);
}

} // namespace

extern "C" void SexyLogWrite(const char *theLine)
{
#if SEXY_LOG_LEVEL >= 1
	if (theLine != nullptr)
		SexyLogSinkWrite(theLine);
#else
	(void)theLine;
#endif
}

extern "C" void SexyLogPrintf(const char *theFormat, ...)
{
#if SEXY_LOG_LEVEL >= 1
	va_list anArgs;
	va_start(anArgs, theFormat);
	SexyLogFormatAndSink(theFormat, anArgs);
	va_end(anArgs);
#else
	(void)theFormat;
#endif
}

extern "C" void SexyLogForce(const char *theLine)
{
	if (theLine != nullptr)
		SexyLogSinkWrite(theLine);
}

extern "C" void SexyLogForcePrintf(const char *theFormat, ...)
{
	va_list anArgs;
	va_start(anArgs, theFormat);
	SexyLogFormatAndSink(theFormat, anArgs);
	va_end(anArgs);
}

// ---------------------------------------------------------------------------
// printf capture.
//
// Linker wraps rather than a source change, so that a printf from anywhere ends
// up in the log: the 134 in game code, and the ones in third-party code we have
// no business editing. PS2 has done this since its port started; the wrappers
// used to live in TodDebug.cpp and are here now so both consoles share one copy
// instead of the Wii growing a second, subtly different one.
//
// These are only built for targets whose link flags actually carry
// -Wl,--wrap=printf (see the console branches in CMakeLists.txt). Elsewhere the
// real printf is fine and redirecting it would be a surprise.
//
// They route through the *gated* entry point, matching what PS2 did before: a
// printf is per-line traffic, and at SEXY_LOG_LEVEL 0 the point is that per-line
// traffic costs nothing.
// ---------------------------------------------------------------------------
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)

extern "C" int __wrap_printf(const char *theFormat, ...)
{
#if SEXY_LOG_LEVEL >= 1
	char aBuffer[SEXY_LOG_LINE_MAX];
	va_list anArgs;
	va_start(anArgs, theFormat);
	const int aCount = vsnprintf(aBuffer, sizeof(aBuffer), theFormat, anArgs);
	va_end(anArgs);

	if (aCount > 0)
		SexyLogWrite(aBuffer);
	return aCount;
#else
	(void)theFormat;
	return 0;
#endif
}

extern "C" int __wrap_puts(const char *theString)
{
#if SEXY_LOG_LEVEL >= 1
	// puts appends its own newline; the sinks expect complete lines, so build
	// one rather than emitting two writes that another thread could interleave.
	char aBuffer[SEXY_LOG_LINE_MAX];
	snprintf(aBuffer, sizeof(aBuffer), "%s\n",
	         theString != nullptr ? theString : "(null)");
	SexyLogWrite(aBuffer);
#else
	(void)theString;
#endif
	return 0;
}

extern "C" int __wrap_putchar(int theChar)
{
#if SEXY_LOG_LEVEL >= 1
	const char aStr[2] = { (char)theChar, '\0' };
	SexyLogWrite(aStr);
#endif
	return theChar;
}

#endif // PS2_PLATFORM || WII_PLATFORM
