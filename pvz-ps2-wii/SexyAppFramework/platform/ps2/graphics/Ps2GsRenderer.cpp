#ifdef PS2_PLATFORM

#include "Ps2GsRenderer.h"

#include <gsKit.h>
#include <gsCore.h>
#include <gsInline.h>

#include "Ps2GsCommandBuffer.h"
#include "Ps2GsTexture.h"
#include "Ps2RenderStats.h"

extern GSGLOBAL* gsGlobal;

static int sLogicalWidth = 800;
static int sLogicalHeight = 600;
static int sBlendMode = 0;

static inline float Ps2GsX(float x)
{
	if (!gsGlobal || sLogicalWidth <= 1)
		return x;
	return x * (float)gsGlobal->Width / (float)(sLogicalWidth - 1);
}

static inline float Ps2GsY(float y)
{
	if (!gsGlobal || sLogicalHeight <= 1)
		return y;
	return y * (float)gsGlobal->Height / (float)(sLogicalHeight - 1);
}

static inline uint64_t Ps2GsColor(uint32_t color)
{
	const uint8_t red = (uint8_t)((color >> 0) & 0xFF);
	const uint8_t green = (uint8_t)((color >> 8) & 0xFF);
	const uint8_t blue = (uint8_t)((color >> 16) & 0xFF);
	const uint8_t alpha = (uint8_t)((color >> 24) & 0xFF);
	return GS_SETREG_RGBAQ(red, green, blue, (uint8_t)(alpha >> 1), 0);
}

static inline uint8_t Ps2GsTextureModulateChannel(uint8_t channel)
{
	// GS MODULATE uses 0x80, not 0xFF, as the neutral vertex-color value.
	return (uint8_t)(((unsigned int)channel * 128u + 127u) / 255u);
}

static inline uint64_t Ps2GsTextureColor(uint32_t color)
{
	const uint8_t red = Ps2GsTextureModulateChannel((uint8_t)((color >> 0) & 0xFF));
	const uint8_t green = Ps2GsTextureModulateChannel((uint8_t)((color >> 8) & 0xFF));
	const uint8_t blue = Ps2GsTextureModulateChannel((uint8_t)((color >> 16) & 0xFF));
	const uint8_t alpha = (uint8_t)((color >> 24) & 0xFF);
	return GS_SETREG_RGBAQ(red, green, blue, (uint8_t)(alpha >> 1), 0);
}

static inline uint64_t Ps2GsPackedXyz(float x, float y)
{
	return GS_SETREG_XYZ2(
		gsKit_float_to_int_x(gsGlobal, Ps2GsX(x)),
		gsKit_float_to_int_y(gsGlobal, Ps2GsY(y)), 0xFFFF);
}

static inline uint64_t Ps2GsPackedUv(const Ps2GsTexture* texture, float u, float v)
{
	return GS_SETREG_UV(
		gsKit_float_to_int_u(&texture->mGs, u * texture->mGs.Width),
		gsKit_float_to_int_v(&texture->mGs, v * texture->mGs.Height));
}

void Ps2GsRendererFlushBatch()
{
	Ps2GsCommandBufferSealRun();
}

void Ps2GsQueueGuardBytes(unsigned int payloadBytes)
{
	Ps2GsCommandBufferGuardQueueBytes(payloadBytes);
}

void Ps2GsRendererInvalidateTextureState(Ps2GsTexture* texture)
{
	Ps2GsCommandBufferInvalidateTextureState(texture);
}

void Ps2GsRendererSetLogicalSize(int logicalWidth, int logicalHeight)
{
	if ((logicalWidth > 0 && logicalWidth != sLogicalWidth) ||
		(logicalHeight > 0 && logicalHeight != sLogicalHeight))
		Ps2GsCommandBufferSealRun();

	if (logicalWidth > 0)
		sLogicalWidth = logicalWidth;
	if (logicalHeight > 0)
		sLogicalHeight = logicalHeight;
}

void Ps2GsRendererSetBlendAdditive(bool additive)
{
	const int mode = additive ? 1 : 0;
	if (!gsGlobal || sBlendMode == mode)
		return;

	Ps2GsCommandBufferSealRun();
	sBlendMode = mode;
	gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
	gsGlobal->PrimAlpha = additive
		? GS_SETREG_ALPHA(0, 2, 0, 1, 0)
		: GS_SETREG_ALPHA(0, 1, 0, 1, 0);
}

void Ps2GsRendererInit(int logicalWidth, int logicalHeight)
{
	Ps2GsCommandBufferInit();
	Ps2GsRendererSetLogicalSize(logicalWidth, logicalHeight);
	if (!gsGlobal)
		return;

	Ps2GsCommandBufferGuardQueueBytes(128);
	gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
	gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
	gsKit_set_test(gsGlobal, GS_ZTEST_OFF);
	gsKit_set_test(gsGlobal, GS_ATEST_OFF);
	gsKit_set_clamp(gsGlobal, GS_CMODE_CLAMP);
	sBlendMode = 0;
}

