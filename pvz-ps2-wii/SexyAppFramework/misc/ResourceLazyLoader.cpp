#include "ResourceManager.h"

using namespace Sexy;

// The single resource-step implementation lives apart from XML parsing and
// resource-map ownership. Console loading screens and future queued loaders
// must delegate here instead of maintaining a second iterator/switch loop.
bool ResourceManager::LoadNextResource()
{
	if (HadError() || mCurResGroupList == NULL)
		return false;

	while (mCurResGroupListItr != mCurResGroupList->end())
	{
		BaseRes* aRes = *mCurResGroupListItr++;
		if (aRes->mFromProgram)
			continue;

		switch (aRes->mType)
		{
		case ResType_Image:
		{
			ImageRes* anImageRes = (ImageRes*)aRes;
			if ((GLImage*)anImageRes->mImage != NULL)
				continue;
			return DoLoadImage(anImageRes);
		}
		case ResType_Sound:
		{
			SoundRes* aSoundRes = (SoundRes*)aRes;
			if (aSoundRes->mSoundId != -1)
				continue;
			return DoLoadSound(aSoundRes);
		}
		case ResType_Font:
		{
			FontRes* aFontRes = (FontRes*)aRes;
			if (aFontRes->mFont != NULL)
				continue;
			return DoLoadFont(aFontRes);
		}
		}
	}
	return false;
}
