/*
 * Copyright (C) 2019-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(WEBASSEMBLY_BBQJIT)
#if USE(JSVALUE32_64)

#include "WasmBBQJIT.h"
#include "WasmCallingConvention.h"
#include "WasmCompilationContext.h"
#include "WasmFunctionParser.h"
#include "WasmLimits.h"

namespace JSC { namespace Wasm { namespace BBQJITImpl {

template<typename IntType, bool IsMod>
void BBQJIT::emitModOrDiv(Value& lhs, Location lhsLocation, Value& rhs, Location rhsLocation, Value& result, Location)
{
    static_assert(sizeof(IntType) == 4 || sizeof(IntType) == 8);

    constexpr bool isSigned = std::is_signed<IntType>();
    constexpr bool is32 = sizeof(IntType) == 4;

    TypeKind argType, returnType;
    if constexpr (is32)
        argType = returnType = TypeKind::I32;
    else
        argType = returnType = TypeKind::I64;

    IntType (*modOrDiv)(IntType, IntType);
    if constexpr (IsMod) {
        if constexpr (is32) {
            if constexpr (isSigned)
                modOrDiv = Math::i32_rem_s;
            else
                modOrDiv = Math::i32_rem_u;
        } else {
            if constexpr (isSigned)
                modOrDiv = Math::i64_rem_s;
            else
                modOrDiv = Math::i64_rem_u;
        }
    } else {
        if constexpr (is32) {
            if constexpr (isSigned)
                modOrDiv = Math::i32_div_s;
            else
                modOrDiv = Math::i32_div_u;
        } else {
            if constexpr (isSigned)
                modOrDiv = Math::i64_div_s;
            else
                modOrDiv = Math::i64_div_u;
        }
    }

    if (lhs.isConst() || rhs.isConst()) {
        auto& constantLocation = ImmHelpers::immLocation(lhsLocation, rhsLocation);
        if constexpr (is32)
            constantLocation = Location::fromGPR(wasmScratchGPR);
        else
            constantLocation = Location::fromGPR2(wasmScratchGPR2, wasmScratchGPR);
        emitMoveConst(ImmHelpers::imm(lhs, rhs), constantLocation);
    }

    bool needsZeroCheck = [&] {
        if constexpr (is32)
            return !(rhs.isConst() && rhs.asI32());
        else
            return !(rhs.isConst() && rhs.asI64());
    }();

    if (needsZeroCheck) {
        if constexpr (is32)
            throwExceptionIf(ExceptionType::DivisionByZero, m_jit.branchTest32(ResultCondition::Zero, rhsLocation.asGPR()));
        else {
            auto loNotZero = m_jit.branchTest32(ResultCondition::NonZero, rhsLocation.asGPRlo());
            throwExceptionIf(ExceptionType::DivisionByZero, m_jit.branchTest32(ResultCondition::Zero, rhsLocation.asGPRhi()));
            loNotZero.link(&m_jit);
        }
    }

    if constexpr (!IsMod) {            
        if constexpr (isSigned) {
            bool needsOverflowCheck = [&] {
                if constexpr (is32) {
                    if (rhs.isConst() && rhs.asI32() != -1)
                        return false;
                    if (lhs.isConst() && lhs.asI32() != std::numeric_limits<int32_t>::min())
                        return false;
                } else {
                    if (rhs.isConst() && rhs.asI64() != -1)
                        return false;
                    if (lhs.isConst() && lhs.asI64() != std::numeric_limits<int64_t>::min())
                        return false;
                }

                return true;
            }();

            if (needsOverflowCheck) {
                if constexpr (is32) {
                    auto rhsIsOk = m_jit.branch32(RelationalCondition::NotEqual, rhsLocation.asGPR(), TrustedImm32(-1));
                    throwExceptionIf(ExceptionType::IntegerOverflow, m_jit.branch32(RelationalCondition::Equal, lhsLocation.asGPR(), TrustedImm32(std::numeric_limits<int32_t>::min())));
                    rhsIsOk.link(&m_jit);
                } else {
                    auto rhsLoIsOk = m_jit.branch32(RelationalCondition::NotEqual, rhsLocation.asGPRlo(), TrustedImm32(-1));
                    auto rhsHiIsOk = m_jit.branch32(RelationalCondition::NotEqual, rhsLocation.asGPRhi(), TrustedImm32(-1));

                    auto lhsLoIsOk = m_jit.branchTest32(ResultCondition::NonZero, lhsLocation.asGPRlo());
                    throwExceptionIf(ExceptionType::IntegerOverflow, m_jit.branch32(RelationalCondition::Equal, lhsLocation.asGPRhi(), TrustedImm32(std::numeric_limits<int32_t>::min())));

                    rhsLoIsOk.link(&m_jit);
                    rhsHiIsOk.link(&m_jit);
                    lhsLoIsOk.link(&m_jit);
                }
            }
        }
    }

    auto lhsArg = Value::pinned(argType, lhsLocation);
    auto rhsArg = Value::pinned(argType, rhsLocation);
    consume(result);
    emitCCall(modOrDiv, Vector<Value> { lhsArg, rhsArg }, result);
}

template<size_t N, typename OverflowHandler>
void BBQJIT::emitShuffleMove(Vector<Value, N, OverflowHandler>& srcVector, Vector<Location, N, OverflowHandler>& dstVector, Vector<ShuffleStatus, N, OverflowHandler>& statusVector, unsigned index)
{
    Location srcLocation = locationOf(srcVector[index]);
    Location dst = dstVector[index];
    if (srcLocation == dst)
        return; // Easily eliminate redundant moves here.

    statusVector[index] = ShuffleStatus::BeingMoved;
    for (unsigned i = 0; i < srcVector.size(); i ++) {
        // This check should handle constants too - constants always have location None, and no
        // dst should ever be a constant. But we assume that's asserted in the caller.
        auto aliasesDst = [&] (Location loc) -> bool {
            if (loc.isGPR()) {
                if (dst.isGPR2())
                    return loc.asGPR() == dst.asGPRhi() || loc.asGPR() == dst.asGPRlo();
            } else if (loc.isGPR2()) {
                if (dst.isGPR())
                    return loc.asGPRhi() == dst.asGPR() || loc.asGPRlo() == dst.asGPR();
                if (dst.isGPR2()) {
                    return loc.asGPRhi() == dst.asGPRhi() || loc.asGPRhi() == dst.asGPRlo()
                        || loc.asGPRlo() == dst.asGPRhi() || loc.asGPRlo() == dst.asGPRlo();
                }
            }
            return loc == dst;
        };
        if (aliasesDst(locationOf(srcVector[i]))) {
            switch (statusVector[i]) {
            case ShuffleStatus::ToMove:
                emitShuffleMove(srcVector, dstVector, statusVector, i);
                break;
            case ShuffleStatus::BeingMoved: {
                Location temp;
                if (srcVector[i].isFloat())
                    temp = Location::fromFPR(wasmScratchFPR);
                else if (typeNeedsGPR2(srcVector[i].type()))
                    temp = Location::fromGPR2(wasmScratchGPR2, wasmScratchGPR);
                else
                    temp = Location::fromGPR(wasmScratchGPR);
                emitMove(srcVector[i], temp);
                srcVector[i] = Value::pinned(srcVector[i].type(), temp);
                break;
            }
            case ShuffleStatus::Moved:
                break;
            }
        }
    }
    emitMove(srcVector[index], dst);
    statusVector[index] = ShuffleStatus::Moved;
}


} } } // namespace JSC::Wasm::BBQJITImpl

#endif // USE(JSVALUE32_64)
#endif // ENABLE(WEBASSEMBLY_OMGJIT)
