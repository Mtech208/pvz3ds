#include "TodDebug.h"
#include "TodCommon.h"
#include "Reanimator.h"
#include "ReanimAtlas.h"
#include "misc/PerfTimer.h"
#include "misc/GraphicsBlockArena.h"
#include "misc/BigBlockArena.h"
#include "graphics/MemoryImage.h"
#ifdef PS2_PLATFORM
#include <new>
#include "SexyAppBase.h"
#endif

//0x470250
ReanimAtlas::ReanimAtlas()
{
	mImageCount = 0;
	mMemoryImage = nullptr;
#ifdef WII_PLATFORM
	memset(mImageHash, 0, sizeof(mImageHash));
	mImageHashValid = false;
#endif
}

void ReanimAtlas::ReanimAtlasDispose()
{
	if (mMemoryImage)
	{
		delete mMemoryImage;
		mMemoryImage = nullptr;
	}
	mImageCount = 0;
#ifdef WII_PLATFORM
	memset(mImageHash, 0, sizeof(mImageHash));
	mImageHashValid = false;
#endif
}

ReanimAtlasImage* ReanimAtlas::GetEncodedReanimAtlas(Image* theImage)
{
	if (theImage == nullptr)
		return nullptr;

	// Wii keeps the real Image* in reanimation transforms.  Apart from being
	// easier to validate, this avoids stale small indices when definitions and
	// atlases are discarded and rebuilt independently on LOW_MEMORY targets.
	if ((uintptr_t)theImage > 1000)
	{
		int aAtlasIndex = FindImage(theImage);
		return aAtlasIndex >= 0 ? &mImageArray[aAtlasIndex] : nullptr;
	}

	uintptr_t aAtlasIndex = (uintptr_t)theImage - 1;
	// A transform can be copied from another reanimation after that source
	// definition has encoded its Image* as an atlas-local small integer.  Such
	// an index has no meaning in this atlas.  Let the draw call omit that image
	// instead of asserting and then dereferencing the fake pointer.
	if (aAtlasIndex >= (uintptr_t)mImageCount)
		return nullptr;
	return &mImageArray[aAtlasIndex];
}

//0x470290
MemoryImage* ReanimAtlasMakeBlankMemoryImage(int theWidth, int theHeight)
{
	MemoryImage* aImage = new MemoryImage();
	int aBitsCount = theWidth * theHeight;

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	// Atlas pixels are transient construction data. Keep them out of both the
	// small-object heap and Wii's texture arena: the console scratch pool is
	// reserved early and reuses 256/512/1024KB size classes without creating
	// new holes in the general heap. If it cannot serve the surface, the
	// caller falls back to the already-supported un-atlased draw path.
	aImage->mBits = static_cast<uint32_t*>(ConsoleScratchAlloc((aBitsCount + 1) * sizeof(uint32_t)));
	if (aImage->mBits == nullptr)
	{
		printf("[ATLAS] scratch unavailable for %dx%d (%uKB); using un-atlased path\n",
			theWidth, theHeight, (unsigned)(((aBitsCount + 1) * sizeof(uint32_t)) / 1024));
		delete aImage;
		return nullptr;
	}
#else
	aImage->mBits = new uint32_t[aBitsCount + 1];
#endif
	aImage->mWidth = theWidth;
	aImage->mHeight = theHeight;
	aImage->mHasTrans = true;
	aImage->mHasAlpha = true;
	memset(aImage->mBits, 0, aBitsCount * 4);
	aImage->mBits[aBitsCount] = Sexy::MEMORYCHECK_ID;
	return aImage;
}

//0x470340
bool sSortByNonIncreasingHeight(const ReanimAtlasImage& image1, const ReanimAtlasImage& image2)
{
	//if (image1->mHeight != image2->mHeight)
	//	return image1->mHeight > image2->mHeight;
	//else if (image1->mWidth != image2->mWidth)
	//	return image1->mWidth > image2->mWidth;
	//else
	//	return (unsigned int)image1 > (unsigned int)image2;

	if (image1.mHeight != image2.mHeight)
		return image1.mHeight > image2.mHeight;
	else if (image1.mWidth != image2.mWidth)
		return image1.mWidth > image2.mWidth;
	return false;
}

static int GetClosestPowerOf2Above(int theNum)
{
	int aPower2 = 1;
	while (aPower2 < theNum)
		aPower2 <<= 1;

	return aPower2;
}

