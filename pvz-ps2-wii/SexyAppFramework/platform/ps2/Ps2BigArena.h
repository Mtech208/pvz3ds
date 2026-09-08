#ifndef PS2BIGARENA_H
#define PS2BIGARENA_H

/*
 * Reserved-at-boot arena for BIG allocations — the fragmentation quarantine.
 *
 * The EE heap is a flat sbrk arena with no MMU remapping: after a heavy
 * screen it ends up fully extended with small long-lived allocations
 * (strings, defs, the GL shim's texture copies) peppered between the holes,
 * and a 1MB contiguous request dies with 11MB "free" (selector alpha
 * compose, Zombie.reanim's 1.19MB staging blob, background index buffers).
 * True defragmentation is impossible (raw pointers everywhere), so instead
 * big blocks get their own region, reserved in .bss before any fragmentation
 * exists. Small-object confetti NEVER enters it, so freeing a big asset
 * always leaves a hole another big asset can actually reuse.
 *
 * Routing is transparent: the link-time operator-new wrap (main_pvz_ps2.cpp)
 * sends every allocation >= PS2_BIG_ALLOC_THRESHOLD here first, and the free
 * wrap (-Wl,--wrap=free; operator delete funnels into free) returns arena
 * pointers here by address range. When the arena is full, callers fall back
 * to the general heap — today's behavior, never worse.
 *
 * The general heap's ceiling shrinks by PS2_BIG_ARENA_BYTES, and arena bytes
 * are invisible to mallinfo — heap-pressure guard lines (IsHeapUnderPressure,
 * the GetPNGImage ceiling) must subtract PS2_BIG_ARENA_BYTES.
 *
 * C linkage so C files (PakInterface path helpers) can call it too.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_BIG_ARENA_BYTES     (4u << 20)
/* Routing threshold. 128KB, not 256: the first arena build died on a 244KB
 * sanding expansion (434x144 cloud) that fell just under a 256KB threshold
 * into a general heap with 8.4MB free and no 244KB hole. */
#define PS2_BIG_ALLOC_THRESHOLD (128u << 10)
/* Last-resort floor: the new-wrap's rescue may park blocks this size or
 * bigger in the arena when the general heap still fails after purging.
 * Keeps tiny allocations out so a death spiral cannot shred the arena. */
#define PS2_BIG_RESCUE_FLOOR    (64u << 10)

#ifdef PS2_PLATFORM

/* First-fit allocation out of the arena; NULL when no block fits (caller
 * falls back to malloc). Thread-safe; 64-byte aligned payloads. */
void* Ps2BigAlloc(unsigned int theSize);

/* Non-zero when thePtr came from Ps2BigAlloc (address-range test; safe to
 * call with any pointer, including NULL, without taking the lock). */
int Ps2BigOwns(const void* thePtr);

/* Return a Ps2BigAlloc'd block. The free() wrap calls this automatically;
 * only code that bypasses free() needs it directly. */
void Ps2BigFree(void* thePtr);

/* Bytes currently allocated out of the arena (diagnostics). */
unsigned int Ps2BigUsedBytes(void);
unsigned int Ps2BigLargestFreeBytes(void);

#else /* !PS2_PLATFORM */

static inline void* Ps2BigAlloc(unsigned int theSize) { (void)theSize; return 0; }
static inline int Ps2BigOwns(const void* thePtr) { (void)thePtr; return 0; }
static inline void Ps2BigFree(void* thePtr) { (void)thePtr; }
static inline unsigned int Ps2BigUsedBytes(void) { return 0; }
static inline unsigned int Ps2BigLargestFreeBytes(void) { return 0; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* PS2BIGARENA_H */
