#ifdef __3DS__
#include <3ds.h>
#include <stdint.h>

// Memory-config wiring for the 3DSX backend.
//
// 1) Double-underscore symbols (__stacksize__, __heap_size__,
//    __linear_heap_size__) are read by 3dsxtool to size the 3DSX header
//    (app grant). They are only metadata; libctru does not read them.
//
// 2) The __ctru_heap_size / __ctru_linear_heap_size variables ARE read by
//    libctru's __system_allocateHeaps() at startup (allocateHeaps.o):
//    heap = remaining APPMEM - linear, linear = the value we ship. A --defsym
//    does NOT work here because the allocator dereferences the symbol address;
//    a strong .data definition with the right name is the correct override.
//
// Rebalance: budget the app heap + linear heap so their SUM stays safely under
// the boot-time commit limit (svcControlMemory/svcBreak in __system_allocateHeaps
// panics whenever __ctru_heap_size + __ctru_linear_heap_size exceeds the
// remaining APPMEM commit, a razor edge that crashes at red-screen boot).
//
// Observed on this console:
 //   - commit limit ~56 MB, textures/render peak ~16.5 MB of linear heap.
 //   - auto split (both 0) caps std at 24 MB -> particles std::bad_alloc.
 //   - 40/16 (total 56 MB) red-screens at boot.
 // 32/20 = 52 MB used to ship, but particles still OOM'd: the MelonImpact
 // decompression demanded big contiguous std blocks (compressed record copy +
 // decompressed scratch) and the arena was fragmented. The load path now
 // decompresses straight from the pak record buffer (no double copy).
 // 34/20 = 54 MB: std maxed for particles; but the main menu then exhausts the
 // 20 MB linear (fonts ~16 MB + menu textures ~15 MB -> C3D_TexInit returns a
 // NULL data and the upload faults). Rebalance: the std peak is now small
 // (fast-path decode, ~8 MB resident at particles), so give linear the headroom
 // the menu actually needs:
 // Crash log shows std heap exhausted (208 KB free) while linear has 29 MB free.
 // Increase std heap to 30 MB, reduce linear to 24 MB (total 54 MB).
 extern "C" {
	__attribute__((section(".data"), used, visibility("default")))
	uint32_t __stacksize__ = 512 * 1024;

	__attribute__((section(".data"), used, visibility("default")))
	uint32_t __heap_size__ = 30 * 1024 * 1024;

	__attribute__((section(".data"), used, visibility("default")))
	uint32_t __linear_heap_size__ = 24 * 1024 * 1024;

	// Operative runtime values (read by libctru allocateHeaps.o):
	__attribute__((section(".data"), used))
	uint32_t __ctru_heap_size = 30 * 1024 * 1024;

	__attribute__((section(".data"), used))
	uint32_t __ctru_linear_heap_size = 24 * 1024 * 1024;
}
#endif