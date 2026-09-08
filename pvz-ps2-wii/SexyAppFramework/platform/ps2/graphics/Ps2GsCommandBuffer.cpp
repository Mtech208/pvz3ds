#ifdef PS2_PLATFORM

#include "Ps2GsCommandBuffer.h"

#include <dmaKit.h>
#include <gsKit.h>
#include <gsCore.h>
#include <gsInline.h>
#include <stdio.h>
#include <string.h>

#include "Ps2GsConfig.h"
#include "Ps2GsTexture.h"

extern GSGLOBAL* gsGlobal;

struct Ps2GsCommandRun
{
	Ps2GsCommandPrimitive mPrimitive;
	Ps2GsTexture* mTexture;
	uint32_t mFilter;
	int mBlendMode;
	int mPrimitiveCount;
	int mRegisterOffset;
};

static Ps2GsCommandRun sRuns[PS2_GS_COMMAND_MAX_RUNS];
static uint64_t sRegisters[PS2_GS_COMMAND_MAX_REGISTERS] __attribute__((aligned(64)));
static int sRunCount = 0;
static int sRegisterCount = 0;
static int sPrimitiveCount = 0;

static Ps2GsCommandPrimitive sActivePrimitive = PS2_GS_COMMAND_SOLID_LINE;
static Ps2GsTexture* sActiveTexture = NULL;
static uint32_t sActiveFilter = GS_FILTER_NEAREST;
static int sActiveBlendMode = 0;
static int sActivePrimitiveCount = 0;
static int sActiveRegisterOffset = 0;

static Ps2GsTexture* sQueuedTexture = NULL;
static uint32_t sQueuedFilter = GS_FILTER_NEAREST;
static bool sQueuedFilterValid = false;
static int sQueuedBlendMode = 0;
static bool sQueuedBlendValid = false;

static bool Ps2GsCommandIsTextured(Ps2GsCommandPrimitive primitive)
{
	return primitive == PS2_GS_COMMAND_TEXTURED_SPRITE ||
		primitive == PS2_GS_COMMAND_TEXTURED_TRIANGLE;
}

static int Ps2GsCommandRegistersPerPrimitive(Ps2GsCommandPrimitive primitive)
{
	switch (primitive)
	{
	case PS2_GS_COMMAND_SOLID_LINE:
	case PS2_GS_COMMAND_SOLID_SPRITE:
		return 3;
	case PS2_GS_COMMAND_SOLID_TRIANGLE:
		return 6;
	case PS2_GS_COMMAND_TEXTURED_SPRITE:
		return 5;
	case PS2_GS_COMMAND_TEXTURED_TRIANGLE:
		return 9;
	default:
		return 0;
	}
}

static uint64_t Ps2GsCommandPrim(Ps2GsCommandPrimitive primitive)
{
	const bool triangle = primitive == PS2_GS_COMMAND_SOLID_TRIANGLE ||
		primitive == PS2_GS_COMMAND_TEXTURED_TRIANGLE;
	const bool line = primitive == PS2_GS_COMMAND_SOLID_LINE;
	const bool textured = Ps2GsCommandIsTextured(primitive);
	const int prim = line ? GS_PRIM_PRIM_LINE :
		(triangle ? GS_PRIM_PRIM_TRIANGLE : GS_PRIM_PRIM_SPRITE);
	return GS_SETREG_PRIM(prim, triangle ? 1 : 0, textured ? 1 : 0,
		gsGlobal->PrimFogEnable, gsGlobal->PrimAlphaEnable, gsGlobal->PrimAAEnable,
		1, gsGlobal->PrimContext, 0);
}

static int Ps2GsTextureExponent(int dimension)
{
	int exponent = 0;
	int size = 1;
	while (size < dimension && exponent < 10)
	{
		size <<= 1;
		++exponent;
	}
	return exponent;
}

// Tracks the CLUT the GS was last told to load, so a texture switch that lands
// on the same palette can let the hardware skip the transfer.
static uint32_t sLoadedClut = 0;
static bool sClutReloadPending = true;

