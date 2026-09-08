#ifndef __SEXYENDIAN_H__
#define __SEXYENDIAN_H__

#include <stdint.h>
#include <string.h>

// Byte-order helpers for on-disk data.
//
// The engine targets both little-endian hosts (x86, MIPS/PS2) and big-endian
// ones (PowerPC/Wii), but every file format it reads was authored on a
// little-endian machine and is therefore little-endian on disk. These helpers
// convert between "the value the file stores" and "the value this CPU wants".
//
// Three rules keep this from spreading:
//
//  1. Name the FORMAT, not the platform. A call site says SexyLE32() because
//     the *file* is little-endian; whether that costs an instruction is this
//     header's problem, not the caller's. Never write #ifdef WII_PLATFORM
//     around a byte swap — the next big-endian target would have to find them
//     all again.
//
//  2. The conversion is symmetric. Reading and writing use the same function:
//     a byte swap is its own inverse, so there is deliberately no separate
//     "to"/"from" pair to get backwards.
//
//  3. Values built with shifts are ALREADY portable and must not be swapped.
//     Code like `(buf[0] << 8) | buf[1]` operates on the value, not on bytes,
//     and reads identically on both endiannesses. These helpers are only for
//     data that was read straight into an integer (fread(&x), a cast over a
//     byte buffer, a struct memcpy) — that is the case where the CPU's byte
//     order leaks in.
//
// Raw struct dumps (SaveGame's SyncBytes, Definition's SMemR) are NOT fixable
// with these helpers: those depend on the ABI's layout and padding as much as
// on byte order, so they have to be regenerated per platform instead.

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
	__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	#define SEXY_BIG_ENDIAN 1
#else
	#define SEXY_BIG_ENDIAN 0
#endif

// constexpr in C++ so these can be checked with static_assert and folded into
// constants; the header still compiles as C, where plain inline is the best
// available. __builtin_bswap* is GCC/Clang-only but is referenced only from the
// big-endian branch, which MSVC never selects.
#ifdef __cplusplus
	#define SEXY_ENDIAN_FN static constexpr
#else
	#define SEXY_ENDIAN_FN static inline
#endif

#if SEXY_BIG_ENDIAN

SEXY_ENDIAN_FN uint16_t SexyLE16(uint16_t theValue) { return __builtin_bswap16(theValue); }
SEXY_ENDIAN_FN uint32_t SexyLE32(uint32_t theValue) { return __builtin_bswap32(theValue); }
SEXY_ENDIAN_FN uint64_t SexyLE64(uint64_t theValue) { return __builtin_bswap64(theValue); }

#else

SEXY_ENDIAN_FN uint16_t SexyLE16(uint16_t theValue) { return theValue; }
SEXY_ENDIAN_FN uint32_t SexyLE32(uint32_t theValue) { return theValue; }
SEXY_ENDIAN_FN uint64_t SexyLE64(uint64_t theValue) { return theValue; }

#endif

// Floats are IEEE-754 on every target here, so the stored bytes only need the
// same reversal an integer would get. memcpy (not a pointer cast) keeps this
// free of strict-aliasing undefined behaviour; every compiler folds it away.
static inline float SexyLEFloat(float theValue)
{
	uint32_t aBits;
	memcpy(&aBits, &theValue, sizeof(aBits));
	aBits = SexyLE32(aBits);
	memcpy(&theValue, &aBits, sizeof(theValue));
	return theValue;
}

static inline double SexyLEDouble(double theValue)
{
	uint64_t aBits;
	memcpy(&aBits, &theValue, sizeof(aBits));
	aBits = SexyLE64(aBits);
	memcpy(&theValue, &aBits, sizeof(theValue));
	return theValue;
}

#endif // __SEXYENDIAN_H__
