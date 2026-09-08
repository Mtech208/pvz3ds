#pragma once

#ifdef PS2_PLATFORM

#include <stdint.h>

struct Ps2GsTexture;

enum Ps2GsCommandPrimitive
{
	PS2_GS_COMMAND_SOLID_LINE = 0,
	PS2_GS_COMMAND_SOLID_SPRITE,
	PS2_GS_COMMAND_SOLID_TRIANGLE,
	PS2_GS_COMMAND_TEXTURED_SPRITE,
	PS2_GS_COMMAND_TEXTURED_TRIANGLE
};

void Ps2GsCommandBufferInit();
uint64_t* Ps2GsCommandBufferReserve(Ps2GsCommandPrimitive primitive,
	Ps2GsTexture* texture, uint32_t filter, int blendMode);
void Ps2GsCommandBufferSealRun();
void Ps2GsCommandBufferEmit();
void Ps2GsCommandBufferExecuteQueue();
void Ps2GsCommandBufferInvalidateTextureState(Ps2GsTexture* texture);
void Ps2GsCommandBufferGuardQueueBytes(unsigned int payloadBytes);

// Bounded replacement for gsKit_finish(). Never spins forever: a GIF that stops
// raising FINISH gets reset instead of hanging the main thread.
void Ps2GsCommandSyncFinish();

#endif