static uint64_t Ps2GsTextureTex0(const Ps2GsTexture* texture)
{
	const int tw = Ps2GsTextureExponent(texture->mGs.Width);
	const int th = Ps2GsTextureExponent(texture->mGs.Height);
	if (texture->mGs.VramClut == 0)
	{
		return GS_SETREG_TEX0(texture->mGs.Vram / 256, texture->mGs.TBW, texture->mGs.PSM,
			tw, th, gsGlobal->PrimAlphaEnable, 0, 0, 0, 0, 0, GS_CLUT_STOREMODE_NOLOAD);
	}

	// CLD 4 compares CBP against CBP0 and only transfers when they differ, which
	// is what makes shared palettes cheap. The comparison is on the address
	// alone, so whenever the bytes behind an address may have changed the load
	// has to be forced with CLD 2, which reloads and refreshes CBP0.
	const bool forceLoad = sClutReloadPending || texture->mGs.VramClut != sLoadedClut;
	sLoadedClut = texture->mGs.VramClut;
	sClutReloadPending = false;

	return GS_SETREG_TEX0(texture->mGs.Vram / 256, texture->mGs.TBW, texture->mGs.PSM,
		tw, th, gsGlobal->PrimAlphaEnable, 0, texture->mGs.VramClut / 256,
		texture->mGs.ClutPSM, texture->mGs.ClutStorageMode, 0,
		forceLoad ? GS_CLUT_STOREMODE_LOAD_CBP0 : GS_CLUT_STOREMODE_COMPARE_CBP0);
}

static uint64_t Ps2GsBlendAlpha(int blendMode)
{
	return blendMode != 0
		? GS_SETREG_ALPHA(0, 2, 0, 1, 0)
		: GS_SETREG_ALPHA(0, 1, 0, 1, 0);
}

static void Ps2GsCommandResetStorage()
{
	sRunCount = 0;
	sRegisterCount = 0;
	sPrimitiveCount = 0;
	sActiveTexture = NULL;
	sActiveFilter = GS_FILTER_NEAREST;
	sActiveBlendMode = 0;
	sActivePrimitiveCount = 0;
	sActiveRegisterOffset = 0;
}

// ---------------------------------------------------------------------------
// GS FINISH synchronization.
//
// gsKit_finish(), and the identical wait inside gsKit_queue_exec_real, are bare
// spins on CSR bit 1 with no timeout and no escape:
//
//     lui v1,0x1200 ; ld v0,0x1000(v1) ; andi v0,v0,2 ; beqz v0,<back>
//
// If the GIF never raises FINISH the main thread spins there forever. The EE and
// IOP stay alive, so the symptom is a total freeze with the music still playing,
// and PCSX2 never reproduces it because it always raises FINISH promptly.
//
// FINISH cannot be forged: writing 1 to the CSR bit *clears* it, so a wedge
// cannot be papered over by setting the flag. The only way out is to reset the
// GIF and then tell gsKit not to wait for the FINISH that reset just discarded,
// which is what gsGlobal->FirstFrame does -- it is the exact flag gsKit tests
// before waiting (verified at gsKit_queue_exec_real+0xfc: lbu v0,50(a0)).
// ---------------------------------------------------------------------------

static volatile uint64_t* const kPs2GsCsr = (volatile uint64_t*)0x12001000;
static volatile uint32_t* const kPs2GifCtrl = (volatile uint32_t*)0x10003000;
static volatile uint32_t* const kPs2D2Chcr = (volatile uint32_t*)0x1000A000;
static volatile uint32_t* const kPs2D2Madr = (volatile uint32_t*)0x1000A010;
static volatile uint32_t* const kPs2D2Qwc = (volatile uint32_t*)0x1000A020;
static volatile uint32_t* const kPs2D2Tadr = (volatile uint32_t*)0x1000A030;
static volatile uint32_t* const kPs2DEnableR = (volatile uint32_t*)0x1000F520;
static volatile uint32_t* const kPs2DEnableW = (volatile uint32_t*)0x1000F590;

#define PS2_GS_CSR_FINISH 2ULL
#define PS2_DMA_SUSPEND_BIT 0x10000u

static unsigned int sQueueFlushesFrame = 0;
static unsigned int sQueueFlushesTotal = 0;
static unsigned int sGifRecoveries = 0;

