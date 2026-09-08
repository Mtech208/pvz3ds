#include <kernel.h>
#include <delaythread.h>
#include <gsKit.h>
#include <libpad.h>
#include <libmouse.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <math.h>

#include "SexyAppBase.h"
#include "graphics/Ps2GraphicsInterface.h"
#include "misc/KeyCodes.h"
#include "widget/WidgetManager.h"
#include "widget/VirtualKeyboard.h"
#include "Ps2PadState.h"
#include "Ps2IoLock.h"
#include "Ps2PvzServices.h"

using namespace Sexy;

extern GSGLOBAL* gsGlobal;

static unsigned char sPadBuf[256] __attribute__((aligned(64)));
static bool sPadReady = false;
static unsigned short sPrevHeld = 0;
static unsigned int sPrevMouseButtons = 0;
static float sCursorX = 0.0f;
static float sCursorY = 0.0f;
static bool sCursorInitialized = false;

static void EnsureCursorInitialized()
{
	if (sCursorInitialized)
		return;

	const float screenW = gsGlobal ? (float)gsGlobal->Width : 640.0f;
	const float screenH = gsGlobal ? (float)gsGlobal->Height : 224.0f;
	sCursorX = screenW * 0.5f;
	sCursorY = screenH * 0.5f;
	sCursorInitialized = true;
}

// Read by the PS2 graphics interface to overlay the software cursor each frame.
void Ps2GetCursorPos(float& x, float& y)
{
	EnsureCursorInitialized();
	x = sCursorX;
	y = sCursorY;
}

static unsigned char sLxCenter = 128;
static unsigned char sLyCenter = 128;
static unsigned char sRxCenter = 128;
static unsigned char sRyCenter = 128;
static int sCenterSamples = 0;
static int sLxSum = 0;
static int sLySum = 0;
static int sRxSum = 0;
static int sRySum = 0;
static bool sCentered = false;

static void WaitPadStable()
{
	for (int i = 0; i < 500; ++i)
	{
		if (padGetState(0, 0) == PAD_STATE_STABLE)
			return;
		DelayThread(1000);
	}
}

static void WaitPadRequest()
{
	for (int i = 0; i < 500; ++i)
	{
		int state = padGetReqState(0, 0);
		if (state == PAD_RSTAT_COMPLETE || state == PAD_RSTAT_FAILED)
			return;
		DelayThread(1000);
	}
}

static void InitPad()
{
	static bool sTriedInit = false;
	if (sTriedInit)
		return;
	sTriedInit = true;

	printf("[PS2] pad init begin\n");
	SifInitRpc(0);
	SifLoadFileInit();
	int sio2 = SifLoadModule("rom0:SIO2MAN", 0, NULL);
	int padman = SifLoadModule("rom0:PADMAN", 0, NULL);
	printf("[PS2] pad modules: SIO2MAN=%d PADMAN=%d\n", sio2, padman);

	if (padInit(0) != 1 || padPortOpen(0, 0, sPadBuf) == 0)
	{
		printf("[PS2] pad init failed\n");
		return;
	}

	WaitPadStable();
	for (int i = 0; i < 30; ++i)
	{
		padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
		WaitPadRequest();
		WaitPadStable();
		if (padInfoMode(0, 0, PAD_MODECURID, 0) == PAD_TYPE_DUALSHOCK)
			break;
		DelayThread(50000);
	}

	sPadReady = true;
	printf("[PS2] pad ready\n");
}

static bool AxisNearCenter(unsigned char v)
{
	return v >= 96 && v <= 160;
}

static void UpdateCalibration(const padButtonStatus& pad)
{
	if (sCentered)
		return;
	if (!AxisNearCenter(pad.ljoy_h) || !AxisNearCenter(pad.ljoy_v))
		return;

	sLxSum += pad.ljoy_h;
	sLySum += pad.ljoy_v;
	sRxSum += AxisNearCenter(pad.rjoy_h) ? pad.rjoy_h : 128;
	sRySum += AxisNearCenter(pad.rjoy_v) ? pad.rjoy_v : 128;
	if (++sCenterSamples < 16)
		return;

	sLxCenter = (unsigned char)(sLxSum / sCenterSamples);
	sLyCenter = (unsigned char)(sLySum / sCenterSamples);
	sRxCenter = (unsigned char)(sRxSum / sCenterSamples);
	sRyCenter = (unsigned char)(sRySum / sCenterSamples);
	sCentered = true;
}

