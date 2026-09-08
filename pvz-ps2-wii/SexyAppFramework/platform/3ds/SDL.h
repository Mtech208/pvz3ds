#pragma once

#ifdef __3DS__

#include <stdint.h>
#include <3ds.h>

typedef uint32_t Uint32;
typedef uint64_t Uint64;

#define SDL_INIT_TIMER 0x00000001u
#define SDL_QUIT       0x100u

typedef struct SDL_QuitEvent
{
	Uint32 type;
	Uint32 timestamp;
} SDL_QuitEvent;

typedef union SDL_Event
{
	Uint32 type;
	SDL_QuitEvent quit;
} SDL_Event;

static inline int SDL_Init(Uint32)
{
	return 0;
}

static inline Uint32 SDL_GetTicks()
{
	return (Uint32)osGetTime();
}

static inline void SDL_Delay(Uint32 ms)
{
	svcSleepThread((s64)ms * 1000000LL);
}

static inline Uint64 SDL_GetPerformanceCounter()
{
	return osGetTime();
}

static inline Uint64 SDL_GetPerformanceFrequency()
{
	return 1000u;
}

static inline int SDL_PushEvent(SDL_Event*)
{
	return 0;
}

#endif