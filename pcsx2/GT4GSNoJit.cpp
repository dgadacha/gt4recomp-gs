// SPDX-FileCopyrightText: 2026 GT4Recomp contributors (based on pcsx2/arm64/RecStubs.cpp, 2002-2026 PCSX2 Dev Team)
// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp fork: symbols for the JIT-free gt4-gs.exe (CMake option GT4GS_ONLY, macro GT4GS_NO_JIT).
//
// A GT4GS_ONLY build compiles none of the EE/IOP recompilers, microVU, the VIF unpack recompilers, the x86 emitter or
// the GS software JIT. The interpreters and the save state code still name a few recompiler entry points. They are
// defined here. None of them is reached by gt4-gs.exe (the stream and dump replayers never execute guest code and
// never save a full VM state); the fastmem backpatch stops the process if it is ever called.

#include "MTVU.h"
#include "SaveState.h"
#include "vtlb.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <array>

#ifndef GT4GS_NO_JIT
#error "GT4GSNoJit.cpp is only built with GT4GS_ONLY"
#endif

void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr, u32 gpr_bitmask,
	u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr)
{
	pxFailRel("GT4GS: no recompiler in the JIT-free build (fastmem backpatch)");
}

bool SaveStateBase::vuJITFreeze()
{
	if (IsSaving())
		vu1Thread.WaitVU();

	Console.Warning("GT4GS: JIT-free build, microVU state not saved (empty block written)");

	// Two empty microRegInfo-sized blocks, as on ARM64 (pcsx2/arm64/RecStubs.cpp).
	std::array<u8, 96> empty_data{};
	Freeze(empty_data);
	Freeze(empty_data);
	return IsOkay();
}