static float Axis(unsigned char value, unsigned char center, float deadzone)
{
	float v = ((int)value - (int)center) / 127.0f;
	if (v > -deadzone && v < deadzone)
		return 0.0f;
	if (v < -1.0f)
		return -1.0f;
	if (v > 1.0f)
		return 1.0f;
	return v;
}

static void SendMouseMove(SexyAppBase* app, int x, int y)
{
	if (!app->mMouseIn)
		app->mMouseIn = true;

	int rx = x;
	int ry = y;
	app->mWidgetManager->RemapMouse(rx, ry);
	app->mLastUserInputTick = app->mLastTimerTime;
	app->mWidgetManager->MouseMove(rx, ry);
}

static void SendMouseButton(SexyAppBase* app, int x, int y, int button, bool down)
{
	int rx = x;
	int ry = y;
	app->mWidgetManager->RemapMouse(rx, ry);
	app->mLastUserInputTick = app->mLastTimerTime;
	app->mWidgetManager->MouseMove(rx, ry);
	if (down)
		app->mWidgetManager->MouseDown(rx, ry, button);
	else
		app->mWidgetManager->MouseUp(rx, ry, button);
}

static void SendKey(SexyAppBase* app, KeyCode key, bool down)
{
	app->mLastUserInputTick = app->mLastTimerTime;
	if (down)
		app->mWidgetManager->KeyDown(key);
	else
		app->mWidgetManager->KeyUp(key);
}

static void SendKeyTransitions(SexyAppBase* app, unsigned short pressed, unsigned short released, bool theBoardMode)
{
	// key = mapping in menus/dialogs; boardKey = mapping while board mode is
	// on (gameplay). KEYCODE_UNKNOWN = the button is not a key there — on the
	// board, D-Pad/Triangle/L1/R1 are consumed from the pad snapshot by
	// Lawn/Ps2BoardControls.cpp instead.
	struct Mapping { unsigned short button; KeyCode key; KeyCode boardKey; };
	static const Mapping kMappings[] = {
		{ PS2_PAD_UP, KEYCODE_UP, KEYCODE_UNKNOWN },
		{ PS2_PAD_DOWN, KEYCODE_DOWN, KEYCODE_UNKNOWN },
		{ PS2_PAD_LEFT, KEYCODE_LEFT, KEYCODE_UNKNOWN },
		{ PS2_PAD_RIGHT, KEYCODE_RIGHT, KEYCODE_UNKNOWN },
		// Circle stays Escape on the board on purpose: Board::KeyDown makes
		// Escape "return what's in the cursor, else open the menu" — exactly
		// the console-style cancel button.
		{ PS2_PAD_CIRCLE, KEYCODE_ESCAPE, KEYCODE_ESCAPE },
		{ PS2_PAD_TRIANGLE, KEYCODE_ESCAPE, KEYCODE_UNKNOWN },
		{ PS2_PAD_START, KEYCODE_RETURN, KEYCODE_ESCAPE },
		{ PS2_PAD_SELECT, KEYCODE_TAB, KEYCODE_TAB }
	};

	for (unsigned i = 0; i < sizeof(kMappings) / sizeof(kMappings[0]); ++i)
	{
		KeyCode aKey = theBoardMode ? kMappings[i].boardKey : kMappings[i].key;
		if (aKey == KEYCODE_UNKNOWN)
			continue;
		// Board mode can flip between a press and its release, so a release
		// may arrive under the other mapping; a stray KeyUp is harmless here.
		if (pressed & kMappings[i].button)
			SendKey(app, aKey, true);
		if (released & kMappings[i].button)
			SendKey(app, aKey, false);
	}
}

// The framebuffer has non-square pixels (e.g. 640x224 scanned out as 4:3), so
// vertical displacements are scaled by this factor to keep the on-screen
// cursor speed uniform. Applies to both pad steps and mouse deltas.
static float AspectY()
{
	const float screenW = gsGlobal ? (float)gsGlobal->Width : 640.0f;
	const float screenH = gsGlobal ? (float)gsGlobal->Height : 224.0f;
	return (screenH / screenW) * (4.0f / 3.0f);
}

// Places the cursor at an absolute GS screen position, clamps to the screen
// and forwards the move to the widget system.
static void SetCursorPos(SexyAppBase* app, float x, float y)
{
	EnsureCursorInitialized();
	const float screenW = gsGlobal ? (float)gsGlobal->Width : 640.0f;
	const float screenH = gsGlobal ? (float)gsGlobal->Height : 224.0f;
	if (x < 0.0f) x = 0.0f;
	if (y < 0.0f) y = 0.0f;
	if (x > screenW - 1.0f) x = screenW - 1.0f;
	if (y > screenH - 1.0f) y = screenH - 1.0f;
	sCursorX = x;
	sCursorY = y;
	SendMouseMove(app, (int)sCursorX, (int)sCursorY);
}