void Ps2GsRendererClear(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
{
	if (!gsGlobal)
		return;
	Ps2GsCommandBufferGuardQueueBytes(128);
	gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(red, green, blue, (uint8_t)(alpha >> 1), 0));
}

void Ps2GsRendererDrawLine(float x1, float y1, float x2, float y2, uint32_t color)
{
	uint64_t* registers = Ps2GsCommandBufferReserve(PS2_GS_COMMAND_SOLID_LINE,
		NULL, GS_FILTER_NEAREST, sBlendMode);
	if (!registers)
		return;

	registers[0] = Ps2GsColor(color);
	registers[1] = Ps2GsPackedXyz(x1, y1);
	registers[2] = Ps2GsPackedXyz(x2, y2);
}

void Ps2GsRendererDrawSolidSprite(float x1, float y1, float x2, float y2, uint32_t color)
{
	uint64_t* registers = Ps2GsCommandBufferReserve(PS2_GS_COMMAND_SOLID_SPRITE,
		NULL, GS_FILTER_NEAREST, sBlendMode);
	if (!registers)
		return;

	registers[0] = Ps2GsColor(color);
	registers[1] = Ps2GsPackedXyz(x1, y1);
	registers[2] = Ps2GsPackedXyz(x2, y2);
}

void Ps2GsRendererDrawTriangle(float x1, float y1, uint32_t color1,
	float x2, float y2, uint32_t color2,
	float x3, float y3, uint32_t color3)
{
	uint64_t* registers = Ps2GsCommandBufferReserve(PS2_GS_COMMAND_SOLID_TRIANGLE,
		NULL, GS_FILTER_NEAREST, sBlendMode);
	if (!registers)
		return;

	registers[0] = Ps2GsColor(color1);
	registers[1] = Ps2GsPackedXyz(x1, y1);
	registers[2] = Ps2GsColor(color2);
	registers[3] = Ps2GsPackedXyz(x2, y2);
	registers[4] = Ps2GsColor(color3);
	registers[5] = Ps2GsPackedXyz(x3, y3);
}

void Ps2GsRendererDrawTexturedSprite(Ps2GsTexture* texture,
	float x1, float y1, float u1, float v1,
	float x2, float y2, float u2, float v2,
	uint32_t color)
{
	if (!texture)
		return;

	// A normal SexyAppFramework Blt used to reach the PS2 wrapper as a
	// triangle strip, i.e. two textured triangles. Do not replace that with a
	// single GS SPRITE here: large textured sprites need special horizontal
	// striping on the GS and otherwise can collapse to nearly uniform samples.
	// Keep the known-correct triangle semantics while the command buffer still
	// batches the two triangles with neighbouring textured geometry.
	Ps2GsRendererDrawTexturedTriangle(texture,
		x1, y1, u1, v1, color,
		x1, y2, u1, v2, color,
		x2, y1, u2, v1, color);
	Ps2GsRendererDrawTexturedTriangle(texture,
		x2, y1, u2, v1, color,
		x1, y2, u1, v2, color,
		x2, y2, u2, v2, color);
}

void Ps2GsRendererDrawTexturedTriangle(Ps2GsTexture* texture,
	float x1, float y1, float u1, float v1, uint32_t color1,
	float x2, float y2, float u2, float v2, uint32_t color2,
	float x3, float y3, float u3, float v3, uint32_t color3)
{
	if (!texture || !gsGlobal)
		return;

	uint64_t* registers = Ps2GsCommandBufferReserve(PS2_GS_COMMAND_TEXTURED_TRIANGLE,
		texture, texture->mGs.Filter, sBlendMode);
	if (!registers)
		return;

	registers[0] = Ps2GsTextureColor(color1);
	registers[1] = Ps2GsPackedUv(texture, u1, v1);
	registers[2] = Ps2GsPackedXyz(x1, y1);
	registers[3] = Ps2GsTextureColor(color2);
	registers[4] = Ps2GsPackedUv(texture, u2, v2);
	registers[5] = Ps2GsPackedXyz(x2, y2);
	registers[6] = Ps2GsTextureColor(color3);
	registers[7] = Ps2GsPackedUv(texture, u3, v3);
	registers[8] = Ps2GsPackedXyz(x3, y3);
}

void Ps2GsRendererFlushQueue()
{
	if (!gsGlobal || !gsGlobal->Os_Queue)
		return;

	Ps2GsCommandBufferExecuteQueue();
}

void Ps2GsRendererSyncQueue()
{
	if (!gsGlobal || !gsGlobal->Os_Queue)
		return;

	Ps2GsRendererFlushQueue();
	// Bounded: the texture-upload paths that call this are exactly the ones that
	// get busy under load, and a raw gsKit_finish() here is the same unbounded
	// CSR spin that froze the game on hardware.
	Ps2GsCommandSyncFinish();
}

void Ps2GsRendererPresent()
{
	if (!gsGlobal)
		return;
	Ps2GsRendererFlushQueue();
	gsKit_sync_flip(gsGlobal);
	// Latch the residency counters for the frame that just finished.
	ps2_dbg_texture_frame_reset();
	ps2_dbg_queue_frame_reset();
}

#endif
