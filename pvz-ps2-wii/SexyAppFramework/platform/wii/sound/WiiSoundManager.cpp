#ifdef WII_PLATFORM

#include "WiiSoundManager.h"
// wiiLog, not printf, for anything that reports a failure: printf reaches only
// stdout, which nothing is reading once GX owns the framebuffer, while wiiLog
// also goes to SYS_Report (the OSREPORT lines in Dolphin's log / USB Gecko) and
// to the engine's userdata/log.txt. Every music failure below used to be
// therefore invisible in exactly the situation worth diagnosing.
#include "wii/WiiEarlyInit.h"

#include "paklib/PakInterface.h"
#include "misc/SexyEndian.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>

#include <gccore.h>
#include <asndlib.h>

// Plain stb_vorbis, same vendored copy the PS2 backend uses (it works
// standalone when STB_VORBIS_SDL is not defined). The implementation is pulled
// in HERE and only here -- which is why the music streaming lives in this file
// rather than its own, exactly as on PS2.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_MAX_CHANNELS 2
#include "../../../sound/SDL-Mixer-X/src/codecs/stb_vorbis/stb_vorbis.h"

using namespace Sexy;

// Voice 0 streams the music; effects get the rest.
#define WII_MUSIC_VOICE		0
#define WII_FIRST_SFX_VOICE	1
#define WII_VOL_MAX			255

namespace
{

// The DSP reads by DMA and ignores the CPU data cache, so every buffer it will
// ever see is cache-line aligned, padded to a whole line, and flushed. Padding
// matters as much as alignment: DCFlushRange rounds outward, and a tail that
// shares its line with another allocation would otherwise be written back over
// whatever else lives there.
short* WiiAllocSampleBuffer(int theWantedBytes, int& thePaddedBytesOut)
{
	const int aPadded = (theWantedBytes + 31) & ~31;
	short* aBuffer = (short*)memalign(32, (size_t)aPadded);
	if (aBuffer == NULL)
	{
		thePaddedBytesOut = 0;
		return NULL;
	}
	// Zero the padding so the DSP plays silence rather than heap garbage if it
	// runs a few samples past the real end.
	memset((unsigned char*)aBuffer + theWantedBytes, 0, (size_t)(aPadded - theWantedBytes));
	thePaddedBytesOut = aPadded;
	return aBuffer;
}

void WiiPublishSampleBuffer(void* theBuffer, int thePaddedBytes)
{
	DCFlushRange(theBuffer, (u32)thePaddedBytes);
}

// Decode a whole OGG into one buffer. stb_vorbis emits interleaved 16-bit
// samples in host byte order -- big-endian here -- which is precisely what
// VOICE_*_16BIT means to ASND, so nothing is swapped on this path.
bool WiiDecodeOgg(const unsigned char* theData, int theSize, WiiSample& theSampleOut)
{
	int aChannels = 0, aRate = 0;
	short* aDecoded = NULL;
	const int aFrames = stb_vorbis_decode_memory(
		(const unsigned char*)theData, theSize, &aChannels, &aRate, &aDecoded);
	if (aFrames <= 0 || aDecoded == NULL)
	{
		if (aDecoded != NULL)
			free(aDecoded);
		return false;
	}
	if (aChannels > 2)
		aChannels = 2;

	const int aBytes = aFrames * aChannels * (int)sizeof(short);
	int aPadded = 0;
	short* aBuffer = WiiAllocSampleBuffer(aBytes, aPadded);
	if (aBuffer == NULL)
	{
		free(aDecoded);
		return false;
	}
	memcpy(aBuffer, aDecoded, (size_t)aBytes);
	free(aDecoded);
	WiiPublishSampleBuffer(aBuffer, aPadded);

	theSampleOut.mPCM = aBuffer;
	theSampleOut.mNumBytes = aPadded;
	theSampleOut.mSampleRate = aRate;
	theSampleOut.mFormat = (aChannels >= 2) ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
	return true;
}

// Minimal RIFF/WAVE reader for 16-bit PCM.
//
// WAV is a little-endian format, so BOTH the chunk headers AND the sample words
// need converting on this big-endian target -- the headers via SexyLE*, and the
// samples in the copy loop below. Getting the second part wrong is the classic
// "the sound plays but it is white noise" bug: the samples survive, byte-
// swapped, which is unrecognisable as audio.
bool WiiDecodeWav(const unsigned char* theData, int theSize, WiiSample& theSampleOut)
{
	if (theSize < 44 || memcmp(theData, "RIFF", 4) != 0 || memcmp(theData + 8, "WAVE", 4) != 0)
		return false;

	int aChannels = 0, aRate = 0, aBitsPerSample = 0;
	const unsigned char* aSamples = NULL;
	int aSampleBytes = 0;

	int aPos = 12;
	while (aPos + 8 <= theSize)
	{
		uint32_t aChunkSize;
		memcpy(&aChunkSize, theData + aPos + 4, 4);
		aChunkSize = SexyLE32(aChunkSize);
		const unsigned char* aBody = theData + aPos + 8;
		const int aBodyMax = theSize - (aPos + 8);
		if ((int)aChunkSize > aBodyMax)
			aChunkSize = (uint32_t)(aBodyMax > 0 ? aBodyMax : 0);

		if (memcmp(theData + aPos, "fmt ", 4) == 0 && aChunkSize >= 16)
		{
			uint16_t aFormatTag, aNumChannels, aBits;
			uint32_t aSampleRate;
			memcpy(&aFormatTag, aBody + 0, 2);
			memcpy(&aNumChannels, aBody + 2, 2);
			memcpy(&aSampleRate, aBody + 4, 4);
			memcpy(&aBits, aBody + 14, 2);
			if (SexyLE16(aFormatTag) != 1)	// PCM only
				return false;
			aChannels = SexyLE16(aNumChannels);
			aRate = (int)SexyLE32(aSampleRate);
			aBitsPerSample = SexyLE16(aBits);
		}
		else if (memcmp(theData + aPos, "data", 4) == 0)
		{
			aSamples = aBody;
			aSampleBytes = (int)aChunkSize;
		}

		// Chunks are word-aligned: an odd size carries a pad byte.
		aPos += 8 + (int)aChunkSize + ((aChunkSize & 1) ? 1 : 0);
	}

	if (aSamples == NULL || aSampleBytes <= 0 || aBitsPerSample != 16 ||
		aChannels < 1 || aChannels > 2 || aRate <= 0)
		return false;

	int aPadded = 0;
	short* aBuffer = WiiAllocSampleBuffer(aSampleBytes, aPadded);
	if (aBuffer == NULL)
		return false;

	const int aCount = aSampleBytes / (int)sizeof(short);
	for (int i = 0; i < aCount; i++)
	{
		uint16_t aWord;
		memcpy(&aWord, aSamples + i * 2, 2);
		aBuffer[i] = (short)SexyLE16(aWord);
	}
	WiiPublishSampleBuffer(aBuffer, aPadded);

	theSampleOut.mPCM = aBuffer;
	theSampleOut.mNumBytes = aPadded;
	theSampleOut.mSampleRate = aRate;
	theSampleOut.mFormat = (aChannels >= 2) ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
	return true;
}

// Reads a whole asset through the pak layer. Effects live inside main.pak, so
// raw stdio would not find them.
unsigned char* WiiReadWholeFile(const std::string& thePath, int& theSizeOut)
{
	PFILE* fp = p_fopen(thePath.c_str(), "rb");
	if (fp == NULL)
		return NULL;

	p_fseek(fp, 0, SEEK_END);
	const int aSize = (int)p_ftell(fp);
	p_fseek(fp, 0, SEEK_SET);
	if (aSize <= 0)
	{
		p_fclose(fp);
		return NULL;
	}

	unsigned char* aData = (unsigned char*)malloc((size_t)aSize);
	if (aData == NULL)
	{
		p_fclose(fp);
		return NULL;
	}
	const bool aOk = ((int)p_fread(aData, 1, aSize, fp) == aSize);
	p_fclose(fp);
	if (!aOk)
	{
		free(aData);
		return NULL;
	}
	theSizeOut = aSize;
	return aData;
}

// Pan arrives in hundredths of a dB, negative meaning left, and attenuates the
// far channel -- same curve the PS2 backend uses, so mixes carry across ports.
void WiiApplyPan(double theVolume, int thePan, int& theVolL, int& theVolR)
{
	double aVolL = theVolume, aVolR = theVolume;
	if (thePan < 0)
		aVolR *= pow(10.0, thePan / 2000.0);
	else if (thePan > 0)
		aVolL *= pow(10.0, -thePan / 2000.0);

	theVolL = (int)(aVolL * WII_VOL_MAX);
	theVolR = (int)(aVolR * WII_VOL_MAX);
	if (theVolL < 0) theVolL = 0;
	if (theVolL > WII_VOL_MAX) theVolL = WII_VOL_MAX;
	if (theVolR < 0) theVolR = 0;
	if (theVolR > WII_VOL_MAX) theVolR = WII_VOL_MAX;
}

} // namespace

