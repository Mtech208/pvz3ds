#include <dmaKit.h>
#include <graph.h>
#include <gsKit.h>
#include <gsMisc.h>
#include <sifrpc.h>
#include <stdio.h>

#include "SexyAppBase.h"
#include "graphics/Ps2GraphicsInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

extern GSGLOBAL* gsGlobal;

static void InitPs2GsKit()
{
	static bool sInited = false;
	if (sInited)
		return;

	SifInitRpc(0);
	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
		D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);

	// Only the persistent queue is shrunk from gsKit_init_global()'s defaults:
	// it is never used (everything renders GS_ONESHOT, switched below), so
	// 256KB -> 32KB x2 buffers is free.
	//
	// The oneshot pool MUST stay at gsKit's 1MB. It was cut to 384KB on
	// 2026-07-18 to save heap, on the theory that overflow is harmless because
	// Ps2GsCommandGuardQueueBytesRaw executes the queue mid-frame and drops
	// nothing. That theory was wrong, and it cost a hard freeze on real
	// hardware (2026-08-17): gsKit's mid-frame exec waits for the previous
	// batch's GS FINISH with a bare, timeout-less spin on the CSR, so it can
	// never return if the GIF does not raise FINISH. PCSX2 always raises it
	// promptly and never reproduced this.
	//
	// So the guard is a corruption backstop, not a routine path. Sizing the
	// pool so a heavy frame (a full zombie horde) fits without tripping it is
	// what keeps the main thread out of that spin. Shrink this again only
	// together with a bounded FINISH wait at the call site.
	gsGlobal = gsKit_init_global_custom(1024 * 1024, 32 * 1024);
	bool isPal = graph_get_region() == GRAPH_MODE_PAL;
	gsGlobal->Mode = isPal ? GS_MODE_PAL : GS_MODE_NTSC;
	// FRAME (not FIELD): with FIELD rendering, anything that moves shears into
	// horizontal comb strips (visible on the drifting menu clouds). FRAME mode
	// renders/display the full progressive buffer per vsync.
	gsGlobal->Interlace = GS_INTERLACED;
	gsGlobal->Field = GS_FRAME;
	gsGlobal->Width = 640;
	gsGlobal->Height = isPal ? 256 : 224;
	// CT32 framebuffer: a CT16 target quantizes the final image to 5 bits per
	// channel, which banded the sky gradients no matter the texture quality.
	gsGlobal->PSM = GS_PSM_CT32;
	gsGlobal->PSMZ = GS_PSMZ_16S;
	gsGlobal->DoubleBuffering = GS_SETTING_ON;
	gsGlobal->ZBuffering = GS_SETTING_OFF;

	gsKit_init_screen(gsGlobal);
	gsKit_mode_switch(gsGlobal, GS_ONESHOT);
	gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
	gsKit_queue_exec(gsGlobal);
	gsKit_sync_flip(gsGlobal);
	if (gsGlobal->Os_Queue)
		gsKit_queue_reset(gsGlobal->Os_Queue);

	printf("[PS2] gsKit initialized: %dx%d %s\n",
		(int)gsGlobal->Width, (int)gsGlobal->Height, isPal ? "PAL" : "NTSC");
	sInited = true;
}

void SexyAppBase::MakeWindow()
{
	if (!mWindow)
	{
		InitPs2GsKit();
		mWindow = (void*)gsGlobal;
		mContext = (void*)gsGlobal;
	}

	if (mGLInterface == NULL)
	{
		printf("[PS2] creating native GS graphics interface\n");
		mGLInterface = new GLInterface(this);
		printf("[PS2] native graphics init begin\n");
		InitGLInterface();
		printf("[PS2] native graphics init done\n");
	}

	bool isActive = mActive;
	mActive = true;

	mPhysMinimized = false;
	if (mMinimized)
	{
		if (mMuteOnLostFocus)
			Unmute(true);

		mMinimized = false;
		isActive = mActive; // set this here so we don't call RehupFocus again.
		RehupFocus();
	}
	
	if (isActive != mActive)
		RehupFocus();

	ReInitImages();

	mWidgetManager->mImage = mGLInterface->GetScreenImage();
	mWidgetManager->MarkAllDirty();

	mGLInterface->UpdateViewport();
	printf("[PS2] viewport updated\n");
	mWidgetManager->Resize(mScreenBounds, mGLInterface->mPresentationRect);
}
