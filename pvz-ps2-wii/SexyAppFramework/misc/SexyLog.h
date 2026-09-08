#ifndef __SEXYLOG_H__
#define __SEXYLOG_H__

// One logging sink for the whole engine.
//
// Before this there were four, none of which knew about the others:
//
//   * wiiLog          90 sites, Wii only -> boot console + SYS_Report + file
//   * TodTrace etc.  122 sites, all platforms -> userdata/log.txt
//   * printf         134 sites -> the IOP TTY on PS2, and nowhere at all on
//                    Wii, where stdout stops being read the moment GX owns the
//                    framebuffer
//   * TodForceAppendToDebugLog, which bypassed whichever gate was in the way
//
// The cost of that was not tidiness. A diagnostic written with the wrong one of
// the four went into a void, and the author had no way to tell: the Wii music
// failures printed five different reasons for silence, with printf, into a
// channel nobody was reading. Two logs also means two timelines, so a boot line
// and a gameplay line could not be ordered against each other.
//
// The shape here is one sink and many façades. Every existing entry point keeps
// its name and signature and funnels in, so the ~350 call sites did not have to
// be touched and can stay in whatever dialect suits their neighbourhood.
//
// printf is captured the way PS2 already captured it: linker wraps, not a
// source change. --wrap=printf is in each console's link flags and the wrappers
// live in SexyLog.cpp, so a printf from anywhere -- including third-party code
// like stb_vorbis, which we are not going to edit -- lands in the same file as
// everything else.
//
// SexyLogSinkWrite is the one seam. Exactly one translation unit per platform
// defines it: WiiEarlyInit.cpp for Wii (three channels), TodDebug.cpp for PS2
// (open-append-close per line, with the border-colour freeze forensics and the
// mixer park it has always had), SexyLog.cpp for everyone else. Adding a target
// means writing that function and nothing else.

// Logging verbosity is supplied by the build as SEXY_LOG_LEVEL.
//
//   0  Off. Per-line logging is skipped entirely. SexyLogForce still writes.
//   1  Normal. Boot, load phases, state transitions, and failures.
//   2  Verbose. Periodic heap/frame reports, per-texture loads, and GL warnings.
//
// Keep this as the only logging definition across every platform so a build
// cannot accidentally compile different translation units at different levels.
#ifndef SEXY_LOG_LEVEL
	#error "SEXY_LOG_LEVEL must be defined by the build system (0, 1, or 2)"
#endif

#if defined(__GNUC__)
	#define SEXY_LOG_PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
	#define SEXY_LOG_PRINTF_FMT(a, b)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Level-gated. Below SEXY_LOG_LEVEL 1 these compile down to nothing at the call
// site's expense only -- the argument expressions are still evaluated, matching
// how TodTrace and wiiLog behaved before.
void SexyLogWrite(const char *theLine);
void SexyLogPrintf(const char *theFormat, ...) SEXY_LOG_PRINTF_FMT(1, 2);

// Ungated. For the fatal path only: an uncaught exception, an allocation
// failure, an assert. These have to survive a build with logging off, because
// they are the one line that explains the run.
void SexyLogForce(const char *theLine);
void SexyLogForcePrintf(const char *theFormat, ...) SEXY_LOG_PRINTF_FMT(1, 2);

// The platform seam. Receives a complete, already-formatted line and is
// responsible for putting it wherever that platform can be read. Called for
// gated and forced writes alike; the gate is applied before this point.
void SexyLogSinkWrite(const char *theLine);

#ifdef __cplusplus
}
#endif

#endif // __SEXYLOG_H__