// Mirrors gsKit's own gate rather than tracking a second copy of the same state:
// gsKit waits for FINISH exactly when FirstFrame is GS_SETTING_OFF, and sets it
// OFF at the end of every gsKit_queue_exec.
static bool Ps2GsFinishPending()
{
	return gsGlobal != NULL && gsGlobal->FirstFrame == GS_SETTING_OFF;
}

// The GIF stopped retiring work. Stop the channel, reset the GIF, and hand gsKit
// a clean slate. Costs the frame in flight; the alternative is a permanent hang.
static void Ps2GsRecoverWedgedGif()
{
	sGifRecoveries++;

	// Suspend every channel before touching a running one: clearing STR on a
	// live channel mid-burst is what turns a wedge into memory corruption.
	*kPs2DEnableW = *kPs2DEnableR | PS2_DMA_SUSPEND_BIT;
	__asm__ __volatile__("sync.l" ::: "memory");

	*kPs2D2Chcr = 0;
	*kPs2D2Qwc = 0;
	*kPs2D2Madr = 0;
	*kPs2D2Tadr = 0;
	__asm__ __volatile__("sync.l" ::: "memory");

	*kPs2GifCtrl = 1;  // RST
	__asm__ __volatile__("sync.l" ::: "memory");

	*kPs2DEnableW = *kPs2DEnableR & ~PS2_DMA_SUSPEND_BIT;
	__asm__ __volatile__("sync.l" ::: "memory");

	dmaKit_chan_init(DMA_CHANNEL_GIF);

	// Drop any stale FINISH (write-1-to-clear) and stop gsKit from waiting on
	// the one the reset threw away.
	*kPs2GsCsr = PS2_GS_CSR_FINISH;
	__asm__ __volatile__("sync" ::: "memory");
	if (gsGlobal != NULL)
	{
		gsGlobal->FirstFrame = GS_SETTING_ON;
		if (gsGlobal->Os_Queue)
			gsKit_queue_reset(gsGlobal->Os_Queue);
	}

	printf("[PS2][GS] GIF wedged: no FINISH in %u spins -- reset #%u\n",
		PS2_GS_FINISH_SPINS, sGifRecoveries);
}

// Bounded stand-in for gsKit_finish(). Returns false only when the GIF is wedged,
// in which case the caller must not enter gsKit's unbounded wait.
static bool Ps2GsWaitFinishBounded()
{
	if (!Ps2GsFinishPending())
		return true;  // nothing armed: no FINISH is coming, and none is owed

	for (unsigned int i = 0; i < PS2_GS_FINISH_SPINS; i++)
	{
		if ((*kPs2GsCsr & PS2_GS_CSR_FINISH) != 0)
			return true;
	}
	return false;
}

void Ps2GsCommandSyncFinish()
{
	if (!Ps2GsWaitFinishBounded())
	{
		Ps2GsRecoverWedgedGif();
		return;
	}
	if (Ps2GsFinishPending())
		gsKit_finish();  // bit is already set, so this returns immediately
}

static void Ps2GsCommandExecQueueRaw()
{
	if (!gsGlobal || !gsGlobal->Os_Queue)
		return;

	// Drain the previous batch ourselves so gsKit's unbounded wait finds the bit
	// already set and falls straight through.
	if (!Ps2GsWaitFinishBounded())
		Ps2GsRecoverWedgedGif();

	gsKit_queue_exec(gsGlobal);
	// No gsKit_queue_reset here: gsKit_queue_exec_real already resets the queue
	// and flips its double buffer. Resetting Os_Queue again after that swap, as
	// this used to, could rewind the buffer whose DMA was still in flight.
}

static void Ps2GsCommandGuardQueueBytesRaw(unsigned int payloadBytes)
{
	if (!gsGlobal || !gsGlobal->Os_Queue || gsGlobal->CurQueue != gsGlobal->Os_Queue)
		return;

	GSQUEUE* queue = gsGlobal->Os_Queue;
	const unsigned int needed = PS2_GS_QUEUE_GUARD_BYTES + payloadBytes;
	const unsigned int remaining = (unsigned int)((uint8_t*)queue->pool_max[queue->dbuf] -
		(uint8_t*)queue->pool_cur);
	if (remaining > needed)
		return;

	// This is the corruption backstop, not a routine path. It firing at all means
	// the oneshot pool is undersized for the frame -- see the sizing note in
	// Window.cpp before considering shrinking it again.
	sQueueFlushesFrame++;
	sQueueFlushesTotal++;
	Ps2GsCommandExecQueueRaw();
}

