#ifdef PS2_PLATFORM

#include "Ps2BoardControls.h"

#include "../LawnApp.h"
#include "Board.h"
#include "CursorObject.h"
#include "SeedPacket.h"
#include "widget/WidgetManager.h"
#include "Ps2PadState.h"

using namespace Sexy;

// Where inside a cell the snapped cursor lands, from the cell's top-left.
// Any point inside the cell maps back to the same cell, so dead-center
// precision is unnecessary; these offsets stay inside every cell variant
// (100px lawn rows, 85px roof rows) without crossing into a neighbour.
static const int kCellOffsetX = 40;
static const int kCellOffsetY = 45;

static bool BoardControlsActive(LawnApp* theApp)
{
	Board* aBoard = theApp->mBoard;
	return aBoard != NULL
		&& theApp->mGameScene == GameScenes::SCENE_PLAYING
		&& !aBoard->mPaused
		&& theApp->GetDialogCount() == 0;
}

// A full click through the WidgetManager, so the Board sees exactly what a PC
// mouse click produces (hit tests, sounds, advice text). Only the game's idea
// of the mouse position visits the target — the on-screen cursor stays put —
// and the caller restores it right after with a MouseMove.
static void SynthesizeClick(LawnApp* theApp, int theX, int theY)
{
	theApp->mWidgetManager->MouseDown(theX, theY, 1);
	theApp->mWidgetManager->MouseUp(theX, theY, 1);
}

static void StepCursorCell(LawnApp* theApp, Board* theBoard, int theDX, int theDY)
{
	int aMouseX = theApp->mWidgetManager->mLastMouseX;
	int aMouseY = theApp->mWidgetManager->mLastMouseY;

	// KeepOnBoard only clamps the low side, so clamp the high side here too.
	int aOrigY = theBoard->PixelToGridYKeepOnBoard(aMouseX, aMouseY);
	if (aOrigY > MAX_GRID_SIZE_Y - 1) aOrigY = MAX_GRID_SIZE_Y - 1;

	int aGridX = theBoard->PixelToGridXKeepOnBoard(aMouseX, aMouseY) + theDX;
	if (aGridX < 0) aGridX = 0;
	if (aGridX > MAX_GRID_SIZE_X - 1) aGridX = MAX_GRID_SIZE_X - 1;

	int aGridY = aOrigY + theDY;
	if (aGridY < 0) aGridY = 0;
	if (aGridY > MAX_GRID_SIZE_Y - 1) aGridY = MAX_GRID_SIZE_Y - 1;

	// Dirt rows (unsodded early-adventure rows, the missing 6th row on 5-row
	// lawns) are part of the grid but not playable: a vertical step keeps
	// going in its direction until it finds a sodded row, and stays put when
	// there is none that way.
	if (theDY != 0)
	{
		while (aGridY >= 0 && aGridY <= MAX_GRID_SIZE_Y - 1
			&& theBoard->mPlantRow[aGridY] == PlantRowType::PLANTROW_DIRT)
			aGridY += theDY;
		if (aGridY < 0 || aGridY > MAX_GRID_SIZE_Y - 1)
			aGridY = aOrigY;
	}

	Ps2WarpCursorToGamePos(theBoard->GridToPixelX(aGridX, aGridY) + kCellOffsetX,
	                       theBoard->GridToPixelY(aGridX, aGridY) + kCellOffsetY);
}

static void CycleSeed(LawnApp* theApp, Board* theBoard, int theDir)
{
	SeedBank* aBank = theBoard->mSeedBank;
	if (aBank == NULL || aBank->mNumPackets <= 0)
		return;

	int aCurrent = -1;
	if (theBoard->mCursorObject->mCursorType == CursorType::CURSOR_TYPE_PLANT_FROM_BANK)
		aCurrent = theBoard->mCursorObject->mSeedBankIndex;

	// Walk the bank in theDir from the held packet (or from the matching end
	// when nothing is held), skipping packets that are recharging or
	// unaffordable, until we either find one or come back around.
	int aIndex = (aCurrent >= 0) ? aCurrent : ((theDir > 0) ? -1 : aBank->mNumPackets);
	for (int i = 0; i < aBank->mNumPackets; i++)
	{
		aIndex = (aIndex + theDir + aBank->mNumPackets) % aBank->mNumPackets;
		if (aIndex == aCurrent)
			return;

		SeedPacket* aPacket = &aBank->mSeedPackets[aIndex];
		if (!aPacket->CanPickUp())
			continue;

		int aMouseX = theApp->mWidgetManager->mLastMouseX;
		int aMouseY = theApp->mWidgetManager->mLastMouseY;

		// Return the held packet the same way the right-click cancel path
		// does, then pick the new one up with a real click on it.
		if (aCurrent >= 0)
			theBoard->RefreshSeedPacketFromCursor();
		SynthesizeClick(theApp,
			aBank->mX + aPacket->mX + aPacket->mOffsetX + aPacket->mWidth / 2,
			aBank->mY + aPacket->mY + aPacket->mHeight / 2);
		theApp->mWidgetManager->MouseMove(aMouseX, aMouseY);
		return;
	}
}

static void ToggleShovel(LawnApp* theApp, Board* theBoard)
{
	if (!theBoard->mShowShovel)
		return;

	// Swap a held seed for the shovel in one press; leave any other tool
	// (watering can, cob cannon target, ...) alone.
	CursorType aCursor = theBoard->mCursorObject->mCursorType;
	if (aCursor == CursorType::CURSOR_TYPE_PLANT_FROM_BANK)
		theBoard->RefreshSeedPacketFromCursor();
	else if (aCursor != CursorType::CURSOR_TYPE_NORMAL && aCursor != CursorType::CURSOR_TYPE_SHOVEL)
		return;

	int aMouseX = theApp->mWidgetManager->mLastMouseX;
	int aMouseY = theApp->mWidgetManager->mLastMouseY;

	// Clicking the shovel button picks the shovel up, and clicking it again
	// with the shovel in hand puts it back, so one button toggles.
	Rect aRect = theBoard->GetShovelButtonRect();
	SynthesizeClick(theApp, aRect.mX + aRect.mWidth / 2, aRect.mY + aRect.mHeight / 2);
	theApp->mWidgetManager->MouseMove(aMouseX, aMouseY);
}

void Ps2UpdateBoardControls(LawnApp* theApp)
{
	bool aActive = BoardControlsActive(theApp);
	ps2SetPadBoardMode(aActive);

	// Copy the edges, then always drain them: edges accumulated while a menu
	// or dialog was up must not fire as actions when the board comes back.
	const Ps2PadSnapshot& aPad = ps2PadGetSnapshot(0);
	bool aConnected = aPad.connected;
	unsigned short aPressed = aPad.pressed;
	ps2PadConsumeEdges(0);

	if (!aActive || !aConnected)
		return;

	Board* aBoard = theApp->mBoard;

	int aDX = ((aPressed & PS2_PAD_RIGHT) != 0) - ((aPressed & PS2_PAD_LEFT) != 0);
	int aDY = ((aPressed & PS2_PAD_DOWN) != 0) - ((aPressed & PS2_PAD_UP) != 0);
	if (aDX != 0 || aDY != 0)
		StepCursorCell(theApp, aBoard, aDX, aDY);

	if (aPressed & PS2_PAD_R1)
		CycleSeed(theApp, aBoard, 1);
	else if (aPressed & PS2_PAD_L1)
		CycleSeed(theApp, aBoard, -1);

	if (aPressed & PS2_PAD_TRIANGLE)
		ToggleShovel(theApp, aBoard);
}

#endif // PS2_PLATFORM
