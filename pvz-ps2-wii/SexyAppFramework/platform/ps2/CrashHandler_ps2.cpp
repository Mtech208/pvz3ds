#ifdef PS2_PLATFORM

#include "Ps2CrashHandler.h"

#include <debug.h>
#include <ee_cop0_defs.h>
#include <ee_debug.h>
#include <stdio.h>
#include <string.h>

#include "Sexy.TodLib/TodDebug.h"
#include "Ps2BigArena.h"

// Level 1 Cause.ExcCode values. ee_cop0_defs.h only supplies the extraction
// macro, so the R5900 codes are named here.
#define PS2_EXC_MOD  1  /* TLB modified */
#define PS2_EXC_TLBL 2  /* TLB miss on load or instruction fetch */
#define PS2_EXC_TLBS 3  /* TLB miss on store */
#define PS2_EXC_ADEL 4  /* address error on load or instruction fetch */
#define PS2_EXC_ADES 5  /* address error on store */
#define PS2_EXC_IBE  6  /* bus error on instruction fetch */
#define PS2_EXC_DBE  7  /* bus error on data access */
#define PS2_EXC_RI   10 /* reserved instruction */
#define PS2_EXC_CPU  11 /* coprocessor unusable */

// Captured before anything else runs, so the numbers survive even if the screen
// or the log write below fails. A debugger attached after the fact can read this
// out of .bss.
struct Ps2CrashRecord
{
	u32 mCause;
	u32 mExcCode;
	u32 mStatus;
	u32 mEpc;
	u32 mErrorEpc;
	u32 mBadVAddr;
	u32 mRa;
	u32 mSp;
	u32 mGp;
	u32 mFp;
	u32 mA0;
	u32 mA1;
	u32 mA2;
	u32 mA3;
	u32 mS0;
	u32 mV0;
	int mValid;
};

static Ps2CrashRecord sCrashRecord;
static volatile int sInCrashHandler = 0;

static const char* Ps2CrashCauseName(u32 theExcCode)
{
	switch (theExcCode)
	{
	case PS2_EXC_MOD:  return "TLB modified";
	case PS2_EXC_TLBL: return "TLB miss (load/fetch)";
	case PS2_EXC_TLBS: return "TLB miss (store)";
	case PS2_EXC_ADEL: return "address error (load/fetch)";
	case PS2_EXC_ADES: return "address error (store)";
	case PS2_EXC_IBE:  return "bus error (fetch)";
	case PS2_EXC_DBE:  return "bus error (data)";
	case PS2_EXC_RI:   return "reserved instruction";
	case PS2_EXC_CPU:  return "coprocessor unusable";
	default:           return "unknown";
	}
}

static void Ps2CrashCapture(EE_RegFrame* theFrame)
{
	// Registers are 128 bit in the frame; the low word is what addresses and
	// pointers live in.
	sCrashRecord.mCause = theFrame->cause;
	sCrashRecord.mExcCode = M_EE_GET_CAUSE_EXCODE(theFrame->cause);
	sCrashRecord.mStatus = theFrame->status;
	sCrashRecord.mEpc = theFrame->epc;
	sCrashRecord.mErrorEpc = theFrame->errorepc;
	sCrashRecord.mBadVAddr = theFrame->badvaddr;
	sCrashRecord.mRa = theFrame->ra[0];
	sCrashRecord.mSp = theFrame->sp[0];
	sCrashRecord.mGp = theFrame->gp[0];
	sCrashRecord.mFp = theFrame->fp[0];
	sCrashRecord.mA0 = theFrame->a0[0];
	sCrashRecord.mA1 = theFrame->a1[0];
	sCrashRecord.mA2 = theFrame->a2[0];
	sCrashRecord.mA3 = theFrame->a3[0];
	sCrashRecord.mS0 = theFrame->s0[0];
	sCrashRecord.mV0 = theFrame->v0[0];
	sCrashRecord.mValid = 1;
}

