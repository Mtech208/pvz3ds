#ifndef __WIISOUNDMANAGER_H__
#define __WIISOUNDMANAGER_H__
#ifdef WII_PLATFORM

// Real sound for the Wii build, on top of libogc's ASND.
//
// Unlike the PS2 backend -- which had to software-mix 32 voices on the EE and
// hand a finished stereo stream to audsrv -- ASND mixes in the DSP, so this
// layer only decodes samples and drives voices. Voice 0 is reserved for the
// streamed music (see WiiMusicInterface); voices 1..MAX_SND_VOICES-1 serve
// effects.
//
// Two hardware facts shape everything below:
//
//   * The DSP reads sample memory by DMA and does not see the CPU's data
//     cache. Every buffer handed to ASND is memalign(32)'d, padded to a whole
//     cache line, and DCFlushRange'd after it is written. Skipping any of the
//     three plays stale or garbage audio.
//   * A buffer must not be freed while a voice is still reading it.
//     ASND_TestPointer() answers exactly that question, so frees are deferred
//     through mPendingFree until no voice claims the pointer.
//
// Sample data is 16-bit host-endian: on PowerPC that is big-endian, which is
// what VOICE_MONO_16BIT/VOICE_STEREO_16BIT already mean (the _BE aliases are
// the same constants), and what stb_vorbis produces natively. No swapping.
//
// Threading: the loading thread and the main thread both load and play sounds,
// so the sample table, the instance list and the pending-free list are all
// guarded by mLock. ASND's own entry points are interrupt-safe.

#include "sound/SoundManager.h"
#include "sound/SoundInstance.h"
#include "sound/MusicInterface.h"

#include <list>
#include <string>
#include <pthread.h>

namespace Sexy
{

class WiiSoundManager;

struct WiiSample
{
	short*	mPCM;			// memalign(32), length padded, cache-flushed
	int		mNumBytes;		// padded up to a multiple of 32
	int		mSampleRate;	// native rate; ASND resamples per voice
	int		mFormat;		// VOICE_MONO_16BIT / VOICE_STEREO_16BIT

	WiiSample() : mPCM(0), mNumBytes(0), mSampleRate(0), mFormat(0) {}
};

class WiiSoundInstance : public SoundInstance
{
public:
	WiiSoundManager*	mManager;
	int					mSfxID;
	int					mVoice;			// -1 when not playing
	double				mBaseVolume;
	double				mVolume;
	int					mBasePan;
	int					mPan;
	double				mPitchRatio;
	bool				mReleased;
	bool				mAutoRelease;

	WiiSoundInstance(WiiSoundManager* theManager, int theSfxID);

	// True while this instance still owns mVoice. A voice that finished (or was
	// stolen for another effect) reads back as not ours, so callers never touch
	// somebody else's voice.
	//
	// The *Locked helpers assume the manager's mLock is already held. mLock is a
	// plain (non-recursive) mutex, so the rule throughout this file is that each
	// public entry point takes it at most once and everything below it is a
	// *Locked helper -- devkitPPC ships PTHREAD_MUTEX_RECURSIVE as a macro but no
	// pthread_mutexattr_settype to reach it, so re-entrant locking is not an
	// option here.
	bool				VoiceAliveLocked();
	void				ComputeChannelVolumes(int& theVolL, int& theVolR);
	void				ApplyVolumeLocked();
	void				ApplyVolume();

	virtual void		Release();
	virtual void		SetBaseVolume(double theBaseVolume);
	virtual void		SetBasePan(int theBasePan);
	virtual void		AdjustPitch(double theNumSteps);
	virtual void		SetVolume(double theVolume);
	virtual void		SetPan(int thePosition);
	virtual bool		Play(bool looping, bool autoRelease);
	virtual void		Stop();
	virtual bool		IsPlaying();
	virtual bool		IsReleased();
	virtual double		GetVolume();
};

class WiiSoundManager : public SoundManager
{
public:
	bool			mInitialized;
	WiiSample		mSamples[MAX_SOURCE_SOUNDS];
	std::string		mSourceFileNames[MAX_SOURCE_SOUNDS];
	double			mBaseVolumes[MAX_SOURCE_SOUNDS];
	int				mBasePans[MAX_SOURCE_SOUNDS];
	double			mMasterVolume;

	std::list<WiiSoundInstance*>	mInstances;
	std::list<short*>				mPendingFree;

	// Which instance owns each voice, so a stolen or finished voice cannot be
	// stopped or re-volumed by the instance that used to hold it.
	WiiSoundInstance*	mVoiceOwner[16];
	unsigned int		mVoiceStamp[16];	// for oldest-first stealing
	unsigned int		mVoiceClock;

