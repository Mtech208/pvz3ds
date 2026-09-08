#include <gsKit.h>
#include <gsMisc.h>

#include "graphics/Ps2GraphicsInterface.h"
#include "graphics/GLImage.h"
#include "graphics/Ps2GsConfig.h"
#include "graphics/Ps2GsRenderer.h"
#include "SexyAppBase.h"
#include "misc/AutoCrit.h"
#include "misc/CritSect.h"
#include "graphics/Graphics.h"
#include "graphics/MemoryImage.h"
#include "widget/WidgetManager.h"

#define GetColorFromTriVertex(theVertex, theColor) (theVertex.color?theVertex.color:theColor)

using namespace Sexy;

extern GSGLOBAL* gsGlobal;

static int gMinTextureWidth;
static int gMinTextureHeight;
static int gMaxTextureWidth;
static int gMaxTextureHeight;
static int gSupportedPixelFormats;
static bool gTextureSizeMustBePow2;
static int MAX_TEXTURE_SIZE = PS2_GS_MAX_TEXTURE_SIZE;
static bool gLinearFilter = false;

#define PIECE_SCRATCH_PIXELS (PS2_GS_MAX_TEXTURE_SIZE*PS2_GS_MAX_TEXTURE_SIZE)
static union
{
	uint32_t u32[PIECE_SCRATCH_PIXELS];
	uint16_t u16[PIECE_SCRATCH_PIXELS];
	uint8_t u8[PIECE_SCRATCH_PIXELS];
} gPieceScratch;

static void DrawVertexTriangle(Ps2GsTexture* texture, const Ps2Vertex& a, const Ps2Vertex& b, const Ps2Vertex& c)
{
	if (texture)
	{
		Ps2GsRendererDrawTexturedTriangle(texture,
			a.sx, a.sy, a.tu, a.tv, a.color,
			b.sx, b.sy, b.tu, b.tv, b.color,
			c.sx, c.sy, c.tu, c.tv, c.color);
	}
	else
	{
		Ps2GsRendererDrawTriangle(a.sx, a.sy, a.color, b.sx, b.sy, b.color, c.sx, c.sy, c.color);
	}
}

static void DrawVertexFan(const VertexList& vertices, Ps2GsTexture* texture)
{
	if (vertices.mSize < 3)
		return;
	for (int i = 1; i + 1 < vertices.mSize; ++i)
		DrawVertexTriangle(texture, vertices.mVerts[0], vertices.mVerts[i], vertices.mVerts[i + 1]);
}