// ===========================================================================
// WiiSoundInstance
// ===========================================================================

WiiSoundInstance::WiiSoundInstance(WiiSoundManager* theManager, int theSfxID)
	: mManager(theManager), mSfxID(theSfxID), mVoice(-1),
	  mBaseVolume(theManager->mBaseVolumes[theSfxID]),
	  mVolume(1.0),
	  mBasePan(theManager->mBasePans[theSfxID]), mPan(0),
	  mPitchRatio(1.0), mReleased(false), mAutoRelease(false)
{
}

bool WiiSoundInstance::VoiceAliveLocked()
{
	if (mVoice < 0)
		return false;
	// Ownership, not just status: a finished voice may already have been handed
	// to another effect, and volume changes must not leak across.
	if (mManager->mVoiceOwner[mVoice] != this)
	{
		mVoice = -1;
		return false;
	}
	if (ASND_StatusVoice(mVoice) == SND_UNUSED)
	{
		mManager->ReleaseVoiceLocked(mVoice, this);
		mVoice = -1;
		return false;
	}
	return true;
}

void WiiSoundInstance::ComputeChannelVolumes(int& theVolL, int& theVolR)
{
	double aVol = mBaseVolume * mVolume * mManager->mMasterVolume;
	if (aVol < 0.0) aVol = 0.0;
	if (aVol > 1.0) aVol = 1.0;
	WiiApplyPan(aVol, mBasePan + mPan, theVolL, theVolR);
}

void WiiSoundInstance::ApplyVolumeLocked()
{
	int aVolL = 0, aVolR = 0;
	ComputeChannelVolumes(aVolL, aVolR);
	if (VoiceAliveLocked())
		ASND_ChangeVolumeVoice(mVoice, aVolL, aVolR);
}

