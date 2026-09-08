#pragma once
#ifdef PS2_PLATFORM

class LawnApp;

// Console-style board controls (modeled on the X360/PS3 port), fed by the pad
// snapshot that platform/ps2/Input.cpp publishes each tick:
//   D-Pad     cursor snaps cell to cell
//   L1/R1     cycle through the usable seed packets
//   Triangle  shovel pick-up / put-back
// Every action lands in the game as ordinary widget mouse events, so the
// Board's PC-proven click handling stays the single authority over what a
// press actually does.
//
// Call once per game update. It also decides whether board mode is active and
// publishes that via ps2SetPadBoardMode, which switches how Input.cpp maps
// the overlapping buttons (D-Pad, Triangle, Start).
void Ps2UpdateBoardControls(LawnApp* theApp);

#endif // PS2_PLATFORM
