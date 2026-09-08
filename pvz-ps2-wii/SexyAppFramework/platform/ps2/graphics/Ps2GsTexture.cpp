#ifdef PS2_PLATFORM

#include "Ps2GsTexture.h"

#include <gsCore.h>
#include <gsTexture.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "Ps2GsClutCache.h"
#include "Ps2GsConfig.h"
#include "Ps2GsRenderer.h"
#include "Ps2RenderStats.h"
#include "Ps2VramAllocator.h"

extern GSGLOBAL* gsGlobal;

// Residency is an intrusive list ordered most-recently-used first. The previous
// flat array had to be scanned to track, untrack and pick a victim, which is
// O(resident) per eviction and ran up to PS2_GS_MAX_RESIDENT_TEXTURES entries
// while the frame was already stalling on VRAM pressure.
static Ps2GsTexture* sLruHead = NULL;
static Ps2GsTexture* sLruTail = NULL;
static int sResidentTextureCount = 0;
static uint32_t sTextureUseSerial = 1;
static uint32_t sUploadTotal = 0;
static uint32_t sEvictionTotal = 0;
static uint32_t sArenaRewindTotal = 0;
static uint32_t sUploadFrame = 0;
static uint32_t sEvictionFrame = 0;
static uint32_t sUploadFrameLast = 0;
static uint32_t sEvictionFrameLast = 0;

static bool Ps2GsTextureIsLinked(const Ps2GsTexture* texture)
{
	return texture == sLruHead || texture->mLruPrev != NULL;
}

static void Ps2GsTextureUntrack(Ps2GsTexture* texture)
{
	if (!Ps2GsTextureIsLinked(texture))
		return;

	if (texture->mLruPrev)
		texture->mLruPrev->mLruNext = texture->mLruNext;
	else
		sLruHead = texture->mLruNext;

	if (texture->mLruNext)
		texture->mLruNext->mLruPrev = texture->mLruPrev;
	else
		sLruTail = texture->mLruPrev;

	texture->mLruPrev = NULL;
	texture->mLruNext = NULL;
	--sResidentTextureCount;
}

static void Ps2GsTextureTrack(Ps2GsTexture* texture)
{
	if (Ps2GsTextureIsLinked(texture))
		return;

	texture->mLruPrev = NULL;
	texture->mLruNext = sLruHead;
	if (sLruHead)
		sLruHead->mLruPrev = texture;
	sLruHead = texture;
	if (!sLruTail)
		sLruTail = texture;
	++sResidentTextureCount;
}

static void Ps2GsTextureTouch(Ps2GsTexture* texture)
{
	texture->mLastUsed = sTextureUseSerial++;
	if (texture == sLruHead)
		return;
	Ps2GsTextureUntrack(texture);
	Ps2GsTextureTrack(texture);
}

static void Ps2GsTextureDropResidencyRaw(Ps2GsTexture* texture)
{
	if (!texture)
		return;

	Ps2GsTextureUntrack(texture);
	if (texture->mUploaded)
	{
		Ps2VramFree(texture->mGs.Vram, texture->mVramSize);
		// A CLUT can back several textures, so it only goes back to the VRAM
		// allocator once the last reference is gone. Addresses the cache never
		// tracked report as releasable, which covers privately owned palettes.
		if (Ps2GsClutCacheRelease(texture->mGs.VramClut))
			Ps2VramFree(texture->mGs.VramClut, texture->mClutVramSize);
		++sEvictionTotal;
		++sEvictionFrame;
	}
	Ps2GsRendererInvalidateTextureState(texture);
	texture->mGs.Vram = GSKIT_ALLOC_ERROR;
	texture->mGs.VramClut = GSKIT_ALLOC_ERROR;
	texture->mClutShared = false;
	texture->mUploaded = false;
}

