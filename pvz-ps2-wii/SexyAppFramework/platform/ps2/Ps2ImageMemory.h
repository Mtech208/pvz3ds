#pragma once

#ifdef PS2_PLATFORM

#include <cstdint>

namespace Sexy
{

long Ps2ImageUsedHeapBytes();
bool Ps2PrepareImageDecode(long thePeakBytes);
uint32_t* Ps2AllocImageBits(int theCount);
unsigned char* Ps2AllocImageBytes(int theCount);

}

#endif // PS2_PLATFORM
