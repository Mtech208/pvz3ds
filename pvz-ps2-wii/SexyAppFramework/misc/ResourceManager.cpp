#include <cstdio>
#include <memory>
#include "ResourceManager.h"
#include "XMLParser.h"
#include "sound/SoundManager.h"
#include "graphics/GLImage.h"
#include "graphics/GLInterface.h"
#include "graphics/ImageFont.h"
#include "../../Sexy.TodLib/TodDebug.h"
//#include "graphics/SysFont.h"
#include "imagelib/ImageLib.h"

#ifdef PS2_PLATFORM
#include <pthread.h>
#endif
#include "PlatformIoLock.h"

//#define SEXY_PERF_ENABLED
#include "PerfTimer.h"

using namespace Sexy;

#ifdef SEXY_COMPACT_IMAGES
// Per-pixel alpha byte from a companion image without forcing its w*h*4
// expansion when it arrived compact (paletted PNG / 24-bit RGB). The byte
// read is the blue channel — exactly what the expanded compose loops read as
// bits[i] & 0xFF. Pointers are only valid while the source image is alive.
struct CompactAlphaSource
{
	const uint32_t* mBits;
	const uint32_t* mTable;
	const unsigned char* mIndices;
	const unsigned char* mRgb;

	bool Init(ImageLib::Image* theImage)
	{
		mBits = theImage->mBits;
		mTable = NULL;
		mIndices = NULL;
		mRgb = NULL;
		if (mBits != NULL)
			return true;
		if (theImage->mColorTable != NULL && theImage->mColorIndices != NULL)
		{
			mTable = theImage->mColorTable;
			mIndices = theImage->mColorIndices;
			return true;
		}
		// Always NULL off PS2 — no other target decodes into this form.
		if (theImage->mRGBBits != NULL)
		{
			mRgb = theImage->mRGBBits;
			return true;
		}
		// Unknown form: last resort is the 32-bit expansion (NULL on OOM).
		mBits = theImage->GetBits();
		return mBits != NULL;
	}

