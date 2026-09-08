#pragma once

#ifdef WII_PLATFORM

#include <stdint.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>

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
	return (Uint32)ticks_to_millisecs(gettime());
}

static inline void SDL_Delay(Uint32 ms)
{
	usleep((useconds_t)ms * 1000u);
}

static inline Uint64 SDL_GetPerformanceCounter()
{
	return (Uint64)gettime();
}

static inline Uint64 SDL_GetPerformanceFrequency()
{
	return (Uint64)TB_TIMER_CLOCK;
}

static inline int SDL_PushEvent(SDL_Event*)
{
	return 0;
}

#endif
