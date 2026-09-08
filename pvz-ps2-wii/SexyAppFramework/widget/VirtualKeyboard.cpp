#include "widget/VirtualKeyboard.h"

#ifdef SEXY_VIRTUAL_KEYBOARD

#include "SexyAppBase.h"
#include "graphics/Graphics.h"
#include "graphics/Font.h"
#include "misc/KeyCodes.h"
#include "widget/WidgetManager.h"
#include "widget/EditWidget.h"

#include <stdlib.h>
#include <string.h>

using namespace Sexy;

namespace
{
// Rows of printable keys. Deliberately QWERTY rather than alphabetical: the
// point-and-click cost of finding a letter is the same either way, and QWERTY
// is what players already know the shape of.
const char* const kRows[] = {
	"1234567890",
	"qwertyuiop",
	"asdfghjkl",
	"zxcvbnm",
};
const int kNumRows = (int)(sizeof(kRows) / sizeof(kRows[0]));

// Auto-repeat, in app updates (PvZ updates at 100Hz), so holding a direction
// walks the grid instead of demanding one press per key.
const int kNavRepeatDelay = 25;
const int kNavRepeatRate = 5;

const unsigned int kNavDirections =
	VKNAV_LEFT | VKNAV_RIGHT | VKNAV_UP | VKNAV_DOWN;

char ToUpperAscii(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}
}

VirtualKeyboard::VirtualKeyboard()
	: mNumKeys(0), mPressedKey(-1), mHoverKey(-1), mShift(true),
	  mFocusKey(-1), mNavHeld(0), mNavRepeatCounter(kNavRepeatDelay)
{
	// Names read better capitalised, and the field is empty when the keyboard
	// opens, so start shifted like a phone keyboard does.
	memset(mKeys, 0, sizeof(mKeys));
	mWantsFocus = false;	// see the header: taking focus would dismiss us
	mMouseVisible = true;
}

VirtualKeyboard::~VirtualKeyboard()
{
}

void VirtualKeyboard::LayoutForScreen(int theScreenWidth, int theScreenHeight)
{
	const int aKeyW = 46;
	const int aKeyH = 38;
	const int aGap = 4;
	const int aPad = 10;

	const int aRowsHeight = (kNumRows + 1) * (aKeyH + aGap) - aGap;	// +1 command row
	const int aBoardH = aRowsHeight + aPad * 2;
	const int aBoardW = 10 * (aKeyW + aGap) - aGap + aPad * 2;
	const int aBoardX = (theScreenWidth - aBoardW) / 2;
	const int aBoardY = theScreenHeight - aBoardH - 12;

	Resize(aBoardX, aBoardY, aBoardW, aBoardH);

	mNumKeys = 0;
	int aY = aPad;
	for (int aRow = 0; aRow < kNumRows; aRow++)
	{
		const char* aChars = kRows[aRow];
		const int aLen = (int)strlen(aChars);
		// Centre short rows, so the layout keeps the familiar QWERTY stagger.
		int aX = aPad + ((10 - aLen) * (aKeyW + aGap)) / 2;
		for (int i = 0; i < aLen && mNumKeys < MAX_KEYS; i++)
		{
			Key& aKey = mKeys[mNumKeys++];
			aKey.mX = aX;
			aKey.mY = aY;
			aKey.mWidth = aKeyW;
			aKey.mHeight = aKeyH;
			aKey.mLower = aChars[i];
			aKey.mUpper = ToUpperAscii(aChars[i]);
			aKey.mCommand = KEYCMD_NONE;
			aKey.mLabel = NULL;
			aX += aKeyW + aGap;
		}
		aY += aKeyH + aGap;
	}

	// Command row: Shift | Space (wide) | Back | Done
	struct { int mCmd; const char* mLabel; int mUnits; } aCommands[] = {
		{ KEYCMD_SHIFT,     "Shift", 2 },
		{ KEYCMD_SPACE,     "Space", 4 },
		{ KEYCMD_BACKSPACE, "Back",  2 },
		{ KEYCMD_DONE,      "Done",  2 },
	};
	int aX = aPad;
	for (int i = 0; i < 4 && mNumKeys < MAX_KEYS; i++)
	{
		const int aWidth = aCommands[i].mUnits * aKeyW + (aCommands[i].mUnits - 1) * aGap;
		Key& aKey = mKeys[mNumKeys++];
		aKey.mX = aX;
		aKey.mY = aY;
		aKey.mWidth = aWidth;
		aKey.mHeight = aKeyH;
		aKey.mLower = 0;
		aKey.mUpper = 0;
		aKey.mCommand = aCommands[i].mCmd;
		aKey.mLabel = aCommands[i].mLabel;
		aX += aWidth + aGap;
	}

	mPressedKey = -1;
	mHoverKey = -1;
	if (mFocusKey >= mNumKeys)
		mFocusKey = mNumKeys > 0 ? 0 : -1;
	MarkDirty();
}