void WiiSoundInstance::ApplyVolume()
{
	pthread_mutex_lock(&mManager->mLock);
	ApplyVolumeLocked();
	pthread_mutex_unlock(&mManager->mLock);
}

void WiiSoundInstance::Release()
{
	Stop();
	mReleased = true;
}

void WiiSoundInstance::SetBaseVolume(double theBaseVolume)
{
	mBaseVolume = theBaseVolume;
	ApplyVolume();
}

void WiiSoundInstance::SetBasePan(int theBasePan)
{
	mBasePan = theBasePan;
	ApplyVolume();
}

void WiiSoundInstance::AdjustPitch(double theNumSteps)
{
	mPitchRatio = pow(2.0, theNumSteps / 12.0);
	pthread_mutex_lock(&mManager->mLock);
	if (VoiceAliveLocked())
	{
		const WiiSample& aSample = mManager->mSamples[mSfxID];
		if (aSample.mSampleRate > 0)
			ASND_ChangePitchVoice(mVoice, (int)(aSample.mSampleRate * mPitchRatio));
	}
	pthread_mutex_unlock(&mManager->mLock);
}

void WiiSoundInstance::SetVolume(double theVolume)
{
	mVolume = theVolume;
	ApplyVolume();
}

void WiiSoundInstance::SetPan(int thePosition)
{
	mPan = thePosition;
	ApplyVolume();
}

bool WiiSoundInstance::Play(bool looping, bool autoRelease)
{
	Stop();
	mAutoRelease = autoRelease;

	// Long-lived instances (TodFoley caches loops for the whole session) can
	// outlive a PurgeSounds(), so the sample is re-decoded here and not only in
	// GetSoundInstance -- otherwise such an instance goes permanently silent
	// after the first purge.
	if (!mManager->EnsureSampleLoaded(mSfxID))
		return false;

	int aVolL = 0, aVolR = 0;
	ComputeChannelVolumes(aVolL, aVolR);

	const WiiSample& aSample = mManager->mSamples[mSfxID];
	const int aRate = (int)(aSample.mSampleRate * mPitchRatio);
	mVoice = mManager->StartVoice(this, aSample, aRate, aVolL, aVolR, looping);
	return mVoice >= 0;
}

void WiiSoundInstance::Stop()
{
	pthread_mutex_lock(&mManager->mLock);
	if (VoiceAliveLocked())
	{
		ASND_StopVoice(mVoice);
		mManager->ReleaseVoiceLocked(mVoice, this);
		mVoice = -1;
	}
	pthread_mutex_unlock(&mManager->mLock);
}

bool WiiSoundInstance::IsPlaying()
{
	pthread_mutex_lock(&mManager->mLock);
	const bool aPlaying = VoiceAliveLocked();
	pthread_mutex_unlock(&mManager->mLock);
	return aPlaying;
}

bool WiiSoundInstance::IsReleased()
{
	return mReleased;
}

double WiiSoundInstance::GetVolume()
{
	return mVolume;
}

// ===========================================================================
// WiiSoundManager
// ===========================================================================

WiiSoundManager::WiiSoundManager()
	: mInitialized(false), mMasterVolume(1.0), mVoiceClock(0)
{
	for (int i = 0; i < MAX_SOURCE_SOUNDS; i++)
	{
		mBaseVolumes[i] = 1.0;
		mBasePans[i] = 0;
	}
	for (int v = 0; v < MAX_SND_VOICES; v++)
	{
		mVoiceOwner[v] = NULL;
		mVoiceStamp[v] = 0;
	}

	pthread_mutex_init(&mLock, NULL);

	// ASND_Init() leaves the DSP paused; nothing plays until it is un-paused.
	ASND_Init();
	ASND_Pause(0);
	mInitialized = true;
}

WiiSoundManager::~WiiSoundManager()
{
	StopAllSounds();
	ReleaseSounds();
	ASND_Pause(1);
	ASND_End();
	pthread_mutex_destroy(&mLock);
}

bool WiiSoundManager::Initialized()
{
	return mInitialized;
}

void WiiSoundManager::ReleaseVoiceLocked(int theVoice, WiiSoundInstance* theOwner)
{
	if (theVoice >= 0 && theVoice < MAX_SND_VOICES && mVoiceOwner[theVoice] == theOwner)
		mVoiceOwner[theVoice] = NULL;
}

// A buffer is only handed back to the allocator once no voice admits to reading
// it. ASND_TestPointer is the primitive that answers that; freeing on the
// strength of ASND_StatusVoice alone races the DSP's in-flight DMA.
void WiiSoundManager::DrainPendingFreeLocked()
{
	std::list<short*>::iterator anItr = mPendingFree.begin();
	while (anItr != mPendingFree.end())
	{
		short* aPtr = *anItr;
		bool aInUse = false;
		for (int v = 0; v < MAX_SND_VOICES; v++)
		{
			if (ASND_StatusVoice(v) != SND_UNUSED && ASND_TestPointer(v, aPtr))
			{
				aInUse = true;
				break;
			}
		}
		if (aInUse)
			++anItr;
		else
		{
			free(aPtr);
			anItr = mPendingFree.erase(anItr);
		}
	}
}