extern "C" void ps2_dbg_queue_frame_reset(void) { sQueueFlushesFrame = 0; }
extern "C" int ps2_dbg_queue_flushes_frame(void) { return (int)sQueueFlushesFrame; }
extern "C" int ps2_dbg_queue_flushes_total(void) { return (int)sQueueFlushesTotal; }
extern "C" int ps2_dbg_gif_recoveries(void) { return (int)sGifRecoveries; }

static bool Ps2GsCommandActiveMatches(Ps2GsCommandPrimitive primitive,
	Ps2GsTexture* texture, uint32_t filter, int blendMode)
{
	if (sActivePrimitiveCount <= 0 || sActivePrimitive != primitive ||
		sActiveTexture != texture || sActiveBlendMode != blendMode)
		return false;
	return !Ps2GsCommandIsTextured(primitive) || sActiveFilter == filter;
}

void Ps2GsCommandBufferSealRun()
{
	if (sActivePrimitiveCount <= 0)
		return;

	Ps2GsCommandRun& run = sRuns[sRunCount++];
	run.mPrimitive = sActivePrimitive;
	run.mTexture = sActiveTexture;
	run.mFilter = sActiveFilter;
	run.mBlendMode = sActiveBlendMode;
	run.mPrimitiveCount = sActivePrimitiveCount;
	run.mRegisterOffset = sActiveRegisterOffset;

	sActiveTexture = NULL;
	sActiveFilter = GS_FILTER_NEAREST;
	sActivePrimitiveCount = 0;
	sActiveRegisterOffset = sRegisterCount;
}

static int Ps2GsCommandStateCount(const Ps2GsCommandRun& run,
	Ps2GsTexture*& queuedTexture, uint32_t& queuedFilter, bool& queuedFilterValid,
	int& queuedBlendMode, bool& queuedBlendValid)
{
	const bool textured = Ps2GsCommandIsTextured(run.mPrimitive);
	const bool writeBlend = !queuedBlendValid || queuedBlendMode != run.mBlendMode;
	const bool writeTex1 = textured && (!queuedFilterValid || queuedFilter != run.mFilter);
	const bool writeTex0 = textured && queuedTexture != run.mTexture;

	if (writeBlend)
	{
		queuedBlendMode = run.mBlendMode;
		queuedBlendValid = true;
	}
	if (writeTex1)
	{
		queuedFilter = run.mFilter;
		queuedFilterValid = true;
	}
	if (writeTex0)
		queuedTexture = run.mTexture;

	return (writeBlend ? 1 : 0) + (writeTex1 ? 1 : 0) + (writeTex0 ? 1 : 0);
}

static inline void Ps2GsCommandWriteAd(uint64_t*& data, uint64_t value, uint64_t address)
{
	*data++ = value;
	*data++ = address;
}

static void Ps2GsCommandWriteState(uint64_t*& data, const Ps2GsCommandRun& run,
	Ps2GsTexture*& queuedTexture, uint32_t& queuedFilter, bool& queuedFilterValid,
	int& queuedBlendMode, bool& queuedBlendValid)
{
	const bool textured = Ps2GsCommandIsTextured(run.mPrimitive);
	const bool writeBlend = !queuedBlendValid || queuedBlendMode != run.mBlendMode;
	const bool writeTex1 = textured && (!queuedFilterValid || queuedFilter != run.mFilter);
	const bool writeTex0 = textured && queuedTexture != run.mTexture;

	if (writeBlend)
	{
		Ps2GsCommandWriteAd(data, Ps2GsBlendAlpha(run.mBlendMode),
			GS_ALPHA_1 + gsGlobal->PrimContext);
		queuedBlendMode = run.mBlendMode;
		queuedBlendValid = true;
	}
	if (writeTex1)
	{
		Ps2GsCommandWriteAd(data, GS_SETREG_TEX1(0, 0, run.mFilter, run.mFilter, 0, 0, 0),
			GS_TEX1_1 + gsGlobal->PrimContext);
		queuedFilter = run.mFilter;
		queuedFilterValid = true;
	}
	if (writeTex0)
	{
		Ps2GsCommandWriteAd(data, Ps2GsTextureTex0(run.mTexture),
			GS_TEX0_1 + gsGlobal->PrimContext);
		queuedTexture = run.mTexture;
	}
}