static void Ps2GsTextureDropResidency(Ps2GsTexture* texture)
{
	if (!texture || !texture->mUploaded)
	{
		Ps2GsTextureDropResidencyRaw(texture);
		return;
	}

	Ps2GsRendererSyncQueue();
	Ps2GsTextureDropResidencyRaw(texture);
}

static Ps2GsTexture* Ps2GsTextureFindLru(Ps2GsTexture* protectedTexture)
{
	// Walk back from the tail so the protected texture cannot stall eviction.
	for (Ps2GsTexture* candidate = sLruTail; candidate; candidate = candidate->mLruPrev)
	{
		if (candidate != protectedTexture && candidate->mUploaded)
			return candidate;
	}
	return NULL;
}

static void Ps2GsTextureEvictAll()
{
	Ps2GsRendererSyncQueue();
	Ps2GsRendererInvalidateTextureState(NULL);
	for (Ps2GsTexture* texture = sLruHead; texture; )
	{
		Ps2GsTexture* next = texture->mLruNext;
		texture->mGs.Vram = GSKIT_ALLOC_ERROR;
		texture->mGs.VramClut = GSKIT_ALLOC_ERROR;
		texture->mClutShared = false;
		if (texture->mUploaded)
		{
			++sEvictionTotal;
			++sEvictionFrame;
		}
		texture->mUploaded = false;
		texture->mLruPrev = NULL;
		texture->mLruNext = NULL;
		texture = next;
	}
	sLruHead = NULL;
	sLruTail = NULL;
	sResidentTextureCount = 0;
	// Rewinding the arena invalidates every recorded CLUT address at once.
	Ps2GsClutCacheReset();
	Ps2VramEvictAll();
	++sArenaRewindTotal;
}

static size_t Ps2GsTextureAlignedIndexBytes(size_t bytes)
{
	return (bytes + 15u) & ~15u;
}

static uint8_t Ps2GsTextureChooseShrink(int width, int height)
{
	if (PS2_GS_TEXTURE_SHRINK_SHIFT <= 0)
		return 0;
	const int block = 1 << PS2_GS_TEXTURE_SHRINK_SHIFT;
	return (width >= block && height >= block) ? PS2_GS_TEXTURE_SHRINK_SHIFT : 0;
}

static bool Ps2GsTextureDownscale(const void* pixels, int width, int height, int pitchBytes,
	Ps2SourcePixelFormat format, uint8_t shrink, uint8_t** outputPixels, int* outputWidth, int* outputHeight)
{
	const int bytesPerPixel = Ps2SourcePixelBytes(format);
	if (!pixels || width <= 0 || height <= 0 || pitchBytes < width * bytesPerPixel)
		return false;

	if (shrink == 0)
	{
		const size_t bytes = (size_t)width * height * 4;
		uint8_t* converted = (uint8_t*)malloc(bytes);
		if (!converted)
			return false;
		for (int y = 0; y < height; ++y)
		{
			const uint8_t* src = (const uint8_t*)pixels + y * pitchBytes;
			for (int x = 0; x < width; ++x)
				Ps2DecodeSourcePixel(src + x * bytesPerPixel, format, converted + (y * width + x) * 4);
		}
		*outputPixels = converted;
		*outputWidth = width;
		*outputHeight = height;
		return true;
	}

	const int block = 1 << shrink;
	const int targetWidth = (width + block - 1) >> shrink;
	const int targetHeight = (height + block - 1) >> shrink;
	uint8_t* converted = (uint8_t*)malloc((size_t)targetWidth * targetHeight * 4);
	if (!converted)
		return false;

	for (int y = 0; y < targetHeight; ++y)
	{
		for (int x = 0; x < targetWidth; ++x)
		{
			uint32_t sumAlpha = 0;
			uint32_t weightedBlue = 0;
			uint32_t weightedGreen = 0;
			uint32_t weightedRed = 0;
			uint32_t samples = 0;
			uint8_t first[4] = {0, 0, 0, 0};

			for (int by = 0; by < block; ++by)
			{
				const int sy = y * block + by;
				if (sy >= height)
					break;
				const uint8_t* row = (const uint8_t*)pixels + sy * pitchBytes;
				for (int bx = 0; bx < block; ++bx)
				{
					const int sx = x * block + bx;
					if (sx >= width)
						break;
					uint8_t bgra[4];
					Ps2DecodeSourcePixel(row + sx * bytesPerPixel, format, bgra);
					if (samples == 0)
						memcpy(first, bgra, sizeof(first));
					sumAlpha += bgra[3];
					weightedBlue += bgra[0] * bgra[3];
					weightedGreen += bgra[1] * bgra[3];
					weightedRed += bgra[2] * bgra[3];
					++samples;
				}
			}

			uint8_t* dst = converted + (y * targetWidth + x) * 4;
			if (sumAlpha)
			{
				dst[0] = (uint8_t)(weightedBlue / sumAlpha);
				dst[1] = (uint8_t)(weightedGreen / sumAlpha);
				dst[2] = (uint8_t)(weightedRed / sumAlpha);
			}
			else if (samples)
			{
				dst[0] = first[0];
				dst[1] = first[1];
				dst[2] = first[2];
			}
			else
			{
				dst[0] = dst[1] = dst[2] = 0;
			}
			dst[3] = samples ? (uint8_t)(sumAlpha / samples) : 0;
		}
	}

	*outputPixels = converted;
	*outputWidth = targetWidth;
	*outputHeight = targetHeight;
	return true;
}