void WiiSoundManager::FreeSampleLocked(unsigned int theSfxID)
{
	WiiSample& aSample = mSamples[theSfxID];
	if (aSample.mPCM == NULL)
		return;

	// Stop anything still playing it first, then defer the free so a DMA that
	// is already in flight can retire.
	for (int v = 0; v < MAX_SND_VOICES; v++)
	{
		if (ASND_StatusVoice(v) != SND_UNUSED && ASND_TestPointer(v, aSample.mPCM))
		{
			ASND_StopVoice(v);
			if (mVoiceOwner[v] != NULL)
			{
				mVoiceOwner[v]->mVoice = -1;
				mVoiceOwner[v] = NULL;
			}
		}
	}
	mPendingFree.push_back(aSample.mPCM);
	aSample.mPCM = NULL;
	aSample.mNumBytes = 0;
	DrainPendingFreeLocked();
}

bool WiiSoundManager::EnsureSampleLoadedLocked(unsigned int theSfxID)
{
	if (mSamples[theSfxID].mPCM != NULL)
		return true;
	if (!mInitialized || mSourceFileNames[theSfxID].empty())
		return false;

	const std::string aBaseName = mSourceFileNames[theSfxID];
	static const char* const kFormats[] = { ".ogg", ".wav" };
	for (int f = 0; f < 2; f++)
	{
		const std::string aFilename = aBaseName + kFormats[f];
		int aSize = 0;
		unsigned char* aData = WiiReadWholeFile(aFilename, aSize);
		if (aData == NULL)
			continue;

		WiiSample aSample;
		const bool aOk = (f == 0) ? WiiDecodeOgg(aData, aSize, aSample)
								  : WiiDecodeWav(aData, aSize, aSample);
		free(aData);

		if (aOk)
		{
			mSamples[theSfxID] = aSample;
			return true;
		}
		printf("[WII][SND] failed to decode %s\n", aFilename.c_str());
	}

	// Missing or undecodable: clear the name so every later play does not retry
	// the whole file probe.
	mSourceFileNames[theSfxID].clear();
	return false;
}

bool WiiSoundManager::EnsureSampleLoaded(unsigned int theSfxID)
{
	if (theSfxID >= MAX_SOURCE_SOUNDS)
		return false;
	pthread_mutex_lock(&mLock);
	const bool aResult = EnsureSampleLoadedLocked(theSfxID);
	pthread_mutex_unlock(&mLock);
	return aResult;
}

int WiiSoundManager::StartVoice(WiiSoundInstance* theInstance, const WiiSample& theSample,
	int theRate, int theVolL, int theVolR, bool looping)
{
	if (theSample.mPCM == NULL || theSample.mNumBytes <= 0)
		return -1;

	pthread_mutex_lock(&mLock);

	int aVoice = -1;
	for (int v = WII_FIRST_SFX_VOICE; v < MAX_SND_VOICES; v++)
	{
		if (ASND_StatusVoice(v) == SND_UNUSED)
		{
			aVoice = v;
			break;
		}
	}
	if (aVoice < 0)
	{
		// All voices busy: steal the one started longest ago. PvZ fires far more
		// effects than there are voices during a big wave, and dropping the
		// newest sound is more noticeable than cutting the oldest short.
		int aOldest = WII_FIRST_SFX_VOICE;
		for (int v = WII_FIRST_SFX_VOICE + 1; v < MAX_SND_VOICES; v++)
			if (mVoiceStamp[v] < mVoiceStamp[aOldest])
				aOldest = v;
		ASND_StopVoice(aOldest);
		if (mVoiceOwner[aOldest] != NULL)
		{
			mVoiceOwner[aOldest]->mVoice = -1;
			mVoiceOwner[aOldest] = NULL;
		}
		aVoice = aOldest;
	}

	const int aRate = (theRate > 0) ? theRate : theSample.mSampleRate;
	int aResult;
	if (looping)
		aResult = ASND_SetInfiniteVoice(aVoice, theSample.mFormat, aRate, 0,
			theSample.mPCM, theSample.mNumBytes, theVolL, theVolR);
	else
		aResult = ASND_SetVoice(aVoice, theSample.mFormat, aRate, 0,
			theSample.mPCM, theSample.mNumBytes, theVolL, theVolR, NULL);

	if (aResult != SND_OK)
	{
		pthread_mutex_unlock(&mLock);
		return -1;
	}

	mVoiceOwner[aVoice] = theInstance;
	mVoiceStamp[aVoice] = ++mVoiceClock;
	pthread_mutex_unlock(&mLock);
	return aVoice;
}

bool WiiSoundManager::LoadSound(unsigned int theSfxID, const std::string& theFilename)
{
	if (theSfxID >= MAX_SOURCE_SOUNDS)
		return false;

	pthread_mutex_lock(&mLock);
	FreeSampleLocked(theSfxID);
	mSourceFileNames[theSfxID] = theFilename;
	pthread_mutex_unlock(&mLock);

	// Decoding is deferred: LOW_MEMORY builds cannot hold every effect at once,
	// and most ids are never played in a given session.
	return true;
}

int WiiSoundManager::LoadSound(const std::string& theFilename)
{
	pthread_mutex_lock(&mLock);
	int aFound = -1;
	for (int i = 0; i < MAX_SOURCE_SOUNDS; i++)
	{
		if (!mSourceFileNames[i].empty() && mSourceFileNames[i] == theFilename)
		{
			aFound = i;
			break;
		}
	}
	if (aFound < 0)
	{
		for (int i = 0; i < MAX_SOURCE_SOUNDS; i++)
		{
			if (mSourceFileNames[i].empty() && mSamples[i].mPCM == NULL)
			{
				mSourceFileNames[i] = theFilename;
				aFound = i;
				break;
			}
		}
	}
	pthread_mutex_unlock(&mLock);
	return aFound;
}