	pthread_mutex_t	mLock;

	WiiSoundManager();
	virtual ~WiiSoundManager();

	void			ReapDeadInstances();
	void			ReapDeadInstancesLocked();
	bool			EnsureSampleLoaded(unsigned int theSfxID);
	bool			EnsureSampleLoadedLocked(unsigned int theSfxID);
	void			FreeSampleLocked(unsigned int theSfxID);
	void			DrainPendingFreeLocked();
	int				StartVoice(WiiSoundInstance* theInstance, const WiiSample& theSample,
						int theRate, int theVolL, int theVolR, bool looping);
	void			ReleaseVoiceLocked(int theVoice, WiiSoundInstance* theOwner);

	virtual bool	Initialized();
	virtual bool	LoadSound(unsigned int theSfxID, const std::string& theFilename);
	virtual int		LoadSound(const std::string& theFilename);
	virtual void	ReleaseSound(unsigned int theSfxID);
	virtual void	SetVolume(double theVolume);
	virtual bool	SetBaseVolume(unsigned int theSfxID, double theBaseVolume);
	virtual bool	SetBasePan(unsigned int theSfxID, int theBasePan);
	virtual SoundInstance* GetSoundInstance(unsigned int theSfxID);
	virtual void	ReleaseSounds();
	virtual void	ReleaseChannels();
	virtual double	GetMasterVolume();
	virtual void	SetMasterVolume(double theVolume);
	virtual void	Flush();
	virtual void	SetCooperativeWindow(HWND theHWnd);
	virtual void	StopAllSounds();
	virtual int		GetFreeSoundId();
	virtual int		GetNumSounds();
	virtual void	PurgeSounds();
	virtual void	PreloadSound(unsigned int theSfxID);
};

// ---------------------------------------------------------------------------
// Streamed music, on ASND voice 0.
//
// The compressed OGG lives in RAM and is decoded incrementally into a pair of
// PCM buffers. Decoding happens in WiiMusicInterface::Update() on the main
// thread, never in the ASND callback: that callback runs from the DSP interrupt
// handler, where a vorbis frame decode would be both far too slow and unsafe
// (it allocates). The callback only hands ASND an already-filled buffer.
//
// Songs are identified by the game's MusicFile id. Only one song streams at a
// time -- PvZ never plays two -- so switching songs replaces the stream.
// ---------------------------------------------------------------------------

class WiiMusicInterface : public MusicInterface
{
public:
	WiiMusicInterface();
	virtual ~WiiMusicInterface();

	virtual bool	LoadMusic(int theSongId, const std::string& theFileName);
	virtual void	PlayMusic(int theSongId, int theOffset = 0, bool noLoop = false);
	virtual void	StopMusic(int theSongId);
	virtual void	PauseMusic(int theSongId);
	virtual void	ResumeMusic(int theSongId);
	virtual void	StopAllMusic();
	virtual void	UnloadMusic(int theSongId);
	virtual void	UnloadAllMusic();
	virtual void	PauseAllMusic();
	virtual void	ResumeAllMusic();
	virtual void	FadeIn(int theSongId, int theOffset = -1, double theSpeed = 0.002, bool noLoop = false);
	virtual void	FadeOut(int theSongId, bool stopSong = true, double theSpeed = 0.004);
	virtual void	FadeOutAll(bool stopSong = true, double theSpeed = 0.004);
	virtual void	SetSongVolume(int theSongId, double theVolume);
	virtual void	SetSongMaxVolume(int theSongId, double theMaxVolume);
	virtual bool	IsPlaying(int theSongId);
	virtual void	SetVolume(double theVolume);
	virtual void	SetMusicAmplify(int theSongId, double theAmp);
	virtual void	Update();
};

// Streaming primitives, main/loading thread only. Mirrors the PS2 OGG mode so
// Music.cpp can drive both from one code path.
//
// Takes ownership of theOggData (malloc'd); frees it on stop or on failure.
bool WiiMusicPlay(unsigned char* theOggData, int theSize, bool theLoop);
void WiiMusicStop();
void WiiMusicSetPaused(bool thePaused);
bool WiiMusicIsPlaying();
void WiiMusicSetVolume(double theVolume);		// per-song volume (fades), 0..1
void WiiMusicSetMasterVolume(double theVolume);	// options slider, 0..1
// Decodes the next chunk if a buffer has drained. Call once per frame.
void WiiMusicPump();

}

#endif // WII_PLATFORM
#endif // __WIISOUNDMANAGER_H__
