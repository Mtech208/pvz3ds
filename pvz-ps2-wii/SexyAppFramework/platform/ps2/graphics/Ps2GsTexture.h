#pragma once

#ifdef PS2_PLATFORM

#include <gsKit.h>
#include <stdint.h>

#include "Ps2TextureConversion.h"

struct Ps2GsTexture
{
	GSTEXTURE mGs;
	uint8_t* mIndices;
	uint32_t* mClut;
	uint32_t mVramSize;
	uint32_t mClutVramSize;
	uint32_t mLastUsed;
	// Residency order is kept as an intrusive MRU-to-LRU list so tracking,
	// untracking and victim selection are all constant time. mLastUsed is still
	// maintained because it is the only human-readable age in a VRAM dump.
	Ps2GsTexture* mLruPrev;
	Ps2GsTexture* mLruNext;
	uint64_t mClutHash;
	int mClutEntryCount;
	int mLogicalWidth;
	int mLogicalHeight;
	uint8_t mShrink;
	bool mClutShared;
	bool mUploaded;
	bool mValid;
};

void Ps2GsTextureInit(Ps2GsTexture* texture);
void Ps2GsTextureRelease(Ps2GsTexture* texture);
bool Ps2GsTextureSetPixels(Ps2GsTexture* texture, const void* pixels, int width, int height,
	int pitchBytes, Ps2SourcePixelFormat format);
bool Ps2GsTextureSetIndexed(Ps2GsTexture* texture, const uint8_t* indices, int width, int height,
	int pitch, const uint32_t* palette, int paletteCount);
void Ps2GsTextureSetLinearFilter(Ps2GsTexture* texture, bool linear);
bool Ps2GsTextureEnsureResident(Ps2GsTexture* texture);
bool Ps2GsTextureRead(const Ps2GsTexture* texture, int x, int y, int width, int height,
	uint32_t* output, int outputPitchPixels);

#endif
