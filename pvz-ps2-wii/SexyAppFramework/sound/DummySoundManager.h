#ifndef __DUMMYSOUNDMANAGER_H__
#define __DUMMYSOUNDMANAGER_H__

#include "SoundManager.h"
#include "SoundInstance.h"

namespace Sexy
{

class DummySoundInstance : public SoundInstance
{
public:
	virtual void Release() { delete this; }
	virtual void SetBaseVolume(double) {}
	virtual void SetBasePan(int) {}
	virtual void AdjustPitch(double) {}
	virtual void SetVolume(double) {}
	virtual void SetPan(int) {}
	virtual bool Play(bool, bool) { return false; }
	virtual void Stop() {}
	virtual bool IsPlaying() { return false; }
	virtual bool IsReleased() { return true; }
	virtual double GetVolume() { return 0.0; }
};

// No-op sound manager for targets with no usable audio backend yet.
// Mirrors DummyMusicInterface: every operation is a zero so the game never
// crashes on a missing sound, and the audio subsystem can be slotted in
// later without touching call sites.
//
// ResourceManager treats a negative GetFreeSoundId() as a fatal
// "Out of free sound ids" error, so we hand out monotonically increasing
// ids and pretend each sound loads. PlaySample() is guarded on a NULL
// instance, so the game stays playable with silent effects.
class DummySoundManager : public SoundManager
{
public:
	DummySoundManager() {}
	virtual ~DummySoundManager() {}

	virtual bool			Initialized() { return false; }

	virtual bool			LoadSound(unsigned int, const std::string&) { return true; }
	virtual int				LoadSound(const std::string&) { return GetFreeSoundId(); }
	virtual void			ReleaseSound(unsigned int) {}

	virtual void			SetVolume(double) {}
	virtual bool			SetBaseVolume(unsigned int, double) { return true; }
	virtual bool			SetBasePan(unsigned int, int) { return true; }

	virtual SoundInstance*	GetSoundInstance(unsigned int) { return NULL; }

	virtual void			ReleaseSounds() {}
	virtual void			ReleaseChannels() {}

	virtual double			GetMasterVolume() { return 0.0; }
	virtual void			SetMasterVolume(double) {}

	virtual void			Flush() {}
	virtual void			SetCooperativeWindow(HWND) {}
	virtual void			StopAllSounds() {}
	virtual int				GetFreeSoundId() { return mNextSoundId++; }
	virtual int				GetNumSounds() { return mNextSoundId; }

private:
	int						mNextSoundId = 0;
};

}

#endif // __DUMMYSOUNDMANAGER_H__