static bool Ps2GsTextureFinishBuild(Ps2GsTexture* texture, uint8_t* indices, uint32_t* clut,
	const unsigned int* linearPalette, int paletteCount,
	int logicalWidth, int logicalHeight, int storedWidth, int storedHeight, uint8_t shrink)
{
	if (!texture || !indices || !clut || !linearPalette || paletteCount <= 0)
		return false;

	// A palette that fits in 16 entries stores losslessly as PSMT4: half the
	// index bytes, and a 256-byte CLUT block instead of 1 KiB. Wider palettes
	// stay PSMT8 because quantizing them further would be visible.
	const bool useT4 = paletteCount <= 16;
	const int clutEntries = useT4 ? 16 : 256;

	// The CSM1 layout depends on the format, so the palette arrives linear and
	// is placed here rather than by the callers.
	memset(clut, 0, 256 * sizeof(uint32_t));
	for (int i = 0; i < paletteCount; ++i)
		clut[useT4 ? Ps2ClutCsm1PosT4(i) : Ps2ClutCsm1Pos(i)] = linearPalette[i];

	if (useT4)
	{
		// Packing shrinks the buffer in place; the destination byte index never
		// runs ahead of the indices still to be read.
		Ps2PackT4Indices(indices, (unsigned int)(storedWidth * storedHeight), indices);
	}

	texture->mIndices = indices;
	texture->mClut = clut;
	texture->mLogicalWidth = logicalWidth;
	texture->mLogicalHeight = logicalHeight;
	texture->mShrink = shrink;
	texture->mGs.Width = storedWidth;
	texture->mGs.Height = storedHeight;
	texture->mGs.PSM = useT4 ? GS_PSM_T4 : GS_PSM_T8;
	texture->mGs.ClutPSM = GS_PSM_CT32;
	texture->mGs.ClutStorageMode = GS_CLUT_STORAGE_CSM1;
	texture->mGs.Filter = GS_FILTER_NEAREST;
	texture->mGs.Mem = (u32*)indices;
	texture->mGs.Clut = (u32*)clut;
	texture->mGs.Vram = GSKIT_ALLOC_ERROR;
	texture->mGs.VramClut = GSKIT_ALLOC_ERROR;
	gsKit_setup_tbw(&texture->mGs);
	texture->mVramSize = gsKit_texture_size(storedWidth, storedHeight, texture->mGs.PSM);
	texture->mClutVramSize = useT4
		? gsKit_texture_size(8, 2, GS_PSM_CT32)
		: gsKit_texture_size(16, 16, GS_PSM_CT32);
	texture->mClutEntryCount = clutEntries;
	texture->mClutHash = Ps2GsClutHash(clut, clutEntries);
	texture->mClutShared = false;
	texture->mUploaded = false;
	texture->mValid = true;
	return true;
}