//0x470370
int ReanimAtlas::PickAtlasWidth()
{
	int totalArea = 0;
	int aMaxWidth = 0;
	for (int i = 0; i < mImageCount; i++)
	{
		ReanimAtlasImage* aImage = &mImageArray[i];
		totalArea += aImage->mWidth * aImage->mHeight;
		if (aMaxWidth <= aImage->mWidth + 2)
			aMaxWidth = aImage->mWidth + 2;
	}

	int aWidth = FloatRoundToInt(sqrt(totalArea));  // 假定为正方向区域时，正方向的边长
	return GetClosestPowerOf2Above(std::min(std::max(aWidth, aMaxWidth), 2048));  // 取“边长”和“最宽贴图的宽度”的较大值（且不超过 2048），并向上取至 2 的整数次幂
}

//0x470420
bool ReanimAtlas::ImageFits(int theImageCount, const Rect& rectTest, int theMaxWidth)
{
	if (rectTest.mX + rectTest.mWidth > theMaxWidth)
		return false;

	for (int i = 0; i < theImageCount; i++)  // 遍历贴图数组的前 theImageCount 个贴图，判断给定矩形是否与某贴图占用的区域有冲突
	{
		ReanimAtlasImage* aImage = &mImageArray[i];
		if (Rect(aImage->mX, aImage->mY, aImage->mWidth, aImage->mHeight).Inflate(1, 1).Intersects(rectTest))  // 贴图占用区域为自身区域及向外延伸 1 像素
			return false;
	}
	return true;
}

//0x4704C0
bool ReanimAtlas::ImageFindPlaceOnSide(ReanimAtlasImage* theAtlasImageToPlace, int theImageCount, int theMaxWidth, bool theToRight)
{
	Rect rectTest;
	rectTest.mWidth = theAtlasImageToPlace->mWidth + 2;
	rectTest.mHeight = theAtlasImageToPlace->mHeight + 2;

	for (int i = 0; i < theImageCount; i++)
	{
		ReanimAtlasImage* aImage = &mImageArray[i];
		if (theToRight)  // 如果规定了居右
		{
			rectTest.mX = aImage->mX + aImage->mWidth + 1;
			rectTest.mY = aImage->mY;
		}
		else  // 否则居于下方
		{
			rectTest.mX = aImage->mX;
			rectTest.mY = aImage->mY + aImage->mHeight + 1;
		}

		if (ImageFits(theImageCount, rectTest, theMaxWidth))  // 如果图片能够放得下
		{
			theAtlasImageToPlace->mX = rectTest.mX;
			theAtlasImageToPlace->mY = rectTest.mY;
			if (theToRight)
				theAtlasImageToPlace->mX += 1;
			else
				theAtlasImageToPlace->mY += 1;

			return true;
		}
	}

	return false;
}

bool ReanimAtlas::ImageFindPlace(ReanimAtlasImage* theAtlasImageToPlace, int theImageCount, int theMaxWidth)
{
	return 
		ImageFindPlaceOnSide(theAtlasImageToPlace, theImageCount, theMaxWidth, true) || 
		ImageFindPlaceOnSide(theAtlasImageToPlace, theImageCount, theMaxWidth, false);  // 分别尝试在居右和居下的位置放置图片
}

bool ReanimAtlas::PlaceAtlasImage(ReanimAtlasImage* theAtlasImageToPlace, int theImageCount, int theMaxWidth)
{
	if (theImageCount == 0)
	{
		theAtlasImageToPlace->mX = 1;
		theAtlasImageToPlace->mY = 1;
		return true;
	}

	if (ImageFindPlace(theAtlasImageToPlace, theImageCount, theMaxWidth))
		return true;

	TOD_ASSERT();
	return false;
}

//0x470580
void ReanimAtlas::ArrangeImages(int& theAtlasWidth, int& theAtlasHeight)
{
	std::sort(mImageArray, mImageArray + mImageCount, sSortByNonIncreasingHeight);  // 将所有图集图片按高度降序排序
	theAtlasWidth = PickAtlasWidth();
	theAtlasHeight = 0;

	for (int i = 0; i < mImageCount; i++)
	{
		ReanimAtlasImage* aImage = &mImageArray[i];
		PlaceAtlasImage(aImage, i, theAtlasWidth);

		/* 
			此处原为“theAtlasHeight = max(GetClosestPowerOf2Above(aImage->mY + aImage->mHeight), theAtlasHeight);”
			这样在 max 宏展开时，会导致 GetClosestPowerOf2Above(aImage->mY + aImage->mHeight) 被重复计算，故稍作修改
		*/ 

		int aImageHeight = GetClosestPowerOf2Above(aImage->mY + aImage->mHeight);
		theAtlasHeight = std::max(aImageHeight, theAtlasHeight);
	}
}

