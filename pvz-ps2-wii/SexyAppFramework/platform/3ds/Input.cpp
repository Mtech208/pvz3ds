#include <3ds.h>
#include <cmath>

#include "SexyAppBase.h"
#include "misc/KeyCodes.h"
#include "widget/WidgetManager.h"
#include "widget/VirtualKeyboard.h"
#include "../../../Sexy.TodLib/TodDebug.h"

#include "../../../LawnApp.h"
#include "../../../Lawn/Board.h"
#include "../../../Lawn/Widget/SeedChooserScreen.h"

using namespace Sexy;

// 3DS input: the game renders to the BOTTOM (touch) screen, so the touch
// screen is the natural mouse: touches land directly in the render target's
// pixel space (0..320, 0..240) and WidgetManager::RemapMouse maps them onto
// the logical game screen (0..mWidth, 0..mHeight) through mPresentationRect.
//
// PvZ is a mouse game, so there is no hard cursor to move for gameplay: the
// finger *is* the pointer. The Circle Pad and the D-Pad are kept as a
// secondary cursor for players who prefer buttons, and for driving the shared
// on-screen keyboard (D-Pad navigation) the same way the Wii backend does.

namespace
{
// Secondary cursor state, used only when the stick/D-pad moves the pointer
// instead of a finger resting on the glass. Initialised to the screen centre.
float sCursorX = 200.0f;
float sCursorY = 150.0f;
const float kStickSpeed = 6.0f;			// logical px / s at full deflection
const float kStickDeadZone = 12.0f;		// of the +-156 circle pad range
const float kDPadSpeed = 6.0f;			// logical px per poll while held

u32 sPrevKeys = 0;
bool sTouchWasDown = false;
bool sAUsesKeyboardSelection = false;

bool AUsesKeyboardSelection()
{
	if (gLawnApp == NULL)
		return false;
	if (gLawnApp->mSeedChooserScreen != NULL &&
		gLawnApp->mSeedChooserScreen->HasKeyboardSelection())
		return true;
	if (gLawnApp->mBoard != NULL &&
		gLawnApp->mBoard->HasKeyboardSeedSelection())
		return true;
	return false;
}

void ClearKeyboardSeedSelection()
{
	if (gLawnApp == NULL)
		return;
	if (gLawnApp->mSeedChooserScreen != NULL)
		gLawnApp->mSeedChooserScreen->ClearKeyboardSelection();
	if (gLawnApp->mBoard != NULL)
		gLawnApp->mBoard->ClearKeyboardSeedSelection();
}

void ClampCursor()
{
	SexyAppBase* app = gSexyAppBase;
	if (app == NULL)
		return;
	int maxX = app->mWidth - 1;
	int maxY = app->mHeight - 1;
	if (sCursorX < 0.0f) sCursorX = 0.0f;
	if (sCursorX > maxX) sCursorX = (float)maxX;
	if (sCursorY < 0.0f) sCursorY = 0.0f;
	if (sCursorY > maxY) sCursorY = (float)maxY;
}

void SendPointerAt(SexyAppBase* app, float x, float y)
{
	int rx = (int)x;
	int ry = (int)y;
	app->mWidgetManager->RemapMouse(rx, ry);
	app->mLastUserInputTick = app->mLastTimerTime;
	app->mWidgetManager->MouseMove(rx, ry);
}

void SendButton(SexyAppBase* app, bool down, int button)
{
	int rx = (int)sCursorX;
	int ry = (int)sCursorY;
	app->mWidgetManager->RemapMouse(rx, ry);
	app->mLastUserInputTick = app->mLastTimerTime;
	app->mWidgetManager->MouseMove(rx, ry);
	if (down)
		app->mWidgetManager->MouseDown(rx, ry, button);
	else
		app->mWidgetManager->MouseUp(rx, ry, button);
}

void SendKey(SexyAppBase* app, KeyCode key, bool down)
{
	app->mLastUserInputTick = app->mLastTimerTime;
	if (down) app->mWidgetManager->KeyDown(key);
	else app->mWidgetManager->KeyUp(key);
}
} // namespace