static bool CopyImageToTexture8888(MemoryImage *theImage, int offx, int offy, int theWidth, int theHeight, int theDestPitch, int theDestHeight, bool rightPad, bool bottomPad, Ps2GsTexture* texture)
{
	if (theDestPitch * theDestHeight > PIECE_SCRATCH_PIXELS)
		return false;
	uint32_t *aDest = gPieceScratch.u32;

#ifdef PS2_PLATFORM
	if (theImage->mRGBBits != NULL)
	{
		uchar *srcRow = theImage->mRGBBits + (offy * theImage->GetWidth() + offx) * 3;
		uint32_t *dstRow = aDest;

		for(int y=0; y<theHeight; y++)
		{
			uchar *src = srcRow;
			uint32_t *dst = dstRow;
			for(int x=0; x<theWidth; x++)
			{
				uint32_t r = *src++;
				uint32_t g = *src++;
				uint32_t b = *src++;
				*dst++ = 0xFF000000 | (r << 16) | (g << 8) | b;
			}

			if (rightPad)
			{
				const uint32_t edge = *(dst - 1);
				while (dst < dstRow + theDestPitch)
					*dst++ = edge;
			}

			srcRow += theImage->GetWidth() * 3;
			dstRow += theDestPitch;
		}
	}
	else
#endif
	if (theImage->mColorTable == NULL)
	{
		uint32_t *aSrcBits = (uint32_t*)theImage->GetBits();
		if (aSrcBits == NULL)
			return false; // OOM during lazy expand; skip the piece instead of crashing
		uint32_t *srcRow = aSrcBits + offy * theImage->GetWidth() + offx;
		uint32_t *dstRow = aDest;

		for(int y=0; y<theHeight; y++)
		{
			uint32_t *src = srcRow;
			uint32_t *dst = dstRow;
			for(int x=0; x<theWidth; x++)
				*dst++ = *src++;

			if (rightPad)
			{
				const uint32_t edge = *(dst - 1);
				while (dst < dstRow + theDestPitch)
					*dst++ = edge;
			}

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}
	else // palette
	{
		uint8_t *srcRow = (uint8_t*)theImage->mColorIndices + offy * theImage->GetWidth() + offx;
		uint32_t *dstRow = aDest;
		uint32_t *palette = (uint32_t*)theImage->mColorTable;

		for(int y=0; y<theHeight; y++)
		{
			uint8_t *src = srcRow;
			uint32_t *dst = dstRow;
			for(int x=0; x<theWidth; x++)
				*dst++ = palette[*src++];

			if (rightPad)
			{
				const uint32_t edge = *(dst - 1);
				while (dst < dstRow + theDestPitch)
					*dst++ = edge;
			}

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}

	if (bottomPad)
	{
		for (int y = theHeight; y < theDestHeight; ++y)
			memcpy(aDest + y * theDestPitch, aDest + (theHeight - 1) * theDestPitch,
				theDestPitch * sizeof(uint32_t));
	}

	return Ps2GsTextureSetPixels(texture, aDest, theDestPitch, theDestHeight,
		theDestPitch * (int)sizeof(uint32_t), PS2_SRC_BGRA8888);
}

static bool CopyImageToTexture4444(MemoryImage *theImage, int offx, int offy, int theWidth, int theHeight, int theDestPitch, int theDestHeight, bool rightPad, bool bottomPad, Ps2GsTexture* texture)
{
	if (theDestPitch * theDestHeight > PIECE_SCRATCH_PIXELS)
		return false;
	uint16_t *aDest = gPieceScratch.u16;

	if (theImage->mColorTable == NULL)
	{
		uint32_t *aSrcBits = (uint32_t*)theImage->GetBits();
		if (aSrcBits == NULL)
			return false; // OOM during lazy expand; skip the piece instead of crashing
		uint32_t *srcRow = aSrcBits + offy * theImage->GetWidth() + offx;
		uint16_t *dstRow = aDest;

		for(int y=0; y<theHeight; y++)
		{
			uint32_t *src = srcRow;
			uint16_t *dst = dstRow;
			for(int x=0; x<theWidth; x++)
			{
				uint32_t aPixel = *src++;
				*dst++ = ((aPixel>>16)&0xF000) | ((aPixel>>12)&0x0F00) | ((aPixel>>8)&0x00F0) | ((aPixel>>4)&0x000F);
			}

			if (rightPad)
			{
				const uint16_t edge = *(dst - 1);
				while (dst < dstRow + theDestPitch)
					*dst++ = edge;
			}

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}
	else // palette
	{
		uint8_t *srcRow = (uint8_t*)theImage->mColorIndices + offy * theImage->GetWidth() + offx;
		uint16_t *dstRow = aDest;
		uint32_t *palette = (uint32_t*)theImage->mColorTable;

		for(int y=0; y<theHeight; y++)
		{
			uint8_t *src = srcRow;
			uint16_t *dst = dstRow;
			for(int x=0; x<theWidth; x++)
			{
				uint32_t aPixel = palette[*src++];
				*dst++ = ((aPixel>>16)&0xF000) | ((aPixel>>12)&0x0F00) | ((aPixel>>8)&0x00F0) | ((aPixel>>4)&0x000F);
			}

			if (rightPad)
			{
				const uint16_t edge = *(dst - 1);
				while (dst < dstRow + theDestPitch)
					*dst++ = edge;
			}

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}

	if (bottomPad)
	{
		for (int y = theHeight; y < theDestHeight; ++y)
			memcpy(aDest + y * theDestPitch, aDest + (theHeight - 1) * theDestPitch,
				theDestPitch * sizeof(uint16_t));
	}

	return Ps2GsTextureSetPixels(texture, aDest, theDestPitch, theDestHeight,
		theDestPitch * (int)sizeof(uint16_t), PS2_SRC_BGRA4444_REV);
}

static bool CopyImageToTexture565(MemoryImage *theImage, int offx, int offy, int theWidth, int theHeight, int theDestPitch, int theDestHeight, bool rightPad, bool bottomPad, Ps2GsTexture* texture)
{
	if (theDestPitch * theDestHeight > PIECE_SCRATCH_PIXELS)
		return false;
	uint16_t *aDest = gPieceScratch.u16;

	if (theImage->mColorTable == NULL)
	{
		uint32_t *aSrcBits = (uint32_t*)theImage->GetBits();
		if (aSrcBits == NULL)
			return false; // OOM during lazy expand; skip the piece instead of crashing
		uint32_t *srcRow = aSrcBits + offy * theImage->GetWidth() + offx;
		uint16_t *dstRow = aDest;

		for(int y=0; y<theHeight; y++)
		{
			uint32_t *src = srcRow;
			uint16_t *dst = dstRow;
			for(int x=0; x<theWidth; x++)
			{
				uint32_t aPixel = *src++;
				*dst++ = ((aPixel>>8)&0xF800) | ((aPixel>>5)&0x07E0) | ((aPixel>>3)&0x001F);
			}

			if (rightPad)
			{
				const uint16_t edge = *(dst - 1);
				while (dst < dstRow + theDestPitch)
					*dst++ = edge;
			}

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}
	else
	{
		uint8_t *srcRow = (uint8_t*)theImage->mColorIndices + offy * theImage->GetWidth() + offx;
		uint16_t *dstRow = aDest;
		uint32_t *palette = (uint32_t*)theImage->mColorTable;

		for(int y=0; y<theHeight; y++)
		{
			uint8_t *src = srcRow;
			uint16_t *dst = dstRow;
			for(int x=0; x<theWidth; x++)
			{
				uint32_t aPixel = palette[*src++];
				*dst++ = ((aPixel>>8)&0xF800) | ((aPixel>>5)&0x07E0) | ((aPixel>>3)&0x001F);
			}

			if (rightPad)
			{
				const uint16_t edge = *(dst - 1);
				while (dst < dstRow + theDestPitch)
					*dst++ = edge;
			}

			srcRow += theImage->GetWidth();
			dstRow += theDestPitch;
		}
	}

	if (bottomPad)
	{
		for (int y = theHeight; y < theDestHeight; ++y)
			memcpy(aDest + y * theDestPitch, aDest + (theHeight - 1) * theDestPitch,
				theDestPitch * sizeof(uint16_t));
	}

	return Ps2GsTextureSetPixels(texture, aDest, theDestPitch, theDestHeight,
		theDestPitch * (int)sizeof(uint16_t), PS2_SRC_RGB565);
}

static bool CopyImageToTexturePalette8(MemoryImage *theImage, int offx, int offy, int theWidth, int theHeight, int theDestPitch, int theDestHeight, bool rightPad, bool bottomPad, Ps2GsTexture* texture)
{
	if (theImage->mColorIndices == NULL || theImage->mColorTable == NULL)
		return false;

	uint8_t *src = (uint8_t*)theImage->mColorIndices + offy * theImage->GetWidth() + offx;
	uint8_t *uploadIndices = src;
	int uploadPitch = theImage->GetWidth();
	uint8_t *padded = NULL;

	if (rightPad || bottomPad || theWidth != theDestPitch || theHeight != theDestHeight)
	{
		if (theDestPitch * theDestHeight > PIECE_SCRATCH_PIXELS)
			return false;
		padded = gPieceScratch.u8;

		for (int y = 0; y < theHeight; y++)
		{
			uint8_t *dst = padded + y * theDestPitch;
			memcpy(dst, src + y * theImage->GetWidth(), theWidth);

			uint8_t edge = dst[theWidth - 1];
			for (int x = theWidth; x < theDestPitch; x++)
				dst[x] = edge;
		}

		for (int y = theHeight; y < theDestHeight; y++)
			memcpy(padded + y * theDestPitch, padded + (theHeight - 1) * theDestPitch, theDestPitch);

		uploadIndices = padded;
		uploadPitch = theDestPitch;
	}

	return Ps2GsTextureSetIndexed(texture, uploadIndices, theDestPitch, theDestHeight, uploadPitch,
		reinterpret_cast<const uint32_t*>(theImage->mColorTable), 256);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static bool CopyImageToTexture(MemoryImage *theImage, int offx, int offy, int texWidth, int texHeight, PixelFormat theFormat, Ps2GsTexture* texture)
{

	int aWidth = std::min(texWidth,(theImage->GetWidth()-offx));
	int aHeight = std::min(texHeight,(theImage->GetHeight()-offy));

	bool rightPad = aWidth<texWidth;
	bool bottomPad = aHeight<texHeight;

	if(aWidth<=0 || aHeight<=0)
		return false;

	switch (theFormat)
	{
		case PixelFormat_A8R8G8B8:	return CopyImageToTexture8888(theImage, offx, offy, aWidth, aHeight, texWidth, texHeight, rightPad, bottomPad, texture);
		case PixelFormat_A4R4G4B4:	return CopyImageToTexture4444(theImage, offx, offy, aWidth, aHeight, texWidth, texHeight, rightPad, bottomPad, texture);
		case PixelFormat_R5G6B5:	return CopyImageToTexture565(theImage, offx, offy, aWidth, aHeight, texWidth, texHeight, rightPad, bottomPad, texture);
		case PixelFormat_Palette8:	return CopyImageToTexturePalette8(theImage, offx, offy, aWidth, aHeight, texWidth, texHeight, rightPad, bottomPad, texture);
		case PixelFormat_Unknown: break;
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static int GetClosestPowerOf2Above(int theNum)
{
	int aPower2 = 1;
	while (aPower2 < theNum)
		aPower2<<=1;
	return aPower2;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static bool IsPowerOf2(int theNum)
{
	int aNumBits = 0;
	while (theNum>0)
	{
		aNumBits += theNum&1;
		theNum >>= 1;
	}

	return aNumBits==1;
}
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void GetBestTextureDimensions(int &theWidth, int &theHeight, bool isEdge, bool usePow2, uint32_t theImageFlags)
{
//	theImageFlags = D3DImageFlag_MinimizeNumSubdivisions;
	if (theImageFlags & D3DImageFlag_Use64By64Subdivisions)
	{
		theWidth = theHeight = 64;
		return;
	}

	static int* aGoodTextureSize;
	static bool haveInited = false;
	if (!haveInited)
	{
		haveInited = true;
		aGoodTextureSize = new int[MAX_TEXTURE_SIZE];

		int i;
		int aPow2 = 1;
		for (i=0; i<MAX_TEXTURE_SIZE; i++)
		{
			if (i > aPow2)
				aPow2 <<= 1;

			int aGoodValue = aPow2;
			if ((aGoodValue - i ) > 64)
			{
				aGoodValue >>= 1;
				while (true)
				{
					int aLeftOver = i % aGoodValue;
					if (aLeftOver<64 || IsPowerOf2(aLeftOver))
						break;

					aGoodValue >>= 1;
				}
			}
			aGoodTextureSize[i] = aGoodValue;
		}
	}

	int aWidth = theWidth;
	int aHeight = theHeight;

	if (usePow2)
	{
		if (isEdge || (theImageFlags & D3DImageFlag_MinimizeNumSubdivisions))
		{
			aWidth = aWidth >= gMaxTextureWidth ? gMaxTextureWidth : GetClosestPowerOf2Above(aWidth);
			aHeight = aHeight >= gMaxTextureHeight ? gMaxTextureHeight : GetClosestPowerOf2Above(aHeight);
		}
		else
		{
			aWidth = aWidth >= gMaxTextureWidth ? gMaxTextureWidth : aGoodTextureSize[aWidth];
			aHeight = aHeight >= gMaxTextureHeight ? gMaxTextureHeight : aGoodTextureSize[aHeight];
		}
	}

	if (aWidth < gMinTextureWidth)
		aWidth = gMinTextureWidth;

	if (aHeight < gMinTextureHeight)
		aHeight = gMinTextureHeight;

	theWidth = aWidth;
	theHeight = aHeight;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
TextureData::TextureData()
{
	mWidth = 0;
	mHeight = 0;
	mTexVecWidth = 0;
	mTexVecHeight = 0;
	mBitsChangedCount = 0;
	mTexMemSize = 0;
	mTexPieceWidth = 64;
	mTexPieceHeight = 64;

	//mPalette = NULL;
	mPixelFormat = PixelFormat_Unknown;
	mImageFlags = 0;
	mCreateFailCooldown = 0;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
TextureData::~TextureData()
{
	ReleaseTextures();
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::ReleaseTextures()
{
	for(int i=0; i<(int)mTextures.size(); i++)
		Ps2GsTextureRelease(&mTextures[i].mTexture);

	mTextures.clear();
	mTexMemSize = 0;

	/*
	if (mPalette!=NULL)
	{
		mPalette->Release();
		mPalette = NULL;
	}
	*/
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::CreateTextureDimensions(MemoryImage *theImage)
{
	int aWidth = theImage->GetWidth();
	int aHeight = theImage->GetHeight();
	unsigned int i;

	// Calculate inner piece sizes
	mTexPieceWidth = aWidth;
	mTexPieceHeight = aHeight;
	bool usePow2 = true; //gTextureSizeMustBePow2 || mPixelFormat==PixelFormat_Palette8;
	GetBestTextureDimensions(mTexPieceWidth, mTexPieceHeight, false, usePow2, mImageFlags);

	// Calculate right boundary piece sizes
	int aRightWidth = aWidth % mTexPieceWidth;
	int aRightHeight = mTexPieceHeight;
	if (aRightWidth > 0)
		GetBestTextureDimensions(aRightWidth, aRightHeight, true, usePow2, mImageFlags);
	else
		aRightWidth = mTexPieceWidth;

	// Calculate bottom boundary piece sizes
	int aBottomWidth = mTexPieceWidth;
	int aBottomHeight = aHeight % mTexPieceHeight;
	if (aBottomHeight > 0)
		GetBestTextureDimensions(aBottomWidth, aBottomHeight, true, usePow2, mImageFlags);
	else
		aBottomHeight = mTexPieceHeight;

	// Calculate corner piece size
	int aCornerWidth = aRightWidth;
	int aCornerHeight = aBottomHeight;
	GetBestTextureDimensions(aCornerWidth, aCornerHeight, true, usePow2, mImageFlags);

	// Allocate texture array
	mTexVecWidth = (aWidth + mTexPieceWidth - 1) / mTexPieceWidth;
	mTexVecHeight = (aHeight + mTexPieceHeight - 1) / mTexPieceHeight;
	mTextures.resize(mTexVecWidth * mTexVecHeight);

	// Assign inner pieces
	for (i = 0; i < mTextures.size(); i++)
	{
		TextureDataPiece& aPiece = mTextures[i];
		Ps2GsTextureInit(&aPiece.mTexture);
		aPiece.mWidth = mTexPieceWidth;
		aPiece.mHeight = mTexPieceHeight;
	}

	// Assign right pieces
	for (i = mTexVecWidth - 1; i < mTextures.size(); i += mTexVecWidth)
	{
		TextureDataPiece& aPiece = mTextures[i];
		aPiece.mWidth = aRightWidth;
		aPiece.mHeight = aRightHeight;
	}

	// Assign bottom pieces
	for (i = mTexVecWidth * (mTexVecHeight - 1); i < mTextures.size(); i++)
	{
		TextureDataPiece& aPiece = mTextures[i];
		aPiece.mWidth = aBottomWidth;
		aPiece.mHeight = aBottomHeight;
	}

	// Assign corner piece
	mTextures.back().mWidth = aCornerWidth;
	mTextures.back().mHeight = aCornerHeight;

	for (i = 0; i < mTextures.size(); i++)
	{
		TextureDataPiece& aPiece = mTextures[i];
		aPiece.mInvWidth = 1.0f / aPiece.mWidth;
		aPiece.mInvHeight = 1.0f / aPiece.mHeight;
	}
	/**/

	mMaxTotalU = aWidth / (float)mTexPieceWidth;
	mMaxTotalV = aHeight / (float)mTexPieceHeight;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::CreateTextures(MemoryImage *theImage)
{
#ifdef PS2_PLATFORM
	if (theImage->mLazyUnloaded)
		gSexyAppBase->LoadLazyImageBits(theImage);

	// A file-backed image with no pixel data here is a failed decode (OOM).
	// Bail before CopyImageToTexture's GetBits() allocates the w*h*4 blank
	// fallback with the THROWING operator new — for a 1400x600 background
	// that throw was fatal ([FATAL] std::bad_alloc, ra=MemoryImage::GetBits).
	// mPixelFormat stays Unknown so this draw is skipped; the re-arm plus the
	// per-image cooldown (CheckCreateTextures) retries the decode once memory
	// has been freed instead of leaving a permanently black image.
	if (theImage->mBits == NULL && theImage->mColorIndices == NULL &&
		theImage->mRGBBits == NULL && !theImage->mFilePath.empty())
	{
		theImage->mLazyUnloaded = true;
		mCreateFailCooldown = 64;
		return;
	}

	if (theImage->mBits != NULL)
#endif
		theImage->DeleteSWBuffers(); // don't need these buffers for 3d drawing

	// Choose appropriate pixel format
	PixelFormat aFormat = PixelFormat_A8R8G8B8;
	//theImage->mD3DFlags = D3DImageFlag_UseA4R4G4B4;

	theImage->CommitBits();
	if (!theImage->mHasAlpha && !theImage->mHasTrans && (gSupportedPixelFormats & PixelFormat_R5G6B5))
	{
		if (!(theImage->mD3DFlags & D3DImageFlag_UseA8R8G8B8))
			aFormat = PixelFormat_R5G6B5;
	}

	if (theImage->mColorIndices != NULL && (gSupportedPixelFormats & PixelFormat_Palette8))
	{
		aFormat = PixelFormat_Palette8;
	}

	if ((theImage->mD3DFlags & D3DImageFlag_UseA4R4G4B4) && aFormat==PixelFormat_A8R8G8B8 && (gSupportedPixelFormats & PixelFormat_A4R4G4B4))
		aFormat = PixelFormat_A4R4G4B4;

	if (aFormat==PixelFormat_A8R8G8B8 && !(gSupportedPixelFormats & PixelFormat_A8R8G8B8))
		aFormat = PixelFormat_A4R4G4B4;

	// Release texture if image size has changed
	if (mWidth!=theImage->mWidth || mHeight!=theImage->mHeight || aFormat!=mPixelFormat || theImage->mD3DFlags!=mImageFlags)
	{
		ReleaseTextures();

		mPixelFormat = aFormat;
		mImageFlags = theImage->mD3DFlags;
		CreateTextureDimensions(theImage);
	}

	int i,x,y;

	int aHeight = theImage->GetHeight();
	int aWidth = theImage->GetWidth();

	mTexMemSize = 0;
	i=0;
	for(y=0; y<aHeight; y+=mTexPieceHeight)
	{
		for(x=0; x<aWidth; x+=mTexPieceWidth, i++)
		{
			TextureDataPiece &aPiece = mTextures[i];
			if (!CopyImageToTexture(theImage, x, y, aPiece.mWidth, aPiece.mHeight, aFormat, &aPiece.mTexture))
			{
				static int sInvalidLogs = 0;
				if (sInvalidLogs < 16)
				{
					++sInvalidLogs;
					printf("[PS2] native texture piece %d/%d failed for '%s' (%dx%d) - retry in 64 draws\n",
						i, (int)mTextures.size(),
						theImage->mFilePath.empty() ? "(runtime)" : theImage->mFilePath.c_str(),
						theImage->mWidth, theImage->mHeight);
				}
				ReleaseTextures();
				mPixelFormat = PixelFormat_Unknown;
				mCreateFailCooldown = 64;
				return;
			}
			Ps2GsTextureSetLinearFilter(&aPiece.mTexture, gLinearFilter);
			mTexMemSize += aPiece.mTexture.mGs.Width * aPiece.mTexture.mGs.Height + 256 * 4;
		}
	}



	mWidth = theImage->mWidth;
	mHeight = theImage->mHeight;
	mBitsChangedCount = theImage->mBitsChangedCount;
	mPixelFormat = aFormat;

#ifdef SEXY_LAZY_IMAGES
	// The native PS2 texture backend keeps its own CPU copy for VRAM eviction
	// re-uploads, so file-backed bits are now redundant in EE RAM.
	// Keep the texture; software blits re-decode the bits from the pak.
	gSexyAppBase->PurgeLazyImage(theImage, true);
#endif
}
	
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::CheckCreateTextures(MemoryImage *theImage)
{
	if(mPixelFormat==PixelFormat_Unknown || theImage->mWidth != mWidth || theImage->mHeight != mHeight || theImage->mBitsChangedCount != mBitsChangedCount || theImage->mD3DFlags != mImageFlags)
	{
#ifdef PS2_PLATFORM
		// After a failed creation, wait out the cooldown before retrying:
		// each attempt can be a full re-decode, and per-draw retries while
		// memory is still tight would thrash the loader instead of letting
		// the purges free room first.
		if (mCreateFailCooldown > 0)
		{
			mCreateFailCooldown--;
			return;
		}
#endif
		CreateTextures(theImage);
	}
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
Ps2GsTexture* TextureData::GetTexture(int x, int y, int &width, int &height, float &u1, float &v1, float &u2, float &v2)
{
	if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
		mTexPieceWidth <= 0 || mTexPieceHeight <= 0 ||
		mTexVecWidth <= 0 || mTexVecHeight <= 0 || mTextures.empty())
	{
		width = 0;
		height = 0;
		return NULL;
	}

	int tx = x/mTexPieceWidth;
	int ty = y/mTexPieceHeight;
	if (tx < 0 || ty < 0 || tx >= mTexVecWidth || ty >= mTexVecHeight)
	{
		width = 0;
		height = 0;
		return NULL;
	}

	TextureDataPiece &aPiece = mTextures[ty*mTexVecWidth + tx];
	if (aPiece.mWidth <= 0 || aPiece.mHeight <= 0)
	{
		width = 0;
		height = 0;
		return NULL;
	}

	int left = x%mTexPieceWidth;
	int top = y%mTexPieceHeight;
	int right = left+width;
	int bottom = top+height;

	if(right > aPiece.mWidth)
		right = aPiece.mWidth;

	if(bottom > aPiece.mHeight)
		bottom = aPiece.mHeight;

	width = right-left;
	height = bottom-top;
	if (width <= 0 || height <= 0)
		return NULL;

	u1 = left * aPiece.mInvWidth;
	v1 = top * aPiece.mInvHeight;
	u2 = right * aPiece.mInvWidth;
	v2 = bottom * aPiece.mInvHeight;

	return &aPiece.mTexture;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
Ps2GsTexture* TextureData::GetTextureF(float x, float y, float &width, float &height, float &u1, float &v1, float &u2, float &v2)
{
	if (x < 0.0f || y < 0.0f || width <= 0.0f || height <= 0.0f ||
		mTexPieceWidth <= 0 || mTexPieceHeight <= 0 ||
		mTexVecWidth <= 0 || mTexVecHeight <= 0 || mTextures.empty())
	{
		width = 0.0f;
		height = 0.0f;
		return NULL;
	}

	int tx = (int)(x/mTexPieceWidth);
	int ty = (int)(y/mTexPieceHeight);
	if (tx < 0 || ty < 0 || tx >= mTexVecWidth || ty >= mTexVecHeight)
	{
		width = 0.0f;
		height = 0.0f;
		return NULL;
	}

	TextureDataPiece &aPiece = mTextures[ty*mTexVecWidth + tx];
	if (aPiece.mWidth <= 0 || aPiece.mHeight <= 0)
	{
		width = 0.0f;
		height = 0.0f;
		return NULL;
	}

	float left = x - tx*mTexPieceWidth;
	float top = y - ty*mTexPieceHeight;
	float right = left+width;
	float bottom = top+height;

	if(right > aPiece.mWidth)
		right = aPiece.mWidth;

	if(bottom > aPiece.mHeight)
		bottom = aPiece.mHeight;

	width = right-left;
	height = bottom-top;
	if (width <= 0.0f || height <= 0.0f)
		return NULL;

	u1 = left * aPiece.mInvWidth;
	v1 = top * aPiece.mInvHeight;
	u2 = right * aPiece.mInvWidth;
	v2 = bottom * aPiece.mInvHeight;

	return &aPiece.mTexture;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void SetLinearFilter(bool linear)
{
	gLinearFilter = linear;
}

static void PrepareTextureForDraw(Ps2GsTexture& texture)
{
	Ps2GsTextureSetLinearFilter(&texture, gLinearFilter);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void TextureData::Blt(float theX, float theY, const Rect& theSrcRect, const Color& theColor)
{
	int srcLeft = theSrcRect.mX;
	int srcTop = theSrcRect.mY;
	int srcRight = srcLeft + theSrcRect.mWidth;
	int srcBottom = srcTop + theSrcRect.mHeight;
	int srcX, srcY;
	float dstX, dstY;
	int aWidth,aHeight;
	float u1,v1,u2,v2;

	srcY = srcTop;
	dstY = theY;

	uint32_t aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);

	if ((srcLeft >= srcRight) || (srcTop >= srcBottom))
		return;

	while(srcY < srcBottom)
	{
		srcX = srcLeft;
		dstX = theX;
		while(srcX < srcRight)
		{
			aWidth = srcRight-srcX;
			aHeight = srcBottom-srcY;
			Ps2GsTexture* aTexture = GetTexture(srcX, srcY, aWidth, aHeight, u1, v1, u2, v2);

			// GetTexture can return width/height <= 0 when the source rect falls
			// outside the (possibly IMG_DOWNSCALE-shrunk) texture piece grid, e.g.
			// sprite-sheet sub-rects addressed in un-scaled coordinates. srcX/srcY
			// would then never advance -> infinite loop (hard hang). Skip this blt.
			if (aTexture == NULL || aWidth <= 0 || aHeight <= 0)
				return;

#if 0
			float x = dstX*IMG_DOWNSCALE - 0.5f;
			float y = dstY*IMG_DOWNSCALE - 0.5f;

			Ps2Vertex aVertex[4] = {
				{ {x},                        {y},                       {0},{aColor},{u1},{v1} },
				{ {x},                        {y+aHeight*IMG_DOWNSCALE}, {0},{aColor},{u1},{v2} },
				{ {x+aWidth*IMG_DOWNSCALE},   {y},                       {0},{aColor},{u2},{v1} },
				{ {x+aWidth*IMG_DOWNSCALE},   {y+aHeight*IMG_DOWNSCALE}, {0},{aColor},{u2},{v2} }
			};
#else
			float x = dstX - 0.5f;
			float y = dstY - 0.5f;

			Ps2Vertex aVertex[4] = {
				{ {x},          {y},         {0},{aColor},{u1},{v1} },
				{ {x},          {y+aHeight}, {0},{aColor},{u1},{v2} },
				{ {x+aWidth},   {y},         {0},{aColor},{u2},{v1} },
				{ {x+aWidth},   {y+aHeight}, {0},{aColor},{u2},{v2} }
			};
#endif
			PrepareTextureForDraw(*aTexture);
			Ps2GsRendererDrawTexturedSprite(aTexture,
				aVertex[0].sx, aVertex[0].sy, aVertex[0].tu, aVertex[0].tv,
				aVertex[3].sx, aVertex[3].sy, aVertex[3].tu, aVertex[3].tv, aColor);

			srcX += aWidth;
			dstX += aWidth;
		}

		srcY += aHeight;
		dstY += aHeight;
	}
}

static inline float GetCoord(const Ps2Vertex& theVertex, int theCoord)
{
	switch (theCoord)
	{
	case 0: return theVertex.sx;
	case 1: return theVertex.sy;
	case 2: return theVertex.sz;
	case 3: return theVertex.tu;
	case 4: return theVertex.tv;
	default: return 0;
	}
}

static inline Ps2Vertex Interpolate(const Ps2Vertex &v1, const Ps2Vertex &v2, float t)
{
	Ps2Vertex aVertex = v1;
	aVertex.sx = v1.sx + t*(v2.sx-v1.sx);
	aVertex.sy = v1.sy + t*(v2.sy-v1.sy);
	aVertex.tu = v1.tu + t*(v2.tu-v1.tu);
	aVertex.tv = v1.tv + t*(v2.tv-v1.tv);
	if (v1.color!=v2.color)
	{
		int r = ((v1.color >> 0) & 0xff) + t*( ((v2.color >> 0) & 0xff) - ((v1.color >> 0) & 0xff) );
		int g = ((v1.color >> 8) & 0xff) + t*( ((v2.color >> 8) & 0xff) - ((v1.color >> 8) & 0xff) );
		int b = ((v1.color >> 16) & 0xff) + t*( ((v2.color >> 16) & 0xff) - ((v1.color >> 16) & 0xff) );
		int a = ((v1.color >> 24) & 0xff) + t*( ((v2.color >> 24) & 0xff) - ((v1.color >> 24) & 0xff) );
		aVertex.color = (r << 0) | (g << 8) | (b << 16) | (a << 24);
	}

	return aVertex;
}

template<class Pred>
struct PointClipper
{
	Pred mPred;

	void ClipPoint(int n, float clipVal, const Ps2Vertex& v1, const Ps2Vertex& v2, VertexList& out)
	{
		if (!mPred(GetCoord(v1, n), clipVal))
		{
			if (!mPred(GetCoord(v2, n), clipVal)) // both inside
				out.push_back(v2);
			else // inside -> outside
			{
				float t = (clipVal - GetCoord(v1, n)) / (GetCoord(v2, n) - GetCoord(v1, n));
				out.push_back(Interpolate(v1, v2, t));
			}
		}
		else
		{
			if (!mPred(GetCoord(v2, n), clipVal)) // outside -> inside
			{
				float t = (clipVal - GetCoord(v1, n)) / (GetCoord(v2, n) - GetCoord(v1, n));
				out.push_back(Interpolate(v1, v2, t));
				out.push_back(v2);
			}
			//			else // outside -> outside
		}
	}

	void ClipPoints(int n, float clipVal, VertexList& in, VertexList& out)
	{
		if (in.size() < 2)
			return;

		ClipPoint(n, clipVal, in[in.size() - 1], in[0], out);
		for (VertexList::size_type i = 0; i < in.size() - 1; i++)
			ClipPoint(n, clipVal, in[i], in[i + 1], out);
	}
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void DrawPolyClipped(const Rect *theClipRect, const VertexList &theList, Ps2GsTexture* texture)
{
	VertexList l1, l2;
	l1 = theList;

	int left = theClipRect->mX;
	int right = left + theClipRect->mWidth;
	int top = theClipRect->mY;
	int bottom = top + theClipRect->mHeight;

	VertexList *in = &l1, *out = &l2;
	PointClipper<std::less<float> > aLessClipper;
	PointClipper<std::greater_equal<float> > aGreaterClipper;

	aLessClipper.ClipPoints(0,left,*in,*out); std::swap(in,out); out->clear();
	aLessClipper.ClipPoints(1,top,*in,*out); std::swap(in,out); out->clear();
	aGreaterClipper.ClipPoints(0,right,*in,*out); std::swap(in,out); out->clear();
	aGreaterClipper.ClipPoints(1,bottom,*in,*out);

	VertexList &aList = *out;

	if (aList.size() >= 3)
		DrawVertexFan(aList, texture);
}


///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void DoPolyTextureClip(VertexList &theList)
{
	VertexList l2;

	float left = 0;
	float right = 1;
	float top = 0;
	float bottom = 1;

	VertexList *in = &theList, *out = &l2;
	PointClipper<std::less<float> > aLessClipper;
	PointClipper<std::greater_equal<float> > aGreaterClipper;

	aLessClipper.ClipPoints(3,left,*in,*out); std::swap(in,out); out->clear();
	aLessClipper.ClipPoints(4,top,*in,*out); std::swap(in,out); out->clear();
	aGreaterClipper.ClipPoints(3,right,*in,*out); std::swap(in,out); out->clear();
	aGreaterClipper.ClipPoints(4,bottom,*in,*out);
}


void TextureData::BltTransformed(const SexyMatrix3 &theTrans, const Rect& theSrcRect, const Color& theColor, const Rect *theClipRect, float theX, float theY, bool center)
{
	int srcLeft = theSrcRect.mX;
	int srcTop = theSrcRect.mY;
	int srcRight = srcLeft + theSrcRect.mWidth;
	int srcBottom = srcTop + theSrcRect.mHeight;
	int srcX, srcY;
	float dstX, dstY;
	int aWidth;
	int aHeight;
	float u1,v1,u2,v2;
	float startx = 0, starty = 0;
	float pixelcorrect = 0.5f;

	if (center)
	{
		startx = -theSrcRect.mWidth/2.0f;
		starty = -theSrcRect.mHeight/2.0f;
		pixelcorrect = 0.0f;
	}			

	srcY = srcTop;
	dstY = starty;

	uint32_t aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);

	if ((srcLeft >= srcRight) || (srcTop >= srcBottom))
		return;

	while(srcY < srcBottom)
	{
		srcX = srcLeft;
		dstX = startx;
		while(srcX < srcRight)
		{
			aWidth = srcRight-srcX;
			aHeight = srcBottom-srcY;
			Ps2GsTexture* aTexture = GetTexture(srcX, srcY, aWidth, aHeight, u1, v1, u2, v2);

			if (aTexture == NULL || aWidth <= 0 || aHeight <= 0)
				return;

#if 0
			float x = dstX*IMG_DOWNSCALE; // - 0.5f;
			float y = dstY*IMG_DOWNSCALE; // - 0.5f;
			float w = aWidth*IMG_DOWNSCALE;
			float h = aHeight*IMG_DOWNSCALE;
#else
			float x = dstX; // - 0.5f;
			float y = dstY; // - 0.5f;
			float w = aWidth;
			float h = aHeight;
#endif

			SexyVector2 p[4] = { SexyVector2(x, y), SexyVector2(x,y+h), SexyVector2(x+w, y) , SexyVector2(x+w, y+h) };
			SexyVector2 tp[4];

			int i;
			for (i=0; i<4; i++)
			{
				tp[i] = theTrans*p[i];
				tp[i].x -= pixelcorrect - theX;
				tp[i].y -= pixelcorrect - theY;
			}

			bool clipped = false;
			if (theClipRect != NULL)
			{
				int left = theClipRect->mX;
				int right = left + theClipRect->mWidth;
				int top = theClipRect->mY;
				int bottom = top + theClipRect->mHeight;
				for (i=0; i<4; i++)
				{
					if (tp[i].x<left || tp[i].x>=right || tp[i].y<top || tp[i].y>=bottom)
					{
						clipped = true;
						break;
					}
				}
			}

			Ps2Vertex aVertex[4] = {
				{ {tp[0].x},{tp[0].y},{0},{aColor},{u1},{v1} },
				{ {tp[1].x},{tp[1].y},{0},{aColor},{u1},{v2} },
				{ {tp[2].x},{tp[2].y},{0},{aColor},{u2},{v1} },
				{ {tp[3].x},{tp[3].y},{0},{aColor},{u2},{v2} }
			};

			PrepareTextureForDraw(*aTexture);

			if (!clipped)
			{
				DrawVertexTriangle(aTexture, aVertex[0], aVertex[1], aVertex[2]);
				DrawVertexTriangle(aTexture, aVertex[2], aVertex[1], aVertex[3]);
			}
			else
			{
				VertexList aList;
				aList.push_back(aVertex[0]);
				aList.push_back(aVertex[1]);
				aList.push_back(aVertex[3]);
				aList.push_back(aVertex[2]);

				DrawPolyClipped(theClipRect, aList, aTexture);
			}

			srcX += aWidth;
			dstX += aWidth;
		}

		srcY += aHeight;
		dstY += aHeight;
	}
}

void TextureData::BltTriangles(const TriVertex theVertices[][3], int theNumTriangles, unsigned int theColor, float tx, float ty)
{
	if ((mMaxTotalU <= 1.0) && (mMaxTotalV <= 1.0))
	{
		Ps2GsTexture& texture = mTextures[0].mTexture;
		PrepareTextureForDraw(texture);
		for (int triangle = 0; triangle < theNumTriangles; ++triangle)
		{
			const TriVertex* vertices = theVertices[triangle];
			Ps2Vertex nativeVertices[3];
			for (int i = 0; i < 3; ++i)
			{
				nativeVertices[i].sx = vertices[i].x + tx;
				nativeVertices[i].sy = vertices[i].y + ty;
				nativeVertices[i].sz = 0;
				nativeVertices[i].color = GetColorFromTriVertex(vertices[i], theColor);
				nativeVertices[i].tu = vertices[i].u * mMaxTotalU;
				nativeVertices[i].tv = vertices[i].v * mMaxTotalV;
			}
			DrawVertexTriangle(&texture, nativeVertices[0], nativeVertices[1], nativeVertices[2]);
		}
	}
	else
	{
		for (int aTriangleNum = 0; aTriangleNum < theNumTriangles; aTriangleNum++)
		{
			TriVertex* aTriVerts = (TriVertex*) theVertices[aTriangleNum];

			Ps2Vertex aVertex[3] = {
				{ {aTriVerts[0].x + tx},{aTriVerts[0].y + ty},	{0},{GetColorFromTriVertex(aTriVerts[0],theColor)},	{aTriVerts[0].u*mMaxTotalU},{aTriVerts[0].v*mMaxTotalV} },
				{ {aTriVerts[1].x + tx},{aTriVerts[1].y + ty},	{0},{GetColorFromTriVertex(aTriVerts[1],theColor)},	{aTriVerts[1].u*mMaxTotalU},{aTriVerts[1].v*mMaxTotalV} },
				{ {aTriVerts[2].x + tx},{aTriVerts[2].y + ty},	{0},{GetColorFromTriVertex(aTriVerts[2],theColor)},	{aTriVerts[2].u*mMaxTotalU},{aTriVerts[2].v*mMaxTotalV} }
			};

			float aMinU = mMaxTotalU, aMinV = mMaxTotalV;
			float aMaxU = 0, aMaxV = 0;

			int i,j,k;
			for (i=0; i<3; i++)
			{
				if(aVertex[i].tu < aMinU)
					aMinU = aVertex[i].tu;

				if(aVertex[i].tv < aMinV)
					aMinV = aVertex[i].tv;

				if(aVertex[i].tu > aMaxU)
					aMaxU = aVertex[i].tu;

				if(aVertex[i].tv > aMaxV)
					aMaxV = aVertex[i].tv;
			}

			VertexList aMasterList;
			aMasterList.push_back(aVertex[0]);
			aMasterList.push_back(aVertex[1]);
			aMasterList.push_back(aVertex[2]);


			VertexList aList;

			int aLeft = floorf(aMinU);
			int aTop = floorf(aMinV);
			int aRight = ceilf(aMaxU);
			int aBottom = ceilf(aMaxV);
			if (aLeft < 0)
				aLeft = 0;
			if (aTop < 0)
				aTop = 0;
			if (aRight > mTexVecWidth)
				aRight = mTexVecWidth;
			if (aBottom > mTexVecHeight)
				aBottom = mTexVecHeight;

			TextureDataPiece &aStandardPiece = mTextures[0];
			for (i=aTop; i<aBottom; i++)
			{
				for (j=aLeft; j<aRight; j++)
				{
					TextureDataPiece &aPiece = mTextures[i*mTexVecWidth + j];


					VertexList aList = aMasterList;
					for(k=0; k<3; k++)
					{
						aList[k].tu -= j;
						aList[k].tv -= i;
						if (i==mTexVecHeight-1)
							aList[k].tv *= (float)aStandardPiece.mHeight / aPiece.mHeight;
						if (j==mTexVecWidth-1)
							aList[k].tu *= (float)aStandardPiece.mWidth / aPiece.mWidth;
					}

					DoPolyTextureClip(aList);
					if (aList.size() >= 3)
					{
						PrepareTextureForDraw(aPiece.mTexture);
						DrawVertexFan(aList, &aPiece.mTexture);
					}
				}
			}
		}
	}
}


GLInterface::GLInterface(SexyAppBase* theApp)
{
	mApp = theApp;
	mWidth = mApp->mWidth;
	mHeight = mApp->mHeight;
	mDisplayWidth = mWidth;
	mDisplayHeight = mHeight;

	mPresentationRect = Rect( 0, 0, mWidth, mHeight );

	mRefreshRate = 60;
	mMillisecondsPerFrame = 1000/mRefreshRate;

	mScreenImage = 0;

	mNextCursorX = 0;
	mNextCursorY = 0;
	mCursorX = 0;
	mCursorY = 0;


}

GLInterface::~GLInterface()
{
	Flush();

	ImageSet::iterator anItr;
	for(anItr = mImageSet.begin(); anItr != mImageSet.end(); ++anItr)
	{
		MemoryImage *anImage = *anItr;
		delete (TextureData*)anImage->mD3DData;
		anImage->mD3DData = NULL;
	}

}

void GLInterface::SetDrawMode(int theDrawMode)
{
	Ps2GsRendererSetBlendAdditive(theDrawMode != Graphics::DRAWMODE_NORMAL);
}

void GLInterface::AddGLImage(GLImage* theGLImage)
{
	AutoCrit anAutoCrit(mCritSect);

	mPs2ImageSet.insert(theGLImage);
}

void GLInterface::RemoveGLImage(GLImage* theGLImage)
{
	AutoCrit anAutoCrit(mCritSect);

	Ps2ImageSet::iterator anItr = mPs2ImageSet.find(theGLImage);
	if (anItr != mPs2ImageSet.end())
		mPs2ImageSet.erase(anItr);
}

void GLInterface::Remove3DData(MemoryImage* theImage)
{
	if (theImage->mD3DData != NULL)
	{
		delete (TextureData*)theImage->mD3DData;
		theImage->mD3DData = NULL;

		AutoCrit aCrit(mCritSect); // Make images thread safe
		mImageSet.erase(theImage);
	}
}

GLImage* GLInterface::GetScreenImage()
{
	return mScreenImage;
}

void GLInterface::UpdateViewport()
{
	int width, viewport_width;
	int height, viewport_height;
	int viewport_x = 0;
	int viewport_y = 0;

	width = gsGlobal ? (int)gsGlobal->Width : mWidth;
	height = gsGlobal ? (int)gsGlobal->Height : mHeight;

	Ps2GsRendererClear(0, 0, 0, 255);
	Flush();

	// No aspect-ratio letterboxing on PS2: the TV scans the full framebuffer
	// out as 4:3 regardless of its pixel dimensions (non-square pixels), so
	// the whole buffer is the correct 4:3 target for the 800x600 game.
	viewport_width = width;
	viewport_height = height;
	mPresentationRect = Rect( viewport_x, viewport_y, viewport_width, viewport_height );
	Ps2GsRendererSetLogicalSize(mWidth, mHeight);

	Ps2GsRendererClear(0, 0, 0, 255);
	Flush();
}

int GLInterface::Init(bool IsWindowed)
{
	const int aMaxSize = PS2_GS_MAX_TEXTURE_SIZE;
	MAX_TEXTURE_SIZE = aMaxSize;

	gTextureSizeMustBePow2 = false;
	gMinTextureWidth = 8;
	gMinTextureHeight = 8;
	gMaxTextureWidth = aMaxSize;
	gMaxTextureHeight = aMaxSize;
	gSupportedPixelFormats = PixelFormat_A8R8G8B8 | PixelFormat_Palette8;
	gLinearFilter = false;

	Ps2GsRendererInit(mWidth, mHeight);
	Ps2GsRendererClear(0, 0, 0, 255);

	mRGBBits = 32;

	mRedBits = 8;
	mGreenBits = 8;
	mBlueBits = 8;
	
	mRedShift = 0;
	mGreenShift = 8;
	mBlueShift = 16;

	mRedMask = (0xFFU << mRedShift);
	mGreenMask = (0xFFU << mGreenShift);
	mBlueMask = (0xFFU << mBlueShift);

	SetVideoOnlyDraw(false);

	return 1;
}

// Defined in Input.cpp: pad-driven cursor position in screen coordinates.
void Ps2GetCursorPos(float& x, float& y);

static void DrawPs2SoftwareCursor(float x, float y)
{
	const uint32_t black = 0xFF000000;
	const uint32_t white = 0xFFFFFFFF;
	Ps2GsRendererSetBlendAdditive(false);
	Ps2GsRendererDrawTriangle(x - 1.5f, y - 2.0f, black,
		x - 1.5f, y + 18.0f, black, x + 13.0f, y + 12.5f, black);
	Ps2GsRendererDrawTriangle(x, y, white, x, y + 14.0f, white,
		x + 10.0f, y + 10.0f, white);
}

bool GLInterface::Redraw(Rect* theClipRect)
{
	// The PS2 has no OS cursor; overlay a software arrow as the very last
	// thing before the flip so it draws on top of the frame. Ps2GetCursorPos
	// returns GS physical-pixel coordinates (the space Input.cpp's pad/mouse
	// handling and RemapMouse both use), while this renderer operates in logical
	// widget space (mWidth x mHeight). Rescale through mPresentationRect ->
	// mApp->mScreenBounds -- the same source/dest rects WidgetManager::
	// RemapMouse uses -- so the drawn arrow lands where the widget system
	// thinks the cursor is, instead of being confined to whatever fraction
	// of the screen the physical GS buffer is smaller than the logical space.
	float aCursorX, aCursorY;
	Ps2GetCursorPos(aCursorX, aCursorY);
	if (mApp != NULL && mPresentationRect.mWidth > 0 && mPresentationRect.mHeight > 0)
	{
		const Rect& aDest = mApp->mScreenBounds;
		aCursorX = (aCursorX - mPresentationRect.mX) * (float)aDest.mWidth / (float)mPresentationRect.mWidth + aDest.mX;
		aCursorY = (aCursorY - mPresentationRect.mY) * (float)aDest.mHeight / (float)mPresentationRect.mHeight + aDest.mY;
	}
	DrawPs2SoftwareCursor(aCursorX, aCursorY);

	// Boot/loading heartbeat. The title screen's first intentional frame is
	// black; this tiny marker shows whether Redraw keeps running after that.
	if (gsGlobal && mApp && !mApp->mLoaded)
	{
		static int sBootMarkerFrame = 0;
		static const uint32_t kMarkerColors[] = {
			0xFFFFFFFF,
			0xFF00FF00,
			0xFF00FFFF,
			0xFFFFFF00
		};
		const uint32_t aColor = kMarkerColors[(sBootMarkerFrame++ / 15) & 3];
		Ps2GsRendererDrawSolidSprite(0.0f, 0.0f, 10.0f, 10.0f, aColor);
	}

	Flush();

	// Widgets normally repaint only their dirty regions, but the GS double
	// buffer plus the cursor overlay leaves stale pixels (cursor trails) in
	// whatever isn't repainted. Force a full widget repaint every frame.
	if (mApp && mApp->mWidgetManager)
		mApp->mWidgetManager->MarkAllDirty();

	return true;
}

void GLInterface::SetVideoOnlyDraw(bool videoOnly)
{
	if (mScreenImage) delete mScreenImage;
	mScreenImage = new GLImage(this);
	//mScreenImage->SetSurface(useSecondary ? mSecondarySurface : mDrawSurface);		
	//mScreenImage->mNoLock = mVideoOnlyDraw;
	//mScreenImage->mVideoMemory = mVideoOnlyDraw;
	mScreenImage->mWidth = mWidth;
	mScreenImage->mHeight = mHeight;
	mScreenImage->SetImageMode(false, false);
}

void GLInterface::SetCursorPos(int theCursorX, int theCursorY)
{
	mNextCursorX = theCursorX;
	mNextCursorY = theCursorY;
}

bool GLInterface::PreDraw()
{
	gLinearFilter = false;
	return true;
}

void GLInterface::Flush()
{
	Ps2GsRendererPresent();
}

bool GLInterface::CreateImageTexture(MemoryImage *theImage)
{
	// A NULL image here comes from a game-side draw of a null/dangling Image*
	// (the Blt entry points cast blindly). Dereferencing it doesn't stop at the
	// first TLB miss on PCSX2 — loads return garbage and execution staggers on
	// until a jump to a garbage address kills the EE. Skip the draw and log the
	// caller (feed the ra to addr2line) so the source is attributable.
	if (theImage == NULL)
	{
		static int sNullDrawLogs = 0;
		if (sNullDrawLogs < 8)
		{
			++sNullDrawLogs;
			printf("[PS2] CreateImageTexture(NULL) — draw skipped, ra=%p\n",
				__builtin_return_address(0));
		}
		return false;
	}

	// A zero logical dimension produces a zero-sized texture grid. Besides
	// being invalid for the GS, CreateTextureDimensions expects at least one
	// piece and would otherwise access vector::back() on an empty vector.
	if (theImage->mWidth <= 0 || theImage->mHeight <= 0)
	{
		static int sZeroSizeLogs = 0;
		if (sZeroSizeLogs < 8)
		{
			++sZeroSizeLogs;
			printf("[PS2] zero-sized image %dx%d skipped in CreateImageTexture, ra=%p\n",
				theImage->mWidth, theImage->mHeight, __builtin_return_address(0));
		}
		return false;
	}

	bool wantPurge = false;

	if(theImage->mD3DData==NULL)
	{
		theImage->mD3DData = new TextureData();
		
		// The actual purging was deferred
		wantPurge = theImage->mPurgeBits;

		AutoCrit aCrit(mCritSect); // Make images thread safe
		mImageSet.insert(theImage);
	}

	TextureData *aData = (TextureData*)theImage->mD3DData;

	// Pin across texture creation: any OOM rescue below (lazy decode, pak
	// record buffer, atlas) can run a full PurgeLazyImages, and unpinned this
	// image qualifies — Remove3DData would delete aData while its own
	// CheckCreateTextures is on the stack (use-after-free, heap corruption).
	bool aWasPinned = theImage->mLazyPinned;
	theImage->mLazyPinned = true;
	aData->CheckCreateTextures(theImage);
	theImage->mLazyPinned = aWasPinned;

	if (wantPurge)
		theImage->PurgeBits();

	return aData->mPixelFormat != PixelFormat_Unknown;
}

bool GLInterface::RecoverBits(MemoryImage* theImage)
{
	if (theImage->mD3DData == NULL)
		return false;

	TextureData* aData = (TextureData*) theImage->mD3DData;
	if (aData->mBitsChangedCount != theImage->mBitsChangedCount) // bits have changed since texture was created
		return false;

	// The GS cannot read textures back; reconstruct the pixels from the CPU
	// copies kept by the native texture backend. mBits was already allocated by
	// the calling GetBits().
	uint32_t* aBits = theImage->mBits;
	if (aBits == NULL)
		return false;

	for (int aPieceRow = 0; aPieceRow < aData->mTexVecHeight; aPieceRow++)
	{
		for (int aPieceCol = 0; aPieceCol < aData->mTexVecWidth; aPieceCol++)
		{
			TextureDataPiece* aPiece = &aData->mTextures[aPieceRow*aData->mTexVecWidth + aPieceCol];

			int offx = aPieceCol*aData->mTexPieceWidth;
			int offy = aPieceRow*aData->mTexPieceHeight;
			int aWidth = std::min(theImage->mWidth-offx, aPiece->mWidth);
			int aHeight = std::min(theImage->mHeight-offy, aPiece->mHeight);
			if (aWidth <= 0 || aHeight <= 0)
				continue;

			if (!Ps2GsTextureRead(&aPiece->mTexture, 0, 0, aWidth, aHeight,
					(aBits + offy*theImage->mWidth + offx), theImage->mWidth))
				return false;
		}
	}

	return true;
}

void GLInterface::PushTransform(const SexyMatrix3 &theTransform, bool concatenate)
{
	if (mTransformStack.empty() || !concatenate)
		mTransformStack.push_back(theTransform);
	else
	{
		SexyMatrix3 &aTrans = mTransformStack.back();
		mTransformStack.push_back(theTransform*aTrans);
	}
}

void GLInterface::PopTransform()
{
	if (!mTransformStack.empty())
		mTransformStack.pop_back();
}

void GLInterface::Blt(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Color& theColor, int theDrawMode, bool linearFilter)
{
	if (!mTransformStack.empty())
	{
		BltClipF(theImage,theX,theY,theSrcRect,NULL,theColor,theDrawMode);
		return;
	}

	if (!PreDraw())
		return;

	MemoryImage* aSrcMemoryImage = (MemoryImage*) theImage;

	if (!CreateImageTexture(aSrcMemoryImage))
		return;

	SetDrawMode(theDrawMode);

	TextureData *aData = (TextureData*)aSrcMemoryImage->mD3DData;

	SetLinearFilter(linearFilter);
	aData->Blt(theX,theY,theSrcRect,theColor);
}

void GLInterface::BltClipF(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Rect *theClipRect, const Color& theColor, int theDrawMode)
{
	SexyTransform2D aTransform;
	aTransform.Translate(theX, theY);

	BltTransformed(theImage,theClipRect,theColor,theDrawMode,theSrcRect,aTransform,true);
}

void GLInterface::BltMirror(Image* theImage, float theX, float theY, const Rect& theSrcRect, const Color& theColor, int theDrawMode, bool linearFilter)
{
	SexyTransform2D aTransform;		

	aTransform.Translate(-theSrcRect.mWidth,0);
	aTransform.Scale(-1, 1);
	aTransform.Translate(theX, theY);

	BltTransformed(theImage,NULL,theColor,theDrawMode,theSrcRect,aTransform,linearFilter);
}

void GLInterface::StretchBlt(Image* theImage,  const Rect& theDestRect, const Rect& theSrcRect, const Rect* theClipRect, const Color &theColor, int theDrawMode, bool fastStretch, bool mirror)
{
	if (theSrcRect.mWidth <= 0 || theSrcRect.mHeight <= 0 ||
		theDestRect.mWidth == 0 || theDestRect.mHeight == 0)
		return;

	float xScale = (float)theDestRect.mWidth / theSrcRect.mWidth;
	float yScale = (float)theDestRect.mHeight / theSrcRect.mHeight;

	SexyTransform2D aTransform;
	if (mirror)
	{
		aTransform.Translate(-theSrcRect.mWidth,0);
		aTransform.Scale(-xScale, yScale);
	}
	else
		aTransform.Scale(xScale, yScale);

	aTransform.Translate(theDestRect.mX, theDestRect.mY);
	BltTransformed(theImage,theClipRect,theColor,theDrawMode,theSrcRect,aTransform,!fastStretch);
}

void GLInterface::BltRotated(Image* theImage, float theX, float theY, const Rect* theClipRect, const Color& theColor, int theDrawMode, double theRot, float theRotCenterX, float theRotCenterY, const Rect& theSrcRect)
{
	SexyTransform2D aTransform;

	aTransform.Translate(-theRotCenterX, -theRotCenterY);
	aTransform.RotateRad(theRot);
	aTransform.Translate(theX+theRotCenterX,theY+theRotCenterY);

	BltTransformed(theImage,theClipRect,theColor,theDrawMode,theSrcRect,aTransform,true);
}

void GLInterface::BltTransformed(Image* theImage, const Rect* theClipRect, const Color& theColor, int theDrawMode, const Rect &theSrcRect, const SexyMatrix3 &theTransform, bool linearFilter, float theX, float theY, bool center)
{
	if (!PreDraw())
		return;

	MemoryImage* aSrcMemoryImage = (MemoryImage*) theImage;

	if (!CreateImageTexture(aSrcMemoryImage))
		return;

	SetDrawMode(theDrawMode);

	TextureData *aData = (TextureData*)aSrcMemoryImage->mD3DData;

	if (!mTransformStack.empty())
	{
		SetLinearFilter(true); // force linear filtering in the case of a global transform
		if (theX!=0 || theY!=0)
		{
			SexyTransform2D aTransform;
			if (center)
				aTransform.Translate(-theSrcRect.mWidth/2.0f,-theSrcRect.mHeight/2.0f);

			aTransform = theTransform * aTransform;
			aTransform.Translate(theX,theY);
			aTransform = mTransformStack.back() * aTransform;

			aData->BltTransformed(aTransform, theSrcRect, theColor, theClipRect);
		}
		else
		{
			SexyTransform2D aTransform = mTransformStack.back()*theTransform;
			aData->BltTransformed(aTransform, theSrcRect, theColor, theClipRect, theX, theY, center);
		}
	}
	else
	{
		SetLinearFilter(linearFilter);
		aData->BltTransformed(theTransform, theSrcRect, theColor, theClipRect, theX, theY, center);
	}
}

void GLInterface::DrawLine(double theStartX, double theStartY, double theEndX, double theEndY, const Color& theColor, int theDrawMode)
{
	if (!PreDraw())
		return;

	SetDrawMode(theDrawMode);

	float x1, y1, x2, y2;

	if (!mTransformStack.empty())
	{
		SexyVector2 p1(theStartX,theStartY);
		SexyVector2 p2(theEndX,theEndY);
		p1 = mTransformStack.back()*p1;
		p2 = mTransformStack.back()*p2;

		x1 = p1.x;
		y1 = p1.y;
		x2 = p2.x;
		y2 = p2.y;
	}
	else
	{
		x1 = theStartX;
		y1 = theStartY;
		x2 = theEndX;
		y2 = theEndY;
	}

	Ps2GsRendererDrawLine(x1, y1, x2, y2, theColor.ToInt());
}

void GLInterface::FillRect(const Rect& theRect, const Color& theColor, int theDrawMode)
{
	if (!PreDraw())
		return;

	SetDrawMode(theDrawMode);

	float x = theRect.mX - 0.5f;
	float y = theRect.mY - 0.5f;
	float aWidth = theRect.mWidth;
	float aHeight = theRect.mHeight;

	if (mTransformStack.empty())
	{
		Ps2GsRendererDrawSolidSprite(x, y, x + aWidth, y + aHeight, theColor.ToInt());
		return;
	}

	Ps2Vertex aVertex[4] = {
		{ {x},        {y},         {0},{theColor.ToInt()},{0},{0} },
		{ {x},        {y+aHeight}, {0},{theColor.ToInt()},{0},{0} },
		{ {x+aWidth}, {y},         {0},{theColor.ToInt()},{0},{0} },
		{ {x+aWidth}, {y+aHeight}, {0},{theColor.ToInt()},{0},{0} }
	};

	if (!mTransformStack.empty())
	{
		SexyVector2 p[4] = { SexyVector2(x, y), SexyVector2(x,y+aHeight), SexyVector2(x+aWidth, y) , SexyVector2(x+aWidth, y+aHeight) };

		int i;
		for (i=0; i<4; i++)
		{
			p[i] = mTransformStack.back()*p[i];
			p[i].x -= 0.5f;
			p[i].y -= 0.5f;
			aVertex[i].sx = p[i].x;
			aVertex[i].sy = p[i].y;
		}
	}

	DrawVertexTriangle(NULL, aVertex[0], aVertex[1], aVertex[2]);
	DrawVertexTriangle(NULL, aVertex[2], aVertex[1], aVertex[3]);
}

void GLInterface::DrawTriangle(const TriVertex &p1, const TriVertex &p2, const TriVertex &p3, const Color &theColor, int theDrawMode)
{
	if (!PreDraw())
		return;

	SetDrawMode(theDrawMode);

	unsigned int aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);
	unsigned int col1 = GetColorFromTriVertex(p1, aColor);
	unsigned int col2 = GetColorFromTriVertex(p2, aColor);
	unsigned int col3 = GetColorFromTriVertex(p3, aColor);

	Ps2GsRendererDrawTriangle(p1.x, p1.y, col1, p2.x, p2.y, col2, p3.x, p3.y, col3);
}

void GLInterface::DrawTriangleTex(const TriVertex &p1, const TriVertex &p2, const TriVertex &p3, const Color &theColor, int theDrawMode, Image *theTexture, bool blend)
{
	TriVertex aVertices[1][3] = {{p1, p2, p3}};
	DrawTrianglesTex(aVertices,1,theColor,theDrawMode,theTexture,blend);
}

void GLInterface::DrawTrianglesTex(const TriVertex theVertices[][3], int theNumTriangles, const Color &theColor, int theDrawMode, Image *theTexture, float tx, float ty, bool blend)
{
	if (!PreDraw()) return;

	MemoryImage* aSrcMemoryImage = (MemoryImage*)theTexture;

	if (!CreateImageTexture(aSrcMemoryImage))
		return;

	SetDrawMode(theDrawMode);

	TextureData *aData = (TextureData*)aSrcMemoryImage->mD3DData;

	SetLinearFilter(blend);

	unsigned int aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);
	aData->BltTriangles(theVertices, theNumTriangles, aColor, tx, ty);
}

void GLInterface::DrawTrianglesTexStrip(const TriVertex theVertices[], int theNumTriangles, const Color &theColor, int theDrawMode, Image *theTexture, float tx, float ty, bool blend)
{
	TriVertex aList[100][3];
	int aTriNum = 0;
	while (aTriNum < theNumTriangles)
	{
		int aMaxTriangles = std::min(100,theNumTriangles - aTriNum);
		for (int i=0; i<aMaxTriangles; i++)
		{
			aList[i][0] = theVertices[aTriNum];
			aList[i][1] = theVertices[aTriNum+1];
			aList[i][2] = theVertices[aTriNum+2];
			aTriNum++;
		}
		DrawTrianglesTex(aList,aMaxTriangles,theColor,theDrawMode,theTexture, tx, ty, blend);
	}
}

void GLInterface::FillPoly(const Point theVertices[], int theNumVertices, const Rect *theClipRect, const Color &theColor, int theDrawMode, int tx, int ty)
{
	if (theNumVertices<3)
		return;

	if (!PreDraw())
		return;

	SetDrawMode(theDrawMode);
	unsigned int aColor = (theColor.mRed << 0) | (theColor.mGreen << 8) | (theColor.mBlue << 16) | (theColor.mAlpha << 24);

	VertexList aList;
	for (int i=0; i<theNumVertices; i++)
	{
		Ps2Vertex vert = { {theVertices[i].mX + (float)tx}, {theVertices[i].mY + (float)ty}, {0}, {aColor}, {0}, {0} };
		if (!mTransformStack.empty())
		{
			SexyVector2 v(vert.sx,vert.sy);
			v = mTransformStack.back()*v;
			vert.sx = v.x;
			vert.sy = v.y;
		}

		aList.push_back(vert);
	}

	if (theClipRect != NULL)
		DrawPolyClipped(theClipRect, aList, NULL);
	else
		DrawVertexFan(aList, NULL);
}