// Borrow whatever font the field being typed into uses, so the keys match the
// dialog they serve. The framework has no default font of its own, and this
// avoids reaching into any particular game's resource set from platform code.
_Font* VirtualKeyboard::GetLabelFont() const
{
	if (mWidgetManager == NULL)
		return NULL;
	EditWidget* anEdit = dynamic_cast<EditWidget*>(mWidgetManager->mFocusWidget);
	return anEdit != NULL ? anEdit->mFont : NULL;
}

int VirtualKeyboard::HitTest(int x, int y) const
{
	for (int i = 0; i < mNumKeys; i++)
	{
		const Key& aKey = mKeys[i];
		if (x >= aKey.mX && x < aKey.mX + aKey.mWidth &&
			y >= aKey.mY && y < aKey.mY + aKey.mHeight)
			return i;
	}
	return -1;
}

// Nearest key whose centre lies in the requested direction. Geometric rather
// than a row/column index because the grid is ragged: rows hold different
// counts and the command row's keys are two to four units wide, so "the key
// below" has no answer in index space.
int VirtualKeyboard::FindNeighbour(int theFrom, int theDx, int theDy) const
{
	if (theFrom < 0 || theFrom >= mNumKeys)
		return -1;

	const Key& aFrom = mKeys[theFrom];
	const int aFromCx = aFrom.mX + aFrom.mWidth / 2;
	const int aFromCy = aFrom.mY + aFrom.mHeight / 2;

	int aBest = -1;
	int aBestScore = 0;
	for (int i = 0; i < mNumKeys; i++)
	{
		if (i == theFrom)
			continue;

		const Key& aKey = mKeys[i];
		const int aDx = (aKey.mX + aKey.mWidth / 2) - aFromCx;
		const int aDy = (aKey.mY + aKey.mHeight / 2) - aFromCy;

		const int aAlong = aDx * theDx + aDy * theDy;
		if (aAlong <= 0)
			continue;	// behind us, or square on the perpendicular

		// Weight the sideways offset heavily so a wide key straight ahead beats
		// a nearer one off to the side; otherwise moving down from 'g' lands on
		// Shift instead of Space.
		const int aAcross = abs(aDx * theDy - aDy * theDx);
		const int aScore = aAlong + aAcross * 3;
		if (aBest < 0 || aScore < aBestScore)
		{
			aBest = i;
			aBestScore = aScore;
		}
	}
	return aBest;
}

void VirtualKeyboard::StepFocus(int theDx, int theDy)
{
	const int aNext = FindNeighbour(mFocusKey, theDx, theDy);
	if (aNext >= 0 && aNext != mFocusKey)
	{
		mFocusKey = aNext;
		MarkDirty();
	}
}

bool VirtualKeyboard::FeedNav(unsigned int theHeldBits)
{
	const unsigned int aPressed = theHeldBits & ~mNavHeld;
	mNavHeld = theHeldBits;

	// First direction press only engages D-Pad mode; it does not also step, so
	// the player sees where the focus landed before moving it.
	if (mFocusKey < 0)
	{
		if ((aPressed & kNavDirections) == 0)
			return false;

		mFocusKey = mHoverKey >= 0 ? mHoverKey : 0;
		mNavRepeatCounter = kNavRepeatDelay;
		MarkDirty();
		return true;
	}

	if (aPressed & kNavDirections)
	{
		if (aPressed & VKNAV_LEFT)	StepFocus(-1, 0);
		if (aPressed & VKNAV_RIGHT)	StepFocus(1, 0);
		if (aPressed & VKNAV_UP)	StepFocus(0, -1);
		if (aPressed & VKNAV_DOWN)	StepFocus(0, 1);
		mNavRepeatCounter = kNavRepeatDelay;
	}

	if ((aPressed & VKNAV_ACCEPT) && mFocusKey >= 0 && mFocusKey < mNumKeys)
		EmitKey(mKeys[mFocusKey]);
	if (aPressed & VKNAV_BACK)
		EmitBackspace();

	return true;
}