// Applies a cursor displacement (from the pad or the mouse).
static void MoveCursor(SexyAppBase* app, float dx, float dy)
{
	EnsureCursorInitialized();
	if (dx == 0.0f && dy == 0.0f)
		return;
	SetCursorPos(app, sCursorX + dx, sCursorY + dy);
}

// Declared in Ps2PadState.h; used by Lawn/Ps2BoardControls.cpp to snap the
// cursor to lawn cells. Widget space (800x600) → GS screen coordinates is the
// inverse of WidgetManager::RemapMouse.
void Ps2WarpCursorToGamePos(int theGameX, int theGameY)
{
	SexyAppBase* app = gSexyAppBase;
	if (app == NULL || app->mWidgetManager == NULL)
		return;

	WidgetManager* aManager = app->mWidgetManager;
	if (aManager->mMouseDestRect.mWidth <= 0 || aManager->mMouseDestRect.mHeight <= 0)
		return;

	float aScreenX = (float)(theGameX - aManager->mMouseDestRect.mX) * aManager->mMouseSourceRect.mWidth
		/ aManager->mMouseDestRect.mWidth + aManager->mMouseSourceRect.mX;
	float aScreenY = (float)(theGameY - aManager->mMouseDestRect.mY) * aManager->mMouseSourceRect.mHeight
		/ aManager->mMouseDestRect.mHeight + aManager->mMouseSourceRect.mY;
	SetCursorPos(app, aScreenX, aScreenY);
}

static void ProcessPadInput(SexyAppBase* app, const padButtonStatus& pad)
{
	EnsureCursorInitialized();
	UpdateCalibration(pad);

	unsigned short held = (unsigned short)(0xFFFF ^ pad.btns);
	unsigned short pressed = held & ~sPrevHeld;
	unsigned short released = sPrevHeld & ~held;
	sPrevHeld = held;

	float lx = sCentered ? Axis(pad.ljoy_h, sLxCenter, 0.18f) : 0.0f;
	float ly = sCentered ? Axis(pad.ljoy_v, sLyCenter, 0.18f) : 0.0f;
	float rx = sCentered ? Axis(pad.rjoy_h, sRxCenter, 0.18f) : 0.0f;
	float ry = sCentered ? Axis(pad.rjoy_v, sRyCenter, 0.18f) : 0.0f;

	ps2PadUpdateSnapshot(0, true, lx, ly, rx, ry, held, pressed, released);

	// On-screen keyboard: it takes the D-Pad, Cross and Triangle while the
	// player is driving it that way, and returns false in pointer mode, where
	// the cursor aims at keys and everything below behaves as it always has.
	// Without this the directions would move the edit caret and Cross would
	// click at wherever the cursor sits.
	unsigned int aNav = 0;
	if (held & PS2_PAD_LEFT)		aNav |= VKNAV_LEFT;
	if (held & PS2_PAD_RIGHT)		aNav |= VKNAV_RIGHT;
	if (held & PS2_PAD_UP)			aNav |= VKNAV_UP;
	if (held & PS2_PAD_DOWN)		aNav |= VKNAV_DOWN;
	if (held & PS2_PAD_CROSS)		aNav |= VKNAV_ACCEPT;
	if (held & PS2_PAD_TRIANGLE)	aNav |= VKNAV_BACK;
	const bool aKeyboardNav = VirtualKeyboardFeedNav(aNav);
	if (aKeyboardNav)
	{
		if (pressed || released)
			app->mLastUserInputTick = app->mLastTimerTime;
		return;
	}

	bool aBoardMode = ps2GetPadBoardMode();

	// Cursor speed in screen pixels per frame. The stick response is squared
	// (keeping sign) so small deflections give fine control and full tilt
	// still moves fast; the D-Pad uses the constant step — except in board
	// mode, where the D-Pad snaps the cursor cell to cell instead
	// (Lawn/Ps2BoardControls.cpp consumes it from the snapshot).
	const float step = 3.0f;
	const float stepY = step * AspectY();
	float dx = lx * fabsf(lx) * step;
	float dy = ly * fabsf(ly) * stepY;
	if (!aBoardMode)
	{
		if (held & PS2_PAD_LEFT)  dx -= step;
		if (held & PS2_PAD_RIGHT) dx += step;
		if (held & PS2_PAD_UP)    dy -= stepY;
		if (held & PS2_PAD_DOWN)  dy += stepY;
	}

	MoveCursor(app, dx, dy);

	if (pressed & PS2_PAD_CROSS)
		SendMouseButton(app, (int)sCursorX, (int)sCursorY, 1, true);
	if (released & PS2_PAD_CROSS)
		SendMouseButton(app, (int)sCursorX, (int)sCursorY, 1, false);
	if (pressed & PS2_PAD_SQUARE)
		SendMouseButton(app, (int)sCursorX, (int)sCursorY, -1, true);
	if (released & PS2_PAD_SQUARE)
		SendMouseButton(app, (int)sCursorX, (int)sCursorY, -1, false);

	SendKeyTransitions(app, pressed, released, aBoardMode);

	if (pressed || released || dx != 0.0f || dy != 0.0f)
		app->mLastUserInputTick = app->mLastTimerTime;
}

