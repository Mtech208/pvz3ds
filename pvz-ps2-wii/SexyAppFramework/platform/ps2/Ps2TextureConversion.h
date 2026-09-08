#pragma once

#ifdef PS2_PLATFORM

#include <stdint.h>

// Canonical source decoding used by the PS2 texture backends. Keeping API
// enums out of this helper lets the conversion code remain reusable by native
// and compatibility upload paths.
enum Ps2SourcePixelFormat
{
	PS2_SRC_BGRA8888 = 0,
	PS2_SRC_RGBA8888,
	PS2_SRC_BGR888,
	PS2_SRC_RGB888,
	PS2_SRC_L8,
	PS2_SRC_LA88,
	PS2_SRC_BGRA4444_REV,
	PS2_SRC_RGB565
};

static inline int Ps2SourcePixelBytes(Ps2SourcePixelFormat format)
{
	switch (format)
	{
		case PS2_SRC_BGRA4444_REV:
		case PS2_SRC_RGB565:
			return 2;
		case PS2_SRC_L8:
			return 1;
		case PS2_SRC_LA88:
			return 2;
		case PS2_SRC_BGR888:
		case PS2_SRC_RGB888:
			return 3;
		default:
			return 4;
	}
}

// Decode one source texel to canonical BGRA8. This is also the byte ordering
// expected by Ps2RgbaToPsmct32/Ps2RgbaToPsmct16 below.
static inline void Ps2DecodeSourcePixel(const uint8_t* src, Ps2SourcePixelFormat format, uint8_t bgra[4])
{
	switch (format)
	{
		case PS2_SRC_RGBA8888:
			bgra[0] = src[2]; bgra[1] = src[1]; bgra[2] = src[0]; bgra[3] = src[3];
			break;
		case PS2_SRC_BGR888:
			bgra[0] = src[0]; bgra[1] = src[1]; bgra[2] = src[2]; bgra[3] = 255;
			break;
		case PS2_SRC_RGB888:
			bgra[0] = src[2]; bgra[1] = src[1]; bgra[2] = src[0]; bgra[3] = 255;
			break;
		case PS2_SRC_L8:
			bgra[0] = src[0]; bgra[1] = src[0]; bgra[2] = src[0]; bgra[3] = 255;
			break;
		case PS2_SRC_LA88:
			bgra[0] = src[0]; bgra[1] = src[0]; bgra[2] = src[0]; bgra[3] = src[1];
			break;
		case PS2_SRC_BGRA4444_REV:
		{
			const uint16_t v = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
			bgra[0] = (uint8_t)((v & 0x000F) * 17);
			bgra[1] = (uint8_t)(((v >> 4) & 0x000F) * 17);
			bgra[2] = (uint8_t)(((v >> 8) & 0x000F) * 17);
			bgra[3] = (uint8_t)(((v >> 12) & 0x000F) * 17);
			break;
		}
		case PS2_SRC_RGB565:
		{
			const uint16_t v = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
			const uint8_t r5 = (uint8_t)((v >> 11) & 31);
			const uint8_t g6 = (uint8_t)((v >> 5) & 63);
			const uint8_t b5 = (uint8_t)(v & 31);
			bgra[0] = (uint8_t)((b5 << 3) | (b5 >> 2));
			bgra[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
			bgra[2] = (uint8_t)((r5 << 3) | (r5 >> 2));
			bgra[3] = 255;
			break;
		}
		case PS2_SRC_BGRA8888:
		default:
			bgra[0] = src[0]; bgra[1] = src[1]; bgra[2] = src[2]; bgra[3] = src[3];
			break;
	}
}

// Canonical BGRA8 -> gsKit PSMCT32 / PSMCT16.
static inline uint32_t Ps2RgbaToPsmct32(const uint8_t* src)
{
	return ((uint32_t)(src[3] >> 1) << 24) |
		   ((uint32_t)src[0]        << 16) |
		   ((uint32_t)src[1]        <<  8) |
			(uint32_t)src[2];
}

static inline uint16_t Ps2RgbaToPsmct16(const uint8_t* src)
{
	uint16_t r = (uint16_t)(src[2] >> 3);
	uint16_t g = (uint16_t)(src[1] >> 3);
	uint16_t b = (uint16_t)(src[0] >> 3);
	uint16_t a = (uint16_t)(src[3] >= 128 ? 1 : 0);
	return (uint16_t)((a << 15) | (b << 10) | (g << 5) | r);
}

int Ps2ClutCsm1Pos(int index);
int Ps2ClutCsm1PosT4(int index);
void Ps2PackT4Indices(const uint8_t* indices, unsigned int npx, uint8_t* packed);
int Ps2PalettizeT8(const uint8_t* src, unsigned int npx, uint8_t* idx, unsigned int* paletteCT32);
int Ps2PalettizeT8Source(const void* src, unsigned int npx, Ps2SourcePixelFormat format,
	uint8_t* idx, unsigned int* paletteCT32);
int Ps2Ct16Dist(uint16_t a, unsigned int b32);

#endif
