#ifndef __WIICURSOR_H__
#define __WIICURSOR_H__

#ifdef WII_PLATFORM

void WiiCursorDraw(float theX, float theY, float theRotation,
	int theScreenWidth, int theScreenHeight);
void WiiCursorShutdown();

#endif

#endif
