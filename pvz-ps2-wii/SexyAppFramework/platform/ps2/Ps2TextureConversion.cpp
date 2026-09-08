#ifdef PS2_PLATFORM

#include "Ps2TextureConversion.h"

#include <stdlib.h>
#include <string.h>

int Ps2ClutCsm1Pos(int i)
{
	return (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
}

int Ps2ClutCsm1PosT4(int i)
{
	// CSM1 addresses CLUT entries in 8x2 tiles. The T8 mapping above swaps the
	// index bit selecting x>=8 with the one selecting an odd y, which is what
	// tiles a linear 256-entry palette across the 16x16 CT32 CLUT region.
	// A T4 palette is uploaded as a single 8x2 CT32 region, so the tile and the
	// buffer coincide and the mapping degenerates to the identity. Reusing the
	// T8 mapping here would emit position 16 for index 8 and run off the end of
	// the 16-entry buffer.
	return i;
}

void Ps2PackT4Indices(const uint8_t* indices, unsigned int npx, uint8_t* packed)
{
	if (!indices || !packed)
		return;

	// PSMT4 stores two indices per byte with the even (left) pixel in the low
	// nibble. An odd final pixel leaves the high nibble as padding.
	const unsigned int pairs = npx >> 1;
	for (unsigned int p = 0; p < pairs; p++)
		packed[p] = (uint8_t)((indices[p * 2] & 0x0F) | ((indices[p * 2 + 1] & 0x0F) << 4));

	if (npx & 1u)
		packed[pairs] = (uint8_t)(indices[npx - 1] & 0x0F);
}

static uint16_t Ps2Key4444(const uint8_t* src)
{
	return (uint16_t)(((uint16_t)(src[3] >> 4) << 12) |
					 ((uint16_t)(src[0] >> 4) <<  8) |
					 ((uint16_t)(src[1] >> 4) <<  4) |
					  (uint16_t)(src[2] >> 4));
}

static int Ps2KeyDist(uint16_t a, uint16_t b)
{
	int dr = (int)(a & 15)        - (int)(b & 15);
	int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
	int db = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
	int da = (int)(a >> 12)       - (int)(b >> 12);
	return dr*dr + dg*dg + db*db + da*da*4;
}

int Ps2PalettizeT8(const uint8_t* src, unsigned int npx, uint8_t* idx, unsigned int* paletteCT32)
{
	// Static scratch (640KB): allocating these per call failed outright once
	// the heap neared its ceiling, which dropped the whole texture (white
	// blob). Uploads are serialized on the render thread, so one set is safe.
	static uint32_t hist[65536];
	static uint16_t lut[65536];
	static uint32_t rep[65536];
	memset(hist, 0, sizeof(hist));

	for (uint32_t p = 0; p < npx; p++)
	{
		uint16_t k = Ps2Key4444(src + p * 4);
		if (!hist[k])
			rep[k] = Ps2RgbaToPsmct32(src + p * 4);
		hist[k]++;
	}

	uint16_t palKey[256];
	int nPal = 0;
	for (int v = 0; v < 65536; v++)
	{
		if (!hist[v])
			continue;
		if (nPal < 256)
		{
			lut[v] = (uint16_t)nPal;
			palKey[nPal] = (uint16_t)v;
			paletteCT32[nPal++] = rep[v];
		}
		else
		{
			nPal = 257;
			break;
		}
	}

	if (nPal > 256)
	{
		nPal = 0;
		for (int k = 0; k < 256; k++)
		{
			uint32_t bestCount = 0;
			int bestVal = -1;
			for (int v = 0; v < 65536; v++)
			{
				if (hist[v] > bestCount)
				{
					bestCount = hist[v];
					bestVal = v;
				}
			}
			if (bestVal < 0)
				break;
			hist[bestVal] = 0;
			lut[bestVal] = (uint16_t)nPal;
			palKey[nPal] = (uint16_t)bestVal;
			paletteCT32[nPal++] = rep[bestVal];
		}

		for (int v = 0; v < 65536; v++)
		{
			if (!hist[v])
				continue;
			int best = 0;
			int bestD = 0x7FFFFFFF;
			for (int k = 0; k < nPal; k++)
			{
				int d = Ps2KeyDist((uint16_t)v, palKey[k]);
				if (d < bestD)
				{
					bestD = d;
					best = k;
				}
			}
			lut[v] = (uint16_t)best;
		}
	}

	for (uint32_t p = 0; p < npx; p++)
		idx[p] = (uint8_t)lut[Ps2Key4444(src + p * 4)];

	return nPal;
}

int Ps2PalettizeT8Source(const void* pixels, unsigned int npx, Ps2SourcePixelFormat format,
	uint8_t* idx, unsigned int* paletteCT32)
{
	if (!pixels || !idx || !paletteCT32)
		return 0;

	// Same histogram/LUT strategy as Ps2PalettizeT8, but decode each source
	// texel on demand. This avoids allocating a temporary RGBA32 image merely
	// because the GL caller uploaded RGB565/RGBA4444/RGBA8.
	static uint32_t hist[65536];
	static uint16_t lut[65536];
	static uint32_t rep[65536];
	memset(hist, 0, sizeof(hist));

	const uint8_t* src = (const uint8_t*)pixels;
	const int bpp = Ps2SourcePixelBytes(format);
	uint8_t bgra[4];
	for (uint32_t p = 0; p < npx; p++)
	{
		Ps2DecodeSourcePixel(src + p * bpp, format, bgra);
		uint16_t k = Ps2Key4444(bgra);
		if (!hist[k])
			rep[k] = Ps2RgbaToPsmct32(bgra);
		hist[k]++;
	}

	uint16_t palKey[256];
	int nPal = 0;
	for (int v = 0; v < 65536; v++)
	{
		if (!hist[v])
			continue;
		if (nPal < 256)
		{
			lut[v] = (uint16_t)nPal;
			palKey[nPal] = (uint16_t)v;
			paletteCT32[nPal++] = rep[v];
		}
		else
		{
			nPal = 257;
			break;
		}
	}

	if (nPal > 256)
	{
		nPal = 0;
		for (int k = 0; k < 256; k++)
		{
			uint32_t bestCount = 0;
			int bestVal = -1;
			for (int v = 0; v < 65536; v++)
			{
				if (hist[v] > bestCount)
				{
					bestCount = hist[v];
					bestVal = v;
				}
			}
			if (bestVal < 0)
				break;
			hist[bestVal] = 0;
			lut[bestVal] = (uint16_t)nPal;
			palKey[nPal] = (uint16_t)bestVal;
			paletteCT32[nPal++] = rep[bestVal];
		}

		for (int v = 0; v < 65536; v++)
		{
			if (!hist[v])
				continue;
			int best = 0;
			int bestD = 0x7FFFFFFF;
			for (int k = 0; k < nPal; k++)
			{
				int d = Ps2KeyDist((uint16_t)v, palKey[k]);
				if (d < bestD)
				{
					bestD = d;
					best = k;
				}
			}
			lut[v] = (uint16_t)best;
		}
	}

	for (uint32_t p = 0; p < npx; p++)
	{
		Ps2DecodeSourcePixel(src + p * bpp, format, bgra);
		idx[p] = (uint8_t)lut[Ps2Key4444(bgra)];
	}
	return nPal;
}

int Ps2Ct16Dist(uint16_t a, unsigned int b32)
{
	uint16_t b = 0;

	uint8_t r = (b32 >> 0) & 0xFF;
	uint8_t g = (b32 >> 8) & 0xFF;
	uint8_t bl = (b32 >> 16) & 0xFF;
	uint8_t a8 = (b32 >> 24) & 0xFF;

	b |= (r >> 3);
	b |= (g >> 3) << 5;
	b |= (bl >> 3) << 10;
	b |= (a8 >= 128) ? 0x8000 : 0;

	int dr = ((a >> 0) & 31) - ((b >> 0) & 31);
	int dg = ((a >> 5) & 31) - ((b >> 5) & 31);
	int db = ((a >> 10) & 31) - ((b >> 10) & 31);
	int da = ((a >> 15) & 1) - ((b >> 15) & 1);

	return dr*dr + dg*dg + db*db + da*64;
}

#endif
