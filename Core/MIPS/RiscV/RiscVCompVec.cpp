// Copyright (c) 2023- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include "Core/MemMap.h"
#include "Core/MIPS/RiscV/RiscVJit.h"
#include "Core/MIPS/RiscV/RiscVRegCache.h"

// This file contains compilation for vector instructions.
//
// All functions should have CONDITIONAL_DISABLE, so we can narrow things down to a file quickly.
// Currently known non working ones should have DISABLE.  No flags because that's in IR already.

// #define CONDITIONAL_DISABLE { CompIR_Generic(inst); return; }
#define CONDITIONAL_DISABLE {}
#define DISABLE { CompIR_Generic(inst); return; }
#define INVALIDOP { _assert_msg_(false, "Invalid IR inst %d", (int)inst.op); CompIR_Generic(inst); return; }

namespace MIPSComp {

using namespace RiscVGen;
using namespace RiscVJitConstants;

void RiscVJitBackend::CompIR_VecAssign(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Vec4Init:
		for (int i = 0; i < 4; ++i)
			fpr.SpillLock(inst.dest + i);
		for (int i = 0; i < 4; ++i)
			fpr.MapReg(inst.dest + i, MIPSMap::NOINIT);
		for (int i = 0; i < 4; ++i)
			fpr.ReleaseSpillLock(inst.dest + i);

		// TODO: Check if FCVT/FMV/FL is better.
		switch ((Vec4Init)inst.src1) {
		case Vec4Init::AllZERO:
			for (int i = 0; i < 4; ++i)
				FCVT(FConv::S, FConv::W, fpr.R(inst.dest + i), R_ZERO);
			break;

		case Vec4Init::AllONE:
			LI(SCRATCH1, 1.0f);
			FMV(FMv::W, FMv::X, fpr.R(inst.dest), SCRATCH1);
			for (int i = 1; i < 4; ++i)
				FMV(32, fpr.R(inst.dest + i), fpr.R(inst.dest));
			break;

		case Vec4Init::AllMinusONE:
			LI(SCRATCH1, -1.0f);
			FMV(FMv::W, FMv::X, fpr.R(inst.dest), SCRATCH1);
			for (int i = 1; i < 4; ++i)
				FMV(32, fpr.R(inst.dest + i), fpr.R(inst.dest));
			break;

		case Vec4Init::Set_1000:
			LI(SCRATCH1, 1.0f);
			for (int i = 0; i < 4; ++i) {
				if (i == 0)
					FMV(FMv::W, FMv::X, fpr.R(inst.dest + i), SCRATCH1);
				else
					FCVT(FConv::S, FConv::W, fpr.R(inst.dest + i), R_ZERO);
			}
			break;

		case Vec4Init::Set_0100:
			LI(SCRATCH1, 1.0f);
			for (int i = 0; i < 4; ++i) {
				if (i == 1)
					FMV(FMv::W, FMv::X, fpr.R(inst.dest + i), SCRATCH1);
				else
					FCVT(FConv::S, FConv::W, fpr.R(inst.dest + i), R_ZERO);
			}
			break;

		case Vec4Init::Set_0010:
			LI(SCRATCH1, 1.0f);
			for (int i = 0; i < 4; ++i) {
				if (i == 2)
					FMV(FMv::W, FMv::X, fpr.R(inst.dest + i), SCRATCH1);
				else
					FCVT(FConv::S, FConv::W, fpr.R(inst.dest + i), R_ZERO);
			}
			break;

		case Vec4Init::Set_0001:
			LI(SCRATCH1, 1.0f);
			for (int i = 0; i < 4; ++i) {
				if (i == 3)
					FMV(FMv::W, FMv::X, fpr.R(inst.dest + i), SCRATCH1);
				else
					FCVT(FConv::S, FConv::W, fpr.R(inst.dest + i), R_ZERO);
			}
			break;
		}
		break;

	case IROp::Vec4Shuffle:
		if (inst.dest == inst.src1) {
			RiscVReg tempReg = fpr.Map4DirtyInTemp(inst.dest, inst.src1);

			// Try to find the least swaps needed to move in place, never worse than 6 FMVs.
			// Would be better with a vmerge and vector regs.
			int state[4]{ 0, 1, 2, 3 };
			int goal[4]{ (inst.src2 >> 0) & 3, (inst.src2 >> 2) & 3, (inst.src2 >> 4) & 3, (inst.src2 >> 6) & 3 };

			static constexpr int NOT_FOUND = 4;
			auto findIndex = [](int *arr, int val, int start = 0) {
				return (int)(std::find(arr + start, arr + 4, val) - arr);
			};
			auto moveChained = [&](const std::vector<int> &lanes, bool rotate) {
				int firstState = state[lanes.front()];
				if (rotate)
					FMV(32, tempReg, fpr.R(inst.dest + lanes.front()));
				for (size_t i = 1; i < lanes.size(); ++i) {
					FMV(32, fpr.R(inst.dest + lanes[i - 1]), fpr.R(inst.dest + lanes[i]));
					state[lanes[i - 1]] = state[lanes[i]];
				}
				if (rotate) {
					FMV(32, fpr.R(inst.dest + lanes.back()), tempReg);
					state[lanes.back()] = firstState;
				}
			};

			for (int i = 0; i < 4; ++i) {
				// Overlap, so if they match, nothing to do.
				if (goal[i] == state[i])
					continue;

				int neededBy = findIndex(goal, state[i], i + 1);
				int foundIn = findIndex(state, goal[i], 0);
				_assert_(foundIn != NOT_FOUND);

				if (neededBy == NOT_FOUND || neededBy == foundIn) {
					moveChained({ i, foundIn }, neededBy == foundIn);
					continue;
				}

				// Maybe we can avoid a swap and move the next thing into place.
				int neededByDepth2 = findIndex(goal, state[neededBy], i + 1);
				if (neededByDepth2 == NOT_FOUND || neededByDepth2 == foundIn) {
					moveChained({ neededBy, i, foundIn }, neededByDepth2 == foundIn);
					continue;
				}

				// Since we only have 4 items, this is as deep as the chain could go.
				int neededByDepth3 = findIndex(goal, state[neededByDepth2], i + 1);
				moveChained({ neededByDepth2, neededBy, i, foundIn }, neededByDepth3 == foundIn);
			}
		} else {
			fpr.Map4DirtyIn(inst.dest, inst.src1);
			for (int i = 0; i < 4; ++i) {
				int lane = (inst.src2 >> (i * 2)) & 3;
				FMV(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + lane));
			}
		}
		break;