	unsigned char At(int thePixelIndex) const
	{
		if (mBits != NULL)
			return (unsigned char)(mBits[thePixelIndex] & 0xFF);
		if (mTable != NULL)
			return (unsigned char)(mTable[mIndices[thePixelIndex]] & 0xFF);
		return mRgb[thePixelIndex * 3 + 2];
	}
};
#endif

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::ImageRes::DeleteResource()
{	
	mImage.Release();
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::SoundRes::DeleteResource()
{
	if (mSoundId >= 0)
		gSexyAppBase->mSoundManager->ReleaseSound(mSoundId);

	mSoundId = -1;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::FontRes::DeleteResource()
{
	delete mFont;
	mFont = NULL;

	delete mImage;
	mImage = NULL;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
ResourceManager::ResourceManager(SexyAppBase *theApp) 
{
	mApp = theApp;
	mHasFailed = false;
	mXMLParser = NULL;

	mAllowMissingProgramResources = false;
	mAllowAlreadyDefinedResources = false;
	mCurResGroupList = NULL;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
ResourceManager::~ResourceManager()
{
	DeleteMap(mImageMap);
	DeleteMap(mSoundMap);
	DeleteMap(mFontMap);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::IsGroupLoaded(const std::string &theGroup)
{
	return mLoadedGroups.find(theGroup)!=mLoadedGroups.end();
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::DeleteMap(ResMap &theMap)
{
	for (ResMap::iterator anItr = theMap.begin(); anItr != theMap.end(); ++anItr)
	{
		anItr->second->DeleteResource();
		delete anItr->second;
	}

	theMap.clear();
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::DeleteResources(ResMap &theMap, const std::string &theGroup)
{
	for (ResMap::iterator anItr = theMap.begin(); anItr != theMap.end(); ++anItr)
	{
		if (theGroup.empty() || anItr->second->mResGroup==theGroup)
			anItr->second->DeleteResource();
	}
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::DeleteResources(const std::string &theGroup)
{
	DeleteResources(mImageMap,theGroup);
	DeleteResources(mSoundMap,theGroup);
	DeleteResources(mFontMap,theGroup);
	mLoadedGroups.erase(theGroup);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::DeleteExtraImageBuffers(const std::string &theGroup)
{
	for (ResMap::iterator anItr = mImageMap.begin(); anItr != mImageMap.end(); ++anItr)
	{
		if (theGroup.empty() || anItr->second->mResGroup==theGroup)
		{
			ImageRes *aRes = (ImageRes*)anItr->second;
			MemoryImage *anImage = (MemoryImage*)aRes->mImage;
			if (anImage != NULL)
				anImage->DeleteExtraBuffers();
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
std::string ResourceManager::GetErrorText()
{
	return mError;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::HadError()
{
	return mHasFailed;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::Fail(const std::string& theErrorText)
{
	if (!mHasFailed)
	{
		mHasFailed = true;
		if (mXMLParser==NULL)
		{
			mError = theErrorText;
			return false;
		}

		int aLineNum = mXMLParser->GetCurrentLineNum();

		char aLineNumStr[16];
		sprintf(aLineNumStr, "%d", aLineNum);	

		mError = theErrorText;

		if (aLineNum > 0)
			mError += std::string(" on Line ") + aLineNumStr;

		if (mXMLParser->GetFileName().length() > 0)
			mError += " in File '" + mXMLParser->GetFileName() + "'";
	}

	return false;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ParseCommonResource(XMLElement &theElement, BaseRes *theRes, ResMap &theMap)
{
	mHadAlreadyDefinedError = false;

	const SexyString &aPath = theElement.mAttributes[__S("path")];
	if (aPath.empty())
		return Fail("No path specified.");

	theRes->mXMLAttributes = theElement.mAttributes;
	theRes->mFromProgram = false;
	if (aPath[0]==__S('!'))
	{
		theRes->mPath = SexyStringToStringFast(aPath);
		if (aPath==__S("!program"))
			theRes->mFromProgram = true;
	}
	else
		theRes->mPath = mDefaultPath + SexyStringToStringFast(aPath);

	
	std::string anId;
	XMLParamMap::iterator anItr = theElement.mAttributes.find(__S("id"));
	if (anItr == theElement.mAttributes.end())
		anId = mDefaultIdPrefix + GetFileName(theRes->mPath,true);
	else
		anId = mDefaultIdPrefix + SexyStringToStringFast(anItr->second);

	theRes->mResGroup = mCurResGroup;
	theRes->mId = anId;

	std::pair<ResMap::iterator,bool> aRet = theMap.insert(ResMap::value_type(anId,theRes));
	if (!aRet.second)
	{
		mHadAlreadyDefinedError = true;
		return Fail("Resource already defined.");
	}

	mCurResGroupList->push_back(theRes);
	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ParseSoundResource(XMLElement &theElement)
{
	SoundRes *aRes = new SoundRes;
	aRes->mSoundId = -1;
	aRes->mVolume = -1;
	aRes->mPanning = 0;

	if (!ParseCommonResource(theElement, aRes, mSoundMap))
	{
		if (mHadAlreadyDefinedError && mAllowAlreadyDefinedResources)
		{
			mError = "";
			mHasFailed = false;
			SoundRes *oldRes = aRes;
			aRes = (SoundRes*)mSoundMap[oldRes->mId];
			aRes->mPath = oldRes->mPath;
			aRes->mXMLAttributes = oldRes->mXMLAttributes;
			delete oldRes;
		}
		else			
		{
			delete aRes;
			return false;
		}
	}
	
	XMLParamMap::iterator anItr;

	anItr = theElement.mAttributes.find(__S("volume"));
	if (anItr != theElement.mAttributes.end())
		sexysscanf(anItr->second.c_str(),__S("%lf"),&aRes->mVolume);

	anItr = theElement.mAttributes.find(__S("pan"));
	if (anItr != theElement.mAttributes.end())
		sexysscanf(anItr->second.c_str(),__S("%d"),&aRes->mPanning);

	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static void ReadIntVector(const SexyString &theVal, std::vector<int> &theVector)
{
	theVector.clear();

	std::string::size_type aPos = 0;
	while (true)
	{
		theVector.push_back(sexyatoi(theVal.c_str()+aPos));
		aPos = theVal.find_first_of(__S(','),aPos);
		if (aPos==std::string::npos)
			break;

		aPos++;
	}	
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ParseImageResource(XMLElement &theElement)
{
	ImageRes *aRes = new ImageRes;
	if (!ParseCommonResource(theElement, aRes, mImageMap))
	{
		if (mHadAlreadyDefinedError && mAllowAlreadyDefinedResources)
		{
			mError = "";
			mHasFailed = false;
			ImageRes *oldRes = aRes;
			aRes = (ImageRes*)mImageMap[oldRes->mId];
			aRes->mPath = oldRes->mPath;
			aRes->mXMLAttributes = oldRes->mXMLAttributes;
			delete oldRes;
		}
		else			
		{
			delete aRes;
			return false;
		}
	}
	
	aRes->mPalletize = theElement.mAttributes.find(__S("nopal")) == theElement.mAttributes.end();
	aRes->mA4R4G4B4 = theElement.mAttributes.find(__S("a4r4g4b4")) != theElement.mAttributes.end();
	aRes->mDDSurface = theElement.mAttributes.find(__S("ddsurface")) != theElement.mAttributes.end();
	aRes->mPurgeBits = (theElement.mAttributes.find(__S("nobits")) != theElement.mAttributes.end()) ||
		((mApp->Is3DAccelerated()) && (theElement.mAttributes.find(__S("nobits3d")) != theElement.mAttributes.end())) ||
		((!mApp->Is3DAccelerated()) && (theElement.mAttributes.find(__S("nobits2d")) != theElement.mAttributes.end()));
	aRes->mA8R8G8B8 = theElement.mAttributes.find(__S("a8r8g8b8")) != theElement.mAttributes.end();
	aRes->mMinimizeSubdivisions = theElement.mAttributes.find(__S("minsubdivide")) != theElement.mAttributes.end();
	aRes->mAutoFindAlpha = theElement.mAttributes.find(__S("noalpha")) == theElement.mAttributes.end();	

	XMLParamMap::iterator anItr;
	anItr = theElement.mAttributes.find(__S("alphaimage"));
	if (anItr != theElement.mAttributes.end())
		aRes->mAlphaImage = mDefaultPath + SexyStringToStringFast(anItr->second);

	aRes->mAlphaColor = 0xFFFFFF;
	anItr = theElement.mAttributes.find(__S("alphacolor"));
	if (anItr != theElement.mAttributes.end())
		sexysscanf(anItr->second.c_str(),__S("%lx"),&aRes->mAlphaColor);

	anItr = theElement.mAttributes.find(__S("variant"));
	if (anItr != theElement.mAttributes.end())
		aRes->mVariant = SexyStringToStringFast(anItr->second);

	anItr = theElement.mAttributes.find(__S("alphagrid"));
	if (anItr != theElement.mAttributes.end())
		aRes->mAlphaGridImage = mDefaultPath + SexyStringToStringFast(anItr->second);

	anItr = theElement.mAttributes.find(__S("rows"));
	if (anItr != theElement.mAttributes.end())
		aRes->mRows = sexyatoi(anItr->second.c_str());
	else
		aRes->mRows = 1;

	anItr = theElement.mAttributes.find(__S("cols"));
	if (anItr != theElement.mAttributes.end())
		aRes->mCols = sexyatoi(anItr->second.c_str());
	else
		aRes->mCols = 1;

	if (aRes->mRows < 1)
		aRes->mRows = 1;
	if (aRes->mCols < 1)
		aRes->mCols = 1;

	anItr = theElement.mAttributes.find(__S("anim"));
	AnimType anAnimType = AnimType_None;
	if (anItr != theElement.mAttributes.end())
	{
		const SexyChar *aType = anItr->second.c_str();

		if (strcasecmp(aType,__S("none"))==0) anAnimType = AnimType_None;
		else if (strcasecmp(aType,__S("once"))==0) anAnimType = AnimType_Once;
		else if (strcasecmp(aType,__S("loop"))==0) anAnimType = AnimType_Loop;
		else if (strcasecmp(aType,__S("pingpong"))==0) anAnimType = AnimType_PingPong;
		else 
		{
			Fail("Invalid animation type.");
			return false;
		}
	}
	aRes->mAnimInfo.mAnimType = anAnimType;
	if (anAnimType != AnimType_None)
	{
		int aNumCels = std::max(aRes->mRows,aRes->mCols);
		int aBeginDelay = 0, anEndDelay = 0;

		anItr = theElement.mAttributes.find(__S("framedelay"));
		if (anItr != theElement.mAttributes.end())
			aRes->mAnimInfo.mFrameDelay = sexyatoi(anItr->second.c_str());

		anItr = theElement.mAttributes.find(__S("begindelay"));
		if (anItr != theElement.mAttributes.end())
			aBeginDelay = sexyatoi(anItr->second.c_str());

		anItr = theElement.mAttributes.find(__S("enddelay"));
		if (anItr != theElement.mAttributes.end())
			anEndDelay = sexyatoi(anItr->second.c_str());

		anItr = theElement.mAttributes.find(__S("perframedelay"));
		if (anItr != theElement.mAttributes.end())
			ReadIntVector(anItr->second,aRes->mAnimInfo.mPerFrameDelay);

		anItr = theElement.mAttributes.find(__S("framemap"));
		if (anItr != theElement.mAttributes.end())
			ReadIntVector(anItr->second,aRes->mAnimInfo.mFrameMap);

		aRes->mAnimInfo.Compute(aNumCels,aBeginDelay,anEndDelay);
	}


	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ParseFontResource(XMLElement &theElement)
{
	FontRes *aRes = new FontRes;
	aRes->mFont = NULL;
	aRes->mImage = NULL;

	if (!ParseCommonResource(theElement, aRes, mFontMap))
	{
		if (mHadAlreadyDefinedError && mAllowAlreadyDefinedResources)
		{
			mError = "";
			mHasFailed = false;
			FontRes *oldRes = aRes;
			aRes = (FontRes*)mFontMap[oldRes->mId];
			aRes->mPath = oldRes->mPath;
			aRes->mXMLAttributes = oldRes->mXMLAttributes;
			delete oldRes;
		}
		else			
		{
			delete aRes;
			return false;
		}
	}


	XMLParamMap::iterator anItr;
	anItr = theElement.mAttributes.find(__S("image"));
	if (anItr != theElement.mAttributes.end())
		aRes->mImagePath = SexyStringToStringFast(anItr->second);

	anItr = theElement.mAttributes.find(__S("tags"));
	if (anItr != theElement.mAttributes.end())
		aRes->mTags = SexyStringToStringFast(anItr->second);

	if (strncmp(aRes->mPath.c_str(),"!sys:",5)==0)
	{
		aRes->mSysFont = true;
		aRes->mPath = aRes->mPath.substr(5);

		anItr = theElement.mAttributes.find(__S("size"));
		if (anItr==theElement.mAttributes.end())
			return Fail("SysFont needs point size");

		aRes->mSize = sexyatoi(anItr->second.c_str());
		if (aRes->mSize<=0)
			return Fail("SysFont needs point size");
			
		aRes->mBold = theElement.mAttributes.find(__S("bold"))!=theElement.mAttributes.end();
		aRes->mItalic = theElement.mAttributes.find(__S("italic"))!=theElement.mAttributes.end();
		aRes->mShadow = theElement.mAttributes.find(__S("shadow"))!=theElement.mAttributes.end();
		aRes->mUnderline = theElement.mAttributes.find(__S("underline"))!=theElement.mAttributes.end();
	}
	else
		aRes->mSysFont = false;

	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ParseSetDefaults(XMLElement &theElement)
{
	XMLParamMap::iterator anItr;
	anItr = theElement.mAttributes.find(__S("path"));
	if (anItr != theElement.mAttributes.end())
		mDefaultPath = RemoveTrailingSlash(SexyStringToStringFast(anItr->second)) + '/';

	anItr = theElement.mAttributes.find(__S("idprefix"));
	if (anItr != theElement.mAttributes.end())
		mDefaultIdPrefix = RemoveTrailingSlash(SexyStringToStringFast(anItr->second));	

	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ParseResources()
{
	for (;;)
	{
		XMLElement aXMLElement;
		if (!mXMLParser->NextElement(&aXMLElement))
			return false;
		
		if (aXMLElement.mType == XMLElement::TYPE_START)
		{
			if (aXMLElement.mValue == __S("Image"))
			{
				if (!ParseImageResource(aXMLElement))
					return false;

				if (!mXMLParser->NextElement(&aXMLElement))
					return false;

				if (aXMLElement.mType != XMLElement::TYPE_END)
					return Fail("Unexpected element found.");
			}
			else if (aXMLElement.mValue == __S("Sound"))
			{
				if (!ParseSoundResource(aXMLElement))
					return false;

				if (!mXMLParser->NextElement(&aXMLElement))
					return false;

				if (aXMLElement.mType != XMLElement::TYPE_END)
					return Fail("Unexpected element found.");
			}
			else if (aXMLElement.mValue == __S("Font"))
			{
				if (!ParseFontResource(aXMLElement))
					return false;

				if (!mXMLParser->NextElement(&aXMLElement))
					return false;

				if (aXMLElement.mType != XMLElement::TYPE_END)
					return Fail("Unexpected element found.");
			}
			else if (aXMLElement.mValue == __S("SetDefaults"))
			{
				if (!ParseSetDefaults(aXMLElement))
					return false;

				if (!mXMLParser->NextElement(&aXMLElement))
					return false;

				if (aXMLElement.mType != XMLElement::TYPE_END)
					return Fail("Unexpected element found.");		
			}
			else
			{
				Fail("Invalid Section '" + SexyStringToStringFast(aXMLElement.mValue) + "'");
				return false;
			}
		}
		else if (aXMLElement.mType == XMLElement::TYPE_ELEMENT)
		{
			Fail("Element Not Expected '" + SexyStringToStringFast(aXMLElement.mValue) + "'");
			return false;
		}		
		else if (aXMLElement.mType == XMLElement::TYPE_END)
		{
			return true;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::DoParseResources()
{
	if (!mXMLParser->HasFailed())
	{
		for (;;)
		{
			XMLElement aXMLElement;
			if (!mXMLParser->NextElement(&aXMLElement))
				break;

			if (aXMLElement.mType == XMLElement::TYPE_START)
			{
				if (aXMLElement.mValue == __S("Resources"))
				{
					mCurResGroup = SexyStringToStringFast(aXMLElement.mAttributes[__S("id")]);
					mCurResGroupList = &mResGroupMap[mCurResGroup];

					if (mCurResGroup.empty())
					{
						Fail("No id specified.");
						break;
					}

					if (!ParseResources())
						break;
				}
				else 
				{
					Fail("Invalid Section '" + SexyStringToStringFast(aXMLElement.mValue) + "'");
					break;
				}
			}
			else if (aXMLElement.mType == XMLElement::TYPE_ELEMENT)
			{
				Fail("Element Not Expected '" + SexyStringToStringFast(aXMLElement.mValue) + "'");
				break;
			}
		}
	}

	if (mXMLParser->HasFailed())
		Fail(SexyStringToStringFast(mXMLParser->GetErrorText()));

	delete mXMLParser;
	mXMLParser = NULL;

	return !mHasFailed;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ParseResourcesFile(const std::string& theFilename)
{
	mXMLParser = new XMLParser();
	if (!mXMLParser->OpenFile(theFilename))
		Fail("Resource file not found: " + theFilename);

	XMLElement aXMLElement;
	while (!mXMLParser->HasFailed())
	{
		if (!mXMLParser->NextElement(&aXMLElement))
			Fail(SexyStringToStringFast(mXMLParser->GetErrorText()));

		if (aXMLElement.mType == XMLElement::TYPE_START)
		{
			if (aXMLElement.mValue != __S("ResourceManifest"))
				break;
			else
				return DoParseResources();
		}
	}
		
	Fail("Expecting ResourceManifest tag");

	return DoParseResources();	
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ReparseResourcesFile(const std::string& theFilename)
{
	bool oldDefined = mAllowAlreadyDefinedResources;
	mAllowAlreadyDefinedResources = true;

	bool aResult = ParseResourcesFile(theFilename);

	mAllowAlreadyDefinedResources = oldDefined;

	return aResult;
}

///////////////////////////////////////////////////////////////////////////////
// Compose core shared by the load-time wrapper below and the lazy re-decode
// path (ReapplyLazyAlpha). Writes the companion's alpha into theImage's
// expanded 32-bit bits; an OOM while expanding leaves the image opaque.
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ComposeAlphaGridInto(MemoryImage *theImage, ImageLib::Image *theAlphaImage, int theNumRows, int theNumCols, const char *theNameForLog)
{
	if (theNumRows < 1)
		theNumRows = 1;
	if (theNumCols < 1)
		theNumCols = 1;

	int aCelWidth = theImage->mWidth/theNumCols;
	int aCelHeight = theImage->mHeight/theNumRows;

	if (theAlphaImage->mWidth!=aCelWidth || theAlphaImage->mHeight!=aCelHeight)
	{
		printf("[IMG] alphagrid size mismatch '%s' (%dx%d vs cel %dx%d); alpha dropped\n",
			theNameForLog, theAlphaImage->mWidth, theAlphaImage->mHeight, aCelWidth, aCelHeight);
		return false;
	}

	// On PS2 the alpha companion can arrive in compact form (24-bit JPG,
	// paletted PNG) with mBits still NULL until GetBits() expands it; composing
	// through the raw pointer reads address 0 — garbage alpha on emulators, a
	// TLB exception and silent freeze on real hardware. NULL from GetBits()
	// means the expansion OOMed: keep the image opaque rather than fail the load.
#ifdef SEXY_COMPACT_IMAGES
	// CompactAlphaSource reads the compact form in place — the companion's own
	// w*h*4 expansion is exactly the transient allocation that OOMs during
	// tight loads.
	CompactAlphaSource anAlphaSrc;
	uint32_t* aMasterRowPtr = theImage->GetBits();
	if (!anAlphaSrc.Init(theAlphaImage) || aMasterRowPtr == NULL)
#else
	uint32_t* anAlphaSrcBits = theAlphaImage->GetBits();
	uint32_t* aMasterRowPtr = theImage->GetBits();
	if (anAlphaSrcBits == NULL || aMasterRowPtr == NULL)
#endif
	{
		printf("[IMG] OOM expanding '%s' for alpha compose; alpha dropped\n", theNameForLog);
		return false;
	}
	for (int i=0; i < theNumRows; i++)
	{
		uint32_t *aMasterColPtr = aMasterRowPtr;
		for (int j=0; j < theNumCols; j++)
		{
			uint32_t* aRowPtr = aMasterColPtr;
#ifdef SEXY_COMPACT_IMAGES
			int anAlphaIndex = 0;
#else
			uint32_t* anAlphaBits = anAlphaSrcBits;
#endif
			for (int y=0; y<aCelHeight; y++)
			{
				uint32_t *aDestPtr = aRowPtr;
				for (int x=0; x<aCelWidth; x++)
				{
#ifdef SEXY_COMPACT_IMAGES
					*aDestPtr = (*aDestPtr & 0x00FFFFFF) | ((uint32_t)anAlphaSrc.At(anAlphaIndex++) << 24);
#else
					*aDestPtr = (*aDestPtr & 0x00FFFFFF) | ((*anAlphaBits & 0xFF) << 24);
					++anAlphaBits;
#endif
					++aDestPtr;
				}
				aRowPtr += theImage->mWidth;
			}

			aMasterColPtr += aCelWidth;
		}
		aMasterRowPtr += aCelHeight*theImage->mWidth;
	}

	theImage->BitsChanged();
	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::LoadAlphaGridImage(ImageRes *theRes, GLImage *theImage)
{
	ImageLib::Image* anAlphaImage = ImageLib::GetImage(theRes->mAlphaGridImage,true);
	if (anAlphaImage==NULL)
		return Fail(StrFormat("Failed to load image: %s",theRes->mAlphaGridImage.c_str()));

	std::unique_ptr<ImageLib::Image> aDelAlphaImage(anAlphaImage);

	int aCelWidth = theImage->mWidth/theRes->mCols;
	int aCelHeight = theImage->mHeight/theRes->mRows;

	if (anAlphaImage->mWidth!=aCelWidth || anAlphaImage->mHeight!=aCelHeight)
		return Fail(StrFormat("GridAlphaImage size mismatch between %s and %s",theRes->mPath.c_str(),theRes->mAlphaGridImage.c_str()));

	// An OOM inside the compose drops the alpha but is not a load failure.
	ComposeAlphaGridInto(theImage, anAlphaImage, theRes->mRows, theRes->mCols, theRes->mAlphaGridImage.c_str());
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Whole-image variant of ComposeAlphaGridInto; same sharing and OOM policy.
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ComposeAlphaInto(MemoryImage *theImage, ImageLib::Image *theAlphaImage, const char *theNameForLog)
{
	if (theAlphaImage->mWidth!=theImage->mWidth || theAlphaImage->mHeight!=theImage->mHeight)
	{
		printf("[IMG] alpha size mismatch '%s' (%dx%d vs %dx%d); alpha dropped\n",
			theNameForLog, theAlphaImage->mWidth, theAlphaImage->mHeight, theImage->mWidth, theImage->mHeight);
		return false;
	}

	// Same compact-image hazard as ComposeAlphaGridInto: expand via GetBits()
	// instead of reading mBits raw (NULL for 24-bit JPG / paletted PNG on PS2).
	int aSize = theImage->mWidth*theImage->mHeight;
#ifdef SEXY_COMPACT_IMAGES
	// Compact companion read in place — skips its w*h*4 expansion.
	CompactAlphaSource anAlphaSrc;
	uint32_t* aBits1 = theImage->GetBits();
	if (aBits1 == NULL || !anAlphaSrc.Init(theAlphaImage))
	{
		printf("[IMG] OOM expanding '%s' for alpha compose; alpha dropped\n", theNameForLog);
		return false;
	}

	for (int i = 0; i < aSize; i++)
		aBits1[i] = (aBits1[i] & 0x00FFFFFF) | ((uint32_t)anAlphaSrc.At(i) << 24);
#else
	uint32_t* aBits1 = theImage->GetBits();
	uint32_t* aBits2 = theAlphaImage->GetBits();
	if (aBits1 == NULL || aBits2 == NULL)
	{
		printf("[IMG] OOM expanding '%s' for alpha compose; alpha dropped\n", theNameForLog);
		return false;
	}

	for (int i = 0; i < aSize; i++)
	{
		*aBits1 = (*aBits1 & 0x00FFFFFF) | ((*aBits2 & 0xFF) << 24);
		++aBits1;
		++aBits2;
	}
#endif

	theImage->BitsChanged();
	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::LoadAlphaImage(ImageRes *theRes, GLImage *theImage)
{
	SEXY_PERF_BEGIN("ResourceManager::GetImage");
	ImageLib::Image* anAlphaImage = ImageLib::GetImage(theRes->mAlphaImage,true);
	SEXY_PERF_END("ResourceManager::GetImage");

	if (anAlphaImage==NULL)
		return Fail(StrFormat("Failed to load image: %s",theRes->mAlphaImage.c_str()));

	std::unique_ptr<ImageLib::Image> aDelAlphaImage(anAlphaImage);

	if (anAlphaImage->mWidth!=theImage->mWidth || anAlphaImage->mHeight!=theImage->mHeight)
		return Fail(StrFormat("AlphaImage size mismatch between %s and %s",theRes->mPath.c_str(),theRes->mAlphaImage.c_str()));

	// An OOM inside the compose drops the alpha but is not a load failure.
	ComposeAlphaInto(theImage, anAlphaImage, theRes->mAlphaImage.c_str());
	return true;
}

#ifdef SEXY_LAZY_IMAGES
///////////////////////////////////////////////////////////////////////////////
// Re-applies the alpha-companion recipe stored by DoLoadImage after every
// lazy (re-)decode of the main image. Because the composite is reproducible
// this way, images with explicit alpha no longer need a lifetime pin — the
// zombie-note letters (~750KB each) and the night-pool overlays used to
// accumulate as unpurgeable residents until the heap tipped over.
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ReapplyLazyAlpha(MemoryImage *theImage)
{
	bool aGrid = !theImage->mLazyAlphaGridPath.empty();
	const std::string& aPath = aGrid ? theImage->mLazyAlphaGridPath : theImage->mLazyAlphaImagePath;
	if (aPath.empty())
		return true;

	ImageLib::Image* anAlphaImage = ImageLib::GetImage(aPath, true);
	if (anAlphaImage == NULL)
	{
		printf("[IMG] lazy alpha companion missing '%s'; alpha dropped\n", aPath.c_str());
		return false;
	}
	std::unique_ptr<ImageLib::Image> aDelAlphaImage(anAlphaImage);

	bool aComposed = aGrid
		? ComposeAlphaGridInto(theImage, anAlphaImage, theImage->mLazyAlphaRows, theImage->mLazyAlphaCols, aPath.c_str())
		: ComposeAlphaInto(theImage, anAlphaImage, aPath.c_str());

	// The compose expanded the image to 32bpp; quantizing recovers the
	// 8-bit+CLUT form (1B/px) when the composited pixels fit in 256 values —
	// the mostly-black zombie notes do.
	if (aComposed)
		theImage->Palletize();
	return aComposed;
}
#endif

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::DoLoadImage(ImageRes *theRes)
{
	//bool lookForAlpha = theRes->mAlphaImage.empty() && theRes->mAlphaGridImage.empty() && theRes->mAutoFindAlpha; // unused
	
	SEXY_PERF_BEGIN("ResourceManager:GetImage");

	//ImageLib::Image *anImage = ImageLib::GetImage(theRes->mPath, lookForAlpha);
	//SEXY_PERF_END("ResourceManager:GetImage");

	bool isNew;
	ImageLib::gAlphaComposeColor = theRes->mAlphaColor;
	SharedImageRef aSharedImageRef = gSexyAppBase->GetSharedImage(theRes->mPath, theRes->mVariant, &isNew);
	ImageLib::gAlphaComposeColor = 0xFFFFFF;

	GLImage* aGLImage = (GLImage*) aSharedImageRef;
	
	if (aGLImage == NULL)
		return Fail(StrFormat("Failed to load image: %s",theRes->mPath.c_str()));

	if (isNew)
	{
#ifdef SEXY_LAZY_IMAGES
		// A lazily-stubbed image with exactly one explicit alpha companion
		// does not compose (or even decode) here: the recipe is stored on the
		// image and LoadLazyImageBits re-applies it on every decode. That
		// keeps these images purgeable — pinning them for the session leaked
		// ~750KB per zombie-note letter as the adventure progressed — and
		// skips their decode entirely until first draw.
		bool aLazyRecipe = aGLImage->mLazyUnloaded &&
			(theRes->mAlphaImage.empty() != theRes->mAlphaGridImage.empty());
		if (aLazyRecipe)
		{
			aGLImage->mLazyAlphaImagePath = theRes->mAlphaImage;
			aGLImage->mLazyAlphaGridPath = theRes->mAlphaGridImage;
			aGLImage->mLazyAlphaRows = theRes->mRows;
			aGLImage->mLazyAlphaCols = theRes->mCols;

			// The deferred compose can only log, so companion problems must
			// still fail the load loudly here; the header probe is cheap.
			const std::string& anAlphaPath = theRes->mAlphaGridImage.empty() ?
				theRes->mAlphaImage : theRes->mAlphaGridImage;
			int anAlphaWidth, anAlphaHeight;
			PlatformIoLockAcquire();
			bool anAlphaDimsOk = ImageLib::GetImageDims(anAlphaPath, anAlphaWidth, anAlphaHeight);
			PlatformIoLockRelease();
			if (!anAlphaDimsOk)
				return Fail(StrFormat("Failed to load image: %s", anAlphaPath.c_str()));
			if (!theRes->mAlphaGridImage.empty())
			{
				if (anAlphaWidth != aGLImage->mWidth/theRes->mCols || anAlphaHeight != aGLImage->mHeight/theRes->mRows)
					return Fail(StrFormat("GridAlphaImage size mismatch between %s and %s",theRes->mPath.c_str(),theRes->mAlphaGridImage.c_str()));
			}
			else if (anAlphaWidth != aGLImage->mWidth || anAlphaHeight != aGLImage->mHeight)
				return Fail(StrFormat("AlphaImage size mismatch between %s and %s",theRes->mPath.c_str(),theRes->mAlphaImage.c_str()));
		}
		else
#endif
		{
			if (!theRes->mAlphaImage.empty() || !theRes->mAlphaGridImage.empty())
			{
				// Explicit alpha composition on an eager-loaded image (or with
				// both companion kinds at once) can't be reproduced by a plain
				// re-decode of mFilePath: force the decode now and never purge.
				aGLImage->GetBits();
				aGLImage->mLazyPinned = true;
			}

			if (!theRes->mAlphaImage.empty())
			{
				if (!LoadAlphaImage(theRes, aSharedImageRef))
					return false;
			}

			if (!theRes->mAlphaGridImage.empty())
			{
				if (!LoadAlphaGridImage(theRes, aSharedImageRef))
					return false;
			}
		}
	}
	
	aGLImage->CommitBits();
	theRes->mImage = aSharedImageRef;
	aGLImage->mPurgeBits = theRes->mPurgeBits;

	if (theRes->mDDSurface)
	{
		SEXY_PERF_BEGIN("ResourceManager:DDSurface");

		aGLImage->CommitBits();
				
		if (!aGLImage->mHasAlpha)
		{
			//aGLImage->mWantDDSurface = true;
			aGLImage->mPurgeBits = true;			
		}

		SEXY_PERF_END("ResourceManager:DDSurface");
	}	

	/*
	if (theRes->mPalletize)
	{
		SEXY_PERF_BEGIN("ResourceManager:Palletize");
		if (aGLImage->mSurface==NULL)
			aGLImage->Palletize();
		else
			aGLImage->mWantPal = true;
		SEXY_PERF_END("ResourceManager:Palletize");
	}
	*/

	if (theRes->mA4R4G4B4)
		aGLImage->mD3DFlags |= D3DImageFlag_UseA4R4G4B4;

	if (theRes->mA8R8G8B8)
		aGLImage->mD3DFlags |= D3DImageFlag_UseA8R8G8B8;

	if (theRes->mMinimizeSubdivisions)
		aGLImage->mD3DFlags |= D3DImageFlag_MinimizeNumSubdivisions;

	if (theRes->mAnimInfo.mAnimType != AnimType_None)
		aGLImage->mAnimInfo = new AnimInfo(theRes->mAnimInfo);

	aGLImage->mNumRows = theRes->mRows;
	aGLImage->mNumCols = theRes->mCols;

#if defined(NINTENDO_3DS)
	// On 3DS, eagerly commit + purge ALL loaded images regardless of
	// mPurgeBits.  The standard heap (operator new) is limited and keeping
	// decoded CPU pixels for 367+ images causes OOM at particle definition
	// load.  GPU textures live in the separate linear heap (linearAlloc) and
	// RecoverBits() can restore pixels from the GPU if needed later.
	{
		bool aHasPixelData = (aGLImage->mBits != NULL) || (aGLImage->mColorIndices != NULL);
		if (aHasPixelData && gSexyAppBase->mGLInterface != nullptr)
		{
			gSexyAppBase->mGLInterface->CreateImageTexture(aGLImage);
		}
		aGLImage->PurgeBits();
	}
#elif defined(PS2_PLATFORM)
	if (aGLImage->mPurgeBits)
	{
		// PurgeBits() only actually frees anything once mD3DData exists
		// (see MemoryImage::PurgeBits: Is3DAccelerated() + mD3DData==NULL
		// just re-arms the flag for later). Nothing calls CreateImageTexture
		// at load time otherwise, so a purge-flagged resource (nobits* attrs
		// above, or an opaque ddsurface one) keeps its full decoded bits
		// resident until its first draw. That is exactly what starves
		// CreditScreen::PreLoadCredits: it loads 6 backgrounds back-to-back
		// before any of them ever draws, so all 6 sit fully decoded through
		// the ~35-atlas load that follows. Commit right here instead, same
		// as ReanimAtlas already does for the same reason.
		//
		// Guard on actually having pixel data: under heap pressure the decode
		// above can itself have OOM'd (LoadLazyImageBits logs "decode FAILED"
		// and leaves the image with no bits at all). Forcing a texture
		// upload from an image with nothing decoded would hand the GS a
		// bogus/empty transfer — that is how you get an emulator-level
		// "FQC = 0 on VIF FIFO READ" instead of a clean OOM log line. Bail
		// the same way PurgeBits() itself already does in that situation.
		bool aHasPixelData = (aGLImage->mBits != NULL) || (aGLImage->mColorIndices != NULL);
#ifdef PS2_PLATFORM
		aHasPixelData = aHasPixelData || (aGLImage->mRGBBits != NULL);
#endif
		if (aHasPixelData && gSexyAppBase->mGLInterface != nullptr &&
			(gSexyAppBase->mPrimaryThreadId == 0 ||
			 (void*)pthread_self() == gSexyAppBase->mPrimaryThreadId))
		{
			gSexyAppBase->mGLInterface->CreateImageTexture(aGLImage);
		}
		else
			aGLImage->PurgeBits();
	}
#else
	if (aGLImage->mPurgeBits)
		aGLImage->PurgeBits();
#endif

	ResourceLoadedHook(theRes);
	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::DeleteImage(const std::string &theName)
{
	ReplaceImage(theName,NULL);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
SharedImageRef ResourceManager::LoadImage(const std::string &theName)
{
	ResMap::iterator anItr = mImageMap.find(theName);
	if (anItr == mImageMap.end())
		return NULL;

	ImageRes *aRes = (ImageRes*)anItr->second;
	if ((GLImage*) aRes->mImage != NULL)
		return aRes->mImage;

	if (aRes->mFromProgram)
		return NULL;

	if (!DoLoadImage(aRes))
		return NULL;

	return aRes->mImage;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::DoLoadSound(SoundRes* theRes)
{
	SoundRes *aRes = theRes;

	SEXY_PERF_BEGIN("ResourceManager:LoadSound");
	int aSoundId = mApp->mSoundManager->GetFreeSoundId();
	if (aSoundId<0)
		return Fail("Out of free sound ids");

	if(!mApp->mSoundManager->LoadSound(aSoundId, aRes->mPath))
		return Fail(StrFormat("Failed to load sound: %s",aRes->mPath.c_str()));
	SEXY_PERF_END("ResourceManager:LoadSound");

	if (aRes->mVolume >= 0)
		mApp->mSoundManager->SetBaseVolume(aSoundId, aRes->mVolume);

	if (aRes->mPanning != 0)
		mApp->mSoundManager->SetBasePan(aSoundId, aRes->mPanning);

	aRes->mSoundId = aSoundId;

	ResourceLoadedHook(theRes);
	return true;
}

#include <../Sexy.TodLib/TodCommon.h>
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::DoLoadFont(FontRes* theRes)
{
	_Font *aFont = NULL;

	SEXY_PERF_BEGIN("ResourceManager:DoLoadFont");

	if (theRes->mSysFont)
	{
		/*
		bool bold = theRes->mBold, simulateBold = false;
		if (Sexy::CheckFor98Mill())
		{
			simulateBold = bold;
			bold = false;
		}
		aFont = new SysFont(theRes->mPath,theRes->mSize,bold,theRes->mItalic,theRes->mUnderline);
		SysFont* aSysFont = (SysFont*)aFont;
		aSysFont->mDrawShadow = theRes->mShadow;
		aSysFont->mSimulateBold = simulateBold;
		*/
	}
	else if (theRes->mImagePath.empty())	
	{
		if (strncmp(theRes->mPath.c_str(),"!ref:",5)==0)
		{
			std::string aRefName = theRes->mPath.substr(5);
			_Font *aRefFont = GetFont(aRefName);
			if (aRefFont==NULL)
				return Fail("Ref font not found: " + aRefName);

			aFont = aRefFont->Duplicate();
		}
		else
			aFont = new ImageFont(mApp, theRes->mPath);
	}
	else
	{
		Image *anImage = mApp->GetImage(theRes->mImagePath);
		if (anImage == NULL)
			anImage = LoadImage(theRes->mImagePath);

		if (anImage == NULL)
			return Fail(StrFormat("Failed to load image: %s", theRes->mImagePath.c_str()));

		theRes->mImage = anImage;
		aFont = new ImageFont(anImage, theRes->mPath);
	}

	ImageFont *anImageFont = dynamic_cast<ImageFont*>(aFont);
	if (anImageFont!=NULL)
	{
		if (anImageFont->mFontData==NULL || !anImageFont->mFontData->mInitialized)
		{
			TodTraceAndLog("[RESOURCEMANAGER ERROR] Failed to load font '%s': mFontData=%p, mInitialized=%d, mError='%s'",
				theRes->mPath.c_str(),
				anImageFont->mFontData,
				anImageFont->mFontData ? (int)anImageFont->mFontData->mInitialized : 0,
				anImageFont->mFontData ? anImageFont->mFontData->mError.c_str() : "NULL");
			delete aFont;
			return Fail(StrFormat("Failed to load font: %s",theRes->mPath.c_str()));
		}

		if (!theRes->mTags.empty())
		{
			char aBuf[1024];
			strcpy(aBuf,theRes->mTags.c_str());
			const char *aPtr = strtok(aBuf,", \r\n\t");
			while (aPtr != NULL)
			{
				anImageFont->AddTag(aPtr);
				aPtr = strtok(NULL,", \r\n\t");
			}
			anImageFont->Prepare();
		}
	}

	theRes->mFont = aFont;

	SEXY_PERF_END("ResourceManager:DoLoadFont");

	ResourceLoadedHook(theRes);
	return true;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
_Font* ResourceManager::LoadFont(const std::string &theName)
{
	ResMap::iterator anItr = mFontMap.find(theName);
	if (anItr == mFontMap.end())
		return NULL;

	FontRes *aRes = (FontRes*)anItr->second;
	if (aRes->mFont != NULL)
		return aRes->mFont;

	if (aRes->mFromProgram)
		return NULL;

	if (!DoLoadFont(aRes))
		return NULL;

	return aRes->mFont;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::DeleteFont(const std::string &theName)
{
	ReplaceFont(theName,NULL);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::ResourceLoadedHook(BaseRes*){}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::StartLoadResources(const std::string &theGroup)
{
	mError = "";
	mHasFailed = false;

	mCurResGroup = theGroup;
	mCurResGroupList = &mResGroupMap[theGroup];
	mCurResGroupListItr = mCurResGroupList->begin();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
void ResourceManager::DumpCurResGroup(std::string& theDestStr)
{
	const ResList* rl = &mResGroupMap.find(mCurResGroup)->second;
	ResList::const_iterator it = rl->begin();
	theDestStr = StrFormat("About to dump %d elements from current res group name %s\r\n", rl->size(), mCurResGroup.c_str());
	
	ResList::const_iterator rl_end = rl->end();
	while (it != rl_end)
	{
		BaseRes* br = *it++;
		std::string prefix = StrFormat("%s: %s\r\n", br->mId.c_str(), br->mPath.c_str());
		theDestStr += prefix;
		if (br->mFromProgram)
			theDestStr += std::string("     res is from program\r\n");
		else if (br->mType == ResType_Image)
			theDestStr += std::string("     res is an image\r\n");
		else if (br->mType == ResType_Sound)
			theDestStr += std::string("     res is a sound\r\n");
		else if (br->mType == ResType_Font)
			theDestStr += std::string("     res is a font\r\n");

		if (it == mCurResGroupListItr)
			theDestStr += std::string("iterator has reached mCurResGroupItr\r\n");

	}

	theDestStr += std::string("Done dumping resources\r\n");
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::LoadResources(const std::string &theGroup)
{
	mError = "";
	mHasFailed = false;
	StartLoadResources(theGroup);
	while (LoadNextResource())
	{
	}

	if (!HadError())
	{
		mLoadedGroups.insert(theGroup);
		return true;
	}
	else
		return false;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
int	ResourceManager::GetNumResources(const std::string &theGroup, ResMap &theMap)
{
	if (theGroup.empty())
		return theMap.size();

	int aCount = 0;
	for (ResMap::iterator anItr = theMap.begin(); anItr != theMap.end(); ++anItr)
	{
		BaseRes *aRes = anItr->second;
		if (aRes->mResGroup==theGroup && !aRes->mFromProgram)
			++aCount;
	}

	return aCount;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
int	ResourceManager::GetNumImages(const std::string &theGroup)
{
	return GetNumResources(theGroup, mImageMap);
}
	
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
int	ResourceManager::GetNumSounds(const std::string &theGroup)
{
	return GetNumResources(theGroup,mSoundMap);
}
	
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
int ResourceManager::GetNumFonts(const std::string &theGroup)
{
	return GetNumResources(theGroup, mFontMap);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
int	ResourceManager::GetNumResources(const std::string &theGroup)
{
	return GetNumImages(theGroup) + GetNumSounds(theGroup) + GetNumFonts(theGroup);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
SharedImageRef ResourceManager::GetImage(const std::string &theId)
{
	ResMap::iterator anItr = mImageMap.find(theId);
	if (anItr != mImageMap.end())
		return ((ImageRes*)anItr->second)->mImage;
	else
		return NULL;
}
	
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
int	ResourceManager::GetSound(const std::string &theId)
{
	ResMap::iterator anItr = mSoundMap.find(theId);
	if (anItr != mSoundMap.end())
		return ((SoundRes*)anItr->second)->mSoundId;
	else
		return -1;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
_Font* ResourceManager::GetFont(const std::string &theId)
{
	ResMap::iterator anItr = mFontMap.find(theId);
	if (anItr != mFontMap.end())
		return ((FontRes*)anItr->second)->mFont;
	else
		return NULL;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
SharedImageRef ResourceManager::GetImageThrow(const std::string &theId)
{
	ResMap::iterator anItr = mImageMap.find(theId);
	if (anItr != mImageMap.end())
	{
		ImageRes *aRes = (ImageRes*)anItr->second;
		if ((MemoryImage*) aRes->mImage != NULL)
			return aRes->mImage;

		if (mAllowMissingProgramResources && aRes->mFromProgram)
			return NULL;
	}


	Fail(StrFormat("Image resource not found: %s",theId.c_str()));
	throw ResourceManagerException(GetErrorText());
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
int	ResourceManager::GetSoundThrow(const std::string &theId)
{
	ResMap::iterator anItr = mSoundMap.find(theId);
	if (anItr != mSoundMap.end())
	{
		SoundRes *aRes = (SoundRes*)anItr->second;
		if (aRes->mSoundId!=-1)
			return aRes->mSoundId;

		if (mAllowMissingProgramResources && aRes->mFromProgram)
			return -1;
	}


	Fail(StrFormat("Sound resource not found: %s",theId.c_str()));
	throw ResourceManagerException(GetErrorText());		
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
_Font* ResourceManager::GetFontThrow(const std::string &theId)
{
	ResMap::iterator anItr = mFontMap.find(theId);
	if (anItr != mFontMap.end())
	{
		FontRes *aRes = (FontRes*)anItr->second;
		if (aRes->mFont!=NULL)
			return aRes->mFont;

		if (mAllowMissingProgramResources && aRes->mFromProgram)
			return NULL;
	}

	Fail(StrFormat("Font resource not found: %s",theId.c_str()));
	throw ResourceManagerException(GetErrorText());
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void ResourceManager::SetAllowMissingProgramImages(bool allow)
{
	mAllowMissingProgramResources = allow;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ReplaceImage(const std::string &theId, Image *theImage)
{
	ResMap::iterator anItr = mImageMap.find(theId);
	if (anItr != mImageMap.end())
	{
		anItr->second->DeleteResource();
		((ImageRes*)anItr->second)->mImage = (MemoryImage*) theImage;
		((ImageRes*)anItr->second)->mImage.mOwnsUnshared = true;
		return true;
	}
	else
		return false;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ReplaceSound(const std::string &theId, int theSound)
{
	ResMap::iterator anItr = mSoundMap.find(theId);
	if (anItr != mSoundMap.end())
	{
		anItr->second->DeleteResource();
		((SoundRes*)anItr->second)->mSoundId = theSound;
		return true;
	}
	else
		return false;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
bool ResourceManager::ReplaceFont(const std::string &theId, _Font *theFont)
{
	ResMap::iterator anItr = mFontMap.find(theId);
	if (anItr != mFontMap.end())
	{
		anItr->second->DeleteResource();
		((FontRes*)anItr->second)->mFont = theFont;
		return true;
	}
	else
		return false;
}


///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
const XMLParamMap& ResourceManager::GetImageAttributes(const std::string &theId)
{
	static XMLParamMap aStrMap;

	ResMap::iterator anItr = mImageMap.find(theId);
	if (anItr != mImageMap.end())
		return anItr->second->mXMLAttributes;
	else
		return aStrMap;
}