void WiiSoundManager::ReleaseSound(unsigned int theSfxID)
{
	if (theSfxID >= MAX_SOURCE_SOUNDS)
		return;
	pthread_mutex_lock(&mLock);
	FreeSampleLocked(theSfxID);
	mSourceFileNames[theSfxID].clear();
	pthread_mutex_unlock(&mLock);
}

void WiiSoundManager::SetVolume(double theVolume)
{
	SetMasterVolume(theVolume);
}

bool WiiSoundManager::SetBaseVolume(unsigned int theSfxID, double theBaseVolume)
{
	if (theSfxID >= MAX_SOURCE_SOUNDS)
		return false;
	mBaseVolumes[theSfxID] = theBaseVolume;
	return true;
}

bool WiiSoundManager::SetBasePan(unsigned int theSfxID, int theBasePan)
{
	if (theSfxID >= MAX_SOURCE_SOUNDS)
		return false;
	mBasePans[theSfxID] = theBasePan;
	return true;
}

// Instances are owned by this list, not by the caller: the game calls Release()
// and walks away. Reaping them on the next manager call keeps deletion on
// whichever thread is already here rather than in an ASND callback.
void WiiSoundManager::ReapDeadInstancesLocked()
{
	std::list<WiiSoundInstance*>::iterator anItr = mInstances.begin();
	while (anItr != mInstances.end())
	{
		WiiSoundInstance* anInstance = *anItr;
		bool aDone = anInstance->mReleased;
		if (!aDone && anInstance->mAutoRelease)
			aDone = !anInstance->VoiceAliveLocked();

		if (aDone)
		{
			anItr = mInstances.erase(anItr);
			delete anInstance;
		}
		else
			++anItr;
	}
}

void WiiSoundManager::ReapDeadInstances()
{
	pthread_mutex_lock(&mLock);
	ReapDeadInstancesLocked();
	pthread_mutex_unlock(&mLock);
}

SoundInstance* WiiSoundManager::GetSoundInstance(unsigned int theSfxID)
{
	if (theSfxID >= MAX_SOURCE_SOUNDS)
		return NULL;

	// mInstances is reachable from the loading thread as well as the main one,
	// so the reap and the insert happen under one lock rather than two.
	pthread_mutex_lock(&mLock);
	ReapDeadInstancesLocked();
	WiiSoundInstance* anInstance = new WiiSoundInstance(this, (int)theSfxID);
	mInstances.push_back(anInstance);
	pthread_mutex_unlock(&mLock);
	return anInstance;
}

void WiiSoundManager::ReleaseSounds()
{
	pthread_mutex_lock(&mLock);
	for (int i = 0; i < MAX_SOURCE_SOUNDS; i++)
	{
		FreeSampleLocked((unsigned int)i);
		mSourceFileNames[i].clear();
	}
	pthread_mutex_unlock(&mLock);
}

void WiiSoundManager::ReleaseChannels()
{
	StopAllSounds();
}

double WiiSoundManager::GetMasterVolume()
{
	return mMasterVolume;
}

void WiiSoundManager::SetMasterVolume(double theVolume)
{
	pthread_mutex_lock(&mLock);
	mMasterVolume = theVolume;
	// ApplyVolumeLocked, not ApplyVolume: the list walk already holds mLock and
	// the mutex is not recursive.
	for (std::list<WiiSoundInstance*>::iterator anItr = mInstances.begin();
		anItr != mInstances.end(); ++anItr)
	{
		(*anItr)->ApplyVolumeLocked();
	}
	pthread_mutex_unlock(&mLock);
}

void WiiSoundManager::Flush()
{
	pthread_mutex_lock(&mLock);
	ReapDeadInstancesLocked();
	DrainPendingFreeLocked();
	pthread_mutex_unlock(&mLock);
}

void WiiSoundManager::SetCooperativeWindow(HWND /*theHWnd*/)
{
}

void WiiSoundManager::StopAllSounds()
{
	pthread_mutex_lock(&mLock);
	for (int v = WII_FIRST_SFX_VOICE; v < MAX_SND_VOICES; v++)
	{
		ASND_StopVoice(v);
		if (mVoiceOwner[v] != NULL)
		{
			mVoiceOwner[v]->mVoice = -1;
			mVoiceOwner[v] = NULL;
		}
	}
	DrainPendingFreeLocked();
	pthread_mutex_unlock(&mLock);
}

int WiiSoundManager::GetFreeSoundId()
{
	pthread_mutex_lock(&mLock);
	int aResult = -1;
	for (int i = 0; i < MAX_SOURCE_SOUNDS; i++)
	{
		if (mSourceFileNames[i].empty() && mSamples[i].mPCM == NULL)
		{
			aResult = i;
			break;
		}
	}
	pthread_mutex_unlock(&mLock);
	return aResult;
}

int WiiSoundManager::GetNumSounds()
{
	pthread_mutex_lock(&mLock);
	int aCount = 0;
	for (int i = 0; i < MAX_SOURCE_SOUNDS; i++)
		if (!mSourceFileNames[i].empty() || mSamples[i].mPCM != NULL)
			aCount++;
	pthread_mutex_unlock(&mLock);
	return aCount;
}