static void Ps2GsCommandWritePrimitiveData(uint64_t*& data, const Ps2GsCommandRun& run)
{
	const int storedRegisters = Ps2GsCommandRegistersPerPrimitive(run.mPrimitive);
	for (int primitiveIndex = 0; primitiveIndex < run.mPrimitiveCount; ++primitiveIndex)
	{
		const uint64_t* registers = &sRegisters[run.mRegisterOffset + primitiveIndex * storedRegisters];
		switch (run.mPrimitive)
		{
		case PS2_GS_COMMAND_SOLID_LINE:
		case PS2_GS_COMMAND_SOLID_SPRITE:
			Ps2GsCommandWriteAd(data, registers[0], GS_RGBAQ);
			Ps2GsCommandWriteAd(data, registers[1], GS_XYZ2);
			Ps2GsCommandWriteAd(data, registers[2], GS_XYZ2);
			break;

		case PS2_GS_COMMAND_SOLID_TRIANGLE:
			Ps2GsCommandWriteAd(data, registers[0], GS_RGBAQ);
			Ps2GsCommandWriteAd(data, registers[1], GS_XYZ2);
			Ps2GsCommandWriteAd(data, registers[2], GS_RGBAQ);
			Ps2GsCommandWriteAd(data, registers[3], GS_XYZ2);
			Ps2GsCommandWriteAd(data, registers[4], GS_RGBAQ);
			Ps2GsCommandWriteAd(data, registers[5], GS_XYZ2);
			break;

		case PS2_GS_COMMAND_TEXTURED_SPRITE:
			Ps2GsCommandWriteAd(data, registers[0], GS_RGBAQ);
			Ps2GsCommandWriteAd(data, registers[1], GS_UV);
			Ps2GsCommandWriteAd(data, registers[2], GS_XYZ2);
			Ps2GsCommandWriteAd(data, registers[3], GS_UV);
			Ps2GsCommandWriteAd(data, registers[4], GS_XYZ2);
			break;

		case PS2_GS_COMMAND_TEXTURED_TRIANGLE:
			Ps2GsCommandWriteAd(data, registers[0], GS_RGBAQ);
			Ps2GsCommandWriteAd(data, registers[1], GS_UV);
			Ps2GsCommandWriteAd(data, registers[2], GS_XYZ2);
			Ps2GsCommandWriteAd(data, registers[3], GS_RGBAQ);
			Ps2GsCommandWriteAd(data, registers[4], GS_UV);
			Ps2GsCommandWriteAd(data, registers[5], GS_XYZ2);
			Ps2GsCommandWriteAd(data, registers[6], GS_RGBAQ);
			Ps2GsCommandWriteAd(data, registers[7], GS_UV);
			Ps2GsCommandWriteAd(data, registers[8], GS_XYZ2);
			break;
		}
	}
}

static uint64_t* Ps2GsCommandAllocPacket(int payloadQwords)
{
	if (payloadQwords <= 0)
		return NULL;

	Ps2GsCommandGuardQueueBytesRaw((unsigned int)((payloadQwords + 1) * 16));
	uint64_t* store = (uint64_t*)gsKit_heap_alloc(gsGlobal, payloadQwords,
		payloadQwords * 16, GIF_AD);
	if (!store)
	{
		Ps2GsCommandExecQueueRaw();
		store = (uint64_t*)gsKit_heap_alloc(gsGlobal, payloadQwords,
			payloadQwords * 16, GIF_AD);
	}
	return store;
}

