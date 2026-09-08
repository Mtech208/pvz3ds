#pragma once
#ifdef PS2_PLATFORM

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Boot-time IOP services for the PvZ PS2 build: loads the memory-card and
// USB-mass IOP modules (embedded in the ELF) plus audsrv for sound, then
// decides where persistent data lives and where assets are read from.
//
// theArgv0 is the ELF boot path (argv[0]). Its device prefix tells us how the
// game was launched: "cdrom0:..." means we booted from a CD/DVD (or a mounted
// ISO), so assets must be read from the disc through the cdfs driver while
// saves go to a writable device. Pass "" if argv is unavailable.
//
// Non-CD boots use the ELF directory chosen by SexyAppBase. CD/DVD boots use
// "mass:/PVZ/" when USB is writable, then "mc0:/PVZ/" as a fallback.
void Ps2PvzInitServices(const char* theArgv0);

// Prefix ending in '/' for CD/DVD save fallback. Empty on normal USB/host
// boots, where SexyAppBase selects the ELF directory as the write location.
const char* Ps2GetSavePrefix(void);

// Prefix prepended to relative asset READ paths. Empty on host:/USB (assets
// live in the current directory). On a CD/DVD boot it is "cdfs:/", so the
// case-insensitive cdfs driver resolves "images/blank.tga" -> the disc's
// IMAGES/BLANK.TGA;1 without the game changing any of its path strings.
const char* Ps2GetResourcePrefix(void);

// True if the embedded audsrv module loaded (sound is available).
bool Ps2AudioAvailable(void);

// True if the USB mouse driver (ps2mouse.irx) loaded and its RPC server
// bound; Input.cpp only polls the mouse when this holds.
bool Ps2MouseAvailable(void);

// Asset-cache pressure policy for the EE heap. The game layer asks this
// instead of depending on mallinfo() or PS2 memory thresholds directly.
bool Ps2ShouldPurgeAssets(void);

#ifdef __cplusplus
}
#endif

#endif // PS2_PLATFORM
