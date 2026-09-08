#pragma once

#ifdef PS2_PLATFORM

// EE exception reporting.
//
// A bad pointer on the EE raises a level 1 exception and kills the faulting
// thread outright: no C++ unwinding, so neither the operator-new rescue nor the
// catch-all in main ever runs, and the only symptom is a frozen picture with the
// mixer thread still playing. PCSX2 tolerates a good deal of what the real TLB
// rejects, so these are also the failures that do not reproduce in the emulator.
//
// This installs handlers for the fatal causes and reports the faulting PC, which
// the workflow already documented in CMakeLists resolves to a source line:
//   mips64r5900el-ps2-elf-addr2line -f -C -e pvz_ps2_syms.elf 0x<epc>
//
// Interrupts and syscalls are deliberately left alone; only the causes that mean
// the program is already dead are claimed.
void Ps2CrashHandlerInstall(void);

#endif