void VirtualKeyboard::Update()
{
	Widget::Update();

	if (mFocusKey < 0)
		return;

	const unsigned int aHeldDirs = mNavHeld & kNavDirections;
	if (aHeldDirs == 0)
	{
		mNavRepeatCounter = kNavRepeatDelay;
		return;
	}

	if (--mNavRepeatCounter > 0)
		return;
	mNavRepeatCounter = kNavRepeatRate;

	if (aHeldDirs & VKNAV_LEFT)		StepFocus(-1, 0);
	if (aHeldDirs & VKNAV_RIGHT)	StepFocus(1, 0);
	if (aHeldDirs & VKNAV_UP)		StepFocus(0, -1);
	if (aHeldDirs & VKNAV_DOWN)		StepFocus(0, 1);
}

void VirtualKeyboard::Draw(Graphics* g)
{
	_Font* aFont = GetLabelFont();

	// Panel. Opaque rather than translucent: it sits over the dialog it is
	// typing into, and a see-through panel makes small glyphs unreadable
	// against PvZ's busy backgrounds.
	g->SetColor(Color(24, 20, 40));
	g->FillRect(0, 0, mWidth, mHeight);
	g->SetColor(Color(120, 110, 160));
	g->FillRect(0, 0, mWidth, 2);
	g->FillRect(0, mHeight - 2, mWidth, 2);

	if (aFont != NULL)
		g->SetFont(aFont);

	for (int i = 0; i < mNumKeys; i++)
	{
		const Key& aKey = mKeys[i];

		const bool aDown = (i == mPressedKey);
		const bool aHot = (i == mHoverKey);
		const bool aActive = (aKey.mCommand == KEYCMD_SHIFT && mShift);
		const bool aFocused = (i == mFocusKey);

		if (aDown || aActive)
			g->SetColor(Color(210, 190, 90));
		else if (aHot || aFocused)
			g->SetColor(Color(90, 84, 130));
		else
			g->SetColor(Color(58, 54, 86));
		g->FillRect(aKey.mX, aKey.mY, aKey.mWidth, aKey.mHeight);

		// The D-Pad cursor needs to read at a glance from couch distance, and
		// the hover tint alone does not: outline the focused key.
		if (aFocused)
		{
			g->SetColor(Color(255, 236, 140));
			g->FillRect(aKey.mX, aKey.mY, aKey.mWidth, 2);
			g->FillRect(aKey.mX, aKey.mY + aKey.mHeight - 2, aKey.mWidth, 2);
			g->FillRect(aKey.mX, aKey.mY, 2, aKey.mHeight);
			g->FillRect(aKey.mX + aKey.mWidth - 2, aKey.mY, 2, aKey.mHeight);
		}

		if (aFont == NULL)
			continue;

		SexyString aLabel;
		if (aKey.mLower != 0)
			aLabel.append(1, mShift ? aKey.mUpper : aKey.mLower);
		else
			aLabel = aKey.mLabel;

		const int aTextW = aFont->StringWidth(aLabel);
		const int aTextX = aKey.mX + (aKey.mWidth - aTextW) / 2;
		const int aTextY = aKey.mY + (aKey.mHeight + aFont->GetAscent()) / 2 - 2;
		g->SetColor((aDown || aActive) ? Color(20, 16, 30) : Color(240, 240, 240));
		g->DrawString(aLabel, aTextX, aTextY);
	}
}

void VirtualKeyboard::MouseDown(int x, int y, int theClickCount)
{
	Widget::MouseDown(x, y, theClickCount);
	mPressedKey = HitTest(x, y);
	MarkDirty();
}

