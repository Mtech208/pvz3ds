#ifdef WII_PLATFORM

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"
#include "wii/gx_wii.h"

using namespace Sexy;

void SexyAppBase::MakeWindow()
{
	if (!mWindow)
	{
		wiigl_init(false);
		mWindow = reinterpret_cast<void*>(1);
		mContext = reinterpret_cast<void*>(1);
	}

	if (!mGLInterface)
	{
		mGLInterface = new GLInterface(this);
		InitGLInterface();
	}

	mActive = true;
	mMinimized = false;
	mPhysMinimized = false;
	ReInitImages();

	mWidgetManager->mImage = mGLInterface->GetScreenImage();
	mWidgetManager->MarkAllDirty();
	mGLInterface->UpdateViewport();
	mWidgetManager->Resize(mScreenBounds, mGLInterface->mPresentationRect);
}

#endif