void Ps2GsTextureInit(Ps2GsTexture* texture)
{
	if (!texture)
		return;
	memset(texture, 0, sizeof(*texture));
	texture->mGs.Vram = GSKIT_ALLOC_ERROR;
	texture->mGs.VramClut = GSKIT_ALLOC_ERROR;
	texture->mGs.Filter = GS_FILTER_NEAREST;
}

void Ps2GsTextureRelease(Ps2GsTexture* texture)
{
	if (!texture)
		return;
	Ps2GsTextureDropResidency(texture);
	free(texture->mIndices);
	free(texture->mClut);
	Ps2GsTextureInit(texture);
}

bool Ps2GsTextureSetPixels(Ps2GsTexture* texture, const void* pixels, int width, int height,
	int pitchBytes, Ps2SourcePixelFormat format)
{
	if (!texture || !pixels || width <= 0 || height <= 0)
		return false;

	const uint8_t shrink = Ps2GsTextureChooseShrink(width, height);
	uint8_t* converted = NULL;
	int storedWidth = 0;
	int storedHeight = 0;
	if (!Ps2GsTextureDownscale(pixels, width, height, pitchBytes, format, shrink,
		&converted, &storedWidth, &storedHeight))
		return false;

	const unsigned int pixelCount = (unsigned int)(storedWidth * storedHeight);
	const size_t indexBytes = Ps2GsTextureAlignedIndexBytes(pixelCount);
	uint8_t* indices = (uint8_t*)memalign(64, indexBytes);
	uint32_t* clut = (uint32_t*)memalign(64, 256 * sizeof(uint32_t));
	if (!indices || !clut)
	{
		free(converted);
		free(indices);
		free(clut);
		return false;
	}
	memset(indices, 0, indexBytes);
	unsigned int linearPalette[256];
	memset(linearPalette, 0, sizeof(linearPalette));
	const int paletteCount = Ps2PalettizeT8Source(converted, pixelCount, PS2_SRC_BGRA8888,
		indices, linearPalette);
	if (paletteCount <= 0)
	{
		free(converted);
		free(indices);
		free(clut);
		return false;
	}
	free(converted);

	Ps2GsTextureRelease(texture);
	return Ps2GsTextureFinishBuild(texture, indices, clut, linearPalette, paletteCount,
		width, height, storedWidth, storedHeight, shrink);
}