// Screen-transition valve for LOW_MEMORY builds: drop decoded PCM but keep the
// file names, so anything needed again simply re-decodes on its next play.
// Samples with a voice still on them are left alone.
void WiiSoundManager::PurgeSounds()
{
	pthread_mutex_lock(&mLock);
	for (int i = 0; i < MAX_SOURCE_SOUNDS; i++)
	{
		WiiSample& aSample = mSamples[i];
		if (aSample.mPCM == NULL)
			continue;

		bool aInUse = false;
		for (int v = 0; v < MAX_SND_VOICES; v++)
		{
			if (ASND_StatusVoice(v) != SND_UNUSED && ASND_TestPointer(v, aSample.mPCM))
			{
				aInUse = true;
				break;
			}
		}
		if (aInUse)
			continue;

		mPendingFree.push_back(aSample.mPCM);
		aSample.mPCM = NULL;
		aSample.mNumBytes = 0;
	}
	DrainPendingFreeLocked();
	pthread_mutex_unlock(&mLock);
}

void WiiSoundManager::PreloadSound(unsigned int theSfxID)
{
	EnsureSampleLoaded(theSfxID);
}

// ===========================================================================
// Streamed music -- ASND voice 0
// ===========================================================================

namespace
{

// ASND can hold one playing buffer and one queued buffer. Give each one enough
// headroom to survive a sizeable render/loading hitch before Update() gets a
// chance to pump the stream again (about 371 ms at the soundtrack's 22050 Hz).
const int kMusicChunkFrames = 8192;
const int kMusicChannels = 2;
const int kMusicBufferBytes = kMusicChunkFrames * kMusicChannels * (int)sizeof(short);

stb_vorbis*		s_musVorbis = NULL;
unsigned char*	s_musOggData = NULL;		// owned; freed on stop
short*			s_musBuffer[2] = { NULL, NULL };
int				s_musBufferBytes[2] = { 0, 0 };
int				s_musFillNext = 0;			// buffer the pump will fill next
int				s_musRate = 44100;
int				s_musChannels = kMusicChannels;
bool			s_musLoop = true;
bool			s_musRunning = false;
bool			s_musPaused = false;
bool			s_musEnded = false;			// decoder hit the end of a non-looping song
double			s_musVolume = 1.0;
double			s_musMasterVolume = 1.0;

int WiiMusicVolumeByte()
{
	double aVol = s_musVolume * s_musMasterVolume;
	if (aVol < 0.0) aVol = 0.0;
	if (aVol > 1.0) aVol = 1.0;
	return (int)(aVol * WII_VOL_MAX);
}

// Decodes one chunk into the given buffer. Returns bytes produced (0 at the end
// of a non-looping song).
int WiiMusicDecodeChunk(int theSlot)
{
	if (s_musVorbis == NULL)
		return 0;

	short* aDest = s_musBuffer[theSlot];
	int aFrames = stb_vorbis_get_samples_short_interleaved(
		s_musVorbis, s_musChannels, aDest, kMusicChunkFrames * s_musChannels);

	if (aFrames <= 0)
	{
		if (!s_musLoop)
		{
			s_musEnded = true;
			return 0;
		}
		stb_vorbis_seek_start(s_musVorbis);
		aFrames = stb_vorbis_get_samples_short_interleaved(
			s_musVorbis, s_musChannels, aDest, kMusicChunkFrames * s_musChannels);
		if (aFrames <= 0)
		{
			s_musEnded = true;
			return 0;
		}
	}

	int aBytes = aFrames * s_musChannels * (int)sizeof(short);
	// Pad the tail to a cache line before flushing, for the same reason sample
	// buffers are padded: DCFlushRange rounds outward.
	const int aPadded = (aBytes + 31) & ~31;
	if (aPadded > kMusicBufferBytes)
		aBytes = kMusicBufferBytes;
	else
	{
		memset((unsigned char*)aDest + aBytes, 0, (size_t)(aPadded - aBytes));
		aBytes = aPadded;
	}
	DCFlushRange(aDest, (u32)aBytes);
	return aBytes;
}

void WiiMusicFreeResources()
{
	if (s_musVorbis != NULL)
	{
		stb_vorbis_close(s_musVorbis);
		s_musVorbis = NULL;
	}
	if (s_musOggData != NULL)
	{
		free(s_musOggData);
		s_musOggData = NULL;
	}
	for (int i = 0; i < 2; i++)
	{
		if (s_musBuffer[i] != NULL)
		{
			free(s_musBuffer[i]);
			s_musBuffer[i] = NULL;
		}
		s_musBufferBytes[i] = 0;
	}
}

} // namespace