void ReanimAtlas::AddImage(Image* theImage)
{
	if (theImage->mNumCols == 1 && theImage->mNumRows == 1)
	{
		TOD_ASSERT(mImageCount < MAX_REANIM_IMAGES);

		ReanimAtlasImage* aImage = &mImageArray[mImageCount++];
#ifdef WII_PLATFORM
		mImageHashValid = false;
#endif
		aImage->mHeight = theImage->mHeight;
		aImage->mWidth = theImage->mWidth;
		aImage->mOriginalImage = theImage;
	}
}

int ReanimAtlas::FindImage(Image* theImage)
{
#ifdef WII_PLATFORM
	if (mImageHashValid)
	{
		unsigned int aSlot = ((uintptr_t)theImage >> 4) & 127U;
		for (unsigned int aProbe = 0; aProbe < 128U; aProbe++)
		{
			unsigned char aStored = mImageHash[aSlot];
			if (aStored == 0)
				return -1;

			int aIndex = (int)aStored - 1;
			if (mImageArray[aIndex].mOriginalImage == theImage)
				return aIndex;

			aSlot = (aSlot + 1U) & 127U;
		}
		return -1;
	}
#endif

	for (int i = 0; i < mImageCount; i++)
		if (mImageArray[i].mOriginalImage == theImage)
			return i;

	return -1;
}

#ifdef WII_PLATFORM
void ReanimAtlas::RebuildImageHash()
{
	memset(mImageHash, 0, sizeof(mImageHash));
	for (int i = 0; i < mImageCount; i++)
	{
		unsigned int aSlot = ((uintptr_t)mImageArray[i].mOriginalImage >> 4) & 127U;
		while (mImageHash[aSlot] != 0)
			aSlot = (aSlot + 1U) & 127U;
		mImageHash[aSlot] = (unsigned char)(i + 1);
	}
	mImageHashValid = true;
}
#endif

//0x470680
bool ReanimAtlas::ReanimAtlasCreate(ReanimatorDefinition* theReanimDef)
{
	PerfTimer aTimer;
	aTimer.Start();

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	printf("[ATLAS] create begin: %d tracks\n", theReanimDef->mTracks.count);
	// One atlas surface is one contiguous heap allocation, built at whatever
	// moment a reanimation of this type is first needed — often mid-
	// gameplay, with the heap already near its pressure ceiling and
	// fragmented by countless small lazy-image frees (see
	// ReanimAtlasMakeBlankMemoryImage). Cap how much source pixel area goes
	// in: the packer rounds each dimension up to a power of 2 and adds a
	// 1px border per image, so the final texture can run well past the raw
	// sum, hence the wide margin (budget targets a final surface around
	// 512x512/1MB instead of growing unbounded up to PickAtlasWidth's own
	// 2048 cap). Images left out keep their real Image* below (FindImage
	// returns -1 for them) and draw un-atlased — already-handled by every
	// draw/lookup site (GetEncodedReanimAtlas treats any value > 1000 as a
	// real pointer, not an atlas index), just without atlas batching.
	const int kMaxAtlasSourcePixels = 128 * 1024;
	int aAtlasPixelBudget = kMaxAtlasSourcePixels;
	int aSkippedCount = 0;
#endif
	for (int aTrackIndex = 0; aTrackIndex < theReanimDef->mTracks.count; aTrackIndex++)
	{
		ReanimatorTrack* aTrack = &theReanimDef->mTracks.tracks[aTrackIndex];
		for (int aKeyIndex = 0; aKeyIndex < aTrack->mTransforms.count; aKeyIndex++)  // 遍历每一帧上的贴图
		{
			Image* aImage = aTrack->mTransforms.mTransforms[aKeyIndex].mImage;
			// 如果存在贴图，且贴图的宽、高均不大于 254 像素，且相同的贴图未加入至图集图片数组中
			if (aImage != nullptr && aImage->mWidth <= 254 && aImage->mHeight <= 254 && FindImage(aImage) < 0)
			{
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
				int aImagePixels = aImage->mWidth * aImage->mHeight;
				if (aImagePixels > aAtlasPixelBudget)
				{
					aSkippedCount++;
					continue;
				}
				aAtlasPixelBudget -= aImagePixels;
#endif
				AddImage(aImage);  // 先将其加入数组中，后续再确定其位于图集中的位置
			}
		}
	}
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM)
	if (aSkippedCount > 0)
		printf("[ATLAS] %d images left un-atlased (pixel budget)\n", aSkippedCount);

	// Text-only/fullscreen reanimations legitimately contain no Image tracks.
	// A zero-image atlas used to become a 1x0 surface; the PS2 native texture
	// path cannot represent that safely on real hardware. No backing surface
	// is needed when there are no source images.
	if (mImageCount == 0)
	{
#ifdef PS2_PLATFORM
		printf("[ATLAS] no images; skipping atlas surface\n");
#endif
		return true;
	}
