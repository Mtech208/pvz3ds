#pragma once

// Low-memory console features shared by engine code. Platform checks live here
// so image/resource code can branch on a capability instead of a console name.
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
#define SEXY_LAZY_IMAGES
#define SEXY_COMPACT_IMAGES
#endif

// Only the PS2 texture upload backend consumes 24-bit RGB image storage.
#ifdef PS2_PLATFORM
#define SEXY_COMPACT_RGB_IMAGES
#endif