static bool Ps2GsCommandEmitAdState(const Ps2GsCommandRun& run,
	Ps2GsTexture*& queuedTexture, uint32_t& queuedFilter, bool& queuedFilterValid,
	int& queuedBlendMode, bool& queuedBlendValid)
{
	Ps2GsTexture* countTexture = queuedTexture;
	uint32_t countFilter = queuedFilter;
	bool countFilterValid = queuedFilterValid;
	int countBlendMode = queuedBlendMode;
	bool countBlendValid = queuedBlendValid;
	const int entries = Ps2GsCommandStateCount(run, countTexture, countFilter,
		countFilterValid, countBlendMode, countBlendValid);
	if (entries <= 0)
		return true;

	uint64_t* store = Ps2GsCommandAllocPacket(entries);
	if (!store)
		return false;

	uint64_t* data = store;
	if (store == gsGlobal->CurQueue->last_tag)
	{
		*data++ = GIF_TAG_AD(entries);
		*data++ = GIF_AD;
	}
	Ps2GsCommandWriteState(data, run, queuedTexture, queuedFilter, queuedFilterValid,
		queuedBlendMode, queuedBlendValid);
	return true;
}

static int Ps2GsCommandAdRangeEntries(int firstRun, int runEnd,
	Ps2GsTexture* queuedTexture, uint32_t queuedFilter, bool queuedFilterValid,
	int queuedBlendMode, bool queuedBlendValid)
{
	int entries = 0;
	for (int i = firstRun; i < runEnd; ++i)
	{
		const Ps2GsCommandRun& run = sRuns[i];
		entries += Ps2GsCommandStateCount(run, queuedTexture, queuedFilter,
			queuedFilterValid, queuedBlendMode, queuedBlendValid);
		entries += 1;
		entries += run.mPrimitiveCount * Ps2GsCommandRegistersPerPrimitive(run.mPrimitive);
	}
	return entries;
}

static bool Ps2GsCommandEmitAdRange(int firstRun, int runEnd,
	Ps2GsTexture*& queuedTexture, uint32_t& queuedFilter, bool& queuedFilterValid,
	int& queuedBlendMode, bool& queuedBlendValid)
{
	if (firstRun >= runEnd)
		return true;

	const int entries = Ps2GsCommandAdRangeEntries(firstRun, runEnd,
		queuedTexture, queuedFilter, queuedFilterValid, queuedBlendMode, queuedBlendValid);
	if (entries <= 0)
		return true;

	uint64_t* store = Ps2GsCommandAllocPacket(entries);
	if (!store)
		return false;

	uint64_t* data = store;
	if (store == gsGlobal->CurQueue->last_tag)
	{
		*data++ = GIF_TAG_AD(entries);
		*data++ = GIF_AD;
	}

	for (int i = firstRun; i < runEnd; ++i)
	{
		const Ps2GsCommandRun& run = sRuns[i];
		Ps2GsCommandWriteState(data, run, queuedTexture, queuedFilter, queuedFilterValid,
			queuedBlendMode, queuedBlendValid);
		Ps2GsCommandWriteAd(data, Ps2GsCommandPrim(run.mPrimitive), GS_PRIM);
		Ps2GsCommandWritePrimitiveData(data, run);
	}
	return true;
}

static const uint8_t sTexturedTriangleReglistOrder[] =
{
	GS_PRIM,
	GS_RGBAQ, GS_UV, GS_XYZ2,
	GS_RGBAQ, GS_UV, GS_XYZ2,
	GS_RGBAQ, GS_UV, GS_XYZ2
};

static int Ps2GsCommandTexturedTriangleReglistRegisterCount()
{
	return (int)(sizeof(sTexturedTriangleReglistOrder) /
		sizeof(sTexturedTriangleReglistOrder[0]));
}

static uint64_t Ps2GsCommandTexturedTriangleReglistDescriptor()
{
	uint64_t descriptor = 0;
	const int registerCount = Ps2GsCommandTexturedTriangleReglistRegisterCount();
	for (int i = 0; i < registerCount; ++i)
		descriptor |= (uint64_t)(sTexturedTriangleReglistOrder[i] & 0x0F) << (i * 4);
	return descriptor;
}