#endif

	int aAtlasWidth, aAtlasHeight;
#ifdef PS2_PLATFORM
	printf("[ATLAS] arranging %d images...\n", mImageCount);
#endif
	ArrangeImages(aAtlasWidth, aAtlasHeight);
#ifdef WII_PLATFORM
	RebuildImageHash();
#endif
#ifdef PS2_PLATFORM
	printf("[ATLAS] arranged: %dx%d (%.2f MB texture)\n",
		aAtlasWidth, aAtlasHeight,
		(double)aAtlasWidth * aAtlasHeight * 4.0 / (1024.0 * 1024.0));
#endif

	// Allocate before replacing any definition Image* with atlas indices. A
	// failed scratch reservation can therefore abandon this atlas atomically
	// and leave the definition fully valid for direct-image rendering.
	mMemoryImage = ReanimAtlasMakeBlankMemoryImage(aAtlasWidth, aAtlasHeight);
	if (mMemoryImage == nullptr)
		return false;

	for (int aTrackIndex = 0; aTrackIndex < theReanimDef->mTracks.count; aTrackIndex++)
	{
		ReanimatorTrack* aTrack = &theReanimDef->mTracks.tracks[aTrackIndex];
		for (int aKeyIndex = 0; aKeyIndex < aTrack->mTransforms.count; aKeyIndex++)  // 遍历每一帧上的贴图
		{
			Image*& aImage = aTrack->mTransforms.mTransforms[aKeyIndex].mImage;
			if (aImage != nullptr && aImage->mWidth <= 254 && aImage->mHeight <= 254)
			{
				intptr_t aImageIndex = FindImage(aImage);
#ifdef WII_PLATFORM
				// Keep the real pointer. GetEncodedReanimAtlas resolves it through
				// this atlas at draw time, so the definition never owns a fragile
				// atlas-local integer disguised as an Image*. A negative index is
				// valid when the console pixel budget leaves the image un-atlased;
				// the draw path then uses the original image directly.
#elif defined(PS2_PLATFORM)
				// -1 means the pixel budget above left this one out; keep its
				// real Image* so it draws un-atlased instead of asserting.
				if (aImageIndex >= 0)
					aImage = (Image*)(aImageIndex + 1);  // ★ 将图片在数组中的序号作为 Image* 修改动画定义
#else
				TOD_ASSERT(aImageIndex >= 0);
				aImage = (Image*)(aImageIndex + 1);  // ★ 将图片在数组中的序号作为 Image* 修改动画定义
#endif
			}
		}
	}

#ifdef PS2_PLATFORM
	printf("[ATLAS] drawing %d images into atlas...\n", mImageCount);
#endif
	Graphics aMemoryGraphis(mMemoryImage);
	for (int aImageIndex = 0; aImageIndex < mImageCount; aImageIndex++)
	{
		ReanimAtlasImage* aImage = &mImageArray[aImageIndex];
		aMemoryGraphis.DrawImage(aImage->mOriginalImage, aImage->mX, aImage->mY);  // 将原贴图绘制在图集上
	}
#ifdef PS2_PLATFORM
	printf("[ATLAS] fixing alpha edges...\n");
#endif
	FixPixelsOnAlphaEdgeForBlending(mMemoryImage);  // 将所有透明像素的颜色修正为其周围像素颜色的平均值
#ifdef PS2_PLATFORM
	printf("[ATLAS] done\n");
#endif
	return true;
}