bool Ps2GsTextureSetIndexed(Ps2GsTexture* texture, const uint8_t* indices, int width, int height,
	int pitch, const uint32_t* palette, int paletteCount)
{
	if (!texture || !indices || !palette || width <= 0 || height <= 0 || pitch < width)
		return false;

	const uint8_t shrink = Ps2GsTextureChooseShrink(width, height);
	const int block = 1 << shrink;
	const int storedWidth = (width + block - 1) >> shrink;
	const int storedHeight = (height + block - 1) >> shrink;
	const size_t indexBytes = Ps2GsTextureAlignedIndexBytes((size_t)storedWidth * storedHeight);
	uint8_t* storedIndices = (uint8_t*)memalign(64, indexBytes);
	uint32_t* clut = (uint32_t*)memalign(64, 256 * sizeof(uint32_t));
	if (!storedIndices || !clut)
	{
		free(storedIndices);
		free(clut);
		return false;
	}

	memset(storedIndices, 0, indexBytes);
	int highestIndex = 0;
	for (int y = 0; y < storedHeight; ++y)
	{
		const int sy = y * block < height ? y * block : height - 1;
		for (int x = 0; x < storedWidth; ++x)
		{
			const int sx = x * block < width ? x * block : width - 1;
			const uint8_t value = indices[sy * pitch + sx];
			if ((int)value > highestIndex)
				highestIndex = (int)value;
			storedIndices[y * storedWidth + x] = value;
		}
	}

	if (paletteCount > 256)
		paletteCount = 256;
	unsigned int linearPalette[256];
	memset(linearPalette, 0, sizeof(linearPalette));
	for (int i = 0; i < paletteCount; ++i)
	{
		const uint32_t argb = palette[i];
		const uint8_t bgra[4] = {
			(uint8_t)(argb & 0xFF),
			(uint8_t)((argb >> 8) & 0xFF),
			(uint8_t)((argb >> 16) & 0xFF),
			(uint8_t)((argb >> 24) & 0xFF)
		};
		linearPalette[i] = Ps2RgbaToPsmct32(bgra);
	}

	// Indexed sources are trusted to stay inside their palette, but a stored
	// index above the declared count would silently wrap to another colour once
	// packed into a nibble. Widen the count so such a texture stays PSMT8.
	if (highestIndex >= paletteCount)
		paletteCount = highestIndex + 1;

	Ps2GsTextureRelease(texture);
	return Ps2GsTextureFinishBuild(texture, storedIndices, clut, linearPalette, paletteCount,
		width, height, storedWidth, storedHeight, shrink);
}

void Ps2GsTextureSetLinearFilter(Ps2GsTexture* texture, bool linear)
{
	if (!texture)
		return;

	const u32 filter = linear ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
	if (texture->mGs.Filter == filter)
		return;

	Ps2GsRendererFlushBatch();
	texture->mGs.Filter = filter;
}

static bool Ps2GsTextureCommitUpload(Ps2GsTexture* texture, uint32_t textureVram)
{
	// Reuse a resident CLUT holding the same palette when there is one. gsKit
	// cannot transfer pixels without their CLUT, so a shared palette is re-sent
	// to the address it already occupies: identical bytes, and the cost is
	// upload bandwidth only, never a per-draw cost.
	uint32_t clutVram = GSKIT_ALLOC_ERROR;
	const bool shared = Ps2GsClutCacheAcquire(texture->mClut, texture->mClutEntryCount,
		texture->mClutHash, &clutVram);
	if (!shared)
	{
		clutVram = Ps2VramAlloc(texture->mClutVramSize);
		if (clutVram == GSKIT_ALLOC_ERROR)
			return false;
	}

	texture->mGs.Vram = textureVram;
	texture->mGs.VramClut = clutVram;
	texture->mGs.Mem = (u32*)texture->mIndices;
	texture->mGs.Clut = (u32*)texture->mClut;
	gsKit_texture_upload(gsGlobal, &texture->mGs);
	Ps2GsRendererInvalidateTextureState(texture);
	texture->mGs.Mem = NULL;
	texture->mUploaded = true;
	texture->mClutShared = shared ||
		Ps2GsClutCacheInsert(texture->mClut, texture->mClutEntryCount, texture->mClutHash,
			clutVram, texture->mClutVramSize);
	Ps2GsTextureTrack(texture);
	texture->mLastUsed = sTextureUseSerial++;
	++sUploadTotal;
	++sUploadFrame;
	return true;
}

bool Ps2GsTextureEnsureResident(Ps2GsTexture* texture)
{
	if (!gsGlobal || !texture || !texture->mValid || !texture->mIndices || !texture->mClut)
		return false;

	if (texture->mUploaded)
	{
		Ps2GsTextureTouch(texture);
		return true;
	}

	bool evictionSynchronized = false;
	for (;;)
	{
		const uint32_t textureVram = Ps2VramAlloc(texture->mVramSize);
		if (textureVram != GSKIT_ALLOC_ERROR)
		{
			if (Ps2GsTextureCommitUpload(texture, textureVram))
				return true;
			Ps2VramFree(textureVram, texture->mVramSize);
		}

		Ps2GsTexture* lru = Ps2GsTextureFindLru(texture);
		if (!lru)
			break;
		if (!evictionSynchronized)
		{
			Ps2GsRendererSyncQueue();
			evictionSynchronized = true;
		}
		Ps2GsTextureDropResidencyRaw(lru);
	}

	Ps2GsTextureEvictAll();
	const uint32_t textureVram = Ps2VramAlloc(texture->mVramSize);
	if (textureVram == GSKIT_ALLOC_ERROR)
		return false;
	if (Ps2GsTextureCommitUpload(texture, textureVram))
		return true;

	Ps2VramFree(textureVram, texture->mVramSize);
	return false;
}