// DIFF-mode deltas are HID counts accumulated by the driver since the last
// read: +x = right, +y = toward the user = down-screen, matching GS
// coordinates, so no axis flip. Horizontal counts map 1:1 to screen pixels
// (desktop-like feel on the 640-wide frame); vertical gets the shared
// non-square-pixel correction.
static void ProcessMouseInput(SexyAppBase* app, const mouse_data& theMouse)
{
	EnsureCursorInitialized();
	MoveCursor(app, (float)theMouse.x, (float)theMouse.y * AspectY());

	// Double-click bits (PS2MOUSE_BTN*DBL, high byte) are ignored: the widget
	// system derives click counts from its own timing, same as the pad path.
	unsigned int buttons = theMouse.buttons & (PS2MOUSE_BTN1 | PS2MOUSE_BTN2);
	unsigned int pressed = buttons & ~sPrevMouseButtons;
	unsigned int released = sPrevMouseButtons & ~buttons;
	sPrevMouseButtons = buttons;

	if (pressed & PS2MOUSE_BTN1)
		SendMouseButton(app, (int)sCursorX, (int)sCursorY, 1, true);
	if (released & PS2MOUSE_BTN1)
		SendMouseButton(app, (int)sCursorX, (int)sCursorY, 1, false);
	if (pressed & PS2MOUSE_BTN2)
		SendMouseButton(app, (int)sCursorX, (int)sCursorY, -1, true);
	if (released & PS2MOUSE_BTN2)
		SendMouseButton(app, (int)sCursorX, (int)sCursorY, -1, false);
}

void SexyAppBase::InitInput()
{
	EnsureCursorInitialized();
	InitPad();
}

// The PS2 has no OS text input, so the new-profile dialog was unreachable
// without a USB keyboard: it refuses an empty name. The shared on-screen
// keyboard (SexyAppFramework/widget/VirtualKeyboard) covers it, driven by the
// pad's D-Pad or by the cursor.
bool SexyAppBase::StartTextInput(std::string&)
{
	return VirtualKeyboardShow(this);
}

void SexyAppBase::StopTextInput()
{
	VirtualKeyboardHide(this);
}

bool SexyAppBase::ProcessDeferredMessages(bool)
{
	if (!mWidgetManager)
		return false;

	// All libpad/libmouse calls below issue SIF RPC (EE<->IOP), which is not
	// thread-safe. Serialize against the resource-loading thread's file I/O
	// with the global SIF RPC lock; hold it only around the actual polling,
	// not the widget dispatch that follows.
	Ps2IoLockAcquire();
	InitPad();

	// Pad poll. A missing or misreporting pad must not starve the mouse poll
	// below, so failures only leave aPadValid false instead of returning.
	bool aPadValid = false;
	padButtonStatus pad = {};
	if (sPadReady)
	{
		int state = padGetState(0, 0);
		if (state == PAD_STATE_DISCONN || state == PAD_STATE_ERROR)
		{
			sPrevHeld = 0;
			ps2PadDisconnect(0);
		}
		else if (padRead(0, 0, &pad) != 0)
		{
			// mode==0 or btns==0x0000 (every button down at once) is
			// transition garbage from the pad, not real input.
			aPadValid = pad.mode != 0 && pad.btns != 0x0000;
		}
	}

	// Mouse poll, under the same lock. Idle or unplugged reads return zero
	// deltas and no buttons, which the processing below ignores naturally.
	mouse_data aMouse = {};
	bool aMouseValid = Ps2MouseAvailable() && PS2MouseRead(&aMouse) >= 0;
	Ps2IoLockRelease();

	if (aPadValid)
		ProcessPadInput(this, pad);
	if (aMouseValid)
		ProcessMouseInput(this, aMouse);

	return false;
}