bool Sexy::WiiMusicPlay(unsigned char* theOggData, int theSize, bool theLoop)
{
	WiiMusicStop();

	int anError = 0;
	stb_vorbis* aVorbis = stb_vorbis_open_memory(theOggData, theSize, &anError, NULL);
	if (aVorbis == NULL)
	{
		free(theOggData);
		wiiLog("[WII][MUS] stb_vorbis_open_memory failed (err %d, %d KB)\n",
		       anError, theSize / 1024);
		return false;
	}

	const stb_vorbis_info anInfo = stb_vorbis_get_info(aVorbis);
	s_musRate = (int)anInfo.sample_rate;
	s_musChannels = (anInfo.channels >= 2) ? 2 : 1;
	wiiLog("[WII][MUS] vorbis opened size=%d rate=%d sourceCh=%d outputCh=%d loop=%d\n",
		theSize, s_musRate, anInfo.channels, s_musChannels, theLoop ? 1 : 0);

	for (int i = 0; i < 2; i++)
	{
		s_musBuffer[i] = (short*)memalign(32, (size_t)kMusicBufferBytes);
		if (s_musBuffer[i] == NULL)
		{
			stb_vorbis_close(aVorbis);
			free(theOggData);
			for (int j = 0; j < i; j++) { free(s_musBuffer[j]); s_musBuffer[j] = NULL; }
			wiiLog("[WII][MUS] out of memory for stream buffers (%d bytes)\n",
			       kMusicBufferBytes);
			return false;
		}
		memset(s_musBuffer[i], 0, (size_t)kMusicBufferBytes);
	}

	s_musVorbis = aVorbis;
	s_musOggData = theOggData;
	s_musLoop = theLoop;
	s_musEnded = false;
	s_musPaused = false;
	s_musFillNext = 0;

	// Prime both buffers before starting, so the voice never begins starved.
	const int aFirst = WiiMusicDecodeChunk(0);
	if (aFirst <= 0)
	{
		// This one returned silently. It is the failure that looks exactly like
		// "the music just doesn't play": the file opened, the header parsed, and
		// then the very first decode produced nothing -- a truncated or
		// mis-transcoded OGG, or stb_vorbis failing to allocate its working
		// buffers on a fragmented heap.
		WiiMusicFreeResources();
		wiiLog("[WII][MUS] first decode produced no samples (%d Hz, %d ch)\n",
		       s_musRate, s_musChannels);
		return false;
	}
	s_musBufferBytes[0] = aFirst;
	s_musBufferBytes[1] = WiiMusicDecodeChunk(1);
	s_musFillNext = 0;

	const int aVol = WiiMusicVolumeByte();
	const int aFormat = (s_musChannels >= 2) ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
	const int aQueuedBytes = s_musBufferBytes[1];
	// No callback: the pump below tops the voice up from Update(). A callback
	// would run in the DSP interrupt, where decoding is not an option.
	if (ASND_SetVoice(WII_MUSIC_VOICE, aFormat, s_musRate, 0,
			s_musBuffer[0], s_musBufferBytes[0], aVol, aVol, NULL) != SND_OK)
	{
		WiiMusicFreeResources();
		wiiLog("[WII][MUS] ASND_SetVoice failed (%d Hz, %d ch)\n",
		       s_musRate, s_musChannels);
		return false;
	}
	// ASND owns buffer 0 until ASND_TestPointer says otherwise. Mark it empty
	// now so the pump refills it only after that ownership check succeeds.
	s_musBufferBytes[0] = 0;
	if (s_musBufferBytes[1] > 0)
	{
		if (ASND_AddVoice(WII_MUSIC_VOICE, s_musBuffer[1], s_musBufferBytes[1]) == SND_OK)
		{
			s_musBufferBytes[1] = 0;
			s_musFillNext = 0;
		}
		else
			s_musFillNext = 1;
	}
	else
		s_musFillNext = 1;

	s_musRunning = true;
	wiiLog("[WII][MUS] ASND voice 0 started first=%d queued=%d volume=%d\n",
		aFirst, aQueuedBytes, aVol);
	return true;
}

void Sexy::WiiMusicStop()
{
	if (!s_musRunning && s_musVorbis == NULL && s_musOggData == NULL)
		return;

	ASND_StopVoice(WII_MUSIC_VOICE);
	s_musRunning = false;
	s_musPaused = false;
	s_musEnded = false;
	WiiMusicFreeResources();
}

void Sexy::WiiMusicSetPaused(bool thePaused)
{
	if (!s_musRunning || s_musPaused == thePaused)
		return;
	s_musPaused = thePaused;
	ASND_PauseVoice(WII_MUSIC_VOICE, thePaused ? 1 : 0);
}

bool Sexy::WiiMusicIsPlaying()
{
	return s_musRunning && !s_musEnded;
}

void Sexy::WiiMusicSetVolume(double theVolume)
{
	s_musVolume = theVolume;
	if (s_musRunning)
	{
		const int aVol = WiiMusicVolumeByte();
		ASND_ChangeVolumeVoice(WII_MUSIC_VOICE, aVol, aVol);
	}
}

void Sexy::WiiMusicSetMasterVolume(double theVolume)
{
	s_musMasterVolume = theVolume;
	if (s_musRunning)
	{
		const int aVol = WiiMusicVolumeByte();
		ASND_ChangeVolumeVoice(WII_MUSIC_VOICE, aVol, aVol);
	}
}