static void Ps2CrashShowOnScreen(void)
{
	// libdebug draws straight into its own framebuffer through the GS, with no
	// IOP round trip, which is why this comes before the log write. gsKit may
	// still own a queue; init_scr reconfigures the display over it.
	init_scr();
	scr_setfontcolor(0x00ffffff);
	scr_setbgcolor(0x00000040);
	scr_printf("\n\n  EE EXCEPTION -- %s\n\n", Ps2CrashCauseName(sCrashRecord.mExcCode));
	scr_printf("  EPC      %08x   <- addr2line this\n", sCrashRecord.mEpc);
	scr_printf("  RA       %08x   <- caller\n", sCrashRecord.mRa);
	scr_printf("  BADVADDR %08x   <- address touched\n\n", sCrashRecord.mBadVAddr);
	scr_printf("  CAUSE %08x  STATUS %08x  ERROREPC %08x\n",
		sCrashRecord.mCause, sCrashRecord.mStatus, sCrashRecord.mErrorEpc);
	scr_printf("  SP %08x  GP %08x  FP %08x  S0 %08x\n",
		sCrashRecord.mSp, sCrashRecord.mGp, sCrashRecord.mFp, sCrashRecord.mS0);
	scr_printf("  A0 %08x  A1 %08x  A2 %08x  A3 %08x  V0 %08x\n",
		sCrashRecord.mA0, sCrashRecord.mA1, sCrashRecord.mA2, sCrashRecord.mA3,
		sCrashRecord.mV0);
	scr_printf("\n  arena %u KB used\n", Ps2BigUsedBytes() >> 10);
	scr_printf("\n  mips64r5900el-ps2-elf-addr2line -f -C -e pvz_ps2_syms.elf 0x%08x\n",
		sCrashRecord.mEpc);
}

static void Ps2CrashWriteLog(void)
{
	// Last, and never depended upon: this reaches the pendrive through a SIF RPC
	// to the IOP, which is not something an exception context can be trusted to
	// complete. By now the same numbers are already on the TV.
	char aLine[512];
	snprintf(aLine, sizeof(aLine),
		"[EE FATAL] %s excode=%u epc=%08x ra=%08x badvaddr=%08x "
		"cause=%08x status=%08x sp=%08x arena=%uKB\n",
		Ps2CrashCauseName(sCrashRecord.mExcCode), (unsigned int)sCrashRecord.mExcCode,
		(unsigned int)sCrashRecord.mEpc, (unsigned int)sCrashRecord.mRa,
		(unsigned int)sCrashRecord.mBadVAddr, (unsigned int)sCrashRecord.mCause,
		(unsigned int)sCrashRecord.mStatus, (unsigned int)sCrashRecord.mSp,
		Ps2BigUsedBytes() >> 10);
	TodForceAppendToDebugLog(aLine);
}

static void Ps2CrashHalt(void)
{
	// Reading the volatile flag keeps an empty infinite loop from being treated
	// as unreachable and removed.
	while (sInCrashHandler)
		;
}

// C linkage: EE_ExceptionHandler is declared inside ee_debug.h's extern "C"
// block, so the installed function has to match it.
extern "C" int Ps2CrashHandler(EE_RegFrame* theFrame)
{
	// A fault inside the handler must not recurse. The background colour is a
	// single store to a GS register and needs nothing else to be working, so it
	// stays as the last signal available.
	if (sInCrashHandler)
	{
		DEBUG_BGCOLOR(0x000000ff);
		Ps2CrashHalt();
	}
	sInCrashHandler = 1;

	DEBUG_BGCOLOR(0x00000060);
	if (theFrame != NULL)
		Ps2CrashCapture(theFrame);

	Ps2CrashShowOnScreen();
	Ps2CrashWriteLog();

	// Returning would resume the faulting instruction and fault again. Hold the
	// screen instead so it can be read and photographed; the mixer thread keeps
	// running, which is the same symptom this build already shows.
	Ps2CrashHalt();
	return 0;
}

void Ps2CrashHandlerInstall(void)
{
	memset(&sCrashRecord, 0, sizeof(sCrashRecord));

	// Bitmask of exception levels to take over. Level 1 carries the memory
	// faults; level 2 (reset, NMI, performance counter, debug) is left to the
	// kernel.
	if (ee_dbg_install(1) < 0)
		return;

	static const int kFatalCauses[] =
	{
		PS2_EXC_MOD, PS2_EXC_TLBL, PS2_EXC_TLBS, PS2_EXC_ADEL, PS2_EXC_ADES,
		PS2_EXC_IBE, PS2_EXC_DBE, PS2_EXC_RI, PS2_EXC_CPU
	};

	// Only the causes that mean the program is already dead are claimed, so
	// syscalls, traps and interrupts keep their kernel paths untouched.
	for (unsigned int i = 0; i < sizeof(kFatalCauses) / sizeof(kFatalCauses[0]); ++i)
		ee_dbg_set_level1_handler(kFatalCauses[i], Ps2CrashHandler);
}

#endif