static bool Ps2GsCommandValidateTexturedTriangleReglist(const Ps2GsCommandRun& run)
{
	if (run.mPrimitive != PS2_GS_COMMAND_TEXTURED_TRIANGLE || !run.mTexture ||
		run.mPrimitiveCount <= 0 || run.mPrimitiveCount > 0x7FFF)
		return false;

	const int storedRegisters = Ps2GsCommandRegistersPerPrimitive(run.mPrimitive);
	if (storedRegisters != 9 || run.mRegisterOffset < 0 ||
		run.mRegisterOffset + run.mPrimitiveCount * storedRegisters > sRegisterCount)
		return false;

	const uint64_t descriptor = Ps2GsCommandTexturedTriangleReglistDescriptor();
	const int registerCount = Ps2GsCommandTexturedTriangleReglistRegisterCount();
	if (registerCount != 10)
		return false;
	for (int i = 0; i < registerCount; ++i)
	{
		const uint8_t encodedRegister = (uint8_t)((descriptor >> (i * 4)) & 0x0F);
		if (encodedRegister != sTexturedTriangleReglistOrder[i])
			return false;
	}

	const uint64_t referencePrim = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 1,
		gsGlobal->PrimFogEnable, gsGlobal->PrimAlphaEnable, gsGlobal->PrimAAEnable,
		1, gsGlobal->PrimContext, 0);
	return Ps2GsCommandPrim(run.mPrimitive) == referencePrim;
}

static bool Ps2GsCommandUseTexturedTriangleReglist(const Ps2GsCommandRun& run)
{
	if (run.mPrimitive != PS2_GS_COMMAND_TEXTURED_TRIANGLE)
		return false;

	static bool warned = false;
	if (!Ps2GsCommandValidateTexturedTriangleReglist(run))
	{
		if (!warned)
		{
			printf("[PS2] Textured REGLIST validation failed; using A+D fallback.\n");
			warned = true;
		}
		return false;
	}
	return true;
}

static bool Ps2GsCommandEmitTexturedTriangleReglist(const Ps2GsCommandRun& run,
	Ps2GsTexture*& queuedTexture, uint32_t& queuedFilter, bool& queuedFilterValid,
	int& queuedBlendMode, bool& queuedBlendValid)
{
	if (!Ps2GsCommandEmitAdState(run, queuedTexture, queuedFilter, queuedFilterValid,
		queuedBlendMode, queuedBlendValid))
		return false;

	const int registersPerTriangle = Ps2GsCommandTexturedTriangleReglistRegisterCount();
	const int payloadQwords = (run.mPrimitiveCount * registersPerTriangle) / 2;
	uint64_t* store = Ps2GsCommandAllocPacket(payloadQwords);
	if (!store)
		return false;

	uint64_t* data = store;
	if (store == gsGlobal->CurQueue->last_tag)
	{
		*data++ = GIF_TAG(run.mPrimitiveCount, 1, 0, 0,
			GSKIT_GIF_FLG_REGLIST, registersPerTriangle);
		*data++ = Ps2GsCommandTexturedTriangleReglistDescriptor();
	}

	const uint64_t prim = Ps2GsCommandPrim(run.mPrimitive);
	for (int primitiveIndex = 0; primitiveIndex < run.mPrimitiveCount; ++primitiveIndex)
	{
		const uint64_t* registers = &sRegisters[run.mRegisterOffset + primitiveIndex * 9];
		*data++ = prim;
		for (int registerIndex = 0; registerIndex < 9; ++registerIndex)
			*data++ = registers[registerIndex];
	}
	return true;
}

void Ps2GsCommandBufferEmit()
{
	Ps2GsCommandBufferSealRun();
	if (!gsGlobal || sRunCount <= 0)
		return;

	Ps2GsTexture* queuedTexture = sQueuedTexture;
	uint32_t queuedFilter = sQueuedFilter;
	bool queuedFilterValid = sQueuedFilterValid;
	int queuedBlendMode = sQueuedBlendMode;
	bool queuedBlendValid = sQueuedBlendValid;
	int adRangeStart = 0;

	for (int i = 0; i < sRunCount; ++i)
	{
		const Ps2GsCommandRun& run = sRuns[i];
		if (!Ps2GsCommandUseTexturedTriangleReglist(run))
			continue;

		if (!Ps2GsCommandEmitAdRange(adRangeStart, i, queuedTexture, queuedFilter,
			queuedFilterValid, queuedBlendMode, queuedBlendValid))
		{
			Ps2GsCommandResetStorage();
			return;
		}

		if (!Ps2GsCommandEmitTexturedTriangleReglist(run, queuedTexture, queuedFilter,
			queuedFilterValid, queuedBlendMode, queuedBlendValid))
		{
			if (!Ps2GsCommandEmitAdRange(i, i + 1, queuedTexture, queuedFilter,
				queuedFilterValid, queuedBlendMode, queuedBlendValid))
			{
				Ps2GsCommandResetStorage();
				return;
			}
		}
		adRangeStart = i + 1;
	}

	if (!Ps2GsCommandEmitAdRange(adRangeStart, sRunCount, queuedTexture, queuedFilter,
		queuedFilterValid, queuedBlendMode, queuedBlendValid))
	{
		Ps2GsCommandResetStorage();
		return;
	}

	sQueuedTexture = queuedTexture;
	sQueuedFilter = queuedFilter;
	sQueuedFilterValid = queuedFilterValid;
	sQueuedBlendMode = queuedBlendMode;
	sQueuedBlendValid = queuedBlendValid;
	Ps2GsCommandResetStorage();
}