	case IROp::Vec4Blend:
		fpr.Map4DirtyInIn(inst.dest, inst.src1, inst.src2);
		for (int i = 0; i < 4; ++i) {
			int which = (inst.constant >> i) & 1;
			FMV(32, fpr.R(inst.dest + i), fpr.R((which ? inst.src2 : inst.src1) + i));
		}
		break;

	case IROp::Vec4Mov:
		fpr.Map4DirtyIn(inst.dest, inst.src1);
		for (int i = 0; i < 4; ++i)
			FMV(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + i));
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJitBackend::CompIR_VecArith(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Vec4Add:
		fpr.Map4DirtyInIn(inst.dest, inst.src1, inst.src2);
		for (int i = 0; i < 4; ++i)
			FADD(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + i), fpr.R(inst.src2 + i));
		break;

	case IROp::Vec4Sub:
		fpr.Map4DirtyInIn(inst.dest, inst.src1, inst.src2);
		for (int i = 0; i < 4; ++i)
			FSUB(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + i), fpr.R(inst.src2 + i));
		break;

	case IROp::Vec4Mul:
		fpr.Map4DirtyInIn(inst.dest, inst.src1, inst.src2);
		for (int i = 0; i < 4; ++i)
			FMUL(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + i), fpr.R(inst.src2 + i));
		break;

	case IROp::Vec4Div:
		fpr.Map4DirtyInIn(inst.dest, inst.src1, inst.src2);
		for (int i = 0; i < 4; ++i)
			FDIV(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + i), fpr.R(inst.src2 + i));
		break;

	case IROp::Vec4Scale:
		fpr.SpillLock(inst.src2);
		fpr.MapReg(inst.src2);
		fpr.Map4DirtyIn(inst.dest, inst.src1);
		fpr.ReleaseSpillLock(inst.src2);
		for (int i = 0; i < 4; ++i)
			FMUL(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + i), fpr.R(inst.src2));
		break;

	case IROp::Vec4Neg:
		fpr.Map4DirtyIn(inst.dest, inst.src1);
		for (int i = 0; i < 4; ++i)
			FNEG(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + i));
		break;

	case IROp::Vec4Abs:
		fpr.Map4DirtyIn(inst.dest, inst.src1);
		for (int i = 0; i < 4; ++i)
			FABS(32, fpr.R(inst.dest + i), fpr.R(inst.src1 + i));
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJitBackend::CompIR_VecHoriz(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Vec4Dot:
		// TODO: Maybe some option to call the slow accurate mode?
		fpr.SpillLock(inst.dest);
		for (int i = 0; i < 4; ++i) {
			fpr.SpillLock(inst.src1 + i);
			fpr.SpillLock(inst.src2 + i);
		}
		for (int i = 0; i < 4; ++i) {
			fpr.MapReg(inst.src1 + i);
			fpr.MapReg(inst.src2 + i);
		}
		fpr.MapReg(inst.dest, MIPSMap::NOINIT);
		for (int i = 0; i < 4; ++i) {
			fpr.ReleaseSpillLock(inst.src1 + i);
			fpr.ReleaseSpillLock(inst.src2 + i);
		}
		fpr.ReleaseSpillLock(inst.dest);

		if ((inst.dest < inst.src1 + 4 && inst.dest >= inst.src1) || (inst.dest < inst.src2 + 4 && inst.dest >= inst.src2)) {
			// This means inst.dest overlaps one of src1 or src2.  We have to do that one first.
			// Technically this may impact -0.0 and such, but dots accurately need to be aligned anyway.
			for (int i = 0; i < 4; ++i) {
				if (inst.dest == inst.src1 + i || inst.dest == inst.src2 + i)
					FMUL(32, fpr.R(inst.dest), fpr.R(inst.src1 + i), fpr.R(inst.src2 + i));
			}
			for (int i = 0; i < 4; ++i) {
				if (inst.dest != inst.src1 + i && inst.dest != inst.src2 + i)
					FMADD(32, fpr.R(inst.dest), fpr.R(inst.src1 + i), fpr.R(inst.src2 + i), fpr.R(inst.dest));
			}
		} else {
			FMUL(32, fpr.R(inst.dest), fpr.R(inst.src1), fpr.R(inst.src2));
			for (int i = 1; i < 4; ++i)
				FMADD(32, fpr.R(inst.dest), fpr.R(inst.src1 + i), fpr.R(inst.src2 + i), fpr.R(inst.dest));
		}
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJitBackend::CompIR_VecPack(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Vec2Unpack16To31:
	case IROp::Vec2Unpack16To32:
	case IROp::Vec4Pack32To8:
	case IROp::Vec2Pack31To16:
		CompIR_Generic(inst);
		break;

	case IROp::Vec4Unpack8To32:
		fpr.SpillLock(inst.src1);
		for (int i = 0; i < 4; ++i)
			fpr.SpillLock(inst.dest + i);
		fpr.MapReg(inst.src1);
		for (int i = 0; i < 4; ++i)
			fpr.MapReg(inst.dest + i, MIPSMap::NOINIT);
		fpr.ReleaseSpillLocksAndDiscardTemps();

		FMV(FMv::X, FMv::W, SCRATCH2, fpr.R(inst.src1));
		for (int i = 0; i < 4; ++i) {
			// Mask using walls.
			if (i != 0) {
				SRLI(SCRATCH1, SCRATCH2, i * 8);
				SLLI(SCRATCH1, SCRATCH1, 24);
			} else {
				SLLI(SCRATCH1, SCRATCH2, 24);
			}
			FMV(FMv::W, FMv::X, fpr.R(inst.dest + i), SCRATCH1);
		}
		break;

	case IROp::Vec4DuplicateUpperBitsAndShift1:
		fpr.Map4DirtyIn(inst.dest, inst.src1);
		for (int i = 0; i < 4; i++) {
			FMV(FMv::X, FMv::W, SCRATCH1, fpr.R(inst.src1 + i));
			SRLIW(SCRATCH2, SCRATCH1, 8);
			OR(SCRATCH1, SCRATCH1, SCRATCH2);
			SRLIW(SCRATCH2, SCRATCH1, 16);
			OR(SCRATCH1, SCRATCH1, SCRATCH2);
			SRLIW(SCRATCH1, SCRATCH1, 1);
			FMV(FMv::W, FMv::X, fpr.R(inst.dest + i), SCRATCH1);
		}
		break;

	case IROp::Vec4Pack31To8:
		fpr.SpillLock(inst.dest);
		for (int i = 0; i < 4; ++i) {
			fpr.SpillLock(inst.src1 + i);
			fpr.MapReg(inst.src1 + i);
		}
		fpr.MapReg(inst.dest, MIPSMap::NOINIT);
		fpr.ReleaseSpillLocksAndDiscardTemps();

		for (int i = 0; i < 4; ++i) {
			FMV(FMv::X, FMv::W, SCRATCH1, fpr.R(inst.src1 + i));
			SRLI(SCRATCH1, SCRATCH1, 23);
			if (i == 0) {
				ANDI(SCRATCH2, SCRATCH1, 0xFF);
			} else {
				ANDI(SCRATCH1, SCRATCH1, 0xFF);
				SLLI(SCRATCH1, SCRATCH1, 8 * i);
				OR(SCRATCH2, SCRATCH2, SCRATCH1);
			}
		}

		FMV(FMv::W, FMv::X, fpr.R(inst.dest), SCRATCH2);
		break;

	case IROp::Vec2Pack32To16:
		fpr.MapDirtyInIn(inst.dest, inst.src1, inst.src1 + 1);
		FMV(FMv::X, FMv::W, SCRATCH1, fpr.R(inst.src1));
		FMV(FMv::X, FMv::W, SCRATCH2, fpr.R(inst.src1 + 1));
		// Keep in mind, this was sign-extended, so we have to zero the upper.
		SLLI(SCRATCH1, SCRATCH1, XLEN - 32);
		// Now we just set (SCRATCH2 & 0xFFFF0000) | SCRATCH1.
		SRLI(SCRATCH1, SCRATCH1, XLEN - 16);
		// Use a wall to mask.  We can ignore the upper 32 here.
		SRLI(SCRATCH2, SCRATCH2, 16);
		SLLI(SCRATCH2, SCRATCH2, 16);
		OR(SCRATCH1, SCRATCH1, SCRATCH2);
		// Okay, to the floating point register.
		FMV(FMv::W, FMv::X, fpr.R(inst.dest), SCRATCH1);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJitBackend::CompIR_VecClamp(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Vec4ClampToZero:
		fpr.Map4DirtyIn(inst.dest, inst.src1);
		for (int i = 0; i < 4; i++) {
			FMV(FMv::X, FMv::W, SCRATCH1, fpr.R(inst.src1 + i));
			SRAIW(SCRATCH2, SCRATCH1, 31);
			if (cpu_info.RiscV_Zbb) {
				ANDN(SCRATCH1, SCRATCH1, SCRATCH2);
			} else {
				NOT(SCRATCH2, SCRATCH2);
				AND(SCRATCH1, SCRATCH1, SCRATCH2);
			}
			FMV(FMv::W, FMv::X, fpr.R(inst.dest + i), SCRATCH1);
		}
		break;

	case IROp::Vec2ClampToZero:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

} // namespace MIPSComp