void VirtualKeyboard::MouseUp(int x, int y, int theClickCount)
{
	Widget::MouseUp(x, y, theClickCount);

	// Only fire when release lands on the same key that was pressed, so a
	// wobbling Wii Remote can slide off a key to cancel it.
	const int aKey = HitTest(x, y);
	if (aKey >= 0 && aKey == mPressedKey)
		EmitKey(mKeys[aKey]);

	mPressedKey = -1;
	MarkDirty();
}

void VirtualKeyboard::MouseMove(int x, int y)
{
	Widget::MouseMove(x, y);

	// Reaching for a key with the pointer hands control back to it: two visible
	// cursors fighting over the same grid is worse than either alone.
	if (mFocusKey >= 0)
	{
		mFocusKey = -1;
		MarkDirty();
	}

	const int aKey = HitTest(x, y);
	if (aKey != mHoverKey)
	{
		mHoverKey = aKey;
		MarkDirty();
	}
}

void VirtualKeyboard::MouseLeave()
{
	Widget::MouseLeave();
	if (mHoverKey != -1 || mPressedKey != -1)
	{
		mHoverKey = -1;
		mPressedKey = -1;
		MarkDirty();
	}
}

void VirtualKeyboard::EmitBackspace()
{
	if (mWidgetManager != NULL)
		mWidgetManager->KeyDown(KEYCODE_BACK);
}

void VirtualKeyboard::EmitKey(const Key& theKey)
{
	if (mWidgetManager == NULL)
		return;

	switch (theKey.mCommand)
	{
	case KEYCMD_SHIFT:
		mShift = !mShift;
		MarkDirty();
		return;

	case KEYCMD_BACKSPACE:
		EmitBackspace();
		return;

	case KEYCMD_SPACE:
		mWidgetManager->KeyChar(__S(' '));
		return;

	case KEYCMD_DONE:
		// Enter is what the dialog's default button listens for, so Done
		// confirms exactly as pressing OK would.
		mWidgetManager->KeyDown(KEYCODE_RETURN);
		return;

	default:
		break;
	}

	if (theKey.mLower != 0)
	{
		mWidgetManager->KeyChar((SexyChar)(mShift ? theKey.mUpper : theKey.mLower));
		// One capital at the start of a name is the common case; drop shift
		// after it fires so the rest types lowercase without a second tap.
		if (mShift)
		{
			mShift = false;
			MarkDirty();
		}
	}
}

// ---------------------------------------------------------------------------
// The one instance, created on first use and reused thereafter. Owned here
// rather than by SexyAppBase so no cross-platform header has to learn about a
// console-only widget.
// ---------------------------------------------------------------------------

namespace
{
VirtualKeyboard* gKeyboard = NULL;
bool gShown = false;
}

bool Sexy::VirtualKeyboardShow(SexyAppBase* theApp)
{
	// EditWidget calls this from GotFocus, so a field is now ready for input.
	if (theApp == NULL || theApp->mWidgetManager == NULL)
		return false;

	if (gKeyboard == NULL)
		gKeyboard = new VirtualKeyboard();

	gKeyboard->LayoutForScreen(theApp->mWidth, theApp->mHeight);
	theApp->mWidgetManager->AddWidget(gKeyboard);
	theApp->mWidgetManager->BringToFront(gKeyboard);
	gShown = true;

	// False: we do not hand back a pre-composed string the way a system IME
	// would. The keyboard types into the field live, through ordinary key
	// events, so the field must keep whatever text it already had.
	return false;
}

void Sexy::VirtualKeyboardHide(SexyAppBase* theApp)
{
	if (gKeyboard != NULL && theApp != NULL && theApp->mWidgetManager != NULL)
		theApp->mWidgetManager->RemoveWidget(gKeyboard);
	gShown = false;
}

bool Sexy::VirtualKeyboardIsActive()
{
	return gShown && gKeyboard != NULL;
}

bool Sexy::VirtualKeyboardFeedNav(unsigned int theHeldBits)
{
	if (!VirtualKeyboardIsActive())
		return false;
	return gKeyboard->FeedNav(theHeldBits);
}

#endif // SEXY_VIRTUAL_KEYBOARD