uint64_t* Ps2GsCommandBufferReserve(Ps2GsCommandPrimitive primitive,
	Ps2GsTexture* texture, uint32_t filter, int blendMode)
{
	if (!gsGlobal)
		return NULL;

	const bool textured = Ps2GsCommandIsTextured(primitive);
	if (textured && !texture)
		return NULL;

	if (!Ps2GsCommandActiveMatches(primitive, texture, filter, blendMode))
	{
		Ps2GsCommandBufferSealRun();
		if (sRunCount >= PS2_GS_COMMAND_MAX_RUNS ||
			sPrimitiveCount >= PS2_GS_COMMAND_MAX_PRIMITIVES)
			Ps2GsCommandBufferEmit();

		if (textured && !Ps2GsTextureEnsureResident(texture))
			return NULL;

		sActivePrimitive = primitive;
		sActiveTexture = texture;
		sActiveFilter = textured ? filter : GS_FILTER_NEAREST;
		sActiveBlendMode = blendMode;
		sActiveRegisterOffset = sRegisterCount;
	}

	const int registersPerPrimitive = Ps2GsCommandRegistersPerPrimitive(primitive);
	if (sPrimitiveCount >= PS2_GS_COMMAND_MAX_PRIMITIVES ||
		sRegisterCount + registersPerPrimitive > PS2_GS_COMMAND_MAX_REGISTERS)
	{
		Ps2GsCommandBufferSealRun();
		Ps2GsCommandBufferEmit();
		if (textured && !Ps2GsTextureEnsureResident(texture))
			return NULL;
		sActivePrimitive = primitive;
		sActiveTexture = texture;
		sActiveFilter = textured ? filter : GS_FILTER_NEAREST;
		sActiveBlendMode = blendMode;
		sActiveRegisterOffset = sRegisterCount;
	}

	uint64_t* registers = &sRegisters[sRegisterCount];
	sRegisterCount += registersPerPrimitive;
	++sPrimitiveCount;
	++sActivePrimitiveCount;
	return registers;
}

void Ps2GsCommandBufferInvalidateTextureState(Ps2GsTexture* texture)
{
	if (!texture || sQueuedTexture == texture)
		sQueuedTexture = NULL;

	// Uploads and evictions are the only events that change what lives behind a
	// CLUT address, and both land here. Forcing the next load from this single
	// point keeps the CBP0 comparison from ever serving a stale palette.
	sClutReloadPending = true;
}

void Ps2GsCommandBufferGuardQueueBytes(unsigned int payloadBytes)
{
	Ps2GsCommandBufferEmit();
	Ps2GsCommandGuardQueueBytesRaw(payloadBytes);
}

void Ps2GsCommandBufferExecuteQueue()
{
	Ps2GsCommandBufferEmit();
	if (!gsGlobal || !gsGlobal->Os_Queue || gsGlobal->Os_Queue->tag_size == 0)
		return;

	Ps2GsCommandExecQueueRaw();
}

void Ps2GsCommandBufferInit()
{
	Ps2GsCommandResetStorage();
	sQueuedTexture = NULL;
	sQueuedFilter = GS_FILTER_NEAREST;
	sQueuedFilterValid = false;
	sQueuedBlendMode = 0;
	sQueuedBlendValid = false;
	sLoadedClut = 0;
	sClutReloadPending = true;
}

#endif
