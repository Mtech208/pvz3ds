#pragma once

#ifdef PS2_PLATFORM

#include <stdint.h>

struct Ps2GsTexture;

void Ps2GsRendererInit(int logicalWidth, int logicalHeight);
void Ps2GsRendererSetLogicalSize(int logicalWidth, int logicalHeight);
void Ps2GsRendererSetBlendAdditive(bool additive);
void Ps2GsRendererClear(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);
void Ps2GsRendererDrawLine(float x1, float y1, float x2, float y2, uint32_t color);
void Ps2GsRendererDrawSolidSprite(float x1, float y1, float x2, float y2, uint32_t color);
void Ps2GsRendererDrawTriangle(float x1, float y1, uint32_t color1,
	float x2, float y2, uint32_t color2,
	float x3, float y3, uint32_t color3);
void Ps2GsRendererDrawTexturedSprite(Ps2GsTexture* texture,
	float x1, float y1, float u1, float v1,
	float x2, float y2, float u2, float v2,
	uint32_t color);
void Ps2GsRendererDrawTexturedTriangle(Ps2GsTexture* texture,
	float x1, float y1, float u1, float v1, uint32_t color1,
	float x2, float y2, float u2, float v2, uint32_t color2,
	float x3, float y3, float u3, float v3, uint32_t color3);
void Ps2GsRendererFlushBatch();
void Ps2GsRendererFlushQueue();
void Ps2GsRendererSyncQueue();
void Ps2GsRendererPresent();
void Ps2GsRendererInvalidateTextureState(Ps2GsTexture* texture);
void Ps2GsQueueGuardBytes(unsigned int payloadBytes);

#endif
