#ifndef __IMAGELIB_H__
#define __IMAGELIB_H__

#include <string>
#include "misc/PlatformCapabilities.h"

// Compact image capabilities are selected in misc/PlatformCapabilities.h.
// SEXY_COMPACT_IMAGES changes Image layout and must be identical in every TU.

namespace ImageLib
{

class Image
{
public:
	int						mWidth;
	int						mHeight;
	uint32_t*				mBits;
#ifdef SEXY_COMPACT_IMAGES
	uint32_t*				mColorTable;
	unsigned char*			mColorIndices;
	// Optional 24-bit RGB storage. Only backends with SEXY_COMPACT_RGB_IMAGES
	// populate it; keeping the member in all compact-image builds preserves layout.
	unsigned char*			mRGBBits;
	bool					mHasTrans;
	bool					mHasAlpha;
	// mBits was allocated with one spare uint32_t past w*h, so a MemoryImage
	// can adopt the buffer as-is and write its MEMORYCHECK_ID canary there
	// instead of allocating a duplicate w*h*4 block (see LoadLazyImageBits).
	bool					mBitsHasCanarySlot;
#endif

public:
	Image();
	virtual ~Image();

	int						GetWidth();
	int						GetHeight();
	uint32_t*				GetBits();
};

bool WriteJPEGImage(const std::string& theFileName, Image* theImage);
bool WritePNGImage(const std::string& theFileName, Image* theImage);
bool WriteTGAImage(const std::string& theFileName, Image* theImage);
bool WriteBMPImage(const std::string& theFileName, Image* theImage);
extern int gAlphaComposeColor;
extern bool gAutoLoadAlpha;
extern bool gIgnoreJPEG2000Alpha;  // I've noticed alpha in jpeg2000's that shouldn't have alpha so this defaults to true


Image* GetImage(const std::string& theFileName, bool lookForAlphaImage = true);

// Reads only the file header to get the decoded dimensions GetImage() would
// produce (including the IMG_DOWNSCALE division and the alpha-only-companion
// fallback), without decoding any pixels. Returns false if no file was found
// or the header could not be parsed.
bool GetImageDims(const std::string& theFileName, int& theWidth, int& theHeight);

//void InitJPEG2000();
//void CloseJPEG2000();
//void SetJ2KCodecKey(const std::string& theKey);

}

#endif //__IMAGELIB_H__