// Keeps the voice fed. ASND accepts one queued buffer at a time: while it is
// playing one and holding another, AddVoice answers SND_BUSY and we simply try
// again next frame.
void Sexy::WiiMusicPump()
{
	if (!s_musRunning || s_musPaused)
		return;

	if (s_musEnded)
	{
		// Non-looping song: let the queued audio drain, then tear down.
		if (ASND_StatusVoice(WII_MUSIC_VOICE) == SND_UNUSED)
			WiiMusicStop();
		return;
	}

	const int aSlot = s_musFillNext;
	// This check must happen before decoding: WiiMusicDecodeChunk writes into
	// the buffer and flushes its cache lines for DMA. Doing that while ASND is
	// still reading the same memory corrupts the in-flight audio and produces
	// short clicks/pops.
	if (ASND_TestPointer(WII_MUSIC_VOICE, s_musBuffer[aSlot]))
		return;

	if (s_musBufferBytes[aSlot] <= 0)
	{
		s_musBufferBytes[aSlot] = WiiMusicDecodeChunk(aSlot);
		if (s_musBufferBytes[aSlot] <= 0)
			return;		// end of a non-looping song; handled above next frame
	}

	const int aResult = ASND_AddVoice(WII_MUSIC_VOICE, s_musBuffer[aSlot], s_musBufferBytes[aSlot]);
	if (aResult == SND_OK)
	{
		s_musBufferBytes[aSlot] = 0;			// consumed; refill next time round
		s_musFillNext = aSlot ^ 1;
	}
	else if (aResult != SND_BUSY)
	{
		// The voice died under us (stopped elsewhere, or an underrun that ASND
		// gave up on). Restart it from the buffer we already have.
		const int aVol = WiiMusicVolumeByte();
		const int aFormat = (s_musChannels >= 2) ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
		if (ASND_SetVoice(WII_MUSIC_VOICE, aFormat, s_musRate, 0,
				s_musBuffer[aSlot], s_musBufferBytes[aSlot], aVol, aVol, NULL) == SND_OK)
		{
			s_musBufferBytes[aSlot] = 0;
			s_musFillNext = aSlot ^ 1;
		}
	}
}

// ===========================================================================
// WiiMusicInterface
//
// PvZ only ever streams one song, so song ids map onto the single voice-0
// stream: the id is remembered to answer IsPlaying() and to let Music.cpp stop
// "its" song, and anything asking for a different id simply replaces it.
// Loading is a no-op -- Music.cpp opens the OGG itself and hands the bytes to
// WiiMusicPlay -- and fades collapse to their end state, matching how the PS2
// OGG mode behaves.
// ===========================================================================

namespace
{
int s_musCurrentSongId = -1;
}

Sexy::WiiMusicInterface::WiiMusicInterface()
{
}

Sexy::WiiMusicInterface::~WiiMusicInterface()
{
	WiiMusicStop();
}

bool Sexy::WiiMusicInterface::LoadMusic(int /*theSongId*/, const std::string& /*theFileName*/)
{
	return true;
}

void Sexy::WiiMusicInterface::PlayMusic(int theSongId, int /*theOffset*/, bool /*noLoop*/)
{
	// Music.cpp starts the stream through WiiMusicPlay; this only records which
	// id owns it, so StopMusic/IsPlaying can be answered per song.
	s_musCurrentSongId = theSongId;
}

void Sexy::WiiMusicInterface::StopMusic(int theSongId)
{
	if (s_musCurrentSongId == theSongId || theSongId < 0)
	{
		WiiMusicStop();
		s_musCurrentSongId = -1;
	}
}

void Sexy::WiiMusicInterface::PauseMusic(int theSongId)
{
	if (s_musCurrentSongId == theSongId)
		WiiMusicSetPaused(true);
}

void Sexy::WiiMusicInterface::ResumeMusic(int theSongId)
{
	if (s_musCurrentSongId == theSongId)
		WiiMusicSetPaused(false);
}

void Sexy::WiiMusicInterface::StopAllMusic()
{
	WiiMusicStop();
	s_musCurrentSongId = -1;
}

void Sexy::WiiMusicInterface::UnloadMusic(int theSongId)
{
	StopMusic(theSongId);
}

void Sexy::WiiMusicInterface::UnloadAllMusic()
{
	StopAllMusic();
}

void Sexy::WiiMusicInterface::PauseAllMusic()
{
	WiiMusicSetPaused(true);
}

void Sexy::WiiMusicInterface::ResumeAllMusic()
{
	WiiMusicSetPaused(false);
}

void Sexy::WiiMusicInterface::FadeIn(int theSongId, int /*theOffset*/, double /*theSpeed*/, bool /*noLoop*/)
{
	s_musCurrentSongId = theSongId;
	WiiMusicSetVolume(1.0);
	WiiMusicSetPaused(false);
}

void Sexy::WiiMusicInterface::FadeOut(int theSongId, bool stopSong, double /*theSpeed*/)
{
	if (s_musCurrentSongId != theSongId)
		return;
	if (stopSong)
		StopMusic(theSongId);
	else
		WiiMusicSetVolume(0.0);
}

void Sexy::WiiMusicInterface::FadeOutAll(bool stopSong, double /*theSpeed*/)
{
	if (stopSong)
		StopAllMusic();
	else
		WiiMusicSetVolume(0.0);
}

void Sexy::WiiMusicInterface::SetSongVolume(int theSongId, double theVolume)
{
	if (s_musCurrentSongId == theSongId)
		WiiMusicSetVolume(theVolume);
}

void Sexy::WiiMusicInterface::SetSongMaxVolume(int theSongId, double theMaxVolume)
{
	if (s_musCurrentSongId == theSongId)
		WiiMusicSetVolume(theMaxVolume);
}

bool Sexy::WiiMusicInterface::IsPlaying(int theSongId)
{
	return s_musCurrentSongId == theSongId && WiiMusicIsPlaying();
}

void Sexy::WiiMusicInterface::SetVolume(double theVolume)
{
	WiiMusicSetMasterVolume(theVolume);
}

void Sexy::WiiMusicInterface::SetMusicAmplify(int /*theSongId*/, double /*theAmp*/)
{
}

void Sexy::WiiMusicInterface::Update()
{
	WiiMusicPump();
}

#endif // WII_PLATFORM
