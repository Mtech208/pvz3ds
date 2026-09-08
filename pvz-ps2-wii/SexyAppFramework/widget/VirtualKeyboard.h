#ifndef __VIRTUALKEYBOARD_H__
#define __VIRTUALKEYBOARD_H__

// Console text entry, shared by every platform with no OS keyboard.
//
// Neither the Wii nor the PS2 has OS text input, and the new-profile dialog
// refuses an empty name -- so without this there is no way to get past it at
// all. This started as a Wii-only widget; nothing in it was ever Wii-specific,
// so it lives here and both consoles switch it on.
//
// It is a plain Widget rather than anything special-cased, and it types by
// synthesising the same events a real keyboard would -- WidgetManager::KeyChar
// for characters, KeyDown for backspace and enter -- so the focused EditWidget
// needs no knowledge of it and every existing text field works unchanged.
//
// Two details make that work:
//
//   * mWantsFocus stays false (the Widget default). WidgetManager only moves
//     focus to a clicked widget when WantsFocus() is true, so tapping keys
//     leaves focus on the edit field. Were it otherwise, the first key press
//     would take focus, the field would fire LostFocus -> StopTextInput, and
//     the keyboard would dismiss itself.
//   * It is added and removed by SexyAppBase::StartTextInput/StopTextInput,
//     which EditWidget already calls on focus change -- so it appears exactly
//     when a field is ready for input, on any screen, with no per-dialog code.
//
// Two ways to drive it, which coexist:
//
//   * Pointer. Natural with the Wii IR or a PS2 USB mouse.
//   * D-Pad. A focused key moves with the directions and fires with the accept
//     button. Pushing a cursor across a 40-key grid with a stick is miserable,
//     which is what a PS2 player without a mouse would otherwise be doing.
//
// The D-Pad cannot arrive as ordinary key events: WidgetManager routes KeyDown
// to the *focused* widget, and this widget deliberately never takes focus. So
// platform input code feeds it directly through VirtualKeyboardFeedNav, which
// also owns the auto-repeat timing so no platform duplicates it.

#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(NINTENDO_3DS)
#define SEXY_VIRTUAL_KEYBOARD 1
#endif

#ifdef SEXY_VIRTUAL_KEYBOARD

#include "widget/Widget.h"

namespace Sexy
{

class _Font;
class SexyAppBase;

// Directions and buttons a console pad can feed the keyboard. Platforms pass
// *held* state and the keyboard derives edges and repeat itself.
enum VirtualKeyboardNavBits
{
	VKNAV_LEFT		= 1 << 0,
	VKNAV_RIGHT		= 1 << 1,
	VKNAV_UP		= 1 << 2,
	VKNAV_DOWN		= 1 << 3,
	VKNAV_ACCEPT	= 1 << 4,	// press the focused key
	VKNAV_BACK		= 1 << 5	// backspace, without aiming at the Back key
};

// Raised and dismissed from SexyAppBase::StartTextInput/StopTextInput. Show
// returns what StartTextInput should: false, because this types into the field
// live through ordinary key events rather than handing back a composed string
// the way a system IME would, so the field must keep whatever text it had.
bool VirtualKeyboardShow(SexyAppBase* theApp);
void VirtualKeyboardHide(SexyAppBase* theApp);

// True while the keyboard is on screen.
bool VirtualKeyboardIsActive();

// Feed once per pad poll, with currently-held VirtualKeyboardNavBits.
//
// Returns true when the keyboard is in D-Pad mode, in which case the caller
// must NOT also apply its normal mapping for those buttons -- otherwise the
// directions would move the edit caret and the accept button would fire a
// click at wherever the cursor happens to sit. Returns false in pointer mode,
// where the platform behaves exactly as it does with no keyboard up.
bool VirtualKeyboardFeedNav(unsigned int theHeldBits);

class VirtualKeyboard : public Widget
{
public:
	VirtualKeyboard();
	virtual ~VirtualKeyboard();

	// Lays the keyboard out for a screen of the given logical size and places
	// it along the bottom edge.
	void			LayoutForScreen(int theScreenWidth, int theScreenHeight);

	bool			FeedNav(unsigned int theHeldBits);

	virtual void	Draw(Graphics* g);
	virtual void	Update();
	virtual void	MouseDown(int x, int y, int theClickCount);
	virtual void	MouseUp(int x, int y, int theClickCount);
	virtual void	MouseMove(int x, int y);
	virtual void	MouseLeave();

private:
	struct Key
	{
		int		mX, mY, mWidth, mHeight;
		char	mLower;			// 0 for the command keys below
		char	mUpper;
		int		mCommand;		// KeyCommand
		const char* mLabel;		// used when mLower is 0
	};

	enum KeyCommand
	{
		KEYCMD_NONE = 0,
		KEYCMD_SHIFT,
		KEYCMD_BACKSPACE,
		KEYCMD_SPACE,
		KEYCMD_DONE
	};

	static const int MAX_KEYS = 64;

	Key		mKeys[MAX_KEYS];
	int		mNumKeys;
	int		mPressedKey;		// index armed by MouseDown, -1 when none
	int		mHoverKey;
	bool	mShift;

	// D-Pad navigation. mFocusKey < 0 means pointer mode.
	int				mFocusKey;
	unsigned int	mNavHeld;
	int				mNavRepeatCounter;

	int		HitTest(int x, int y) const;
	int		FindNeighbour(int theFrom, int theDx, int theDy) const;
	void	StepFocus(int theDx, int theDy);
	void	EmitKey(const Key& theKey);
	void	EmitBackspace();
	_Font*	GetLabelFont() const;
};

}

#endif // SEXY_VIRTUAL_KEYBOARD
#endif // __VIRTUALKEYBOARD_H__
