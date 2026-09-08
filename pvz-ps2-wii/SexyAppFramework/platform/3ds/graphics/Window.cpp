#include <3ds.h>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

extern "C" void PvZ3dsInstallCrashHandler();

void SexyAppBase::MakeWindow()
{
	if (mGLInterface == NULL)
	{
		osSetSpeedupEnable(true);
		gfxInitDefault();
		gfxSet3D(false); // Disable the stereoscopic 3D parallax barrier —
		                 // leaving it enabled causes visible horizontal
		                 // scanline artifacts across the top screen even
		                 // when rendering in 2D (non-stereo) mode.
		// Game data lives on the SD card ("sdmc:/3ds/PlantsvsZombies").
		// Modern libctru (>= 2.0) mounts sdmc: automatically for any homebrew
		// that has SD access at build time, so there is no explicit fsInit/
		// sdmcInit anymore (they were removed); romfs:/ remains opt-in for
		// assets bundled inside a .cia.
		romfsInit();

		// Crash logging needs sdmc mounted; install it now that it is.
		PvZ3dsInstallCrashHandler();

		C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

		// Game renders on the TOP screen (GLInterface target = GFX_TOP). The
		// BOTTOM screen is free: give it to the console so stdout / printf /
		// [HB]/[IP]/[MEM] debug lines are visible live while testing.
		consoleInit(GFX_BOTTOM, NULL);

		mGLInterface = new GLInterface(this);
		InitGLInterface();

		mGLInterface->UpdateViewport();
		mWidgetManager->Resize(mScreenBounds, mGLInterface->mPresentationRect);
	}

	bool isActive = mActive;
	mActive = true;

	mPhysMinimized = false;

	if (isActive != mActive)
		RehupFocus();

	ReInitImages();

	mWidgetManager->mImage = mGLInterface->GetScreenImage();
	mWidgetManager->MarkAllDirty();
}