// Per-frame residency counters. A steady upload count above zero once a scene
// is running means the working set does not fit and every draw is paying for a
// re-upload plus the GS fence that eviction needs.
extern "C" void ps2_dbg_texture_frame_reset(void)
{
	sUploadFrameLast = sUploadFrame;
	sEvictionFrameLast = sEvictionFrame;
	sUploadFrame = 0;
	sEvictionFrame = 0;
}

extern "C" int ps2_dbg_texture_uploads_frame(void)
{
	return (int)sUploadFrameLast;
}

extern "C" int ps2_dbg_texture_evictions_frame(void)
{
	return (int)sEvictionFrameLast;
}

extern "C" int ps2_dbg_texture_uploads_total(void)
{
	return (int)sUploadTotal;
}

extern "C" int ps2_dbg_texture_evictions_total(void)
{
	return (int)sEvictionTotal;
}

extern "C" int ps2_dbg_texture_arena_rewinds(void)
{
	return (int)sArenaRewindTotal;
}

extern "C" int ps2_dbg_texture_resident_count(void)
{
	return sResidentTextureCount;
}

extern "C" int ps2_dbg_clut_shared_kb(void)
{
	return Ps2GsClutCacheSharedBytes() / 1024;
}

extern "C" int ps2_dbg_clut_unique_count(void)
{
	return Ps2GsClutCacheCount();
}

bool Ps2GsTextureRead(const Ps2GsTexture* texture, int x, int y, int width, int height,
	uint32_t* output, int outputPitchPixels)
{
	if (!texture || !texture->mValid || !texture->mIndices || !texture->mClut || !output ||
		x < 0 || y < 0 || width <= 0 || height <= 0 ||
		x + width > texture->mLogicalWidth || y + height > texture->mLogicalHeight)
		return false;

	const bool isT4 = texture->mGs.PSM == GS_PSM_T4;
	for (int row = 0; row < height; ++row)
	{
		uint32_t* dst = output + row * outputPitchPixels;
		const int sy = (y + row) >> texture->mShrink;
		for (int col = 0; col < width; ++col)
		{
			const int sx = (x + col) >> texture->mShrink;
			const int linear = sy * texture->mGs.Width + sx;
			uint8_t index;
			if (isT4)
			{
				// Nibbles are stored low first, matching Ps2PackT4Indices.
				const uint8_t packed = texture->mIndices[linear >> 1];
				index = (linear & 1) ? (uint8_t)(packed >> 4) : (uint8_t)(packed & 0x0F);
			}
			else
			{
				index = texture->mIndices[linear];
			}
			const uint32_t gsColor = texture->mClut[isT4
				? Ps2ClutCsm1PosT4(index) : Ps2ClutCsm1Pos(index)];
			const uint8_t red = (uint8_t)(gsColor & 0xFF);
			const uint8_t green = (uint8_t)((gsColor >> 8) & 0xFF);
			const uint8_t blue = (uint8_t)((gsColor >> 16) & 0xFF);
			const uint8_t gsAlpha = (uint8_t)((gsColor >> 24) & 0xFF);
			const uint8_t alpha = gsAlpha >= 0x80 ? 0xFF : (uint8_t)(gsAlpha << 1);
			dst[col] = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) |
				((uint32_t)green << 8) | blue;
		}
	}
	return true;
}

#endif