void SexyAppBase::InitInput()
{
	// gfxInitDefault() only sets up graphics -- it does NOT bring up the HID
	// service. Without hidInit(), libctru's hidSharedMem global stays NULL and
	// the first hidScanInput() (called from ProcessDeferredMessages below) data
	// aborts on the null pointer (offsets 0x108..0x168 of the shared block).
	if (hidInit() != 0)
		TodTraceAndLog("[HID] hidInit failed; touch/pad input will be dead");

	if (!mMouseIn)
		mMouseIn = true;
}

bool SexyAppBase::StartTextInput(std::string&)
{
	// Shared on-screen keyboard, same as Wii/PS2: types into the focused
	// EditWidget through ordinary key events rather than composing a string
	// here, so return false like those backends do.
	return VirtualKeyboardShow(this);
}

void SexyAppBase::StopTextInput()
{
	VirtualKeyboardHide(this);
}

bool SexyAppBase::ProcessDeferredMessages(bool)
{
	// Read the whole pad state once per call.
	hidScanInput();
	u32 kDown = hidKeysDown();
	u32 kHeld = hidKeysHeld();
	u32 kUp = hidKeysUp();

	// ---- Button edges ------------------------------------------------

	// Click actions for the on-screen keyboard and the seed chooser.
	const bool anAIsHeld = (kHeld & KEY_A) != 0;
	const bool anAWasHeld = (sPrevKeys & KEY_A) != 0;
	sAUsesKeyboardSelection = AUsesKeyboardSelection();

	// D-Pad + A navigate the on-screen keyboard when it is up. When it is in
	// nav mode the platform must not apply its own mapping for those buttons.
	unsigned int aNav = 0;
	if (kHeld & KEY_DLEFT) aNav |= VKNAV_LEFT;
	if (kHeld & KEY_DRIGHT) aNav |= VKNAV_RIGHT;
	if (kHeld & KEY_DUP) aNav |= VKNAV_UP;
	if (kHeld & KEY_DDOWN) aNav |= VKNAV_DOWN;
	if (anAIsHeld) aNav |= VKNAV_ACCEPT;
	if (kHeld & KEY_B) aNav |= VKNAV_BACK;
	const bool aKeyboardNav = VirtualKeyboardFeedNav(aNav);

	if (aKeyboardNav)
	{
		sPrevKeys = kHeld;
		return false;
	}
	(void)anAWasHeld;

	// Left click from A (one-shot press+release keeps PvZ's click semantics).
	if (kDown & KEY_A)
	{
		if (sAUsesKeyboardSelection) SendKey(this, KEYCODE_RETURN, true);
		else SendButton(this, true, 1);
	}
	if (kUp & KEY_A)
	{
		if (sAUsesKeyboardSelection) SendKey(this, KEYCODE_RETURN, false);
		else SendButton(this, false, 1);
	}

	// Right click from B.
	if (kDown & KEY_B)
		SendButton(this, true, -1);
	if (kUp & KEY_B)
		SendButton(this, false, -1);

	// L/R also act as left click (handy right-handed with the stylus in the
	// other hand already, or when the touch is being used to drag).
	if (kDown & (KEY_L | KEY_R))
		SendButton(this, true, 1);
	if (kUp & (KEY_L | KEY_R))
		SendButton(this, false, 1);

	// START toggles pause / confirms dialogs. Reuse the Wii scheme: Escape
	// while playing, Return otherwise.
	const bool aBoardIsActive = gLawnApp != NULL && gLawnApp->mBoard != NULL &&
		gLawnApp->mGameScene == GameScenes::SCENE_PLAYING &&
		!gLawnApp->mBoard->mPaused && gLawnApp->GetDialogCount() == 0;
	if (kDown & KEY_START)
		SendKey(this, aBoardIsActive ? KEYCODE_ESCAPE : KEYCODE_RETURN, true);
	if (kUp & KEY_START)
	{
		SendKey(this, KEYCODE_ESCAPE, false);
		SendKey(this, KEYCODE_RETURN, false);
	}

	// SELECT doubles as Space.
	if (kDown & KEY_SELECT)
		SendKey(this, KEYCODE_SPACE, true);
	if (kUp & KEY_SELECT)
		SendKey(this, KEYCODE_SPACE, false);

	// D-Pad arrow keys (menus / screen-scrolling list boxes).
	struct DirectionMapping { u32 key; KeyCode code; };
	static const DirectionMapping kDirections[] = {
		{ KEY_DUP, KEYCODE_UP }, { KEY_DDOWN, KEYCODE_DOWN },
		{ KEY_DLEFT, KEYCODE_LEFT }, { KEY_DRIGHT, KEYCODE_RIGHT }
	};
	for (unsigned i = 0; i < sizeof(kDirections) / sizeof(kDirections[0]); i++)
	{
		const DirectionMapping& d = kDirections[i];
		if (kDown & d.key)
			SendKey(this, d.code, true);
		if (kUp & d.key)
			SendKey(this, d.code, false);
	}

	// ---- Cursor ------------------------------------------------------

	bool aCursorMoved = false;

	// Touch screen: absolute pointer in render-target space. libctru gates
	// touch reads behind KEY_TOUCH in the pad state (hidTouchCount was removed
	// from modern libctru) -- the coordinate from hidTouchRead is only valid
	// while a finger is actually down.
	touchPosition aTouch = {0};
	hidTouchRead(&aTouch);
	const bool aTouchDown = (hidKeysHeld() & KEY_TOUCH) != 0;
	if (aTouchDown)
	{
		sCursorX = (float)aTouch.px;
		sCursorY = (float)aTouch.py;
		// pX/pY are the image-space touch position (0..320, 0..240) which
		// maps 1:1 onto the bottom render target, so feed them straight the
		// same way PvZ-Portable's 3DS backend does.
		SendPointerAt(this, sCursorX, sCursorY);
		aCursorMoved = true;
	}

	// Circle Pad: relative cursor movement (secondary, when no finger).
	circlePosition aStick = {0};
	hidCircleRead(&aStick);
	float aStickX = (float)aStick.dx;
	float aStickY = (float)aStick.dy;
	if (aStickX > -kStickDeadZone && aStickX < kStickDeadZone) aStickX = 0.0f;
	if (aStickY > -kStickDeadZone && aStickY < kStickDeadZone) aStickY = 0.0f;
	if (aStickX != 0.0f || aStickY != 0.0f)
	{
		ClearKeyboardSeedSelection();
		sCursorX += aStickX * (kStickSpeed / 156.0f);
		sCursorY -= aStickY * (kStickSpeed / 156.0f);	// stick Y is up-positive
		aCursorMoved = true;
	}

	// D-Pad also nudges the cursor (and clears a keyboard seed select).
	const bool aDLeft = (kHeld & KEY_DLEFT) != 0;
	const bool aDRight = (kHeld & KEY_DRIGHT) != 0;
	const bool aDUp = (kHeld & KEY_DUP) != 0;
	const bool aDDown = (kHeld & KEY_DDOWN) != 0;
	if (aDLeft || aDRight || aDUp || aDDown)
	{
		ClearKeyboardSeedSelection();
		if (aDLeft) sCursorX -= kDPadSpeed;
		if (aDRight) sCursorX += kDPadSpeed;
		if (aDUp) sCursorY -= kDPadSpeed;
		if (aDDown) sCursorY += kDPadSpeed;
		aCursorMoved = true;
	}

	if (aCursorMoved)
		ClampCursor();

	// Synthetic click while the finger is down (touch has no hover before
	// contact, so a bare MouseMove already fired above; only the button edge
	// needs handling here).
	if (aTouchDown && !sTouchWasDown)
	{
		int rx = (int)sCursorX;
		int ry = (int)sCursorY;
		mWidgetManager->RemapMouse(rx, ry);
		mLastUserInputTick = mLastTimerTime;
		mWidgetManager->MouseDown(rx, ry, 1);
	}
	else if (!aTouchDown && sTouchWasDown)
	{
		int rx = (int)sCursorX;
		int ry = (int)sCursorY;
		mWidgetManager->RemapMouse(rx, ry);
		mLastUserInputTick = mLastTimerTime;
		mWidgetManager->MouseUp(rx, ry, 1);
	}
	sTouchWasDown = aTouchDown;

	sPrevKeys = kHeld;

	// The queue is drained on every call; there is nothing left pending, so
	// say false exactly like the Wii/PS2 backends (see their comment about
	// why returning true here hangs the loading bar).
	return false;
}
