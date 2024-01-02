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

#include "config.h"
#include "WasmBBQJIT.h"

#if ENABLE(WEBASSEMBLY_BBQJIT)

#include "B3Common.h"
#include "B3ValueRep.h"
#include "BinarySwitch.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "CPU.h"
#include "CompilerTimingScope.h"
#include "GPRInfo.h"
#include "JSCast.h"
#include "JSWebAssemblyArray.h"
#include "JSWebAssemblyException.h"
#include "JSWebAssemblyStruct.h"
#include "MacroAssembler.h"
#include "RegisterSet.h"
#include "WasmB3IRGenerator.h"
#include "WasmBBQDisassembler.h"
#include "WasmCallingConvention.h"
#include "WasmCompilationMode.h"
#include "WasmFormat.h"
#include "WasmFunctionParser.h"
#include "WasmIRGeneratorHelpers.h"
#include "WasmInstance.h"
#include "WasmMemoryInformation.h"
#include "WasmModule.h"
#include "WasmModuleInformation.h"
#include "WasmOperations.h"
#include "WasmOps.h"
#include "WasmThunks.h"
#include "WasmTypeDefinition.h"
#include <bit>
#include <wtf/Assertions.h>
#include <wtf/Compiler.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/PlatformRegisters.h>
#include <wtf/SmallSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace Wasm {

namespace BBQJITImpl {

        Location Location::none()
        {
            return Location();
        }

        Location Location::fromStack(int32_t stackOffset)
        {
            Location loc;
            loc.m_kind = Stack;
            loc.m_offset = stackOffset;
            return loc;
        }

        Location Location::fromStackArgument(int32_t stackOffset)
        {
            Location loc;
            loc.m_kind = StackArgument;
            loc.m_offset = stackOffset;
            return loc;
        }

        Location Location::fromGPR(GPRReg gpr)
        {
            Location loc;
            loc.m_kind = Gpr;
            loc.m_gpr = gpr;
            return loc;
        }

#if USE(JSVALUE32_64)
        Location Location::fromGPR2(GPRReg hi, GPRReg lo)
        {
            ASSERT(hi != lo);

            // we canonicalize hi and lo to avoid a dreadful situation of having two
            // GPR2 locations (x, y) and (y, x) which we can't easliy generate a move between

            Location loc;
            loc.m_kind = Gpr2;
            if (hi > lo) {
                loc.m_gprhi = hi;
                loc.m_gprlo = lo;
            } else {
                loc.m_gprhi = lo;
                loc.m_gprlo = hi;
            }
            return loc;
        }
#endif

        Location Location::fromFPR(FPRReg fpr)
        {
            Location loc;
            loc.m_kind = Fpr;
            loc.m_fpr = fpr;
            return loc;
        }

        Location Location::fromGlobal(int32_t globalOffset)
        {
            Location loc;
            loc.m_kind = Global;
            loc.m_offset = globalOffset;
            return loc;
        }

        Location Location::fromArgumentLocation(ArgumentLocation argLocation, TypeKind type)
        {
            switch (argLocation.location.kind()) {
            case ValueLocation::Kind::GPRRegister:
#if USE(JSVALUE64)
                UNUSED_PARAM(type);
                return Location::fromGPR(argLocation.location.jsr().gpr());
#else
                if (typeNeedsGPR2(type))
                    return Location::fromGPR2(argLocation.location.jsr().tagGPR(), argLocation.location.jsr().payloadGPR());
                return Location::fromGPR(argLocation.location.jsr().payloadGPR());
#endif
            case ValueLocation::Kind::FPRRegister:
                return Location::fromFPR(argLocation.location.fpr());
            case ValueLocation::Kind::StackArgument:
                return Location::fromStackArgument(argLocation.location.offsetFromSP());
            case ValueLocation::Kind::Stack:
                return Location::fromStack(argLocation.location.offsetFromFP());
            }
            RELEASE_ASSERT_NOT_REACHED();
        }

        bool Location::isNone() const
        {
            return m_kind == None;
        }

        bool Location::isGPR() const
        {
            return m_kind == Gpr;
        }

#if USE(JSVALUE32_64)
        bool Location::isGPR2() const
        {
            return m_kind == Gpr2;
        }
#endif

        bool Location::isFPR() const
        {
            return m_kind == Fpr;
        }

#if USE(JSVALUE64)
        bool Location::isRegister() const
        {
            return isGPR() || isFPR();
        }
#else
        bool Location::isRegister() const
        {
            return isGPR() || isGPR2() || isFPR();
        }
#endif

        bool Location::isStack() const
        {
            return m_kind == Stack;
        }

        bool Location::isStackArgument() const
        {
            return m_kind == StackArgument;
        }

        bool Location::isGlobal() const
        {
            return m_kind == Global;
        }

        bool Location::isMemory() const
        {
            return isStack() || isStackArgument() || isGlobal();
        }

        int32_t Location::asStackOffset() const
        {
            ASSERT(isStack());
            return m_offset;
        }

        Address Location::asStackAddress() const
        {
            ASSERT(isStack());
            return Address(GPRInfo::callFrameRegister, asStackOffset());
        }

        int32_t Location::asGlobalOffset() const
        {
            ASSERT(isGlobal());
            return m_offset;
        }

        Address Location::asGlobalAddress() const
        {
            ASSERT(isGlobal());
            return Address(GPRInfo::wasmContextInstancePointer, asGlobalOffset());
        }

        int32_t Location::asStackArgumentOffset() const
        {
            ASSERT(isStackArgument());
            return m_offset;
        }

        Address Location::asStackArgumentAddress() const
        {
            ASSERT(isStackArgument());
            return Address(MacroAssembler::stackPointerRegister, asStackArgumentOffset());
        }

        Address Location::asAddress() const
        {
            switch (m_kind) {
            case Stack:
                return asStackAddress();
            case Global:
                return asGlobalAddress();
            case StackArgument:
                return asStackArgumentAddress();
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }

        GPRReg Location::asGPR() const
        {
            ASSERT(isGPR());
            return m_gpr;
        }

        FPRReg Location::asFPR() const
        {
            ASSERT(isFPR());
            return m_fpr;
        }

#if USE(JSVALUE32_64)
        GPRReg Location::asGPRlo() const
        {
            ASSERT(isGPR2());
            return m_gprlo;
        }

        GPRReg Location::asGPRhi() const
        {
            ASSERT(isGPR2());
            return m_gprhi;
        }
#endif

        void Location::dump(PrintStream& out) const
        {
            switch (m_kind) {
            case None:
                out.print("None");
                break;
            case Gpr:
                out.print("GPR(", MacroAssembler::gprName(m_gpr), ")");
                break;
            case Fpr:
                out.print("FPR(", MacroAssembler::fprName(m_fpr), ")");
                break;
            case Stack:
                out.print("Stack(", m_offset, ")");
                break;
            case Global:
                out.print("Global(", m_offset, ")");
                break;
            case StackArgument:
                out.print("StackArgument(", m_offset, ")");
                break;
#if USE(JSVALUE32_64)
            case Gpr2:
                out.print("GPR2(", m_gprhi, ",", m_gprlo, ")");
                break;
#endif
            }
        }

        bool Location::operator==(Location other) const
        {
            if (m_kind != other.m_kind)
                return false;
            switch (m_kind) {
            case Gpr:
                return m_gpr == other.m_gpr;
#if USE(JSVALUE32_64)
            case Gpr2:
                return m_gprlo == other.m_gprlo && m_gprhi == other.m_gprhi;
#endif
            case Fpr:
                return m_fpr == other.m_fpr;
            case Stack:
            case StackArgument:
            case Global:
                return m_offset == other.m_offset;
            case None:
                return true;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }

        Location::Kind Location::kind() const
        {
            return Kind(m_kind);
        }

    bool BBQJIT::isValidValueTypeKind(TypeKind kind)
    {
        switch (kind) {
        case TypeKind::I64:
        case TypeKind::I32:
        case TypeKind::F64:
        case TypeKind::F32:
        case TypeKind::V128:
            return true;
        default:
            return false;
        }
    }

    TypeKind BBQJIT::pointerType() { return is64Bit() ? TypeKind::I64 : TypeKind::I32; }

    bool BBQJIT::isFloatingPointType(TypeKind type)
    {
        return type == TypeKind::F32 || type == TypeKind::F64 || type == TypeKind::V128;
    }

#if USE(JSVALUE32_64)
    bool BBQJIT::typeNeedsGPR2(TypeKind type)
    {
        return toValueKind(type) == TypeKind::I64;
    }
#endif

#if USE(JSVALUE64)
    ALWAYS_INLINE uint32_t BBQJIT::sizeOfType(TypeKind type)
    {
        switch (type) {
        case TypeKind::I32:
        case TypeKind::F32:
        case TypeKind::I31ref:
            return 4;
        case TypeKind::I64:
        case TypeKind::F64:
            return 8;
        case TypeKind::V128:
            return 16;
        case TypeKind::Func:
        case TypeKind::Funcref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Rec:
        case TypeKind::Sub:
        case TypeKind::Subfinal:
        case TypeKind::Struct:
        case TypeKind::Structref:
        case TypeKind::Externref:
        case TypeKind::Array:
        case TypeKind::Arrayref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            return sizeof(EncodedJSValue);
        case TypeKind::Void:
            return 0;
        }
        return 0;
    }
#else
    ALWAYS_INLINE uint32_t BBQJIT::sizeOfType(TypeKind type)
    {
        switch (type) {
        case TypeKind::I32:
        case TypeKind::F32:
        case TypeKind::I31ref:
            return sizeof(EncodedJSValue);
        case TypeKind::I64:
        case TypeKind::F64:
            return 8;
        case TypeKind::V128:
            return 16;
        case TypeKind::Func:
        case TypeKind::Funcref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Rec:
        case TypeKind::Sub:
        case TypeKind::Subfinal:
        case TypeKind::Struct:
        case TypeKind::Structref:
        case TypeKind::Externref:
        case TypeKind::Array:
        case TypeKind::Arrayref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            return sizeof(EncodedJSValue);
        case TypeKind::Void:
            return 0;
        }
        return 0;
    }
#endif

    TypeKind BBQJIT::toValueKind(TypeKind kind)
    {
        switch (kind) {
        case TypeKind::I32:
        case TypeKind::F32:
        case TypeKind::I64:
        case TypeKind::F64:
        case TypeKind::V128:
            return kind;
        case TypeKind::Func:
        case TypeKind::I31ref:
        case TypeKind::Funcref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Rec:
        case TypeKind::Sub:
        case TypeKind::Subfinal:
        case TypeKind::Struct:
        case TypeKind::Structref:
        case TypeKind::Externref:
        case TypeKind::Array:
        case TypeKind::Arrayref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            return TypeKind::I64;
        case TypeKind::Void:
            RELEASE_ASSERT_NOT_REACHED();
            return kind;
        }
        return kind;
    }

        ALWAYS_INLINE bool Value::isNone() const
        {
            return m_kind == None;
        }

        ALWAYS_INLINE bool Value::isTemp() const
        {
            return m_kind == Temp;
        }

        ALWAYS_INLINE bool Value::isLocal() const
        {
            return m_kind == Local;
        }

        ALWAYS_INLINE bool Value::isPinned() const
        {
            return m_kind == Pinned;
        }

        ALWAYS_INLINE Value::Kind Value::kind() const
        {
            return m_kind;
        }

        ALWAYS_INLINE int32_t Value::asI32() const
        {
            ASSERT(m_kind == Const);
            return m_i32;
        }

#if USE(JSVALUE32_64)
        ALWAYS_INLINE int32_t Value::asI64hi() const
        {
            ASSERT(m_kind == Const);
            return m_i32_pair.hi;
        }

        ALWAYS_INLINE int32_t Value::asI64lo() const
        {
            ASSERT(m_kind == Const);
            return m_i32_pair.lo;
        }
#endif

        ALWAYS_INLINE int64_t Value::asI64() const
        {
            ASSERT(m_kind == Const);
            return m_i64;
        }

        ALWAYS_INLINE float Value::asF32() const
        {
            ASSERT(m_kind == Const);
            return m_f32;
        }

        ALWAYS_INLINE double Value::asF64() const
        {
            ASSERT(m_kind == Const);
            return m_f64;
        }

        ALWAYS_INLINE EncodedJSValue Value::asRef() const
        {
            ASSERT(m_kind == Const);
            return m_ref;
        }

        ALWAYS_INLINE LocalOrTempIndex Value::asTemp() const
        {
            ASSERT(m_kind == Temp);
            return m_index;
        }

        ALWAYS_INLINE LocalOrTempIndex Value::asLocal() const
        {
            ASSERT(m_kind == Local);
            return m_index;
        }

        ALWAYS_INLINE Location Value::asPinned() const
        {
            ASSERT(m_kind == Pinned);
            return m_pinned;
        }

        ALWAYS_INLINE Value Value::fromI32(int32_t immediate)
        {
            Value val;
            val.m_kind = Const;
            val.m_type = TypeKind::I32;
            val.m_i32 = immediate;
            return val;
        }

        ALWAYS_INLINE Value Value::fromI64(int64_t immediate)
        {
            Value val;
            val.m_kind = Const;
            val.m_type = TypeKind::I64;
            val.m_i64 = immediate;
            return val;
        }

        ALWAYS_INLINE Value Value::fromF32(float immediate)
        {
            Value val;
            val.m_kind = Const;
            val.m_type = TypeKind::F32;
            val.m_f32 = immediate;
            return val;
        }

        ALWAYS_INLINE Value Value::fromF64(double immediate)
        {
            Value val;
            val.m_kind = Const;
            val.m_type = TypeKind::F64;
            val.m_f64 = immediate;
            return val;
        }

        ALWAYS_INLINE Value Value::fromRef(TypeKind refType, EncodedJSValue ref)
        {
            Value val;
            val.m_kind = Const;
            val.m_type = toValueKind(refType);
            val.m_ref = ref;
            return val;
        }

        ALWAYS_INLINE Value Value::fromTemp(TypeKind type, LocalOrTempIndex temp)
        {
            Value val;
            val.m_kind = Temp;
            val.m_type = toValueKind(type);
            val.m_index = temp;
            return val;
        }

        ALWAYS_INLINE Value Value::fromLocal(TypeKind type, LocalOrTempIndex local)
        {
            Value val;
            val.m_kind = Local;
            val.m_type = toValueKind(type);
            val.m_index = local;
            return val;
        }

        ALWAYS_INLINE Value Value::pinned(TypeKind type, Location location)
        {
            Value val;
            val.m_kind = Pinned;
            val.m_type = toValueKind(type);
            val.m_pinned = location;
            return val;
        }

        ALWAYS_INLINE uint32_t Value::size() const
        {
            return sizeOfType(m_type);
        }

        ALWAYS_INLINE bool Value::isFloat() const
        {
            return isFloatingPointType(m_type);
        }

        ALWAYS_INLINE TypeKind Value::type() const
        {
            ASSERT(isValidValueTypeKind(m_type));
            return m_type;
        }

        void Value::dump(PrintStream& out) const
        {
            switch (m_kind) {
            case Const:
                out.print("Const(");
                if (m_type == TypeKind::I32)
                    out.print(m_i32);
                else if (m_type == TypeKind::I64)
                    out.print(m_i64);
                else if (m_type == TypeKind::F32)
                    out.print(m_f32);
                else if (m_type == TypeKind::F64)
                    out.print(m_f64);
                out.print(")");
                break;
            case Local:
                out.print("Local(", m_index, ")");
                break;
            case Temp:
                out.print("Temp(", m_index, ")");
                break;
            case None:
                out.print("None");
                break;
            case Pinned:
                out.print(m_pinned);
            }
        }

        RegisterBinding RegisterBinding::fromValue(Value value)
        {
            ASSERT(value.isLocal() || value.isTemp());
            RegisterBinding binding;
            binding.m_type = value.type();
            binding.m_kind = value.isLocal() ? Local : Temp;
            binding.m_index = value.isLocal() ? value.asLocal() : value.asTemp();
            return binding;
        }

        RegisterBinding RegisterBinding::none()
        {
            return RegisterBinding();
        }

        RegisterBinding RegisterBinding::scratch()
        {
            RegisterBinding binding;
            binding.m_kind = Scratch;
            return binding;
        }

        Value RegisterBinding::toValue() const
        {
            switch (m_kind) {
            case None:
            case Scratch:
                return Value::none();
            case Local:
                return Value::fromLocal(m_type, m_index);
            case Temp:
                return Value::fromTemp(m_type, m_index);
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }

        bool RegisterBinding::isNone() const
        {
            return m_kind == None;
        }

        bool RegisterBinding::isValid() const
        {
            return m_kind != None;
        }

        bool RegisterBinding::isScratch() const
        {
            return m_kind == Scratch;
        }

        bool RegisterBinding::operator==(RegisterBinding other) const
        {
            if (m_kind != other.m_kind)
                return false;

            return m_kind == None || (m_index == other.m_index && m_type == other.m_type);
        }

        void RegisterBinding::dump(PrintStream& out) const
        {
            switch (m_kind) {
            case None:
                out.print("None");
                break;
            case Scratch:
                out.print("Scratch");
                break;
            case Local:
                out.print("Local(", m_index, ")");
                break;
            case Temp:
                out.print("Temp(", m_index, ")");
                break;
            }
        }

        unsigned RegisterBinding::hash() const
        {
            return pairIntHash(static_cast<unsigned>(m_kind), m_index);
        }

        uint32_t RegisterBinding::encode() const
        {
            return m_uintValue;
        }

        ControlData::ControlData(BBQJIT& generator, BlockType blockType, BlockSignature signature, LocalOrTempIndex enclosedHeight)
            : m_signature(signature)
            , m_blockType(blockType)
            , m_enclosedHeight(enclosedHeight)
        {
            if (blockType == BlockType::TopLevel) {
                // Abide by calling convention instead.
                CallInformation wasmCallInfo = wasmCallingConvention().callInformationFor(*signature, CallRole::Callee);
                for (unsigned i = 0; i < signature->as<FunctionSignature>()->argumentCount(); ++i)
                    m_argumentLocations.append(Location::fromArgumentLocation(wasmCallInfo.params[i], signature->as<FunctionSignature>()->argumentType(i).kind));
                for (unsigned i = 0; i < signature->as<FunctionSignature>()->returnCount(); ++i)
                    m_resultLocations.append(Location::fromArgumentLocation(wasmCallInfo.results[i], signature->as<FunctionSignature>()->returnType(i).kind));
                return;
            }

            const auto& functionSignature = signature->as<FunctionSignature>();
            if (!isAnyCatch(*this)) {
                auto gprSetCopy = generator.m_validGPRs;
                auto fprSetCopy = generator.m_validFPRs;
                // We intentionally exclude GPRInfo::nonPreservedNonArgumentGPR1 from argument locations. See explanation in addIf and emitIndirectCall.
                gprSetCopy.remove(GPRInfo::nonPreservedNonArgumentGPR1);
                for (unsigned i = 0; i < functionSignature->argumentCount(); ++i)
                    m_argumentLocations.append(allocateArgumentOrResult(generator, functionSignature->argumentType(i).kind, i, gprSetCopy, fprSetCopy));
            }

            auto gprSetCopy = generator.m_validGPRs;
            auto fprSetCopy = generator.m_validFPRs;
            for (unsigned i = 0; i < functionSignature->returnCount(); ++i)
                m_resultLocations.append(allocateArgumentOrResult(generator, functionSignature->returnType(i).kind, i, gprSetCopy, fprSetCopy));
        }

        // This function is intentionally not using implicitSlots since arguments and results should not include implicit slot.
#if USE(JSVALUE64)
        Location ControlData::allocateArgumentOrResult(BBQJIT& generator, TypeKind type, unsigned i, RegisterSet& remainingGPRs, RegisterSet& remainingFPRs)
        {
            switch (type) {
            case TypeKind::V128:
            case TypeKind::F32:
            case TypeKind::F64: {
                if (remainingFPRs.isEmpty())
                    return generator.canonicalSlot(Value::fromTemp(type, this->enclosedHeight() + i));
                auto reg = *remainingFPRs.begin();
                remainingFPRs.remove(reg);
                return Location::fromFPR(reg.fpr());
            }
            default:
                if (remainingGPRs.isEmpty())
                    return generator.canonicalSlot(Value::fromTemp(type, this->enclosedHeight() + i));
                auto reg = *remainingGPRs.begin();
                remainingGPRs.remove(reg);
                return Location::fromGPR(reg.gpr());
            }
        }
#else
        Location ControlData::allocateArgumentOrResult(BBQJIT& generator, TypeKind type, unsigned i, RegisterSet& remainingGPRs, RegisterSet& remainingFPRs)
        {
            switch (type) {
            case TypeKind::V128:
            case TypeKind::F32:
            case TypeKind::F64: {
                if (remainingFPRs.isEmpty())
                    return generator.canonicalSlot(Value::fromTemp(type, this->enclosedHeight() + i));
                auto reg = *remainingFPRs.begin();
                remainingFPRs.remove(reg);
                return Location::fromFPR(reg.fpr());
            }
            default:
                if (typeNeedsGPR2(type)) {
                    auto next = remainingGPRs.begin();
                    if (next != remainingGPRs.end()) {
                        auto lo = (*next);
                        if (++next != remainingGPRs.end()) {
                            auto hi = (*next);
                            remainingGPRs.remove(lo);
                            remainingGPRs.remove(hi);
                            return Location::fromGPR2(hi.gpr(), lo.gpr());
                        }
                    }
                    return generator.canonicalSlot(Value::fromTemp(type, this->enclosedHeight() + i));
                }
                if (remainingGPRs.isEmpty())
                    return generator.canonicalSlot(Value::fromTemp(type, this->enclosedHeight() + i));
                auto reg = *remainingGPRs.begin();
                remainingGPRs.remove(reg);
                return Location::fromGPR(reg.gpr());
            }
        }
#endif

        void ControlData::convertIfToBlock()
        {
            ASSERT(m_blockType == BlockType::If);
            m_blockType = BlockType::Block;
        }

        void ControlData::convertLoopToBlock()
        {
            ASSERT(m_blockType == BlockType::Loop);
            m_blockType = BlockType::Block;
        }

        void ControlData::addBranch(Jump jump)
        {
            m_branchList.append(jump);
        }

        void ControlData::addLabel(Box<CCallHelpers::Label>&& label)
        {
            m_labels.append(WTFMove(label));
        }

        void ControlData::delegateJumpsTo(ControlData& delegateTarget)
        {
            delegateTarget.m_branchList.append(std::exchange(m_branchList, { }));
            delegateTarget.m_labels.appendVector(std::exchange(m_labels, { }));
        }

        void ControlData::linkJumps(MacroAssembler::AbstractMacroAssemblerType* masm)
        {
            m_branchList.link(masm);
            fillLabels(masm->label());
        }

        void ControlData::linkJumpsTo(MacroAssembler::Label label, MacroAssembler::AbstractMacroAssemblerType* masm)
        {
            m_branchList.linkTo(label, masm);
            fillLabels(label);
        }

        void ControlData::linkIfBranch(MacroAssembler::AbstractMacroAssemblerType* masm)
        {
            ASSERT(m_blockType == BlockType::If);
            if (m_ifBranch.isSet())
                m_ifBranch.link(masm);
        }

        void ControlData::dump(PrintStream& out) const
        {
            UNUSED_PARAM(out);
        }

        LocalOrTempIndex ControlData::enclosedHeight() const
        {
            return m_enclosedHeight;
        }

        unsigned ControlData::implicitSlots() const { return isAnyCatch(*this) ? 1 : 0; }

        const Vector<Location, 2>& ControlData::targetLocations() const
        {
            return blockType() == BlockType::Loop
                ? argumentLocations()
                : resultLocations();
        }

        const Vector<Location, 2>& ControlData::argumentLocations() const
        {
            return m_argumentLocations;
        }

        const Vector<Location, 2>& ControlData::resultLocations() const
        {
            return m_resultLocations;
        }

        BlockType ControlData::blockType() const { return m_blockType; }
        BlockSignature ControlData::signature() const { return m_signature; }

        FunctionArgCount ControlData::branchTargetArity() const
        {
            if (blockType() == BlockType::Loop)
                return m_signature->as<FunctionSignature>()->argumentCount();
            return m_signature->as<FunctionSignature>()->returnCount();
        }

        Type ControlData::branchTargetType(unsigned i) const
        {
            ASSERT(i < branchTargetArity());
            if (m_blockType == BlockType::Loop)
                return m_signature->as<FunctionSignature>()->argumentType(i);
            return m_signature->as<FunctionSignature>()->returnType(i);
        }

        Type ControlData::argumentType(unsigned i) const
        {
            ASSERT(i < m_signature->as<FunctionSignature>()->argumentCount());
            return m_signature->as<FunctionSignature>()->argumentType(i);
        }

        CatchKind ControlData::catchKind() const
        {
            ASSERT(m_blockType == BlockType::Catch);
            return m_catchKind;
        }

        void ControlData::setCatchKind(CatchKind catchKind)
        {
            ASSERT(m_blockType == BlockType::Catch);
            m_catchKind = catchKind;
        }

        unsigned ControlData::tryStart() const
        {
            return m_tryStart;
        }

        unsigned ControlData::tryEnd() const
        {
            return m_tryEnd;
        }

        unsigned ControlData::tryCatchDepth() const
        {
            return m_tryCatchDepth;
        }

        void ControlData::setTryInfo(unsigned tryStart, unsigned tryEnd, unsigned tryCatchDepth)
        {
            m_tryStart = tryStart;
            m_tryEnd = tryEnd;
            m_tryCatchDepth = tryCatchDepth;
        }

        void ControlData::setIfBranch(MacroAssembler::Jump branch)
        {
            ASSERT(m_blockType == BlockType::If);
            m_ifBranch = branch;
        }

        void ControlData::setLoopLabel(MacroAssembler::Label label)
        {
            ASSERT(m_blockType == BlockType::Loop);
            m_loopLabel = label;
        }

        const MacroAssembler::Label& ControlData::loopLabel() const
        {
            return m_loopLabel;
        }

        void ControlData::touch(LocalOrTempIndex local)
        {
            m_touchedLocals.add(local);
        }

        void ControlData::fillLabels(CCallHelpers::Label label)
        {
            for (auto& box : m_labels)
                *box = label;
        }

    BBQJIT::BBQJIT(CCallHelpers& jit, const TypeDefinition& signature, BBQCallee& callee, const FunctionData& function, uint32_t functionIndex, const ModuleInformation& info, Vector<UnlinkedWasmToWasmCall>& unlinkedWasmToWasmCalls, MemoryMode mode, InternalFunction* compilation, std::optional<bool> hasExceptionHandlers, unsigned loopIndexForOSREntry, TierUpCount* tierUp)
        : m_jit(jit)
        , m_callee(callee)
        , m_function(function)
        , m_functionSignature(signature.expand().as<FunctionSignature>())
        , m_functionIndex(functionIndex)
        , m_info(info)
        , m_mode(mode)
        , m_unlinkedWasmToWasmCalls(unlinkedWasmToWasmCalls)
        , m_hasExceptionHandlers(hasExceptionHandlers)
        , m_tierUp(tierUp)
        , m_loopIndexForOSREntry(loopIndexForOSREntry)
        , m_gprBindings(jit.numberOfRegisters(), RegisterBinding::none())
        , m_fprBindings(jit.numberOfFPRegisters(), RegisterBinding::none())
        , m_gprLRU(jit.numberOfRegisters())
        , m_fprLRU(jit.numberOfFPRegisters())
        , m_lastUseTimestamp(0)
        , m_compilation(compilation)
        , m_pcToCodeOriginMapBuilder(Options::useSamplingProfiler())
    {
        RegisterSetBuilder gprSetBuilder = RegisterSetBuilder::allGPRs();
        gprSetBuilder.exclude(RegisterSetBuilder::specialRegisters());
        gprSetBuilder.exclude(RegisterSetBuilder::macroClobberedGPRs());
        gprSetBuilder.exclude(RegisterSetBuilder::wasmPinnedRegisters());
        // TODO: handle callee-saved registers better.
        gprSetBuilder.exclude(RegisterSetBuilder::vmCalleeSaveRegisters());

        RegisterSetBuilder fprSetBuilder = RegisterSetBuilder::allFPRs();
        RegisterSetBuilder::macroClobberedFPRs().forEach([&](Reg reg) {
            fprSetBuilder.remove(reg);
        });
#if USE(JSVALUE32_64) && CPU(ARM_NEON)
        // Remove NEON fprs
        for (auto reg = ARMRegisters::d16; reg <= ARMRegisters::d31; reg = MacroAssembler::nextFPRegister(reg))
            fprSetBuilder.remove(reg);
#endif
        // TODO: handle callee-saved registers better.
        RegisterSetBuilder::vmCalleeSaveRegisters().forEach([&](Reg reg) {
            fprSetBuilder.remove(reg);
        });

        RegisterSetBuilder callerSaveGprs = gprSetBuilder;
        RegisterSetBuilder callerSaveFprs = fprSetBuilder;

        gprSetBuilder.remove(wasmScratchGPR);
#if USE(JSVALUE32_64)
        gprSetBuilder.remove(wasmScratchGPR2);
#endif
        fprSetBuilder.remove(wasmScratchFPR);

        m_gprSet = m_validGPRs = gprSetBuilder.buildAndValidate();
        m_fprSet = m_validFPRs = fprSetBuilder.buildAndValidate();
        m_callerSaveGPRs = callerSaveGprs.buildAndValidate();
        m_callerSaveFPRs = callerSaveFprs.buildAndValidate();
        m_callerSaves = callerSaveGprs.merge(callerSaveFprs).buildAndValidate();

        m_gprLRU.add(m_gprSet);
        m_fprLRU.add(m_fprSet);

        if ((Options::verboseBBQJITAllocation()))
            dataLogLn("BBQ\tUsing GPR set: ", m_gprSet, "\n   \tFPR set: ", m_fprSet);

        if (UNLIKELY(shouldDumpDisassemblyFor(CompilationMode::BBQMode))) {
            m_disassembler = makeUnique<BBQDisassembler>();
            m_disassembler->setStartOfCode(m_jit.label());
        }

        CallInformation callInfo = wasmCallingConvention().callInformationFor(signature.expand(), CallRole::Callee);
        ASSERT(callInfo.params.size() == m_functionSignature->argumentCount());
        for (unsigned i = 0; i < m_functionSignature->argumentCount(); i ++) {
            const Type& type = m_functionSignature->argumentType(i);
            m_localSlots.append(allocateStack(Value::fromLocal(type.kind, i)));
            m_locals.append(Location::none());
            m_localTypes.append(type.kind);

            Value parameter = Value::fromLocal(type.kind, i);
            bind(parameter, Location::fromArgumentLocation(callInfo.params[i], type.kind));
            m_arguments.append(i);
        }
        m_localStorage = m_frameSize; // All stack slots allocated so far are locals.
    }

    void BBQJIT::setParser(FunctionParser<BBQJIT>* parser)
    {
        m_parser = parser;
    }

    bool BBQJIT::addArguments(const TypeDefinition& signature)
    {
        RELEASE_ASSERT(m_arguments.size() == signature.as<FunctionSignature>()->argumentCount()); // We handle arguments in the prologue
        return true;
    }

    Value BBQJIT::addConstant(Type type, uint64_t value)
    {
        Value result;
        switch (type.kind) {
        case TypeKind::I32:
            result = Value::fromI32(value);
            LOG_INSTRUCTION("I32Const", RESULT(result));
            break;
        case TypeKind::I64:
            result = Value::fromI64(value);
            LOG_INSTRUCTION("I64Const", RESULT(result));
            break;
        case TypeKind::F32: {
            int32_t tmp = static_cast<int32_t>(value);
            result = Value::fromF32(*reinterpret_cast<float*>(&tmp));
            LOG_INSTRUCTION("F32Const", RESULT(result));
            break;
        }
        case TypeKind::F64:
            result = Value::fromF64(*reinterpret_cast<double*>(&value));
            LOG_INSTRUCTION("F64Const", RESULT(result));
            break;
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Structref:
        case TypeKind::Arrayref:
        case TypeKind::Funcref:
        case TypeKind::Externref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            result = Value::fromRef(type.kind, static_cast<EncodedJSValue>(value));
            LOG_INSTRUCTION("RefConst", makeString(type.kind), RESULT(result));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented constant type.\n");
            return Value::none();
        }
        return result;
    }

    PartialResult BBQJIT::addDrop(Value value)
    {
        LOG_INSTRUCTION("Drop", value);
        consume(value);
        return { };
    }

    PartialResult BBQJIT::addLocal(Type type, uint32_t numberOfLocals)
    {
        for (uint32_t i = 0; i < numberOfLocals; i ++) {
            uint32_t localIndex = m_locals.size();
            m_localSlots.append(allocateStack(Value::fromLocal(type.kind, localIndex)));
            m_localStorage = m_frameSize;
            m_locals.append(m_localSlots.last());
            m_localTypes.append(type.kind);
        }
        return { };
    }

#if USE(JSVALUE64)
    Value BBQJIT::instanceValue()
    {
        return Value::pinned(TypeKind::I64, Location::fromGPR(GPRInfo::wasmContextInstancePointer));
    }
#else
    Value BBQJIT::instanceValue()
    {
        return Value::pinned(TypeKind::I32, Location::fromGPR(GPRInfo::wasmContextInstancePointer));
    }
#endif

    // Tables
#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addTableGet(unsigned tableIndex, Value index, Value& result)
    {
        // FIXME: Emit this inline <https://bugs.webkit.org/show_bug.cgi?id=198506>.
        ASSERT(index.type() == TypeKind::I32);
        TypeKind returnType = m_info.tables[tableIndex].wasmType().kind;
        ASSERT(typeKindSizeInBytes(returnType) == 8);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(tableIndex),
            index
        };
        result = topValue(returnType);
        emitCCall(&operationGetWasmTableElement, arguments, result);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("TableGet", tableIndex, index, RESULT(result));

        throwExceptionIf(ExceptionType::OutOfBoundsTableAccess, m_jit.branchTest64(ResultCondition::Zero, resultLocation.asGPR()));
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addTableGet(unsigned tableIndex, Value index, Value& result)
    {
        // FIXME: Emit this inline <https://bugs.webkit.org/show_bug.cgi?id=198506>.
        ASSERT(index.type() == TypeKind::I32);
        TypeKind returnType = m_info.tables[tableIndex].wasmType().kind;
        ASSERT(typeKindSizeInBytes(returnType) == 8);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(tableIndex),
            index
        };
        result = topValue(returnType);
        emitCCall(&operationGetWasmTableElement, arguments, result);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("TableGet", tableIndex, index, RESULT(result));

        m_jit.or32(resultLocation.asGPRhi(), resultLocation.asGPRlo(), wasmScratchGPR);
        throwExceptionIf(ExceptionType::OutOfBoundsTableAccess, m_jit.branchTest32(ResultCondition::Zero, wasmScratchGPR));

        return { };
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addTableSet(unsigned tableIndex, Value index, Value value)
    {
        // FIXME: Emit this inline <https://bugs.webkit.org/show_bug.cgi?id=198506>.
        ASSERT(index.type() == TypeKind::I32);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(tableIndex),
            index,
            value
        };

        Value shouldThrow = topValue(TypeKind::I32);
        emitCCall(&operationSetWasmTableElement, arguments, shouldThrow);
        Location shouldThrowLocation = allocate(shouldThrow);

        LOG_INSTRUCTION("TableSet", tableIndex, index, value);

        throwExceptionIf(ExceptionType::OutOfBoundsTableAccess, m_jit.branchTest32(ResultCondition::Zero, shouldThrowLocation.asGPR()));

        consume(shouldThrow);

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addTableInit(unsigned elementIndex, unsigned tableIndex, ExpressionType dstOffset, ExpressionType srcOffset, ExpressionType length)
    {
        ASSERT(dstOffset.type() == TypeKind::I32);
        ASSERT(srcOffset.type() == TypeKind::I32);
        ASSERT(length.type() == TypeKind::I32);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(elementIndex),
            Value::fromI32(tableIndex),
            dstOffset,
            srcOffset,
            length
        };
        Value shouldThrow = topValue(TypeKind::I32);
        emitCCall(&operationWasmTableInit, arguments, shouldThrow);
        Location shouldThrowLocation = allocate(shouldThrow);

        LOG_INSTRUCTION("TableInit", tableIndex, dstOffset, srcOffset, length);

        throwExceptionIf(ExceptionType::OutOfBoundsTableAccess, m_jit.branchTest32(ResultCondition::Zero, shouldThrowLocation.asGPR()));

        consume(shouldThrow);

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addElemDrop(unsigned elementIndex)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(elementIndex)
        };
        emitCCall(&operationWasmElemDrop, arguments);

        LOG_INSTRUCTION("ElemDrop", elementIndex);
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addTableSize(unsigned tableIndex, Value& result)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(tableIndex)
        };
        result = topValue(TypeKind::I32);
        emitCCall(&operationGetWasmTableSize, arguments, result);

        LOG_INSTRUCTION("TableSize", tableIndex, RESULT(result));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addTableGrow(unsigned tableIndex, Value fill, Value delta, Value& result)
    {
        ASSERT(delta.type() == TypeKind::I32);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(tableIndex),
            fill, delta
        };
        result = topValue(TypeKind::I32);
        emitCCall(&operationWasmTableGrow, arguments, result);

        LOG_INSTRUCTION("TableGrow", tableIndex, fill, delta, RESULT(result));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addTableFill(unsigned tableIndex, Value offset, Value fill, Value count)
    {
        ASSERT(offset.type() == TypeKind::I32);
        ASSERT(count.type() == TypeKind::I32);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(tableIndex),
            offset, fill, count
        };
        Value shouldThrow = topValue(TypeKind::I32);
        emitCCall(&operationWasmTableFill, arguments, shouldThrow);
        Location shouldThrowLocation = allocate(shouldThrow);

        LOG_INSTRUCTION("TableFill", tableIndex, fill, offset, count);

        throwExceptionIf(ExceptionType::OutOfBoundsTableAccess, m_jit.branchTest32(ResultCondition::Zero, shouldThrowLocation.asGPR()));

        consume(shouldThrow);

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addTableCopy(unsigned dstTableIndex, unsigned srcTableIndex, Value dstOffset, Value srcOffset, Value length)
    {
        ASSERT(dstOffset.type() == TypeKind::I32);
        ASSERT(srcOffset.type() == TypeKind::I32);
        ASSERT(length.type() == TypeKind::I32);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(dstTableIndex),
            Value::fromI32(srcTableIndex),
            dstOffset, srcOffset, length
        };
        Value shouldThrow = topValue(TypeKind::I32);
        emitCCall(&operationWasmTableCopy, arguments, shouldThrow);
        Location shouldThrowLocation = allocate(shouldThrow);

        LOG_INSTRUCTION("TableCopy", dstTableIndex, srcTableIndex, dstOffset, srcOffset, length);

        throwExceptionIf(ExceptionType::OutOfBoundsTableAccess, m_jit.branchTest32(ResultCondition::Zero, shouldThrowLocation.asGPR()));

        consume(shouldThrow);

        return { };
    }

    // Locals

    PartialResult WARN_UNUSED_RETURN BBQJIT::getLocal(uint32_t localIndex, Value& result)
    {
        // Currently, we load locals as temps, which basically prevents register allocation of locals.
        // This is probably not ideal, we have infrastructure to support binding locals to registers, but
        // we currently can't track local versioning (meaning we can get SSA-style issues where assigning
        // to a local also updates all previous uses).
        result = topValue(m_parser->typeOfLocal(localIndex).kind);
        Location resultLocation = allocate(result);
        emitLoad(Value::fromLocal(m_parser->typeOfLocal(localIndex).kind, localIndex), resultLocation);
        LOG_INSTRUCTION("GetLocal", localIndex, RESULT(result));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::setLocal(uint32_t localIndex, Value value)
    {
        if (!value.isConst())
            loadIfNecessary(value);
        Value local = Value::fromLocal(m_parser->typeOfLocal(localIndex).kind, localIndex);
        Location localLocation = locationOf(local);
        emitStore(value, localLocation);
        consume(value);
        LOG_INSTRUCTION("SetLocal", localIndex, value);
        return { };
    }

    // Globals

    Value BBQJIT::topValue(TypeKind type)
    {
        return Value::fromTemp(type, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + m_parser->expressionStack().size());
    }

    Value BBQJIT::exception(const ControlData& control)
    {
        ASSERT(ControlData::isAnyCatch(control));
        return Value::fromTemp(TypeKind::Externref, control.enclosedHeight());
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::getGlobal(uint32_t index, Value& result)
    {
        const Wasm::GlobalInformation& global = m_info.globals[index];
        Type type = global.type;

        int32_t offset = Instance::offsetOfGlobalPtr(m_info.importFunctionCount(), m_info.tableCount(), index);
        Value globalValue = Value::pinned(type.kind, Location::fromGlobal(offset));

        switch (global.bindingMode) {
        case Wasm::GlobalInformation::BindingMode::EmbeddedInInstance:
            result = topValue(type.kind);
            emitLoad(globalValue, loadIfNecessary(result));
            break;
        case Wasm::GlobalInformation::BindingMode::Portable:
            ASSERT(global.mutability == Wasm::Mutability::Mutable);
            m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, offset), wasmScratchGPR);
            result = topValue(type.kind);
            Location resultLocation = allocate(result);
            switch (type.kind) {
            case TypeKind::I32:
                m_jit.load32(Address(wasmScratchGPR), resultLocation.asGPR());
                break;
            case TypeKind::I64:
                m_jit.load64(Address(wasmScratchGPR), resultLocation.asGPR());
                break;
            case TypeKind::F32:
                m_jit.loadFloat(Address(wasmScratchGPR), resultLocation.asFPR());
                break;
            case TypeKind::F64:
                m_jit.loadDouble(Address(wasmScratchGPR), resultLocation.asFPR());
                break;
            case TypeKind::V128:
                m_jit.loadVector(Address(wasmScratchGPR), resultLocation.asFPR());
                break;
            case TypeKind::Func:
            case TypeKind::Funcref:
            case TypeKind::Ref:
            case TypeKind::RefNull:
            case TypeKind::Rec:
            case TypeKind::Sub:
            case TypeKind::Subfinal:
            case TypeKind::Struct:
            case TypeKind::Structref:
            case TypeKind::Externref:
            case TypeKind::Array:
            case TypeKind::Arrayref:
            case TypeKind::I31ref:
            case TypeKind::Eqref:
            case TypeKind::Anyref:
            case TypeKind::Nullref:
            case TypeKind::Nullfuncref:
            case TypeKind::Nullexternref:
                m_jit.load64(Address(wasmScratchGPR), resultLocation.asGPR());
                break;
            case TypeKind::Void:
                break;
            }
            break;
        }

        LOG_INSTRUCTION("GetGlobal", index, RESULT(result));

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::getGlobal(uint32_t index, Value& result)
    {
        const Wasm::GlobalInformation& global = m_info.globals[index];
        Type type = global.type;

        int32_t offset = Instance::offsetOfGlobalPtr(m_info.importFunctionCount(), m_info.tableCount(), index);
        Value globalValue = Value::pinned(type.kind, Location::fromGlobal(offset));

        switch (global.bindingMode) {
        case Wasm::GlobalInformation::BindingMode::EmbeddedInInstance:
            result = topValue(type.kind);
            emitLoad(globalValue, loadIfNecessary(result));
            break;
        case Wasm::GlobalInformation::BindingMode::Portable:
            ASSERT(global.mutability == Wasm::Mutability::Mutable);
            m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, offset), wasmScratchGPR);
            result = topValue(type.kind);
            Location resultLocation = allocate(result);
            switch (type.kind) {
            case TypeKind::I32:
                m_jit.load32(Address(wasmScratchGPR), resultLocation.asGPR());
                break;
            case TypeKind::I31ref:
                m_jit.load32(Address(wasmScratchGPR), resultLocation.asGPRlo());
                m_jit.move(TrustedImm32(JSValue::Int32Tag), resultLocation.asGPRhi());
                break;
            case TypeKind::I64:
                m_jit.loadPair32(Address(wasmScratchGPR), resultLocation.asGPRlo(), resultLocation.asGPRhi());
                break;
            case TypeKind::F32:
                m_jit.loadFloat(Address(wasmScratchGPR), resultLocation.asFPR());
                break;
            case TypeKind::F64:
                m_jit.loadDouble(Address(wasmScratchGPR), resultLocation.asFPR());
                break;
            case TypeKind::V128:
                m_jit.loadVector(Address(wasmScratchGPR), resultLocation.asFPR());
                break;
            case TypeKind::Func:
            case TypeKind::Funcref:
            case TypeKind::Ref:
            case TypeKind::RefNull:
            case TypeKind::Rec:
            case TypeKind::Sub:
            case TypeKind::Subfinal:
            case TypeKind::Struct:
            case TypeKind::Structref:
            case TypeKind::Externref:
            case TypeKind::Array:
            case TypeKind::Arrayref:
            case TypeKind::Eqref:
            case TypeKind::Anyref:
            case TypeKind::Nullref:
            case TypeKind::Nullfuncref:
            case TypeKind::Nullexternref:
                m_jit.loadPair32(Address(wasmScratchGPR), resultLocation.asGPRlo(), resultLocation.asGPRhi());
                break;
            case TypeKind::Void:
                break;
            }
            break;
        }

        LOG_INSTRUCTION("GetGlobal", index, RESULT(result));

        return { };
    }
#endif

    void BBQJIT::emitWriteBarrier(GPRReg cellGPR)
    {
        GPRReg vmGPR;
        GPRReg cellStateGPR;
        {
            ScratchScope<2, 0> scratches(*this);
            vmGPR = scratches.gpr(0);
            cellStateGPR = scratches.gpr(1);
        }

        // We must flush everything first. Jumping over flush (emitCCall) is wrong since paths need to get merged.
        flushRegisters();

        m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, Instance::offsetOfVM()), vmGPR);
        m_jit.load8(Address(cellGPR, JSCell::cellStateOffset()), cellStateGPR);
        auto noFenceCheck = m_jit.branch32(RelationalCondition::Above, cellStateGPR, Address(vmGPR, VM::offsetOfHeapBarrierThreshold()));

        // Fence check path
        auto toSlowPath = m_jit.branchTest8(ResultCondition::Zero, Address(vmGPR, VM::offsetOfHeapMutatorShouldBeFenced()));

        // Fence path
        m_jit.memoryFence();
        Jump belowBlackThreshold = m_jit.branch8(RelationalCondition::Above, Address(cellGPR, JSCell::cellStateOffset()), TrustedImm32(blackThreshold));

        // Slow path
        toSlowPath.link(&m_jit);
        m_jit.prepareWasmCallOperation(GPRInfo::wasmContextInstancePointer);
        m_jit.setupArguments<decltype(operationWasmWriteBarrierSlowPath)>(cellGPR, vmGPR);
        m_jit.callOperation(operationWasmWriteBarrierSlowPath);

        // Continuation
        noFenceCheck.link(&m_jit);
        belowBlackThreshold.link(&m_jit);
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::setGlobal(uint32_t index, Value value)
    {
        const Wasm::GlobalInformation& global = m_info.globals[index];
        Type type = global.type;

        int32_t offset = Instance::offsetOfGlobalPtr(m_info.importFunctionCount(), m_info.tableCount(), index);
        Location valueLocation = locationOf(value);

        switch (global.bindingMode) {
        case Wasm::GlobalInformation::BindingMode::EmbeddedInInstance: {
            emitMove(value, Location::fromGlobal(offset));
            consume(value);
            if (isRefType(type)) {
                m_jit.load64(Address(GPRInfo::wasmContextInstancePointer, Instance::offsetOfOwner()), wasmScratchGPR);
                emitWriteBarrier(wasmScratchGPR);
            }
            break;
        }
        case Wasm::GlobalInformation::BindingMode::Portable: {
            ASSERT(global.mutability == Wasm::Mutability::Mutable);
            m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, offset), wasmScratchGPR);

            Location valueLocation;
            if (value.isConst() && value.isFloat()) {
                ScratchScope<0, 1> scratches(*this);
                valueLocation = Location::fromFPR(scratches.fpr(0));
                emitMoveConst(value, valueLocation);
            } else if (value.isConst()) {
                ScratchScope<1, 0> scratches(*this);
                valueLocation = Location::fromGPR(scratches.gpr(0));
                emitMoveConst(value, valueLocation);
            } else
                valueLocation = loadIfNecessary(value);
            ASSERT(valueLocation.isRegister());
            consume(value);

            switch (type.kind) {
            case TypeKind::I32:
                m_jit.store32(valueLocation.asGPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::I64:
                m_jit.store64(valueLocation.asGPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::F32:
                m_jit.storeFloat(valueLocation.asFPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::F64:
                m_jit.storeDouble(valueLocation.asFPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::V128:
                m_jit.storeVector(valueLocation.asFPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::Func:
            case TypeKind::Funcref:
            case TypeKind::Ref:
            case TypeKind::RefNull:
            case TypeKind::Rec:
            case TypeKind::Sub:
            case TypeKind::Subfinal:
            case TypeKind::Struct:
            case TypeKind::Structref:
            case TypeKind::Externref:
            case TypeKind::Array:
            case TypeKind::Arrayref:
            case TypeKind::I31ref:
            case TypeKind::Eqref:
            case TypeKind::Anyref:
            case TypeKind::Nullref:
            case TypeKind::Nullfuncref:
            case TypeKind::Nullexternref:
                m_jit.store64(valueLocation.asGPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::Void:
                break;
            }
            if (isRefType(type)) {
                m_jit.loadPtr(Address(wasmScratchGPR, Wasm::Global::offsetOfOwner() - Wasm::Global::offsetOfValue()), wasmScratchGPR);
                emitWriteBarrier(wasmScratchGPR);
            }
            break;
        }
        }

        LOG_INSTRUCTION("SetGlobal", index, value, valueLocation);

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::setGlobal(uint32_t index, Value value)
    {
        const Wasm::GlobalInformation& global = m_info.globals[index];
        Type type = global.type;

        int32_t offset = Instance::offsetOfGlobalPtr(m_info.importFunctionCount(), m_info.tableCount(), index);
        Location valueLocation = locationOf(value);

        switch (global.bindingMode) {
        case Wasm::GlobalInformation::BindingMode::EmbeddedInInstance: {
            emitMove(value, Location::fromGlobal(offset));
            consume(value);
            if (isRefType(type)) {
                m_jit.load32(Address(GPRInfo::wasmContextInstancePointer, Instance::offsetOfOwner()), wasmScratchGPR);
                emitWriteBarrier(wasmScratchGPR);
            }
            break;
        }
        case Wasm::GlobalInformation::BindingMode::Portable: {
            ASSERT(global.mutability == Wasm::Mutability::Mutable);
            m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, offset), wasmScratchGPR);

            Location valueLocation;
            if (value.isConst() && value.isFloat()) {
                ScratchScope<0, 1> scratches(*this);
                valueLocation = Location::fromFPR(scratches.fpr(0));
                emitMoveConst(value, valueLocation);
            } else if (value.isConst()) {
                if (typeNeedsGPR2(value.type())) {
                    ScratchScope<2, 0> scratches(*this);
                    valueLocation = Location::fromGPR2(scratches.gpr(1), scratches.gpr(0));
                } else {
                    ScratchScope<1, 0> scratches(*this);
                    valueLocation = Location::fromGPR(scratches.gpr(0));
                }
                emitMoveConst(value, valueLocation);
            } else
                valueLocation = loadIfNecessary(value);
            ASSERT(valueLocation.isRegister());
            consume(value);

            switch (type.kind) {
            case TypeKind::I32:
                m_jit.store32(valueLocation.asGPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::I31ref:
                m_jit.store32(valueLocation.asGPRlo(), Address(wasmScratchGPR));
                break;
            case TypeKind::I64:
                m_jit.storePair32(valueLocation.asGPRlo(), valueLocation.asGPRhi(), Address(wasmScratchGPR));
                break;
            case TypeKind::F32:
                m_jit.storeFloat(valueLocation.asFPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::F64:
                m_jit.storeDouble(valueLocation.asFPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::V128:
                m_jit.storeVector(valueLocation.asFPR(), Address(wasmScratchGPR));
                break;
            case TypeKind::Func:
            case TypeKind::Funcref:
            case TypeKind::Ref:
            case TypeKind::RefNull:
            case TypeKind::Rec:
            case TypeKind::Sub:
            case TypeKind::Subfinal:
            case TypeKind::Struct:
            case TypeKind::Structref:
            case TypeKind::Externref:
            case TypeKind::Array:
            case TypeKind::Arrayref:
            case TypeKind::Eqref:
            case TypeKind::Anyref:
            case TypeKind::Nullref:
            case TypeKind::Nullfuncref:
            case TypeKind::Nullexternref:
                m_jit.storePair32(valueLocation.asGPRlo(), valueLocation.asGPRhi(), Address(wasmScratchGPR));
                break;
            case TypeKind::Void:
                break;
            }
            if (isRefType(type)) {
                m_jit.loadPtr(Address(wasmScratchGPR, Wasm::Global::offsetOfOwner() - Wasm::Global::offsetOfValue()), wasmScratchGPR);
                emitWriteBarrier(wasmScratchGPR);
            }
            break;
        }
        }

        LOG_INSTRUCTION("SetGlobal", index, value, valueLocation);

        return { };
    }
#endif

    // Memory
    inline Location BBQJIT::emitCheckAndPreparePointer(Value pointer, uint32_t uoffset, uint32_t sizeOfOperation)
    {
        ScratchScope<1, 0> scratches(*this);
        Location pointerLocation;
        if (pointer.isConst()) {
            pointerLocation = Location::fromGPR(scratches.gpr(0));
            emitMoveConst(pointer, pointerLocation);
        } else
            pointerLocation = loadIfNecessary(pointer);
        ASSERT(pointerLocation.isGPR());

#if USE(JSVALUE32_64)
        ScratchScope<2, 0> globals(*this);
        GPRReg wasmBaseMemoryPointer = globals.gpr(0);
        GPRReg wasmBoundsCheckingSizeRegister = globals.gpr(1);
        loadWebAssemblyGlobalState(wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister);
#endif

        uint64_t boundary = static_cast<uint64_t>(sizeOfOperation) + uoffset - 1;
        switch (m_mode) {
        case MemoryMode::BoundsChecking: {
            // We're not using signal handling only when the memory is not shared.
            // Regardless of signaling, we must check that no memory access exceeds the current memory size.
            m_jit.zeroExtend32ToWord(pointerLocation.asGPR(), wasmScratchGPR);
            if (boundary)
                m_jit.addPtr(TrustedImmPtr(boundary), wasmScratchGPR);
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, wasmBoundsCheckingSizeRegister));
            break;
        }

        case MemoryMode::Signaling: {
            // We've virtually mapped 4GiB+redzone for this memory. Only the user-allocated pages are addressable, contiguously in range [0, current],
            // and everything above is mapped PROT_NONE. We don't need to perform any explicit bounds check in the 4GiB range because WebAssembly register
            // memory accesses are 32-bit. However WebAssembly register + offset accesses perform the addition in 64-bit which can push an access above
            // the 32-bit limit (the offset is unsigned 32-bit). The redzone will catch most small offsets, and we'll explicitly bounds check any
            // register + large offset access. We don't think this will be generated frequently.
            //
            // We could check that register + large offset doesn't exceed 4GiB+redzone since that's technically the limit we need to avoid overflowing the
            // PROT_NONE region, but it's better if we use a smaller immediate because it can codegens better. We know that anything equal to or greater
            // than the declared 'maximum' will trap, so we can compare against that number. If there was no declared 'maximum' then we still know that
            // any access equal to or greater than 4GiB will trap, no need to add the redzone.
            if (uoffset >= Memory::fastMappedRedzoneBytes()) {
                uint64_t maximum = m_info.memory.maximum() ? m_info.memory.maximum().bytes() : std::numeric_limits<uint32_t>::max();
                m_jit.zeroExtend32ToWord(pointerLocation.asGPR(), wasmScratchGPR);
                if (boundary)
                    m_jit.addPtr(TrustedImmPtr(boundary), wasmScratchGPR);
                throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, TrustedImmPtr(static_cast<int64_t>(maximum))));
            }
            break;
        }
        }

#if CPU(ARM64)
        m_jit.addZeroExtend64(wasmBaseMemoryPointer, pointerLocation.asGPR(), wasmScratchGPR);
#else
        m_jit.zeroExtend32ToWord(pointerLocation.asGPR(), wasmScratchGPR);
        m_jit.addPtr(wasmBaseMemoryPointer, wasmScratchGPR);
#endif

        consume(pointer);
        return Location::fromGPR(wasmScratchGPR);
    }

    template<typename Functor>
    auto BBQJIT::emitCheckAndPrepareAndMaterializePointerApply(Value pointer, uint32_t uoffset, uint32_t sizeOfOperation, Functor&& functor) -> decltype(auto)
    {
        uint64_t boundary = static_cast<uint64_t>(sizeOfOperation) + uoffset - 1;

        ScratchScope<1, 0> scratches(*this);
        Location pointerLocation;

#if USE(JSVALUE32_64)
        ScratchScope<2, 0> globals(*this);
        GPRReg wasmBaseMemoryPointer = globals.gpr(0);
        GPRReg wasmBoundsCheckingSizeRegister = globals.gpr(1);
        loadWebAssemblyGlobalState(wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister);
#endif

        if (pointer.isConst()) {
            uint64_t constantPointer = static_cast<uint64_t>(static_cast<uint32_t>(pointer.asI32()));
            uint64_t finalOffset = constantPointer + uoffset;
            if (!(finalOffset > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || !B3::Air::Arg::isValidAddrForm(B3::Air::Move, finalOffset, Width::Width128))) {
                switch (m_mode) {
                case MemoryMode::BoundsChecking: {
                    m_jit.move(TrustedImmPtr(constantPointer + boundary), wasmScratchGPR);
                    throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, wasmBoundsCheckingSizeRegister));
                    break;
                }
                case MemoryMode::Signaling: {
                    if (uoffset >= Memory::fastMappedRedzoneBytes()) {
                        uint64_t maximum = m_info.memory.maximum() ? m_info.memory.maximum().bytes() : std::numeric_limits<uint32_t>::max();
                        if ((constantPointer + boundary) >= maximum)
                            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.jump());
                    }
                    break;
                }
                }
                return functor(CCallHelpers::Address(wasmBaseMemoryPointer, static_cast<int32_t>(finalOffset)));
            }
            pointerLocation = Location::fromGPR(scratches.gpr(0));
            emitMoveConst(pointer, pointerLocation);
        } else
            pointerLocation = loadIfNecessary(pointer);
        ASSERT(pointerLocation.isGPR());

        switch (m_mode) {
        case MemoryMode::BoundsChecking: {
            // We're not using signal handling only when the memory is not shared.
            // Regardless of signaling, we must check that no memory access exceeds the current memory size.
            m_jit.zeroExtend32ToWord(pointerLocation.asGPR(), wasmScratchGPR);
            if (boundary)
#if USE(JSVALUE64)
                m_jit.addPtr(TrustedImmPtr(boundary), wasmScratchGPR);
#else
                // NB: On 32-bit we have to check the addition for overflow
                throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchAdd32(ResultCondition::Carry, wasmScratchGPR, TrustedImm32(boundary), wasmScratchGPR));
#endif
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, wasmBoundsCheckingSizeRegister));
            break;
        }

        case MemoryMode::Signaling: {
            // We've virtually mapped 4GiB+redzone for this memory. Only the user-allocated pages are addressable, contiguously in range [0, current],
            // and everything above is mapped PROT_NONE. We don't need to perform any explicit bounds check in the 4GiB range because WebAssembly register
            // memory accesses are 32-bit. However WebAssembly register + offset accesses perform the addition in 64-bit which can push an access above
            // the 32-bit limit (the offset is unsigned 32-bit). The redzone will catch most small offsets, and we'll explicitly bounds check any
            // register + large offset access. We don't think this will be generated frequently.
            //
            // We could check that register + large offset doesn't exceed 4GiB+redzone since that's technically the limit we need to avoid overflowing the
            // PROT_NONE region, but it's better if we use a smaller immediate because it can codegens better. We know that anything equal to or greater
            // than the declared 'maximum' will trap, so we can compare against that number. If there was no declared 'maximum' then we still know that
            // any access equal to or greater than 4GiB will trap, no need to add the redzone.
            if (uoffset >= Memory::fastMappedRedzoneBytes()) {
                uint64_t maximum = m_info.memory.maximum() ? m_info.memory.maximum().bytes() : std::numeric_limits<uint32_t>::max();
                m_jit.zeroExtend32ToWord(pointerLocation.asGPR(), wasmScratchGPR);
                if (boundary)
                    m_jit.addPtr(TrustedImmPtr(boundary), wasmScratchGPR);
                throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, TrustedImmPtr(static_cast<int64_t>(maximum))));
            }
            break;
        }
        }

#if CPU(ARM64)
        if (!(static_cast<uint64_t>(uoffset) > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || !B3::Air::Arg::isValidAddrForm(B3::Air::Move, uoffset, Width::Width128)))
            return functor(CCallHelpers::BaseIndex(wasmBaseMemoryPointer, pointerLocation.asGPR(), CCallHelpers::TimesOne, static_cast<int32_t>(uoffset), CCallHelpers::Extend::ZExt32));

        m_jit.addZeroExtend64(wasmBaseMemoryPointer, pointerLocation.asGPR(), wasmScratchGPR);
#else
        m_jit.zeroExtend32ToWord(pointerLocation.asGPR(), wasmScratchGPR);
        m_jit.addPtr(wasmBaseMemoryPointer, wasmScratchGPR);
#endif

        if (static_cast<uint64_t>(uoffset) > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || !B3::Air::Arg::isValidAddrForm(B3::Air::Move, uoffset, Width::Width128)) {
            m_jit.addPtr(TrustedImmPtr(static_cast<int64_t>(uoffset)), wasmScratchGPR);
            return functor(Address(wasmScratchGPR));
        }
        return functor(Address(wasmScratchGPR, static_cast<int32_t>(uoffset)));
    }

    inline uint32_t BBQJIT::sizeOfLoadOp(LoadOpType op)
    {
        switch (op) {
        case LoadOpType::I32Load8S:
        case LoadOpType::I32Load8U:
        case LoadOpType::I64Load8S:
        case LoadOpType::I64Load8U:
            return 1;
        case LoadOpType::I32Load16S:
        case LoadOpType::I64Load16S:
        case LoadOpType::I32Load16U:
        case LoadOpType::I64Load16U:
            return 2;
        case LoadOpType::I32Load:
        case LoadOpType::I64Load32S:
        case LoadOpType::I64Load32U:
        case LoadOpType::F32Load:
            return 4;
        case LoadOpType::I64Load:
        case LoadOpType::F64Load:
            return 8;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    inline TypeKind BBQJIT::typeOfLoadOp(LoadOpType op)
    {
        switch (op) {
        case LoadOpType::I32Load8S:
        case LoadOpType::I32Load8U:
        case LoadOpType::I32Load16S:
        case LoadOpType::I32Load16U:
        case LoadOpType::I32Load:
            return TypeKind::I32;
        case LoadOpType::I64Load8S:
        case LoadOpType::I64Load8U:
        case LoadOpType::I64Load16S:
        case LoadOpType::I64Load16U:
        case LoadOpType::I64Load32S:
        case LoadOpType::I64Load32U:
        case LoadOpType::I64Load:
            return TypeKind::I64;
        case LoadOpType::F32Load:
            return TypeKind::F32;
        case LoadOpType::F64Load:
            return TypeKind::F64;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Address BBQJIT::materializePointer(Location pointerLocation, uint32_t uoffset)
    {
        if (static_cast<uint64_t>(uoffset) > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || !B3::Air::Arg::isValidAddrForm(B3::Air::Move, uoffset, Width::Width128)) {
            m_jit.addPtr(TrustedImmPtr(static_cast<int64_t>(uoffset)), pointerLocation.asGPR());
            return Address(pointerLocation.asGPR());
        }
        return Address(pointerLocation.asGPR(), static_cast<int32_t>(uoffset));
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::load(LoadOpType loadOp, Value pointer, Value& result, uint32_t uoffset)
    {
        if (UNLIKELY(sumOverflows<uint32_t>(uoffset, sizeOfLoadOp(loadOp)))) {
            // FIXME: Same issue as in AirIRGenerator::load(): https://bugs.webkit.org/show_bug.cgi?id=166435
            emitThrowException(ExceptionType::OutOfBoundsMemoryAccess);
            consume(pointer);

            // Unreachable at runtime, so we just add a constant that makes the types work out.
            switch (loadOp) {
            case LoadOpType::I32Load8S:
            case LoadOpType::I32Load16S:
            case LoadOpType::I32Load:
            case LoadOpType::I32Load16U:
            case LoadOpType::I32Load8U:
                result = Value::fromI32(0);
                break;
            case LoadOpType::I64Load8S:
            case LoadOpType::I64Load8U:
            case LoadOpType::I64Load16S:
            case LoadOpType::I64Load32U:
            case LoadOpType::I64Load32S:
            case LoadOpType::I64Load:
            case LoadOpType::I64Load16U:
                result = Value::fromI64(0);
                break;
            case LoadOpType::F32Load:
                result = Value::fromF32(0);
                break;
            case LoadOpType::F64Load:
                result = Value::fromF64(0);
                break;
            }
        } else {
            result = emitCheckAndPrepareAndMaterializePointerApply(pointer, uoffset, sizeOfLoadOp(loadOp), [&](auto location) -> Value {
                consume(pointer);
                Value result = topValue(typeOfLoadOp(loadOp));
                Location resultLocation = allocate(result);

                switch (loadOp) {
                case LoadOpType::I32Load8S:
                    m_jit.load8SignedExtendTo32(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load8S:
                    m_jit.load8SignedExtendTo32(location, resultLocation.asGPR());
                    m_jit.signExtend32To64(resultLocation.asGPR(), resultLocation.asGPR());
                    break;
                case LoadOpType::I32Load8U:
                    m_jit.load8(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load8U:
                    m_jit.load8(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I32Load16S:
                    m_jit.load16SignedExtendTo32(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load16S:
                    m_jit.load16SignedExtendTo32(location, resultLocation.asGPR());
                    m_jit.signExtend32To64(resultLocation.asGPR(), resultLocation.asGPR());
                    break;
                case LoadOpType::I32Load16U:
                    m_jit.load16(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load16U:
                    m_jit.load16(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I32Load:
                    m_jit.load32(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load32U:
                    m_jit.load32(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load32S:
                    m_jit.load32(location, resultLocation.asGPR());
                    m_jit.signExtend32To64(resultLocation.asGPR(), resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load:
                    m_jit.load64(location, resultLocation.asGPR());
                    break;
                case LoadOpType::F32Load:
                    m_jit.loadFloat(location, resultLocation.asFPR());
                    break;
                case LoadOpType::F64Load:
                    m_jit.loadDouble(location, resultLocation.asFPR());
                    break;
                }

                return result;
            });
        }

        LOG_INSTRUCTION(LOAD_OP_NAMES[(unsigned)loadOp - (unsigned)I32Load], pointer, uoffset, RESULT(result));

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::load(LoadOpType loadOp, Value pointer, Value& result, uint32_t uoffset)
    {
        if (UNLIKELY(sumOverflows<uint32_t>(uoffset, sizeOfLoadOp(loadOp)))) {
            // FIXME: Same issue as in AirIRGenerator::load(): https://bugs.webkit.org/show_bug.cgi?id=166435
            emitThrowException(ExceptionType::OutOfBoundsMemoryAccess);
            consume(pointer);

            // Unreachable at runtime, so we just add a constant that makes the types work out.
            switch (loadOp) {
            case LoadOpType::I32Load8S:
            case LoadOpType::I32Load16S:
            case LoadOpType::I32Load:
            case LoadOpType::I32Load16U:
            case LoadOpType::I32Load8U:
                result = Value::fromI32(0);
                break;
            case LoadOpType::I64Load8S:
            case LoadOpType::I64Load8U:
            case LoadOpType::I64Load16S:
            case LoadOpType::I64Load32U:
            case LoadOpType::I64Load32S:
            case LoadOpType::I64Load:
            case LoadOpType::I64Load16U:
                result = Value::fromI64(0);
                break;
            case LoadOpType::F32Load:
                result = Value::fromF32(0);
                break;
            case LoadOpType::F64Load:
                result = Value::fromF64(0);
                break;
            }
        } else {
            result = emitCheckAndPrepareAndMaterializePointerApply(pointer, uoffset, sizeOfLoadOp(loadOp), [&](auto location) -> Value {
                consume(pointer);
                Value result = topValue(typeOfLoadOp(loadOp));
                Location resultLocation = allocate(result);

                switch (loadOp) {
                case LoadOpType::I32Load8S:
                    m_jit.load8SignedExtendTo32(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load8S:
                    m_jit.load8SignedExtendTo32(location, resultLocation.asGPRlo());
                    m_jit.rshift32(resultLocation.asGPRlo(), TrustedImm32(31), resultLocation.asGPRhi());
                    break;
                case LoadOpType::I32Load8U:
                    m_jit.load8(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load8U:
                    m_jit.load8(location, resultLocation.asGPRlo());
                    m_jit.xor32(resultLocation.asGPRhi(), resultLocation.asGPRhi());
                    break;
                case LoadOpType::I32Load16S:
                    m_jit.load16SignedExtendTo32(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load16S:
                    m_jit.load16SignedExtendTo32(location, resultLocation.asGPRlo());
                    m_jit.rshift32(resultLocation.asGPRlo(), TrustedImm32(31), resultLocation.asGPRhi());
                    break;
                case LoadOpType::I32Load16U:
                    m_jit.load16(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load16U:
                    m_jit.load16(location, resultLocation.asGPRlo());
                    m_jit.xor32(resultLocation.asGPRhi(), resultLocation.asGPRhi());
                    break;
                case LoadOpType::I32Load:
                    m_jit.load32(location, resultLocation.asGPR());
                    break;
                case LoadOpType::I64Load32U:
                    m_jit.load32(location, resultLocation.asGPRlo());
                    m_jit.xor32(resultLocation.asGPRhi(), resultLocation.asGPRhi());
                    break;
                case LoadOpType::I64Load32S:
                    m_jit.load32(location, resultLocation.asGPRlo());
                    m_jit.rshift32(resultLocation.asGPRlo(), TrustedImm32(31), resultLocation.asGPRhi());
                    break;
                case LoadOpType::I64Load:
                    m_jit.load32(location, resultLocation.asGPRlo());
                    m_jit.load32(location.withOffset(4), resultLocation.asGPRhi());
                    break;
                case LoadOpType::F32Load:
                    m_jit.load32(location, wasmScratchGPR);
                    m_jit.move32ToFloat(wasmScratchGPR, resultLocation.asFPR());
                    break;
                case LoadOpType::F64Load:
                    m_jit.load32(location.withOffset(4), wasmScratchGPR2);
                    m_jit.load32(location, wasmScratchGPR);
                    m_jit.move64ToDouble(wasmScratchGPR2, wasmScratchGPR, resultLocation.asFPR());
                    break;
                }

                return result;
            });
        }

        LOG_INSTRUCTION(LOAD_OP_NAMES[(unsigned)loadOp - (unsigned)I32Load], pointer, uoffset, RESULT(result));

        return { };
    }
#endif

    inline uint32_t BBQJIT::sizeOfStoreOp(StoreOpType op)
    {
        switch (op) {
        case StoreOpType::I32Store8:
        case StoreOpType::I64Store8:
            return 1;
        case StoreOpType::I32Store16:
        case StoreOpType::I64Store16:
            return 2;
        case StoreOpType::I32Store:
        case StoreOpType::I64Store32:
        case StoreOpType::F32Store:
            return 4;
        case StoreOpType::I64Store:
        case StoreOpType::F64Store:
            return 8;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::store(StoreOpType storeOp, Value pointer, Value value, uint32_t uoffset)
    {
        Location valueLocation = locationOf(value);
        if (UNLIKELY(sumOverflows<uint32_t>(uoffset, sizeOfStoreOp(storeOp)))) {
            // FIXME: Same issue as in AirIRGenerator::load(): https://bugs.webkit.org/show_bug.cgi?id=166435
            emitThrowException(ExceptionType::OutOfBoundsMemoryAccess);
            consume(pointer);
            consume(value);
        } else {
            emitCheckAndPrepareAndMaterializePointerApply(pointer, uoffset, sizeOfStoreOp(storeOp), [&](auto location) -> void {
                Location valueLocation;
                if (value.isConst() && value.isFloat()) {
                    ScratchScope<0, 1> scratches(*this);
                    valueLocation = Location::fromFPR(scratches.fpr(0));
                    emitMoveConst(value, valueLocation);
                } else if (value.isConst()) {
                    ScratchScope<1, 0> scratches(*this);
                    valueLocation = Location::fromGPR(scratches.gpr(0));
                    emitMoveConst(value, valueLocation);
                } else
                    valueLocation = loadIfNecessary(value);
                ASSERT(valueLocation.isRegister());

                consume(value);
                consume(pointer);

                switch (storeOp) {
                case StoreOpType::I64Store8:
                case StoreOpType::I32Store8:
                    m_jit.store8(valueLocation.asGPR(), location);
                    return;
                case StoreOpType::I64Store16:
                case StoreOpType::I32Store16:
                    m_jit.store16(valueLocation.asGPR(), location);
                    return;
                case StoreOpType::I64Store32:
                case StoreOpType::I32Store:
                    m_jit.store32(valueLocation.asGPR(), location);
                    return;
                case StoreOpType::I64Store:
                    m_jit.store64(valueLocation.asGPR(), location);
                    return;
                case StoreOpType::F32Store:
                    m_jit.storeFloat(valueLocation.asFPR(), location);
                    return;
                case StoreOpType::F64Store:
                    m_jit.storeDouble(valueLocation.asFPR(), location);
                    return;
                }
            });
        }

        LOG_INSTRUCTION(STORE_OP_NAMES[(unsigned)storeOp - (unsigned)I32Store], pointer, uoffset, value, valueLocation);

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::store(StoreOpType storeOp, Value pointer, Value value, uint32_t uoffset)
    {
        Location valueLocation = locationOf(value);
        if (UNLIKELY(sumOverflows<uint32_t>(uoffset, sizeOfStoreOp(storeOp)))) {
            // FIXME: Same issue as in AirIRGenerator::load(): https://bugs.webkit.org/show_bug.cgi?id=166435
            emitThrowException(ExceptionType::OutOfBoundsMemoryAccess);
            consume(pointer);
            consume(value);
        } else {
            emitCheckAndPrepareAndMaterializePointerApply(pointer, uoffset, sizeOfStoreOp(storeOp), [&](auto location) -> void {
                Location valueLocation;
                if (value.isConst() && value.isFloat()) {
                    ScratchScope<0, 1> scratches(*this);
                    valueLocation = Location::fromFPR(scratches.fpr(0));
                    emitMoveConst(value, valueLocation);
                } else if (value.isConst() && typeNeedsGPR2(value.type())) {
                    ScratchScope<2, 0> scratches(*this);
                    valueLocation = Location::fromGPR2(scratches.gpr(1), scratches.gpr(0));
                    emitMoveConst(value, valueLocation);
                } else if (value.isConst()) {
                    ScratchScope<1, 0> scratches(*this);
                    valueLocation = Location::fromGPR(scratches.gpr(0));
                    emitMoveConst(value, valueLocation);
                } else
                    valueLocation = loadIfNecessary(value);
                ASSERT(valueLocation.isRegister());

                consume(value);
                consume(pointer);

                switch (storeOp) {
                case StoreOpType::I64Store8:
                    m_jit.store8(valueLocation.asGPRlo(), location);
                    return;
                case StoreOpType::I32Store8:
                    m_jit.store8(valueLocation.asGPR(), location);
                    return;
                case StoreOpType::I64Store16:
                    m_jit.store16(valueLocation.asGPRlo(), location);
                    return;
                case StoreOpType::I32Store16:
                    m_jit.store16(valueLocation.asGPR(), location);
                    return;
                case StoreOpType::I64Store32:
                    m_jit.store32(valueLocation.asGPRlo(), location);
                    return;
                case StoreOpType::I32Store:
                    m_jit.store32(valueLocation.asGPR(), location);
                    return;
                case StoreOpType::I64Store:
                    m_jit.store32(valueLocation.asGPRlo(), location);
                    m_jit.store32(valueLocation.asGPRhi(), location.withOffset(4));
                    return;
                case StoreOpType::F32Store: {
                    ScratchScope<1, 0> scratches(*this);
                    m_jit.moveFloatTo32(valueLocation.asFPR(), scratches.gpr(0));
                    m_jit.store32(scratches.gpr(0), location);
                    return;
                }
                case StoreOpType::F64Store: {
                    ScratchScope<1, 0> scratches(*this);
                    m_jit.moveDoubleTo64(valueLocation.asFPR(), wasmScratchGPR2, scratches.gpr(0));
                    m_jit.store32(scratches.gpr(0), location);
                    m_jit.store32(wasmScratchGPR2, location.withOffset(4));
                    return;
                }
                }
            });
        }

        LOG_INSTRUCTION(STORE_OP_NAMES[(unsigned)storeOp - (unsigned)I32Store], pointer, uoffset, value, valueLocation);

        return { };
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addGrowMemory(Value delta, Value& result)
    {
        Vector<Value, 8> arguments = { instanceValue(), delta };
        result = topValue(TypeKind::I32);
        emitCCall(&operationGrowMemory, arguments, result);
        restoreWebAssemblyGlobalState();

        LOG_INSTRUCTION("GrowMemory", delta, RESULT(result));

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCurrentMemory(Value& result)
    {
        result = topValue(TypeKind::I32);
        Location resultLocation = allocate(result);
        m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, Instance::offsetOfMemory()), wasmScratchGPR);
        m_jit.loadPtr(Address(wasmScratchGPR, Memory::offsetOfHandle()), wasmScratchGPR);
        m_jit.loadPtr(Address(wasmScratchGPR, BufferMemoryHandle::offsetOfSize()), wasmScratchGPR);

        constexpr uint32_t shiftValue = 16;
        static_assert(PageCount::pageSize == 1ull << shiftValue, "This must hold for the code below to be correct.");
        m_jit.urshiftPtr(Imm32(shiftValue), wasmScratchGPR);
        m_jit.zeroExtend32ToWord(wasmScratchGPR, resultLocation.asGPR());

        LOG_INSTRUCTION("CurrentMemory", RESULT(result));

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addMemoryFill(Value dstAddress, Value targetValue, Value count)
    {
        ASSERT(dstAddress.type() == TypeKind::I32);
        ASSERT(targetValue.type() == TypeKind::I32);
        ASSERT(count.type() == TypeKind::I32);

        Vector<Value, 8> arguments = {
            instanceValue(),
            dstAddress, targetValue, count
        };
        Value shouldThrow = topValue(TypeKind::I32);
        emitCCall(&operationWasmMemoryFill, arguments, shouldThrow);
        Location shouldThrowLocation = allocate(shouldThrow);

        throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest32(ResultCondition::Zero, shouldThrowLocation.asGPR()));

        LOG_INSTRUCTION("MemoryFill", dstAddress, targetValue, count);

        consume(shouldThrow);

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addMemoryCopy(Value dstAddress, Value srcAddress, Value count)
    {
        ASSERT(dstAddress.type() == TypeKind::I32);
        ASSERT(srcAddress.type() == TypeKind::I32);
        ASSERT(count.type() == TypeKind::I32);

        Vector<Value, 8> arguments = {
            instanceValue(),
            dstAddress, srcAddress, count
        };
        Value shouldThrow = topValue(TypeKind::I32);
        emitCCall(&operationWasmMemoryCopy, arguments, shouldThrow);
        Location shouldThrowLocation = allocate(shouldThrow);

        throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest32(ResultCondition::Zero, shouldThrowLocation.asGPR()));

        LOG_INSTRUCTION("MemoryCopy", dstAddress, srcAddress, count);

        consume(shouldThrow);

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addMemoryInit(unsigned dataSegmentIndex, Value dstAddress, Value srcAddress, Value length)
    {
        ASSERT(dstAddress.type() == TypeKind::I32);
        ASSERT(srcAddress.type() == TypeKind::I32);
        ASSERT(length.type() == TypeKind::I32);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(dataSegmentIndex),
            dstAddress, srcAddress, length
        };
        Value shouldThrow = topValue(TypeKind::I32);
        emitCCall(&operationWasmMemoryInit, arguments, shouldThrow);
        Location shouldThrowLocation = allocate(shouldThrow);

        throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest32(ResultCondition::Zero, shouldThrowLocation.asGPR()));

        LOG_INSTRUCTION("MemoryInit", dataSegmentIndex, dstAddress, srcAddress, length);

        consume(shouldThrow);

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addDataDrop(unsigned dataSegmentIndex)
    {
        Vector<Value, 8> arguments = { instanceValue(), Value::fromI32(dataSegmentIndex) };
        emitCCall(&operationWasmDataDrop, arguments);

        LOG_INSTRUCTION("DataDrop", dataSegmentIndex);
        return { };
    }

    // Atomics

    inline Width BBQJIT::accessWidth(ExtAtomicOpType op)
    {
        return static_cast<Width>(memoryLog2Alignment(op));
    }

    inline uint32_t BBQJIT::sizeOfAtomicOpMemoryAccess(ExtAtomicOpType op)
    {
        return bytesForWidth(accessWidth(op));
    }

#if USE(JSVALUE64)
    void BBQJIT::emitSanitizeAtomicResult(ExtAtomicOpType op, TypeKind resultType, GPRReg source, GPRReg dest)
    {
        switch (resultType) {
        case TypeKind::I64: {
            switch (accessWidth(op)) {
            case Width8:
                m_jit.zeroExtend8To32(source, dest);
                return;
            case Width16:
                m_jit.zeroExtend16To32(source, dest);
                return;
            case Width32:
                m_jit.zeroExtend32ToWord(source, dest);
                return;
            case Width64:
                m_jit.move(source, dest);
                return;
            case Width128:
                RELEASE_ASSERT_NOT_REACHED();
                return;
            }
            return;
        }
        case TypeKind::I32:
            switch (accessWidth(op)) {
            case Width8:
                m_jit.zeroExtend8To32(source, dest);
                return;
            case Width16:
                m_jit.zeroExtend16To32(source, dest);
                return;
            case Width32:
            case Width64:
                m_jit.move(source, dest);
                return;
            case Width128:
                RELEASE_ASSERT_NOT_REACHED();
                return;
            }
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return;
        }
    }

    void BBQJIT::emitSanitizeAtomicResult(ExtAtomicOpType op, TypeKind resultType, GPRReg result)
    {
        emitSanitizeAtomicResult(op, resultType, result, result);
    }
#else
    void BBQJIT::emitSanitizeAtomicResult(ExtAtomicOpType op, TypeKind resultType, Location source, Location dest)
    {
        switch (resultType) {
        case TypeKind::I64: {
            switch (accessWidth(op)) {
            case Width8:
                m_jit.zeroExtend8To32(source.asGPR(), dest.asGPRlo());
                m_jit.xor32(dest.asGPRhi(), dest.asGPRhi());
                return;
            case Width16:
                m_jit.zeroExtend16To32(source.asGPR(), dest.asGPRlo());
                m_jit.xor32(dest.asGPRhi(), dest.asGPRhi());
                return;
            case Width32:
                m_jit.zeroExtend32ToWord(source.asGPR(), dest.asGPRlo());
                m_jit.xor32(dest.asGPRhi(), dest.asGPRhi());
                return;
            case Width64:
                ASSERT(dest.asGPRlo() != source.asGPRhi());
                m_jit.move(source.asGPRlo(), dest.asGPRlo());
                m_jit.move(source.asGPRhi(), dest.asGPRhi());
                return;
            case Width128:
                RELEASE_ASSERT_NOT_REACHED();
                return;
            }
            return;
        }
        case TypeKind::I32:
            switch (accessWidth(op)) {
            case Width8:
                m_jit.zeroExtend8To32(source.asGPR(), dest.asGPR());
                return;
            case Width16:
                m_jit.zeroExtend16To32(source.asGPR(), dest.asGPR());
                return;
            case Width32:
                m_jit.move(source.asGPR(), dest.asGPR());
                return;
            case Width64:
                m_jit.move(source.asGPRlo(), dest.asGPR());
                return;
            case Width128:
                RELEASE_ASSERT_NOT_REACHED();
                return;
            }
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return;
        }
    }
#endif

#if USE(JSVALUE32_64)
    void BBQJIT::emitSanitizeAtomicOperand(ExtAtomicOpType op, TypeKind operandType, Location source, Location dest)
    {
        switch (operandType) {
        case TypeKind::I64:
            switch (accessWidth(op)) {
            case Width8:
                m_jit.zeroExtend8To32(source.asGPRlo(), dest.asGPR());
                return;
            case Width16:
                m_jit.zeroExtend16To32(source.asGPRlo(), dest.asGPR());
                return;
            case Width32:
                m_jit.move(source.asGPRlo(), dest.asGPR());
                return;
            case Width64:
                ASSERT(dest.asGPRlo() != source.asGPRhi());
                m_jit.move(source.asGPRlo(), dest.asGPRlo());
                m_jit.move(source.asGPRhi(), dest.asGPRhi());
                return;
            case Width128:
                RELEASE_ASSERT_NOT_REACHED();
                return;
            }
            return;
        case TypeKind::I32:
            switch (accessWidth(op)) {
            case Width8:
                m_jit.zeroExtend8To32(source.asGPR(), dest.asGPR());
                return;
            case Width16:
                m_jit.zeroExtend16To32(source.asGPR(), dest.asGPR());
                return;
            case Width32:
                m_jit.move(source.asGPR(), dest.asGPR());
                return;
            case Width64:
                m_jit.move(source.asGPR(), dest.asGPRlo());
                m_jit.xor32(dest.asGPRhi(), dest.asGPRhi());
                return;
            case Width128:
                RELEASE_ASSERT_NOT_REACHED();
                return;
            }
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return;
        }

    }

    Location BBQJIT::emitMaterializeAtomicOperand(Value value)
    {
        Location valueLocation;

        if (value.isConst()) {
            switch (value.type()) {
            case TypeKind::I64: {
                ScratchScope<2, 0> scratches(*this);
                valueLocation = Location::fromGPR2(scratches.gpr(1), scratches.gpr(0));
                break;
            }
            case TypeKind::I32: {
                ScratchScope<1, 0> scratches(*this);
                valueLocation = Location::fromGPR(scratches.gpr(0));
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            emitMoveConst(value, valueLocation);
        } else
            valueLocation = loadIfNecessary(value);
        ASSERT(valueLocation.isRegister());

        consume(value);

        return valueLocation;
    }
#endif

#if USE(JSVALUE64)
    template<typename Functor>
    void BBQJIT::emitAtomicOpGeneric(ExtAtomicOpType op, Address address, GPRReg oldGPR, GPRReg scratchGPR, const Functor& functor)
    {
        Width accessWidth = this->accessWidth(op);

        // We need a CAS loop or a LL/SC loop. Using prepare/attempt jargon, we want:
        //
        // Block #reloop:
        //     Prepare
        //     Operation
        //     Attempt
        //   Successors: Then:#done, Else:#reloop
        // Block #done:
        //     Move oldValue, result

        // Prepare
        auto reloopLabel = m_jit.label();
        switch (accessWidth) {
        case Width8:
#if CPU(ARM64)
            m_jit.loadLinkAcq8(address, oldGPR);
#else
            m_jit.load8SignedExtendTo32(address, oldGPR);
#endif
            break;
        case Width16:
#if CPU(ARM64)
            m_jit.loadLinkAcq16(address, oldGPR);
#else
            m_jit.load16SignedExtendTo32(address, oldGPR);
#endif
            break;
        case Width32:
#if CPU(ARM64)
            m_jit.loadLinkAcq32(address, oldGPR);
#else
            m_jit.load32(address, oldGPR);
#endif
            break;
        case Width64:
#if CPU(ARM64)
            m_jit.loadLinkAcq64(address, oldGPR);
#else
            m_jit.load64(address, oldGPR);
#endif
            break;
        case Width128:
            RELEASE_ASSERT_NOT_REACHED();
        }

        // Operation
        functor(oldGPR, scratchGPR);

#if CPU(X86_64)
        switch (accessWidth) {
        case Width8:
            m_jit.branchAtomicStrongCAS8(StatusCondition::Failure, oldGPR, scratchGPR, address).linkTo(reloopLabel, &m_jit);
            break;
        case Width16:
            m_jit.branchAtomicStrongCAS16(StatusCondition::Failure, oldGPR, scratchGPR, address).linkTo(reloopLabel, &m_jit);
            break;
        case Width32:
            m_jit.branchAtomicStrongCAS32(StatusCondition::Failure, oldGPR, scratchGPR, address).linkTo(reloopLabel, &m_jit);
            break;
        case Width64:
            m_jit.branchAtomicStrongCAS64(StatusCondition::Failure, oldGPR, scratchGPR, address).linkTo(reloopLabel, &m_jit);
            break;
        case Width128:
            RELEASE_ASSERT_NOT_REACHED();
        }
#elif CPU(ARM64)
        switch (accessWidth) {
        case Width8:
            m_jit.storeCondRel8(scratchGPR, address, scratchGPR);
            break;
        case Width16:
            m_jit.storeCondRel16(scratchGPR, address, scratchGPR);
            break;
        case Width32:
            m_jit.storeCondRel32(scratchGPR, address, scratchGPR);
            break;
        case Width64:
            m_jit.storeCondRel64(scratchGPR, address, scratchGPR);
            break;
        case Width128:
            RELEASE_ASSERT_NOT_REACHED();
        }
        m_jit.branchTest32(ResultCondition::NonZero, scratchGPR).linkTo(reloopLabel, &m_jit);
#endif
    }
#else
    template<typename Functor>
    void BBQJIT::emitAtomicOpGeneric32_64(ExtAtomicOpType op, Address address, Location old, Location cur, const Functor& functor)
    {
        Width accessWidth = this->accessWidth(op);

        // We need a CAS loop or a LL/SC loop. Using prepare/attempt jargon, we want:
        //
        // Block #reloop:
        //     Prepare
        //     Operation
        //     Attempt
        //   Successors: Then:#done, Else:#reloop
        // Block #done:
        //     Move oldValue, result

        // Prepare
        auto reloopLabel = m_jit.label();
        switch (accessWidth) {
        case Width8:
            m_jit.loadLink8(address, old.asGPR());
            break;
        case Width16:
            m_jit.loadLink16(address, old.asGPR());
            break;
        case Width32:
            m_jit.loadLink32(address, old.asGPR());
            break;
        case Width64:
            m_jit.loadLinkPair32(address, old.asGPRlo(), old.asGPRhi());
            break;
        case Width128:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }

        // Operation
        functor(old, cur);

        switch (accessWidth) {
        case Width8:
            m_jit.storeCond8(cur.asGPR(), address, wasmScratchGPR2);
            break;
        case Width16:
            m_jit.storeCond16(cur.asGPR(), address, wasmScratchGPR2);
            break;
        case Width32:
            m_jit.storeCond32(cur.asGPR(), address, wasmScratchGPR2);
            break;
        case Width64:
            m_jit.storeCondPair32(cur.asGPRlo(), cur.asGPRhi(), address, wasmScratchGPR2);
            break;
        case Width128:
            RELEASE_ASSERT_NOT_REACHED();
        }
        m_jit.branchTest32(ResultCondition::NonZero, wasmScratchGPR2).linkTo(reloopLabel, &m_jit);
    }
#endif

#if USE(JSVALUE64)
    Value WARN_UNUSED_RETURN BBQJIT::emitAtomicLoadOp(ExtAtomicOpType loadOp, Type valueType, Location pointer, uint32_t uoffset)
    {
        ASSERT(pointer.isGPR());

        // For Atomic access, we need SimpleAddress (uoffset = 0).
        if (uoffset)
            m_jit.add64(TrustedImm64(static_cast<int64_t>(uoffset)), pointer.asGPR());
        Address address = Address(pointer.asGPR());

        if (accessWidth(loadOp) != Width8)
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest64(ResultCondition::NonZero, pointer.asGPR(), TrustedImm64(sizeOfAtomicOpMemoryAccess(loadOp) - 1)));

        Value result = topValue(valueType.kind);
        Location resultLocation = allocate(result);

        if (!(isARM64_LSE() || isX86_64())) {
            ScratchScope<1, 0> scratches(*this);
            emitAtomicOpGeneric(loadOp, address, resultLocation.asGPR(), scratches.gpr(0), [&](GPRReg oldGPR, GPRReg newGPR) {
                emitSanitizeAtomicResult(loadOp, canonicalWidth(accessWidth(loadOp)) == Width64 ? TypeKind::I64 : TypeKind::I32, oldGPR, newGPR);
            });
            emitSanitizeAtomicResult(loadOp, valueType.kind, resultLocation.asGPR());
            return result;
        }

        m_jit.move(TrustedImm32(0), resultLocation.asGPR());
        switch (loadOp) {
        case ExtAtomicOpType::I32AtomicLoad: {
#if CPU(ARM64)
            m_jit.atomicXchgAdd32(resultLocation.asGPR(), address, resultLocation.asGPR());
#else
            m_jit.atomicXchgAdd32(resultLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I64AtomicLoad: {
#if CPU(ARM64)
            m_jit.atomicXchgAdd64(resultLocation.asGPR(), address, resultLocation.asGPR());
#else
            m_jit.atomicXchgAdd64(resultLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I32AtomicLoad8U: {
#if CPU(ARM64)
            m_jit.atomicXchgAdd8(resultLocation.asGPR(), address, resultLocation.asGPR());
#else
            m_jit.atomicXchgAdd8(resultLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I32AtomicLoad16U: {
#if CPU(ARM64)
            m_jit.atomicXchgAdd16(resultLocation.asGPR(), address, resultLocation.asGPR());
#else
            m_jit.atomicXchgAdd16(resultLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I64AtomicLoad8U: {
#if CPU(ARM64)
            m_jit.atomicXchgAdd8(resultLocation.asGPR(), address, resultLocation.asGPR());
#else
            m_jit.atomicXchgAdd8(resultLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I64AtomicLoad16U: {
#if CPU(ARM64)
            m_jit.atomicXchgAdd16(resultLocation.asGPR(), address, resultLocation.asGPR());
#else
            m_jit.atomicXchgAdd16(resultLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I64AtomicLoad32U: {
#if CPU(ARM64)
            m_jit.atomicXchgAdd32(resultLocation.asGPR(), address, resultLocation.asGPR());
#else
            m_jit.atomicXchgAdd32(resultLocation.asGPR(), address);
#endif
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }

        emitSanitizeAtomicResult(loadOp, valueType.kind, resultLocation.asGPR());

        return result;
    }
#else
    Value WARN_UNUSED_RETURN BBQJIT::emitAtomicLoadOp(ExtAtomicOpType loadOp, Type valueType, Location pointer, uint32_t uoffset)
    {
        ASSERT(pointer.isGPR());

        // For Atomic access, we need SimpleAddress (uoffset = 0).
        if (uoffset)
            m_jit.addPtr(TrustedImmPtr(static_cast<int64_t>(uoffset)), pointer.asGPR());
        Address address = Address(pointer.asGPR());

        if (accessWidth(loadOp) != Width8)
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest32(ResultCondition::NonZero, pointer.asGPR(), TrustedImm32(sizeOfAtomicOpMemoryAccess(loadOp) - 1)));

        Value result = topValue(valueType.kind);
        Location resultLocation;
        Location old, cur;

        switch (valueType.kind) {
        case TypeKind::I64:
            switch (canonicalWidth(accessWidth(loadOp))) {
            case Width32: {
                resultLocation = allocate(result);
                cur = Location::fromGPR(resultLocation.asGPRlo());
                ScratchScope<1, 0> scratches(*this);
                old = Location::fromGPR(scratches.gpr(0));
                break;
            }
            case Width64: {
                resultLocation = allocate(result);
                cur = resultLocation;
                ScratchScope<2, 0> scratches(*this);
                old = Location::fromGPR2(scratches.gpr(1), scratches.gpr(0));
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            break;
        case TypeKind::I32:
            switch (canonicalWidth(accessWidth(loadOp))) {
            case Width32: {
                resultLocation = allocate(result);
                cur = resultLocation;
                ScratchScope<1, 0> scratches(*this);
                old = Location::fromGPR(scratches.gpr(0));
                break;
            }
            case Width64: {
                ScratchScope<4, 0> scratches(*this);
                cur = Location::fromGPR2(scratches.gpr(1), scratches.gpr(0));
                resultLocation = Location::fromGPR(scratches.gpr(0));
                ASSERT(resultLocation == allocateWithHint(result, resultLocation));
                old = Location::fromGPR2(scratches.gpr(3), scratches.gpr(2));
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        emitAtomicOpGeneric32_64(loadOp, address, old, cur, [&](Location old, Location cur) {
            if (old.isGPR2()) {
                ASSERT(cur.asGPRlo() != old.asGPRhi());
                m_jit.move(old.asGPRlo(), cur.asGPRlo());
                m_jit.move(old.asGPRhi(), cur.asGPRhi());
            } else
                m_jit.move(old.asGPR(), cur.asGPR());
        });
        emitSanitizeAtomicResult(loadOp, valueType.kind, cur, resultLocation);

        return result;
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::atomicLoad(ExtAtomicOpType loadOp, Type valueType, ExpressionType pointer, ExpressionType& result, uint32_t uoffset)
    {
        if (UNLIKELY(sumOverflows<uint32_t>(uoffset, sizeOfAtomicOpMemoryAccess(loadOp)))) {
            // FIXME: Same issue as in AirIRGenerator::load(): https://bugs.webkit.org/show_bug.cgi?id=166435
            emitThrowException(ExceptionType::OutOfBoundsMemoryAccess);
            consume(pointer);
            result = valueType.isI64() ? Value::fromI64(0) : Value::fromI32(0);
        } else
            result = emitAtomicLoadOp(loadOp, valueType, emitCheckAndPreparePointer(pointer, uoffset, sizeOfAtomicOpMemoryAccess(loadOp)), uoffset);

        LOG_INSTRUCTION(makeString(loadOp), pointer, uoffset, RESULT(result));

        return { };
    }

#if USE(JSVALUE64)
    void BBQJIT::emitAtomicStoreOp(ExtAtomicOpType storeOp, Type, Location pointer, Value value, uint32_t uoffset)
    {
        ASSERT(pointer.isGPR());

        // For Atomic access, we need SimpleAddress (uoffset = 0).
        if (uoffset)
            m_jit.add64(TrustedImm64(static_cast<int64_t>(uoffset)), pointer.asGPR());
        Address address = Address(pointer.asGPR());

        if (accessWidth(storeOp) != Width8)
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest64(ResultCondition::NonZero, pointer.asGPR(), TrustedImm64(sizeOfAtomicOpMemoryAccess(storeOp) - 1)));

        GPRReg scratch1GPR = InvalidGPRReg;
        GPRReg scratch2GPR = InvalidGPRReg;
        Location valueLocation;
        if (value.isConst()) {
            ScratchScope<3, 0> scratches(*this);
            valueLocation = Location::fromGPR(scratches.gpr(0));
            emitMoveConst(value, valueLocation);
            scratch1GPR = scratches.gpr(1);
            scratch2GPR = scratches.gpr(2);
        } else {
            ScratchScope<2, 0> scratches(*this);
            valueLocation = loadIfNecessary(value);
            scratch1GPR = scratches.gpr(0);
            scratch2GPR = scratches.gpr(1);
        }
        ASSERT(valueLocation.isRegister());

        consume(value);

        if (!(isARM64_LSE() || isX86_64())) {
            emitAtomicOpGeneric(storeOp, address, scratch1GPR, scratch2GPR, [&](GPRReg, GPRReg newGPR) {
                m_jit.move(valueLocation.asGPR(), newGPR);
            });
            return;
        }

        switch (storeOp) {
        case ExtAtomicOpType::I32AtomicStore: {
#if CPU(ARM64)
            m_jit.atomicXchg32(valueLocation.asGPR(), address, scratch1GPR);
#else
            m_jit.store32(valueLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I64AtomicStore: {
#if CPU(ARM64)
            m_jit.atomicXchg64(valueLocation.asGPR(), address, scratch1GPR);
#else
            m_jit.store64(valueLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I32AtomicStore8U: {
#if CPU(ARM64)
            m_jit.atomicXchg8(valueLocation.asGPR(), address, scratch1GPR);
#else
            m_jit.store8(valueLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I32AtomicStore16U: {
#if CPU(ARM64)
            m_jit.atomicXchg16(valueLocation.asGPR(), address, scratch1GPR);
#else
            m_jit.store16(valueLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I64AtomicStore8U: {
#if CPU(ARM64)
            m_jit.atomicXchg8(valueLocation.asGPR(), address, scratch1GPR);
#else
            m_jit.store8(valueLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I64AtomicStore16U: {
#if CPU(ARM64)
            m_jit.atomicXchg16(valueLocation.asGPR(), address, scratch1GPR);
#else
            m_jit.store16(valueLocation.asGPR(), address);
#endif
            break;
        }
        case ExtAtomicOpType::I64AtomicStore32U: {
#if CPU(ARM64)
            m_jit.atomicXchg32(valueLocation.asGPR(), address, scratch1GPR);
#else
            m_jit.store32(valueLocation.asGPR(), address);
#endif
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    }
#else
    void BBQJIT::emitAtomicStoreOp(ExtAtomicOpType storeOp, Type, Location pointer, Value value, uint32_t uoffset)
    {
        ASSERT(pointer.isGPR());

        // For Atomic access, we need SimpleAddress (uoffset = 0).
        if (uoffset)
            m_jit.addPtr(TrustedImmPtr(static_cast<int64_t>(uoffset)), pointer.asGPR());
        Address address = Address(pointer.asGPR());

        if (accessWidth(storeOp) != Width8)
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest32(ResultCondition::NonZero, pointer.asGPR(), TrustedImm32(sizeOfAtomicOpMemoryAccess(storeOp) - 1)));

        Location source, old, cur;
        switch (canonicalWidth(accessWidth(storeOp))) {
        case Width32: {
            Location valueLocation = emitMaterializeAtomicOperand(value);
            ScratchScope<1, 0> scratchSource(*this);
            source = Location::fromGPR(scratchSource.gpr(0));
            emitSanitizeAtomicOperand(storeOp, value.type(), valueLocation, source);
            ScratchScope<2, 0> scratchTmp(*this);
            old = Location::fromGPR(scratchTmp.gpr(0));
            cur = Location::fromGPR(scratchTmp.gpr(1));
            break;
        }
        case Width64: {
            Location valueLocation = emitMaterializeAtomicOperand(value);
            ScratchScope<2, 0> scratchSource(*this);
            source = Location::fromGPR2(scratchSource.gpr(1), scratchSource.gpr(0));
            emitSanitizeAtomicOperand(storeOp, value.type(), valueLocation, source);
            ScratchScope<4, 0> scratchTmp(*this);
            old = Location::fromGPR2(scratchTmp.gpr(1), scratchTmp.gpr(0));
            cur = Location::fromGPR2(scratchTmp.gpr(3), scratchTmp.gpr(2));
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        emitAtomicOpGeneric32_64(storeOp, address, old, cur, [&](Location, Location cur) {
            if (source.isGPR2()) {
                ASSERT(cur.asGPRlo() != source.asGPRhi());
                m_jit.move(source.asGPRlo(), cur.asGPRlo());
                m_jit.move(source.asGPRhi(), cur.asGPRhi());
            } else
                m_jit.move(source.asGPR(), cur.asGPR());
        });
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::atomicStore(ExtAtomicOpType storeOp, Type valueType, ExpressionType pointer, ExpressionType value, uint32_t uoffset)
    {
        Location valueLocation = locationOf(value);
        if (UNLIKELY(sumOverflows<uint32_t>(uoffset, sizeOfAtomicOpMemoryAccess(storeOp)))) {
            // FIXME: Same issue as in AirIRGenerator::load(): https://bugs.webkit.org/show_bug.cgi?id=166435
            emitThrowException(ExceptionType::OutOfBoundsMemoryAccess);
            consume(pointer);
            consume(value);
        } else
            emitAtomicStoreOp(storeOp, valueType, emitCheckAndPreparePointer(pointer, uoffset, sizeOfAtomicOpMemoryAccess(storeOp)), value, uoffset);

        LOG_INSTRUCTION(makeString(storeOp), pointer, uoffset, value, valueLocation);

        return { };
    }

#if USE(JSVALUE64)
    Value BBQJIT::emitAtomicBinaryRMWOp(ExtAtomicOpType op, Type valueType, Location pointer, Value value, uint32_t uoffset)
    {
        ASSERT(pointer.isGPR());

        // For Atomic access, we need SimpleAddress (uoffset = 0).
        if (uoffset)
            m_jit.add64(TrustedImm64(static_cast<int64_t>(uoffset)), pointer.asGPR());
        Address address = Address(pointer.asGPR());

        if (accessWidth(op) != Width8)
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest64(ResultCondition::NonZero, pointer.asGPR(), TrustedImm64(sizeOfAtomicOpMemoryAccess(op) - 1)));

        Value result = topValue(valueType.kind);
        Location resultLocation = allocate(result);

        GPRReg scratchGPR = InvalidGPRReg;
        Location valueLocation;
        if (value.isConst()) {
            ScratchScope<2, 0> scratches(*this);
            valueLocation = Location::fromGPR(scratches.gpr(0));
            emitMoveConst(value, valueLocation);
            scratchGPR = scratches.gpr(1);
        } else {
            ScratchScope<1, 0> scratches(*this);
            valueLocation = loadIfNecessary(value);
            scratchGPR = scratches.gpr(0);
        }
        ASSERT(valueLocation.isRegister());
        consume(value);

        switch (op) {
        case ExtAtomicOpType::I32AtomicRmw8AddU:
        case ExtAtomicOpType::I32AtomicRmw16AddU:
        case ExtAtomicOpType::I32AtomicRmwAdd:
        case ExtAtomicOpType::I64AtomicRmw8AddU:
        case ExtAtomicOpType::I64AtomicRmw16AddU:
        case ExtAtomicOpType::I64AtomicRmw32AddU:
        case ExtAtomicOpType::I64AtomicRmwAdd:
            if (isX86() || isARM64_LSE()) {
                switch (accessWidth(op)) {
                case Width8:
#if CPU(ARM64)
                    m_jit.atomicXchgAdd8(valueLocation.asGPR(), address, resultLocation.asGPR());
#else
                    m_jit.move(valueLocation.asGPR(), resultLocation.asGPR());
                    m_jit.atomicXchgAdd8(resultLocation.asGPR(), address);
#endif
                    break;
                case Width16:
#if CPU(ARM64)
                    m_jit.atomicXchgAdd16(valueLocation.asGPR(), address, resultLocation.asGPR());
#else
                    m_jit.move(valueLocation.asGPR(), resultLocation.asGPR());
                    m_jit.atomicXchgAdd16(resultLocation.asGPR(), address);
#endif
                    break;
                case Width32:
#if CPU(ARM64)
                    m_jit.atomicXchgAdd32(valueLocation.asGPR(), address, resultLocation.asGPR());
#else
                    m_jit.move(valueLocation.asGPR(), resultLocation.asGPR());
                    m_jit.atomicXchgAdd32(resultLocation.asGPR(), address);
#endif
                    break;
                case Width64:
#if CPU(ARM64)
                    m_jit.atomicXchgAdd64(valueLocation.asGPR(), address, resultLocation.asGPR());
#else
                    m_jit.move(valueLocation.asGPR(), resultLocation.asGPR());
                    m_jit.atomicXchgAdd64(resultLocation.asGPR(), address);
#endif
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                emitSanitizeAtomicResult(op, valueType.kind, resultLocation.asGPR());
                return result;
            }
            break;
        case ExtAtomicOpType::I32AtomicRmw8SubU:
        case ExtAtomicOpType::I32AtomicRmw16SubU:
        case ExtAtomicOpType::I32AtomicRmwSub:
        case ExtAtomicOpType::I64AtomicRmw8SubU:
        case ExtAtomicOpType::I64AtomicRmw16SubU:
        case ExtAtomicOpType::I64AtomicRmw32SubU:
        case ExtAtomicOpType::I64AtomicRmwSub:
            if (isX86() || isARM64_LSE()) {
                m_jit.move(valueLocation.asGPR(), scratchGPR);
                if (valueType.isI64())
                    m_jit.neg64(scratchGPR);
                else
                    m_jit.neg32(scratchGPR);

                switch (accessWidth(op)) {
                case Width8:
#if CPU(ARM64)
                    m_jit.atomicXchgAdd8(scratchGPR, address, resultLocation.asGPR());
#else
                    m_jit.move(scratchGPR, resultLocation.asGPR());
                    m_jit.atomicXchgAdd8(resultLocation.asGPR(), address);
#endif
                    break;
                case Width16:
#if CPU(ARM64)
                    m_jit.atomicXchgAdd16(scratchGPR, address, resultLocation.asGPR());
#else
                    m_jit.move(scratchGPR, resultLocation.asGPR());
                    m_jit.atomicXchgAdd16(resultLocation.asGPR(), address);
#endif
                    break;
                case Width32:
#if CPU(ARM64)
                    m_jit.atomicXchgAdd32(scratchGPR, address, resultLocation.asGPR());
#else
                    m_jit.move(scratchGPR, resultLocation.asGPR());
                    m_jit.atomicXchgAdd32(resultLocation.asGPR(), address);
#endif
                    break;
                case Width64:
#if CPU(ARM64)
                    m_jit.atomicXchgAdd64(scratchGPR, address, resultLocation.asGPR());
#else
                    m_jit.move(scratchGPR, resultLocation.asGPR());
                    m_jit.atomicXchgAdd64(resultLocation.asGPR(), address);
#endif
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                emitSanitizeAtomicResult(op, valueType.kind, resultLocation.asGPR());
                return result;
            }
            break;
        case ExtAtomicOpType::I32AtomicRmw8AndU:
        case ExtAtomicOpType::I32AtomicRmw16AndU:
        case ExtAtomicOpType::I32AtomicRmwAnd:
        case ExtAtomicOpType::I64AtomicRmw8AndU:
        case ExtAtomicOpType::I64AtomicRmw16AndU:
        case ExtAtomicOpType::I64AtomicRmw32AndU:
        case ExtAtomicOpType::I64AtomicRmwAnd:
#if CPU(ARM64)
            if (isARM64_LSE()) {
                m_jit.move(valueLocation.asGPR(), scratchGPR);
                if (valueType.isI64())
                    m_jit.not64(scratchGPR);
                else
                    m_jit.not32(scratchGPR);

                switch (accessWidth(op)) {
                case Width8:
                    m_jit.atomicXchgClear8(scratchGPR, address, resultLocation.asGPR());
                    break;
                case Width16:
                    m_jit.atomicXchgClear16(scratchGPR, address, resultLocation.asGPR());
                    break;
                case Width32:
                    m_jit.atomicXchgClear32(scratchGPR, address, resultLocation.asGPR());
                    break;
                case Width64:
                    m_jit.atomicXchgClear64(scratchGPR, address, resultLocation.asGPR());
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                emitSanitizeAtomicResult(op, valueType.kind, resultLocation.asGPR());
                return result;
            }
#endif
            break;
        case ExtAtomicOpType::I32AtomicRmw8OrU:
        case ExtAtomicOpType::I32AtomicRmw16OrU:
        case ExtAtomicOpType::I32AtomicRmwOr:
        case ExtAtomicOpType::I64AtomicRmw8OrU:
        case ExtAtomicOpType::I64AtomicRmw16OrU:
        case ExtAtomicOpType::I64AtomicRmw32OrU:
        case ExtAtomicOpType::I64AtomicRmwOr:
#if CPU(ARM64)
            if (isARM64_LSE()) {
                switch (accessWidth(op)) {
                case Width8:
                    m_jit.atomicXchgOr8(valueLocation.asGPR(), address, resultLocation.asGPR());
                    break;
                case Width16:
                    m_jit.atomicXchgOr16(valueLocation.asGPR(), address, resultLocation.asGPR());
                    break;
                case Width32:
                    m_jit.atomicXchgOr32(valueLocation.asGPR(), address, resultLocation.asGPR());
                    break;
                case Width64:
                    m_jit.atomicXchgOr64(valueLocation.asGPR(), address, resultLocation.asGPR());
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                emitSanitizeAtomicResult(op, valueType.kind, resultLocation.asGPR());
                return result;
            }
#endif
            break;
        case ExtAtomicOpType::I32AtomicRmw8XorU:
        case ExtAtomicOpType::I32AtomicRmw16XorU:
        case ExtAtomicOpType::I32AtomicRmwXor:
        case ExtAtomicOpType::I64AtomicRmw8XorU:
        case ExtAtomicOpType::I64AtomicRmw16XorU:
        case ExtAtomicOpType::I64AtomicRmw32XorU:
        case ExtAtomicOpType::I64AtomicRmwXor:
#if CPU(ARM64)
            if (isARM64_LSE()) {
                switch (accessWidth(op)) {
                case Width8:
                    m_jit.atomicXchgXor8(valueLocation.asGPR(), address, resultLocation.asGPR());
                    break;
                case Width16:
                    m_jit.atomicXchgXor16(valueLocation.asGPR(), address, resultLocation.asGPR());
                    break;
                case Width32:
                    m_jit.atomicXchgXor32(valueLocation.asGPR(), address, resultLocation.asGPR());
                    break;
                case Width64:
                    m_jit.atomicXchgXor64(valueLocation.asGPR(), address, resultLocation.asGPR());
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                emitSanitizeAtomicResult(op, valueType.kind, resultLocation.asGPR());
                return result;
            }
#endif
            break;
        case ExtAtomicOpType::I32AtomicRmw8XchgU:
        case ExtAtomicOpType::I32AtomicRmw16XchgU:
        case ExtAtomicOpType::I32AtomicRmwXchg:
        case ExtAtomicOpType::I64AtomicRmw8XchgU:
        case ExtAtomicOpType::I64AtomicRmw16XchgU:
        case ExtAtomicOpType::I64AtomicRmw32XchgU:
        case ExtAtomicOpType::I64AtomicRmwXchg:
            if (isX86() || isARM64_LSE()) {
                switch (accessWidth(op)) {
                case Width8:
#if CPU(ARM64)
                    m_jit.atomicXchg8(valueLocation.asGPR(), address, resultLocation.asGPR());
#else
                    m_jit.move(valueLocation.asGPR(), resultLocation.asGPR());
                    m_jit.atomicXchg8(resultLocation.asGPR(), address);
#endif
                    break;
                case Width16:
#if CPU(ARM64)
                    m_jit.atomicXchg16(valueLocation.asGPR(), address, resultLocation.asGPR());
#else
                    m_jit.move(valueLocation.asGPR(), resultLocation.asGPR());
                    m_jit.atomicXchg16(resultLocation.asGPR(), address);
#endif
                    break;
                case Width32:
#if CPU(ARM64)
                    m_jit.atomicXchg32(valueLocation.asGPR(), address, resultLocation.asGPR());
#else
                    m_jit.move(valueLocation.asGPR(), resultLocation.asGPR());
                    m_jit.atomicXchg32(resultLocation.asGPR(), address);
#endif
                    break;
                case Width64:
#if CPU(ARM64)
                    m_jit.atomicXchg64(valueLocation.asGPR(), address, resultLocation.asGPR());
#else
                    m_jit.move(valueLocation.asGPR(), resultLocation.asGPR());
                    m_jit.atomicXchg64(resultLocation.asGPR(), address);
#endif
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                emitSanitizeAtomicResult(op, valueType.kind, resultLocation.asGPR());
                return result;
            }
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }

        emitAtomicOpGeneric(op, address, resultLocation.asGPR(), scratchGPR, [&](GPRReg oldGPR, GPRReg newGPR) {
            switch (op) {
            case ExtAtomicOpType::I32AtomicRmw16AddU:
            case ExtAtomicOpType::I32AtomicRmw8AddU:
            case ExtAtomicOpType::I32AtomicRmwAdd:
                m_jit.add32(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I64AtomicRmw8AddU:
            case ExtAtomicOpType::I64AtomicRmw16AddU:
            case ExtAtomicOpType::I64AtomicRmw32AddU:
            case ExtAtomicOpType::I64AtomicRmwAdd:
                m_jit.add64(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I32AtomicRmw8SubU:
            case ExtAtomicOpType::I32AtomicRmw16SubU:
            case ExtAtomicOpType::I32AtomicRmwSub:
                m_jit.sub32(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I64AtomicRmw8SubU:
            case ExtAtomicOpType::I64AtomicRmw16SubU:
            case ExtAtomicOpType::I64AtomicRmw32SubU:
            case ExtAtomicOpType::I64AtomicRmwSub:
                m_jit.sub64(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I32AtomicRmw8AndU:
            case ExtAtomicOpType::I32AtomicRmw16AndU:
            case ExtAtomicOpType::I32AtomicRmwAnd:
                m_jit.and32(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I64AtomicRmw8AndU:
            case ExtAtomicOpType::I64AtomicRmw16AndU:
            case ExtAtomicOpType::I64AtomicRmw32AndU:
            case ExtAtomicOpType::I64AtomicRmwAnd:
                m_jit.and64(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I32AtomicRmw8OrU:
            case ExtAtomicOpType::I32AtomicRmw16OrU:
            case ExtAtomicOpType::I32AtomicRmwOr:
                m_jit.or32(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I64AtomicRmw8OrU:
            case ExtAtomicOpType::I64AtomicRmw16OrU:
            case ExtAtomicOpType::I64AtomicRmw32OrU:
            case ExtAtomicOpType::I64AtomicRmwOr:
                m_jit.or64(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I32AtomicRmw8XorU:
            case ExtAtomicOpType::I32AtomicRmw16XorU:
            case ExtAtomicOpType::I32AtomicRmwXor:
                m_jit.xor32(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I64AtomicRmw8XorU:
            case ExtAtomicOpType::I64AtomicRmw16XorU:
            case ExtAtomicOpType::I64AtomicRmw32XorU:
            case ExtAtomicOpType::I64AtomicRmwXor:
                m_jit.xor64(oldGPR, valueLocation.asGPR(), newGPR);
                break;
            case ExtAtomicOpType::I32AtomicRmw8XchgU:
            case ExtAtomicOpType::I32AtomicRmw16XchgU:
            case ExtAtomicOpType::I32AtomicRmwXchg:
            case ExtAtomicOpType::I64AtomicRmw8XchgU:
            case ExtAtomicOpType::I64AtomicRmw16XchgU:
            case ExtAtomicOpType::I64AtomicRmw32XchgU:
            case ExtAtomicOpType::I64AtomicRmwXchg:
                emitSanitizeAtomicResult(op, valueType.kind, valueLocation.asGPR(), newGPR);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
        });
        emitSanitizeAtomicResult(op, valueType.kind, resultLocation.asGPR());
        return result;
    }
#else
    Value BBQJIT::emitAtomicBinaryRMWOp(ExtAtomicOpType op, Type valueType, Location pointer, Value value, uint32_t uoffset)
    {
        ASSERT(pointer.isGPR());

        // For Atomic access, we need SimpleAddress (uoffset = 0).
        if (uoffset)
            m_jit.addPtr(TrustedImmPtr(static_cast<int64_t>(uoffset)), pointer.asGPR());
        Address address = Address(pointer.asGPR());

        if (accessWidth(op) != Width8)
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest32(ResultCondition::NonZero, pointer.asGPR(), TrustedImm32(sizeOfAtomicOpMemoryAccess(op) - 1)));

        Value result = topValue(valueType.kind);
        Location resultLocation;
        Location operand, old, cur;

        switch (valueType.kind) {
        case TypeKind::I64:
            switch (canonicalWidth(accessWidth(op))) {
            case Width32: {
                resultLocation = allocate(result);
                old = Location::fromGPR(resultLocation.asGPRlo());
                Location valueLocation = emitMaterializeAtomicOperand(value);
                ScratchScope<1, 0> scratchOperand(*this, old);
                operand = Location::fromGPR(scratchOperand.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), valueLocation, operand);
                ScratchScope<1, 0> scratchTmp(*this, old);
                cur = Location::fromGPR(scratchTmp.gpr(0));
                break;
            }
            case Width64: {
                resultLocation = allocate(result);
                old = resultLocation;
                Location valueLocation = emitMaterializeAtomicOperand(value);
                ScratchScope<2, 0> scratchOperand(*this, old);
                operand = Location::fromGPR2(scratchOperand.gpr(1), scratchOperand.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), valueLocation, operand);
                ScratchScope<2, 0> scratchTmp(*this, old);
                cur = Location::fromGPR2(scratchTmp.gpr(1), scratchTmp.gpr(0));
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            break;
        case TypeKind::I32:
            switch (canonicalWidth(accessWidth(op))) {
            case Width32: {
                resultLocation = allocate(result);
                old = resultLocation;
                Location valueLocation = emitMaterializeAtomicOperand(value);
                ScratchScope<1, 0> scratchOperand(*this, old);
                operand = Location::fromGPR(scratchOperand.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), valueLocation, operand);
                ScratchScope<1, 0> scratchTmp(*this, old);
                cur = Location::fromGPR(scratchTmp.gpr(0));
                break;
            }
            case Width64: {
                ScratchScope<2, 0> scratchResult(*this);
                old = Location::fromGPR2(scratchResult.gpr(1), scratchResult.gpr(0));
                resultLocation = Location::fromGPR(scratchResult.gpr(0));
                ASSERT(resultLocation == allocateWithHint(result, resultLocation));
                Location valueLocation = emitMaterializeAtomicOperand(value);
                ScratchScope<2, 0> scratchOperand(*this);
                operand = Location::fromGPR2(scratchOperand.gpr(1), scratchOperand.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), valueLocation, operand);
                ScratchScope<2, 0> scratchTmp(*this);
                cur = Location::fromGPR2(scratchTmp.gpr(1), scratchTmp.gpr(0));
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        emitAtomicOpGeneric32_64(op, address, old, cur, [&](Location old, Location cur) {
            switch (op) {
            case ExtAtomicOpType::I32AtomicRmw16AddU:
            case ExtAtomicOpType::I32AtomicRmw8AddU:
            case ExtAtomicOpType::I32AtomicRmwAdd:
            case ExtAtomicOpType::I64AtomicRmw8AddU:
            case ExtAtomicOpType::I64AtomicRmw16AddU:
            case ExtAtomicOpType::I64AtomicRmw32AddU:
                m_jit.add32(old.asGPR(), operand.asGPR(), cur.asGPR());
                break;
            case ExtAtomicOpType::I64AtomicRmwAdd:
                m_jit.add64(old.asGPRhi(), old.asGPRlo(),
                    operand.asGPRhi(), operand.asGPRlo(),
                    cur.asGPRhi(), cur.asGPRlo());
                break;
            case ExtAtomicOpType::I32AtomicRmw8SubU:
            case ExtAtomicOpType::I32AtomicRmw16SubU:
            case ExtAtomicOpType::I32AtomicRmwSub:
            case ExtAtomicOpType::I64AtomicRmw8SubU:
            case ExtAtomicOpType::I64AtomicRmw16SubU:
            case ExtAtomicOpType::I64AtomicRmw32SubU:
                m_jit.sub32(old.asGPR(), operand.asGPR(), cur.asGPR());
                break;
            case ExtAtomicOpType::I64AtomicRmwSub:
                m_jit.sub64(old.asGPRhi(), old.asGPRlo(),
                    operand.asGPRhi(), operand.asGPRlo(),
                    cur.asGPRhi(), cur.asGPRlo());
                break;
            case ExtAtomicOpType::I32AtomicRmw8AndU:
            case ExtAtomicOpType::I32AtomicRmw16AndU:
            case ExtAtomicOpType::I32AtomicRmwAnd:
            case ExtAtomicOpType::I64AtomicRmw8AndU:
            case ExtAtomicOpType::I64AtomicRmw16AndU:
            case ExtAtomicOpType::I64AtomicRmw32AndU:
                m_jit.and32(old.asGPR(), operand.asGPR(), cur.asGPR());
                break;
            case ExtAtomicOpType::I64AtomicRmwAnd:
                m_jit.and32(old.asGPRhi(), operand.asGPRhi(), cur.asGPRhi());
                m_jit.and32(old.asGPRlo(), operand.asGPRlo(), cur.asGPRlo());
                break;
            case ExtAtomicOpType::I32AtomicRmw8OrU:
            case ExtAtomicOpType::I32AtomicRmw16OrU:
            case ExtAtomicOpType::I32AtomicRmwOr:
            case ExtAtomicOpType::I64AtomicRmw8OrU:
            case ExtAtomicOpType::I64AtomicRmw16OrU:
            case ExtAtomicOpType::I64AtomicRmw32OrU:
                m_jit.or32(old.asGPR(), operand.asGPR(), cur.asGPR());
                break;
            case ExtAtomicOpType::I64AtomicRmwOr:
                m_jit.or32(old.asGPRhi(), operand.asGPRhi(), cur.asGPRhi());
                m_jit.or32(old.asGPRlo(), operand.asGPRlo(), cur.asGPRlo());
                break;
            case ExtAtomicOpType::I32AtomicRmw8XorU:
            case ExtAtomicOpType::I32AtomicRmw16XorU:
            case ExtAtomicOpType::I32AtomicRmwXor:
            case ExtAtomicOpType::I64AtomicRmw8XorU:
            case ExtAtomicOpType::I64AtomicRmw16XorU:
            case ExtAtomicOpType::I64AtomicRmw32XorU:
                m_jit.xor32(old.asGPR(), operand.asGPR(), cur.asGPR());
                break;
            case ExtAtomicOpType::I64AtomicRmwXor:
                m_jit.xor32(old.asGPRhi(), operand.asGPRhi(), cur.asGPRhi());
                m_jit.xor32(old.asGPRlo(), operand.asGPRlo(), cur.asGPRlo());
                break;
            case ExtAtomicOpType::I32AtomicRmw8XchgU:
            case ExtAtomicOpType::I32AtomicRmw16XchgU:
            case ExtAtomicOpType::I32AtomicRmwXchg:
            case ExtAtomicOpType::I64AtomicRmw8XchgU:
            case ExtAtomicOpType::I64AtomicRmw16XchgU:
            case ExtAtomicOpType::I64AtomicRmw32XchgU:
                m_jit.move(operand.asGPR(), cur.asGPR());
                break;
            case ExtAtomicOpType::I64AtomicRmwXchg:
                ASSERT(cur.asGPRlo() != operand.asGPRhi());
                m_jit.move(operand.asGPRlo(), cur.asGPRlo());
                m_jit.move(operand.asGPRhi(), cur.asGPRhi());
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
        });
        emitSanitizeAtomicResult(op, valueType.kind, old, resultLocation);

        return result;
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::atomicBinaryRMW(ExtAtomicOpType op, Type valueType, ExpressionType pointer, ExpressionType value, ExpressionType& result, uint32_t uoffset)
    {
        Location valueLocation = locationOf(value);
        if (UNLIKELY(sumOverflows<uint32_t>(uoffset, sizeOfAtomicOpMemoryAccess(op)))) {
            // FIXME: Even though this is provably out of bounds, it's not a validation error, so we have to handle it
            // as a runtime exception. However, this may change: https://bugs.webkit.org/show_bug.cgi?id=166435
            emitThrowException(ExceptionType::OutOfBoundsMemoryAccess);
            consume(pointer);
            consume(value);
            result = valueType.isI64() ? Value::fromI64(0) : Value::fromI32(0);
        } else
            result = emitAtomicBinaryRMWOp(op, valueType, emitCheckAndPreparePointer(pointer, uoffset, sizeOfAtomicOpMemoryAccess(op)), value, uoffset);

        LOG_INSTRUCTION(makeString(op), pointer, uoffset, value, valueLocation, RESULT(result));

        return { };
    }

#if USE(JSVALUE64)
    Value WARN_UNUSED_RETURN BBQJIT::emitAtomicCompareExchange(ExtAtomicOpType op, Type, Location pointer, Value expected, Value value, uint32_t uoffset)
    {
        ASSERT(pointer.isGPR());

        // For Atomic access, we need SimpleAddress (uoffset = 0).
        if (uoffset)
            m_jit.add64(TrustedImm64(static_cast<int64_t>(uoffset)), pointer.asGPR());
        Address address = Address(pointer.asGPR());
        Width accessWidth = this->accessWidth(op);

        if (accessWidth != Width8)
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest64(ResultCondition::NonZero, pointer.asGPR(), TrustedImm64(sizeOfAtomicOpMemoryAccess(op) - 1)));

        Value result = topValue(expected.type());
        Location resultLocation = allocate(result);

        ScratchScope<1, 0> scratches(*this);
        GPRReg scratchGPR = scratches.gpr(0);

        // FIXME: We should have a better way to write this.
        Location valueLocation;
        Location expectedLocation;
        if (value.isConst()) {
            if (expected.isConst()) {
                ScratchScope<2, 0> scratches(*this);
                valueLocation = Location::fromGPR(scratches.gpr(0));
                expectedLocation = Location::fromGPR(scratches.gpr(1));
                emitMoveConst(value, valueLocation);
                emitMoveConst(expected, expectedLocation);
            } else {
                ScratchScope<1, 0> scratches(*this);
                valueLocation = Location::fromGPR(scratches.gpr(0));
                emitMoveConst(value, valueLocation);
                expectedLocation = loadIfNecessary(expected);
            }
        } else {
            valueLocation = loadIfNecessary(value);
            if (expected.isConst()) {
                ScratchScope<1, 0> scratches(*this);
                expectedLocation = Location::fromGPR(scratches.gpr(0));
                emitMoveConst(expected, expectedLocation);
            } else
                expectedLocation = loadIfNecessary(expected);
        }

        ASSERT(valueLocation.isRegister());
        ASSERT(expectedLocation.isRegister());

        consume(value);
        consume(expected);

        auto emitStrongCAS = [&](GPRReg expectedGPR, GPRReg valueGPR, GPRReg resultGPR) {
            if (isX86_64() || isARM64_LSE()) {
                m_jit.move(expectedGPR, resultGPR);
                switch (accessWidth) {
                case Width8:
                    m_jit.atomicStrongCAS8(resultGPR, valueGPR, address);
                    break;
                case Width16:
                    m_jit.atomicStrongCAS16(resultGPR, valueGPR, address);
                    break;
                case Width32:
                    m_jit.atomicStrongCAS32(resultGPR, valueGPR, address);
                    break;
                case Width64:
                    m_jit.atomicStrongCAS64(resultGPR, valueGPR, address);
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                return;
            }

            m_jit.move(expectedGPR, resultGPR);
            switch (accessWidth) {
            case Width8:
                m_jit.atomicStrongCAS8(StatusCondition::Success, resultGPR, valueGPR, address, scratchGPR);
                break;
            case Width16:
                m_jit.atomicStrongCAS16(StatusCondition::Success, resultGPR, valueGPR, address, scratchGPR);
                break;
            case Width32:
                m_jit.atomicStrongCAS32(StatusCondition::Success, resultGPR, valueGPR, address, scratchGPR);
                break;
            case Width64:
                m_jit.atomicStrongCAS64(StatusCondition::Success, resultGPR, valueGPR, address, scratchGPR);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
        };

        switch (accessWidth) {
        case Width8:
            m_jit.and64(TrustedImm64(0xFF), expectedLocation.asGPR());
            break;
        case Width16:
            m_jit.and64(TrustedImm64(0xFFFF), expectedLocation.asGPR());
            break;
        case Width32:
            m_jit.and64(TrustedImm64(0xFFFFFFFF), expectedLocation.asGPR());
            break;
        default:
            break;
        }

        emitStrongCAS(expectedLocation.asGPR(), valueLocation.asGPR(), resultLocation.asGPR());
        emitSanitizeAtomicResult(op, expected.type(), resultLocation.asGPR());
        return result;
    }
#else
    Value WARN_UNUSED_RETURN BBQJIT::emitAtomicCompareExchange(ExtAtomicOpType op, Type valueType, Location pointer, Value expected, Value value, uint32_t uoffset)
    {
        ASSERT(pointer.isGPR());

        // For Atomic access, we need SimpleAddress (uoffset = 0).
        if (uoffset)
            m_jit.addPtr(TrustedImmPtr(static_cast<int64_t>(uoffset)), pointer.asGPR());
        Address address = Address(pointer.asGPR());

        if (accessWidth(op) != Width8)
            throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchTest32(ResultCondition::NonZero, pointer.asGPR(), TrustedImm32(sizeOfAtomicOpMemoryAccess(op) - 1)));

        Value result = topValue(valueType.kind);
        Location resultLocation;
        Location a, b, old, cur;

        switch (valueType.kind) {
        case TypeKind::I64:
            switch (canonicalWidth(accessWidth(op))) {
            case Width32: {
                resultLocation = allocate(result);
                old = Location::fromGPR(resultLocation.asGPRlo());
                Location aLocation = emitMaterializeAtomicOperand(expected);
                ScratchScope<1, 0> scratchA(*this, old);
                a = Location::fromGPR(scratchA.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), aLocation, a);
                Location bLocation = emitMaterializeAtomicOperand(value);
                ScratchScope<1, 0> scratchB(*this, old);
                b = Location::fromGPR(scratchB.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), bLocation, b);
                cur = old;
                break;
            }
            case Width64: {
                resultLocation = allocate(result);
                old = resultLocation;
                Location aLocation = emitMaterializeAtomicOperand(expected);
                ScratchScope<2, 0> scratchA(*this, old);
                a = Location::fromGPR2(scratchA.gpr(1), scratchA.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), aLocation, a);
                Location bLocation = emitMaterializeAtomicOperand(value);
                ScratchScope<2, 0> scratchB(*this, old);
                b = Location::fromGPR2(scratchB.gpr(1), scratchB.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), bLocation, b);
                cur = old;
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            break;
        case TypeKind::I32:
            switch (canonicalWidth(accessWidth(op))) {
            case Width32: {
                resultLocation = allocate(result);
                old = resultLocation;
                Location aLocation = emitMaterializeAtomicOperand(expected);
                ScratchScope<1, 0> scratchA(*this, old);
                a = Location::fromGPR(scratchA.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), aLocation, a);
                Location bLocation = emitMaterializeAtomicOperand(value);
                ScratchScope<1, 0> scratchB(*this, old);
                b = Location::fromGPR(scratchB.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), bLocation, b);
                cur = old;
                break;
            }
            case Width64: {
                ScratchScope<2, 0> scratchResult(*this);
                old = Location::fromGPR2(scratchResult.gpr(1), scratchResult.gpr(0));
                resultLocation = Location::fromGPR(scratchResult.gpr(0));
                ASSERT(resultLocation == allocateWithHint(result, resultLocation));
                Location aLocation = emitMaterializeAtomicOperand(expected);
                ScratchScope<2, 0> scratchA(*this);
                a = Location::fromGPR2(scratchA.gpr(1), scratchA.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), aLocation, a);
                Location bLocation = emitMaterializeAtomicOperand(value);
                ScratchScope<2, 0> scratchB(*this);
                b = Location::fromGPR2(scratchB.gpr(1), scratchB.gpr(0));
                emitSanitizeAtomicOperand(op, value.type(), bLocation, b);
                cur = old;
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        JumpList failure;

        emitAtomicOpGeneric32_64(op, address, old, cur, [&](Location old, Location cur) {
            emitSanitizeAtomicResult(op, valueType.kind, old, resultLocation);
            if (old.isGPR2()) {
                failure.append(m_jit.branch32(RelationalCondition::NotEqual, old.asGPRlo(), a.asGPRlo()));
                failure.append(m_jit.branch32(RelationalCondition::NotEqual, old.asGPRhi(), a.asGPRhi()));
                m_jit.move(b.asGPRlo(), cur.asGPRlo());
                m_jit.move(b.asGPRhi(), cur.asGPRhi());
            } else {
                failure.append(m_jit.branch32(RelationalCondition::NotEqual, old.asGPR(), a.asGPR()));
                m_jit.move(b.asGPR(), cur.asGPR());
            }
        });
        emitSanitizeAtomicResult(op, valueType.kind, a, resultLocation);
        failure.link(&m_jit);

        return result;
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::atomicCompareExchange(ExtAtomicOpType op, Type valueType, ExpressionType pointer, ExpressionType expected, ExpressionType value, ExpressionType& result, uint32_t uoffset)
    {
        Location valueLocation = locationOf(value);
        if (UNLIKELY(sumOverflows<uint32_t>(uoffset, sizeOfAtomicOpMemoryAccess(op)))) {
            // FIXME: Even though this is provably out of bounds, it's not a validation error, so we have to handle it
            // as a runtime exception. However, this may change: https://bugs.webkit.org/show_bug.cgi?id=166435
            emitThrowException(ExceptionType::OutOfBoundsMemoryAccess);
            consume(pointer);
            consume(expected);
            consume(value);
            result = valueType.isI64() ? Value::fromI64(0) : Value::fromI32(0);
        } else
            result = emitAtomicCompareExchange(op, valueType, emitCheckAndPreparePointer(pointer, uoffset, sizeOfAtomicOpMemoryAccess(op)), expected, value, uoffset);

        LOG_INSTRUCTION(makeString(op), pointer, expected, value, valueLocation, uoffset, RESULT(result));

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::atomicWait(ExtAtomicOpType op, ExpressionType pointer, ExpressionType value, ExpressionType timeout, ExpressionType& result, uint32_t uoffset)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            pointer,
            Value::fromI32(uoffset),
            value,
            timeout
        };

        result = topValue(TypeKind::I32);
        if (op == ExtAtomicOpType::MemoryAtomicWait32)
            emitCCall(&operationMemoryAtomicWait32, arguments, result);
        else
            emitCCall(&operationMemoryAtomicWait64, arguments, result);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION(makeString(op), pointer, value, timeout, uoffset, RESULT(result));

        throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branch32(RelationalCondition::LessThan, resultLocation.asGPR(), TrustedImm32(0)));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::atomicNotify(ExtAtomicOpType op, ExpressionType pointer, ExpressionType count, ExpressionType& result, uint32_t uoffset)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            pointer,
            Value::fromI32(uoffset),
            count
        };
        result = topValue(TypeKind::I32);
        emitCCall(&operationMemoryAtomicNotify, arguments, result);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION(makeString(op), pointer, count, uoffset, RESULT(result));

        throwExceptionIf(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branch32(RelationalCondition::LessThan, resultLocation.asGPR(), TrustedImm32(0)));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::atomicFence(ExtAtomicOpType, uint8_t)
    {
        m_jit.memoryFence();
        return { };
    }

    // Saturated truncation.

    struct BBQJIT::FloatingPointRange {
        Value min, max;
        bool closedLowerEndpoint;
    };

    TruncationKind BBQJIT::truncationKind(OpType truncationOp)
    {
        switch (truncationOp) {
        case OpType::I32TruncSF32:
            return TruncationKind::I32TruncF32S;
        case OpType::I32TruncUF32:
            return TruncationKind::I32TruncF32U;
        case OpType::I64TruncSF32:
            return TruncationKind::I64TruncF32S;
        case OpType::I64TruncUF32:
            return TruncationKind::I64TruncF32U;
        case OpType::I32TruncSF64:
            return TruncationKind::I32TruncF64S;
        case OpType::I32TruncUF64:
            return TruncationKind::I32TruncF64U;
        case OpType::I64TruncSF64:
            return TruncationKind::I64TruncF64S;
        case OpType::I64TruncUF64:
            return TruncationKind::I64TruncF64U;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Not a truncation op");
        }
    }

    TruncationKind BBQJIT::truncationKind(Ext1OpType truncationOp)
    {
        switch (truncationOp) {
        case Ext1OpType::I32TruncSatF32S:
            return TruncationKind::I32TruncF32S;
        case Ext1OpType::I32TruncSatF32U:
            return TruncationKind::I32TruncF32U;
        case Ext1OpType::I64TruncSatF32S:
            return TruncationKind::I64TruncF32S;
        case Ext1OpType::I64TruncSatF32U:
            return TruncationKind::I64TruncF32U;
        case Ext1OpType::I32TruncSatF64S:
            return TruncationKind::I32TruncF64S;
        case Ext1OpType::I32TruncSatF64U:
            return TruncationKind::I32TruncF64U;
        case Ext1OpType::I64TruncSatF64S:
            return TruncationKind::I64TruncF64S;
        case Ext1OpType::I64TruncSatF64U:
            return TruncationKind::I64TruncF64U;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Not a truncation op");
        }
    }

    FloatingPointRange BBQJIT::lookupTruncationRange(TruncationKind truncationKind)
    {
        Value min;
        Value max;
        bool closedLowerEndpoint = false;

        switch (truncationKind) {
        case TruncationKind::I32TruncF32S:
            closedLowerEndpoint = true;
            max = Value::fromF32(-static_cast<float>(std::numeric_limits<int32_t>::min()));
            min = Value::fromF32(static_cast<float>(std::numeric_limits<int32_t>::min()));
            break;
        case TruncationKind::I32TruncF32U:
            max = Value::fromF32(static_cast<float>(std::numeric_limits<int32_t>::min()) * static_cast<float>(-2.0));
            min = Value::fromF32(static_cast<float>(-1.0));
            break;
        case TruncationKind::I32TruncF64S:
            max = Value::fromF64(-static_cast<double>(std::numeric_limits<int32_t>::min()));
            min = Value::fromF64(static_cast<double>(std::numeric_limits<int32_t>::min()) - 1.0);
            break;
        case TruncationKind::I32TruncF64U:
            max = Value::fromF64(static_cast<double>(std::numeric_limits<int32_t>::min()) * -2.0);
            min = Value::fromF64(-1.0);
            break;
        case TruncationKind::I64TruncF32S:
            closedLowerEndpoint = true;
            max = Value::fromF32(-static_cast<float>(std::numeric_limits<int64_t>::min()));
            min = Value::fromF32(static_cast<float>(std::numeric_limits<int64_t>::min()));
            break;
        case TruncationKind::I64TruncF32U:
            max = Value::fromF32(static_cast<float>(std::numeric_limits<int64_t>::min()) * static_cast<float>(-2.0));
            min = Value::fromF32(static_cast<float>(-1.0));
            break;
        case TruncationKind::I64TruncF64S:
            closedLowerEndpoint = true;
            max = Value::fromF64(-static_cast<double>(std::numeric_limits<int64_t>::min()));
            min = Value::fromF64(static_cast<double>(std::numeric_limits<int64_t>::min()));
            break;
        case TruncationKind::I64TruncF64U:
            max = Value::fromF64(static_cast<double>(std::numeric_limits<int64_t>::min()) * -2.0);
            min = Value::fromF64(-1.0);
            break;
        }

        return FloatingPointRange { min, max, closedLowerEndpoint };
    }

#if USE(JSVALUE64)

    void BBQJIT::truncInBounds(TruncationKind truncationKind, Location operandLocation, Location resultLocation, FPRReg scratch1FPR, FPRReg scratch2FPR)
    {
        switch (truncationKind) {
        case TruncationKind::I32TruncF32S:
            m_jit.truncateFloatToInt32(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I32TruncF64S:
            m_jit.truncateDoubleToInt32(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I32TruncF32U:
            m_jit.truncateFloatToUint32(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I32TruncF64U:
            m_jit.truncateDoubleToUint32(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I64TruncF32S:
            m_jit.truncateFloatToInt64(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I64TruncF64S:
            m_jit.truncateDoubleToInt64(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I64TruncF32U: {
            if constexpr (isX86())
                emitMoveConst(Value::fromF32(static_cast<float>(std::numeric_limits<uint64_t>::max() - std::numeric_limits<int64_t>::max())), Location::fromFPR(scratch2FPR));
            m_jit.truncateFloatToUint64(operandLocation.asFPR(), resultLocation.asGPR(), scratch1FPR, scratch2FPR);
            break;
        }
        case TruncationKind::I64TruncF64U: {
            if constexpr (isX86())
                emitMoveConst(Value::fromF64(static_cast<double>(std::numeric_limits<uint64_t>::max() - std::numeric_limits<int64_t>::max())), Location::fromFPR(scratch2FPR));
            m_jit.truncateDoubleToUint64(operandLocation.asFPR(), resultLocation.asGPR(), scratch1FPR, scratch2FPR);
            break;
        }
        }
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::truncTrapping(OpType truncationOp, Value operand, Value& result, Type returnType, Type operandType)
    {
        ScratchScope<0, 2> scratches(*this);

        Location operandLocation;
        if (operand.isConst()) {
            operandLocation = Location::fromFPR(wasmScratchFPR);
            emitMoveConst(operand, operandLocation);
        } else
            operandLocation = loadIfNecessary(operand);
        ASSERT(operandLocation.isRegister());

        consume(operand); // Allow temp operand location to be reused

        result = topValue(returnType.kind);
        Location resultLocation = allocate(result);
        TruncationKind kind = truncationKind(truncationOp);
        auto range = lookupTruncationRange(kind);
        auto minFloatConst = range.min;
        auto maxFloatConst = range.max;
        Location minFloat = Location::fromFPR(scratches.fpr(0));
        Location maxFloat = Location::fromFPR(scratches.fpr(1));

        // FIXME: Can we do better isel here? Two floating-point constant materializations for every
        // trunc seems costly.
        emitMoveConst(minFloatConst, minFloat);
        emitMoveConst(maxFloatConst, maxFloat);

        LOG_INSTRUCTION("TruncSaturated", operand, operandLocation, RESULT(result));

        DoubleCondition minCondition = range.closedLowerEndpoint ? DoubleCondition::DoubleLessThanOrUnordered : DoubleCondition::DoubleLessThanOrEqualOrUnordered;
        Jump belowMin = operandType == Types::F32
            ? m_jit.branchFloat(minCondition, operandLocation.asFPR(), minFloat.asFPR())
            : m_jit.branchDouble(minCondition, operandLocation.asFPR(), minFloat.asFPR());
        throwExceptionIf(ExceptionType::OutOfBoundsTrunc, belowMin);

        Jump aboveMax = operandType == Types::F32
            ? m_jit.branchFloat(DoubleCondition::DoubleGreaterThanOrEqualOrUnordered, operandLocation.asFPR(), maxFloat.asFPR())
            : m_jit.branchDouble(DoubleCondition::DoubleGreaterThanOrEqualOrUnordered, operandLocation.asFPR(), maxFloat.asFPR());
        throwExceptionIf(ExceptionType::OutOfBoundsTrunc, aboveMax);

        truncInBounds(kind, operandLocation, resultLocation, scratches.fpr(0), scratches.fpr(1));

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::truncSaturated(Ext1OpType truncationOp, Value operand, Value& result, Type returnType, Type operandType)
    {
        ScratchScope<0, 2> scratches(*this);

        TruncationKind kind = truncationKind(truncationOp);
        auto range = lookupTruncationRange(kind);
        auto minFloatConst = range.min;
        auto maxFloatConst = range.max;
        Location minFloat = Location::fromFPR(scratches.fpr(0));
        Location maxFloat = Location::fromFPR(scratches.fpr(1));

        // FIXME: Can we do better isel here? Two floating-point constant materializations for every
        // trunc seems costly.
        emitMoveConst(minFloatConst, minFloat);
        emitMoveConst(maxFloatConst, maxFloat);

        // FIXME: Lots of this is duplicated from AirIRGeneratorBase. Might be nice to unify it?
        uint64_t minResult = 0;
        uint64_t maxResult = 0;
        switch (kind) {
        case TruncationKind::I32TruncF32S:
        case TruncationKind::I32TruncF64S:
            maxResult = bitwise_cast<uint32_t>(INT32_MAX);
            minResult = bitwise_cast<uint32_t>(INT32_MIN);
            break;
        case TruncationKind::I32TruncF32U:
        case TruncationKind::I32TruncF64U:
            maxResult = bitwise_cast<uint32_t>(UINT32_MAX);
            minResult = bitwise_cast<uint32_t>(0U);
            break;
        case TruncationKind::I64TruncF32S:
        case TruncationKind::I64TruncF64S:
            maxResult = bitwise_cast<uint64_t>(INT64_MAX);
            minResult = bitwise_cast<uint64_t>(INT64_MIN);
            break;
        case TruncationKind::I64TruncF32U:
        case TruncationKind::I64TruncF64U:
            maxResult = bitwise_cast<uint64_t>(UINT64_MAX);
            minResult = bitwise_cast<uint64_t>(0ULL);
            break;
        }

        Location operandLocation;
        if (operand.isConst()) {
            operandLocation = Location::fromFPR(wasmScratchFPR);
            emitMoveConst(operand, operandLocation);
        } else
            operandLocation = loadIfNecessary(operand);
        ASSERT(operandLocation.isRegister());

        consume(operand); // Allow temp operand location to be reused

        result = topValue(returnType.kind);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("TruncSaturated", operand, operandLocation, RESULT(result));

        Jump lowerThanMin = operandType == Types::F32
            ? m_jit.branchFloat(DoubleCondition::DoubleLessThanOrEqualOrUnordered, operandLocation.asFPR(), minFloat.asFPR())
            : m_jit.branchDouble(DoubleCondition::DoubleLessThanOrEqualOrUnordered, operandLocation.asFPR(), minFloat.asFPR());
        Jump higherThanMax = operandType == Types::F32
            ? m_jit.branchFloat(DoubleCondition::DoubleGreaterThanOrEqualOrUnordered, operandLocation.asFPR(), maxFloat.asFPR())
            : m_jit.branchDouble(DoubleCondition::DoubleGreaterThanOrEqualOrUnordered, operandLocation.asFPR(), maxFloat.asFPR());

        // In-bounds case. Emit normal truncation instructions.
        truncInBounds(kind, operandLocation, resultLocation, scratches.fpr(0), scratches.fpr(1));

        Jump afterInBounds = m_jit.jump();

        // Below-minimum case.
        lowerThanMin.link(&m_jit);

        // As an optimization, if the min result is 0; we can unconditionally return
        // that if the above-minimum-range check fails; otherwise, we need to check
        // for NaN since it also will fail the above-minimum-range-check
        if (!minResult) {
            if (returnType == Types::I32)
                m_jit.move(TrustedImm32(0), resultLocation.asGPR());
            else
                m_jit.move(TrustedImm64(0), resultLocation.asGPR());
        } else {
            Jump isNotNaN = operandType == Types::F32
                ? m_jit.branchFloat(DoubleCondition::DoubleEqualAndOrdered, operandLocation.asFPR(), operandLocation.asFPR())
                : m_jit.branchDouble(DoubleCondition::DoubleEqualAndOrdered, operandLocation.asFPR(), operandLocation.asFPR());

            // NaN case. Set result to zero.
            if (returnType == Types::I32)
                m_jit.move(TrustedImm32(0), resultLocation.asGPR());
            else
                m_jit.move(TrustedImm64(0), resultLocation.asGPR());
            Jump afterNaN = m_jit.jump();

            // Non-NaN case. Set result to the minimum value.
            isNotNaN.link(&m_jit);
            emitMoveConst(returnType == Types::I32 ? Value::fromI32(static_cast<int32_t>(minResult)) : Value::fromI64(static_cast<int64_t>(minResult)), resultLocation);
            afterNaN.link(&m_jit);
        }
        Jump afterMin = m_jit.jump();

        // Above maximum case.
        higherThanMax.link(&m_jit);
        emitMoveConst(returnType == Types::I32 ? Value::fromI32(static_cast<int32_t>(maxResult)) : Value::fromI64(static_cast<int64_t>(maxResult)), resultLocation);

        afterInBounds.link(&m_jit);
        afterMin.link(&m_jit);

        return { };
    }

#else

    void BBQJIT::truncInBounds32_64(TruncationKind truncationKind, Location operandLocation, Value& result, Location resultLocation)
    {
        switch (truncationKind) {
        case TruncationKind::I32TruncF32S:
            m_jit.truncateFloatToInt32(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I32TruncF64S:
            m_jit.truncateDoubleToInt32(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I32TruncF32U:
            m_jit.truncateFloatToUint32(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I32TruncF64U:
            m_jit.truncateDoubleToUint32(operandLocation.asFPR(), resultLocation.asGPR());
            break;
        case TruncationKind::I64TruncF32S: {
            auto operand = Value::pinned(TypeKind::F32, operandLocation);
            consume(result);
            emitCCall(Math::i64_trunc_s_f32, Vector<Value> { operand }, result);
            break;
        }
        case TruncationKind::I64TruncF64S: {
            auto operand = Value::pinned(TypeKind::F64, operandLocation);
            consume(result);
            emitCCall(Math::i64_trunc_s_f64, Vector<Value> { operand }, result);
            break;
        }
        case TruncationKind::I64TruncF32U: {
            auto operand = Value::pinned(TypeKind::F32, operandLocation);
            consume(result);
            emitCCall(Math::i64_trunc_u_f32, Vector<Value> { operand }, result);
            break;
        }
        case TruncationKind::I64TruncF64U: {
            auto operand = Value::pinned(TypeKind::F64, operandLocation);
            consume(result);
            emitCCall(Math::i64_trunc_u_f64, Vector<Value> { operand }, result);
            break;
        }
        }
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::truncTrapping(OpType truncationOp, Value operand, Value& result, Type returnType, Type operandType)
    {
        ScratchScope<0, 2> scratches(*this);

        Location operandLocation;
        if (operand.isConst()) {
            operandLocation = Location::fromFPR(wasmScratchFPR);
            emitMoveConst(operand, operandLocation);
        } else
            operandLocation = loadIfNecessary(operand);
        ASSERT(operandLocation.isRegister());

        consume(operand); // Allow temp operand location to be reused

        result = topValue(returnType.kind);
        Location resultLocation = allocate(result);
        TruncationKind kind = truncationKind(truncationOp);
        auto range = lookupTruncationRange(kind);
        auto minFloatConst = range.min;
        auto maxFloatConst = range.max;
        Location minFloat = Location::fromFPR(scratches.fpr(0));
        Location maxFloat = Location::fromFPR(scratches.fpr(1));

        // FIXME: Can we do better isel here? Two floating-point constant materializations for every
        // trunc seems costly.
        emitMoveConst(minFloatConst, minFloat);
        emitMoveConst(maxFloatConst, maxFloat);

        LOG_INSTRUCTION("TruncSaturated", operand, operandLocation, RESULT(result));

        DoubleCondition minCondition = range.closedLowerEndpoint ? DoubleCondition::DoubleLessThanOrUnordered : DoubleCondition::DoubleLessThanOrEqualOrUnordered;
        Jump belowMin = operandType == Types::F32
            ? m_jit.branchFloat(minCondition, operandLocation.asFPR(), minFloat.asFPR())
            : m_jit.branchDouble(minCondition, operandLocation.asFPR(), minFloat.asFPR());
        throwExceptionIf(ExceptionType::OutOfBoundsTrunc, belowMin);

        Jump aboveMax = operandType == Types::F32
            ? m_jit.branchFloat(DoubleCondition::DoubleGreaterThanOrEqualOrUnordered, operandLocation.asFPR(), maxFloat.asFPR())
            : m_jit.branchDouble(DoubleCondition::DoubleGreaterThanOrEqualOrUnordered, operandLocation.asFPR(), maxFloat.asFPR());
        throwExceptionIf(ExceptionType::OutOfBoundsTrunc, aboveMax);

        truncInBounds32_64(kind, operandLocation, result, resultLocation);

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::truncSaturated(Ext1OpType truncationOp, Value operand, Value& result, Type returnType, Type operandType)
    {
        ScratchScope<0, 2> scratches(*this);

        TruncationKind kind = truncationKind(truncationOp);
        auto range = lookupTruncationRange(kind);
        auto minFloatConst = range.min;
        auto maxFloatConst = range.max;
        Location minFloat = Location::fromFPR(scratches.fpr(0));
        Location maxFloat = Location::fromFPR(scratches.fpr(1));

        // FIXME: Can we do better isel here? Two floating-point constant materializations for every
        // trunc seems costly.
        emitMoveConst(minFloatConst, minFloat);
        emitMoveConst(maxFloatConst, maxFloat);

        // FIXME: Lots of this is duplicated from AirIRGeneratorBase. Might be nice to unify it?
        uint64_t minResult = 0;
        uint64_t maxResult = 0;
        switch (kind) {
        case TruncationKind::I32TruncF32S:
        case TruncationKind::I32TruncF64S:
            maxResult = bitwise_cast<uint32_t>(INT32_MAX);
            minResult = bitwise_cast<uint32_t>(INT32_MIN);
            break;
        case TruncationKind::I32TruncF32U:
        case TruncationKind::I32TruncF64U:
            maxResult = bitwise_cast<uint32_t>(UINT32_MAX);
            minResult = bitwise_cast<uint32_t>(0U);
            break;
        case TruncationKind::I64TruncF32S:
        case TruncationKind::I64TruncF64S:
            maxResult = bitwise_cast<uint64_t>(INT64_MAX);
            minResult = bitwise_cast<uint64_t>(INT64_MIN);
            break;
        case TruncationKind::I64TruncF32U:
        case TruncationKind::I64TruncF64U:
            maxResult = bitwise_cast<uint64_t>(UINT64_MAX);
            minResult = bitwise_cast<uint64_t>(0ULL);
            break;
        }

        Location operandLocation;
        if (operand.isConst()) {
            operandLocation = Location::fromFPR(wasmScratchFPR);
            emitMoveConst(operand, operandLocation);
        } else
            operandLocation = loadIfNecessary(operand);
        ASSERT(operandLocation.isRegister());

        consume(operand); // Allow temp operand location to be reused

        result = topValue(returnType.kind);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("TruncSaturated", operand, operandLocation, RESULT(result));

        Jump lowerThanMin = operandType == Types::F32
            ? m_jit.branchFloat(DoubleCondition::DoubleLessThanOrEqualOrUnordered, operandLocation.asFPR(), minFloat.asFPR())
            : m_jit.branchDouble(DoubleCondition::DoubleLessThanOrEqualOrUnordered, operandLocation.asFPR(), minFloat.asFPR());
        Jump higherThanMax = operandType == Types::F32
            ? m_jit.branchFloat(DoubleCondition::DoubleGreaterThanOrEqualOrUnordered, operandLocation.asFPR(), maxFloat.asFPR())
            : m_jit.branchDouble(DoubleCondition::DoubleGreaterThanOrEqualOrUnordered, operandLocation.asFPR(), maxFloat.asFPR());

        // In-bounds case. Emit normal truncation instructions.
        truncInBounds32_64(kind, operandLocation, result, resultLocation);
        resultLocation = locationOf(result);

        Jump afterInBounds = m_jit.jump();

        // Below-minimum case.
        lowerThanMin.link(&m_jit);

        // As an optimization, if the min result is 0; we can unconditionally return
        // that if the above-minimum-range check fails; otherwise, we need to check
        // for NaN since it also will fail the above-minimum-range-check
        if (!minResult) {
            if (returnType == Types::I32)
                m_jit.move(TrustedImm32(0), resultLocation.asGPR());
            else {
                m_jit.move(TrustedImm32(0), resultLocation.asGPRlo());
                m_jit.move(TrustedImm32(0), resultLocation.asGPRhi());
            }
        } else {
            Jump isNotNaN = operandType == Types::F32
                ? m_jit.branchFloat(DoubleCondition::DoubleEqualAndOrdered, operandLocation.asFPR(), operandLocation.asFPR())
                : m_jit.branchDouble(DoubleCondition::DoubleEqualAndOrdered, operandLocation.asFPR(), operandLocation.asFPR());

            // NaN case. Set result to zero.
            if (returnType == Types::I32)
                m_jit.move(TrustedImm32(0), resultLocation.asGPR());
            else {
                m_jit.move(TrustedImm32(0), resultLocation.asGPRlo());
                m_jit.move(TrustedImm32(0), resultLocation.asGPRhi());
            }
            Jump afterNaN = m_jit.jump();

            // Non-NaN case. Set result to the minimum value.
            isNotNaN.link(&m_jit);
            emitMoveConst(returnType == Types::I32 ? Value::fromI32(static_cast<int32_t>(minResult)) : Value::fromI64(static_cast<int64_t>(minResult)), resultLocation);
            afterNaN.link(&m_jit);
        }
        Jump afterMin = m_jit.jump();

        // Above maximum case.
        higherThanMax.link(&m_jit);
        emitMoveConst(returnType == Types::I32 ? Value::fromI32(static_cast<int32_t>(maxResult)) : Value::fromI64(static_cast<int64_t>(maxResult)), resultLocation);

        afterInBounds.link(&m_jit);
        afterMin.link(&m_jit);

        return { };
    }

#endif

    // GC
#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefI31(ExpressionType value, ExpressionType& result)
    {
        if (value.isConst()) {
            result = Value::fromI64((((value.asI32() & 0x7fffffff) << 1) >> 1) | JSValue::NumberTag);
            LOG_INSTRUCTION("RefI31", value, RESULT(result));
            return { };
        }

        Location initialValue = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::I64);
        Location resultLocation = allocateWithHint(result, initialValue);

        LOG_INSTRUCTION("RefI31", value, RESULT(result));

        m_jit.and32(TrustedImm32(0x7fffffff), initialValue.asGPR(), resultLocation.asGPR());
        m_jit.lshift32(TrustedImm32(1), resultLocation.asGPR());
        m_jit.rshift32(TrustedImm32(1), resultLocation.asGPR());
        m_jit.or64(TrustedImm64(JSValue::NumberTag), resultLocation.asGPR());
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefI31(ExpressionType value, ExpressionType& result)
    {
        if (value.isConst()) {
            result = Value::fromI64(JSValue::encode(JSValue(JSValue::Int32Tag, value.asI32() & 0x7fffffff)));
            LOG_INSTRUCTION("RefI31", value, RESULT(result));
            return { };
        }

        Location initialValue = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::I64);
        Location resultLocation = allocateWithHint(result, initialValue);

        LOG_INSTRUCTION("RefI31", value, RESULT(result));
        m_jit.and32(TrustedImm32(0x7fffffff), initialValue.asGPR(), resultLocation.asGPRlo());
        m_jit.move(TrustedImm32(JSValue::Int32Tag), resultLocation.asGPRhi());
        return { };
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI31GetS(ExpressionType value, ExpressionType& result)
    {
        if (value.isConst()) {
            if (JSValue::decode(value.asI64()).isNumber())
                result = Value::fromI32((value.asI64() << 33) >> 33);
            else {
                emitThrowException(ExceptionType::NullI31Get);
                result = Value::fromI32(0);
            }

            LOG_INSTRUCTION("I31GetS", value, RESULT(result));

            return { };
        }


        Location initialValue = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::I64);
        Location resultLocation = allocateWithHint(result, initialValue);

        LOG_INSTRUCTION("I31GetS", value, RESULT(result));

        m_jit.move(initialValue.asGPR(), resultLocation.asGPR());
        emitThrowOnNullReference(ExceptionType::NullI31Get, resultLocation);

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI31GetS(ExpressionType value, ExpressionType& result)
    {
        if (value.isConst()) {
            if (JSValue::decode(value.asI64()).isNumber())
                result = Value::fromI32((value.asI64() << 33) >> 33);
            else {
                emitThrowException(ExceptionType::NullI31Get);
                result = Value::fromI32(0);
            }

            LOG_INSTRUCTION("I31GetS", value, RESULT(result));

            return { };
        }


        Location initialValue = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::I32);
        Location resultLocation = allocateWithHint(result, initialValue);

        LOG_INSTRUCTION("I31GetS", value, RESULT(result));

        emitThrowOnNullReference(ExceptionType::NullI31Get, initialValue);
        m_jit.move(initialValue.asGPRlo(), resultLocation.asGPR());

        m_jit.lshift32(TrustedImm32(1), resultLocation.asGPR());
        m_jit.rshift32(TrustedImm32(1), resultLocation.asGPR());
        return { };
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI31GetU(ExpressionType value, ExpressionType& result)
    {
        if (value.isConst()) {
            if (JSValue::decode(value.asI64()).isNumber())
                result = Value::fromI32(value.asI64() & 0x7fffffffu);
            else {
                emitThrowException(ExceptionType::NullI31Get);
                result = Value::fromI32(0);
            }

            LOG_INSTRUCTION("I31GetU", value, RESULT(result));

            return { };
        }


        Location initialValue = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::I64);
        Location resultLocation = allocateWithHint(result, initialValue);

        LOG_INSTRUCTION("I31GetU", value, RESULT(result));

        m_jit.move(initialValue.asGPR(), resultLocation.asGPR());
        emitThrowOnNullReference(ExceptionType::NullI31Get, resultLocation);
        m_jit.and32(TrustedImm32(0x7fffffff), resultLocation.asGPR(), resultLocation.asGPR());

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI31GetU(ExpressionType value, ExpressionType& result)
    {
        if (value.isConst()) {
            if (JSValue::decode(value.asI64()).isNumber())
                result = Value::fromI32(value.asI64());
            else {
                emitThrowException(ExceptionType::NullI31Get);
                result = Value::fromI32(0);
            }

            LOG_INSTRUCTION("I31GetU", value, RESULT(result));

            return { };
        }


        Location initialValue = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::I32);
        Location resultLocation = allocateWithHint(result, initialValue);

        LOG_INSTRUCTION("I31GetU", value, RESULT(result));

        emitThrowOnNullReference(ExceptionType::NullI31Get, initialValue);
        m_jit.move(initialValue.asGPRlo(), resultLocation.asGPR());

        return { };
    }
#endif

    const Ref<TypeDefinition> BBQJIT::getTypeDefinition(uint32_t typeIndex) { return m_info.typeSignatures[typeIndex]; }

    // Given a type index, verify that it's an array type and return its expansion
    const ArrayType* BBQJIT::getArrayTypeDefinition(uint32_t typeIndex)
    {
        Ref<Wasm::TypeDefinition> typeDef = getTypeDefinition(typeIndex);
        const Wasm::TypeDefinition& arraySignature = typeDef->expand();
        return arraySignature.as<ArrayType>();
    }

    // Given a type index for an array signature, look it up, expand it and
    // return the element type
    StorageType BBQJIT::getArrayElementType(uint32_t typeIndex)
    {
        const ArrayType* arrayType = getArrayTypeDefinition(typeIndex);
        return arrayType->elementType().type;
    }

    // This will replace the existing value with a new value. Note that if this is an F32 then the top bits may be garbage but that's ok for our current usage.
#if USE(JSVALUE64)
    Value BBQJIT::marshallToI64(Value value)
    {
        ASSERT(!value.isLocal());
        if (value.type() == TypeKind::F32 || value.type() == TypeKind::F64) {
            if (value.isConst())
                return Value::fromI64(value.type() == TypeKind::F32 ? bitwise_cast<uint32_t>(value.asI32()) : bitwise_cast<uint64_t>(value.asF64()));
            // This is a bit silly. We could just move initValue to the right argument GPR if we know it's in an FPR already.
            flushValue(value);
            return Value::fromTemp(TypeKind::I64, value.asTemp());
        }
        return value;
    }
#else
    Value BBQJIT::marshallToI64(Value value)
    {
        ASSERT(!value.isLocal());
        if (value.isConst()) {
            switch (value.type()) {
            case TypeKind::F64:
                return Value::fromI64(bitwise_cast<uint64_t>(value.asF64()));
            case TypeKind::F32:
            case TypeKind::I32:
                return Value::fromI64(bitwise_cast<uint32_t>(value.asI32()));
            default:
                return value;
            }
        }
        switch (value.type()) {
        case TypeKind::F64:
            flushValue(value);
            return Value::fromTemp(TypeKind::I64, value.asTemp());
        case TypeKind::F32:
        case TypeKind::I32: {
            flushValue(value);
            Location slot = canonicalSlot(value);
            m_jit.store32(TrustedImm32(0), slot.asAddress().withOffset(4));
            return Value::fromTemp(TypeKind::I64, value.asTemp());
        }
        default:
            return value;
        }
    }
#endif


    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayNew(uint32_t typeIndex, ExpressionType size, ExpressionType initValue, ExpressionType& result)
    {
        initValue = marshallToI64(initValue);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
            size,
            initValue,
        };
        result = topValue(TypeKind::Arrayref);
        emitCCall(operationWasmArrayNew, arguments, result);

        LOG_INSTRUCTION("ArrayNew", typeIndex, size, initValue, RESULT(result));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayNewDefault(uint32_t typeIndex, ExpressionType size, ExpressionType& result)
    {
        StorageType arrayElementType = getArrayElementType(typeIndex);
        Value initValue = Value::fromI64(isRefType(arrayElementType) ? JSValue::encode(jsNull()) : 0);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
            size,
            initValue,
        };
        result = topValue(TypeKind::Arrayref);
        emitCCall(&operationWasmArrayNew, arguments, result);

        LOG_INSTRUCTION("ArrayNewDefault", typeIndex, size, RESULT(result));
        return { };
    }

    using arraySegmentOperation = EncodedJSValue (&)(JSC::Wasm::Instance*, uint32_t, uint32_t, uint32_t, uint32_t);
    void BBQJIT::pushArrayNewFromSegment(arraySegmentOperation operation, uint32_t typeIndex, uint32_t segmentIndex, ExpressionType arraySize, ExpressionType offset, ExceptionType exceptionType, ExpressionType& result)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
            Value::fromI32(segmentIndex),
            arraySize,
            offset,
        };
        result = topValue(TypeKind::I64);
        emitCCall(operation, arguments, result);
        Location resultLocation = allocate(result);

        emitThrowOnNullReference(exceptionType, resultLocation);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayNewData(uint32_t typeIndex, uint32_t dataIndex, ExpressionType arraySize, ExpressionType offset, ExpressionType& result)
    {
        pushArrayNewFromSegment(operationWasmArrayNewData, typeIndex, dataIndex, arraySize, offset, ExceptionType::OutOfBoundsDataSegmentAccess, result);
        LOG_INSTRUCTION("ArrayNewData", typeIndex, dataIndex, arraySize, offset, RESULT(result));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayNewElem(uint32_t typeIndex, uint32_t elemSegmentIndex, ExpressionType arraySize, ExpressionType offset, ExpressionType& result)
    {
        pushArrayNewFromSegment(operationWasmArrayNewElem, typeIndex, elemSegmentIndex, arraySize, offset, ExceptionType::OutOfBoundsElementSegmentAccess, result);
        LOG_INSTRUCTION("ArrayNewElem", typeIndex, elemSegmentIndex, arraySize, offset, RESULT(result));
        return { };
    }

    void BBQJIT::emitArraySetUnchecked(uint32_t typeIndex, Value arrayref, Value index, Value value)
    {
        value = marshallToI64(value);

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
            arrayref,
            index,
            value,
        };
        // FIXME: Emit this inline.
        // https://bugs.webkit.org/show_bug.cgi?id=245405
        emitCCall(operationWasmArraySet, arguments);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayNewFixed(uint32_t typeIndex, Vector<ExpressionType>& args, ExpressionType& result)
    {
        // Allocate an uninitialized array whose length matches the argument count
        // FIXME: inline the allocation.
        // https://bugs.webkit.org/show_bug.cgi?id=244388
        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
            Value::fromI32(args.size()),
        };
        Value allocationResult = Value::fromTemp(TypeKind::Arrayref, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + m_parser->expressionStack().size() + args.size());
        emitCCall(operationWasmArrayNewEmpty, arguments, allocationResult);

        Location allocationResultLocation = allocate(allocationResult);
        emitThrowOnNullReference(ExceptionType::NullArraySet, allocationResultLocation);

        for (uint32_t i = 0; i < args.size(); ++i) {
            // Emit the array set code -- note that this omits the bounds check, since
            // if operationWasmArrayNewEmpty() returned a non-null value, it's an array of the right size
            allocationResultLocation = loadIfNecessary(allocationResult);
            Value pinnedResult = Value::pinned(TypeKind::I64, allocationResultLocation);
            emitArraySetUnchecked(typeIndex, pinnedResult, Value::fromI32(i), args[i]);
        }

        result = topValue(TypeKind::Arrayref);
        Location resultLocation = allocate(result);
        emitMove(allocationResult, resultLocation);
        // If args.isEmpty() then allocationResult.asTemp() == result.asTemp() so we will consume our result.
        if (args.size())
            consume(allocationResult);

        LOG_INSTRUCTION("ArrayNewFixed", typeIndex, args.size(), RESULT(result));
        return { };
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayGet(ExtGCOpType arrayGetKind, uint32_t typeIndex, ExpressionType arrayref, ExpressionType index, ExpressionType& result)
    {
        StorageType elementType = getArrayElementType(typeIndex);
        Type resultType = elementType.unpacked();

        if (arrayref.isConst()) {
            ASSERT(arrayref.asI64() == JSValue::encode(jsNull()));
            emitThrowException(ExceptionType::NullArrayGet);
            result = Value::fromRef(resultType.kind, 0);
            return { };
        }

        Location arrayLocation = loadIfNecessary(arrayref);
        emitThrowOnNullReference(ExceptionType::NullArrayGet, arrayLocation);

        if (index.isConst()) {
            m_jit.load32(MacroAssembler::Address(arrayLocation.asGPR(), JSWebAssemblyArray::offsetOfSize()), wasmScratchGPR);
            throwExceptionIf(ExceptionType::OutOfBoundsArrayGet,
                m_jit.branch32(MacroAssembler::BelowOrEqual, wasmScratchGPR, TrustedImm32(index.asI32())));
        } else {
            Location indexLocation = loadIfNecessary(index);
            throwExceptionIf(ExceptionType::OutOfBoundsArrayGet,
                m_jit.branch32(MacroAssembler::AboveOrEqual, indexLocation.asGPR(), MacroAssembler::Address(arrayLocation.asGPR(), JSWebAssemblyArray::offsetOfSize())));
        }

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
            arrayref,
            index,
        };

        Value getResult = topValue(TypeKind::I64);
        emitCCall(operationWasmArrayGet, arguments, getResult);
        Location getResultLocation = loadIfNecessary(getResult);

        if (isFloatingPointType(resultType.kind)) {
            consume(getResult);
            result = topValue(resultType.kind);
            Location resultLocation = allocate(result);
            m_jit.move64ToDouble(getResultLocation.asGPR(), resultLocation.asFPR());
        } else
            result = getResult;

        switch (arrayGetKind) {
        case ExtGCOpType::ArrayGet:
            LOG_INSTRUCTION("ArrayGet", typeIndex, arrayref, index, RESULT(result));
            break;
        case ExtGCOpType::ArrayGetU:
            ASSERT(resultType.kind == TypeKind::I32);

            LOG_INSTRUCTION("ArrayGetU", typeIndex, arrayref, index, RESULT(result));
            break;
        case ExtGCOpType::ArrayGetS: {
            ASSERT(resultType.kind == TypeKind::I32);
            size_t elementSize = elementType.as<PackedType>() == PackedType::I8 ? sizeof(uint8_t) : sizeof(uint16_t);
            uint8_t bitShift = (sizeof(uint32_t) - elementSize) * 8;
            Location resultLocation = allocate(result);

            m_jit.lshift32(TrustedImm32(bitShift), resultLocation.asGPR());
            m_jit.rshift32(TrustedImm32(bitShift), resultLocation.asGPR());
            LOG_INSTRUCTION("ArrayGetS", typeIndex, arrayref, index, RESULT(result));
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return { };
        }
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayGet(ExtGCOpType arrayGetKind, uint32_t typeIndex, ExpressionType arrayref, ExpressionType index, ExpressionType& result)
    {
        StorageType elementType = getArrayElementType(typeIndex);
        Type resultType = elementType.unpacked();

        if (arrayref.isConst()) {
            ASSERT(arrayref.asI64() == JSValue::encode(jsNull()));
            emitThrowException(ExceptionType::NullArrayGet);
            result = Value::fromRef(resultType.kind, 0);
            return { };
        }

        Location arrayLocation = loadIfNecessary(arrayref);
        emitThrowOnNullReference(ExceptionType::NullArrayGet, arrayLocation);

        if (index.isConst()) {
            m_jit.load32(MacroAssembler::Address(arrayLocation.asGPRlo(), JSWebAssemblyArray::offsetOfSize()), wasmScratchGPR);
            throwExceptionIf(ExceptionType::OutOfBoundsArrayGet,
                m_jit.branch32(MacroAssembler::BelowOrEqual, wasmScratchGPR, TrustedImm32(index.asI32())));
        } else {
            Location indexLocation = loadIfNecessary(index);
            throwExceptionIf(ExceptionType::OutOfBoundsArrayGet,
                m_jit.branch32(MacroAssembler::AboveOrEqual, indexLocation.asGPR(), MacroAssembler::Address(arrayLocation.asGPRlo(), JSWebAssemblyArray::offsetOfSize())));
        }

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
            arrayref,
            index,
        };

        Value getResult = topValue(TypeKind::I64);
        emitCCall(operationWasmArrayGet, arguments, getResult);
        Location getResultLocation = loadIfNecessary(getResult);

        if (isFloatingPointType(resultType.kind)) {
            consume(getResult);
            result = topValue(resultType.kind);
            Location resultLocation = allocate(result);
            m_jit.move64ToDouble(getResultLocation.asGPRhi(), getResultLocation.asGPRlo(), resultLocation.asFPR());
        } else if (resultType.kind == TypeKind::I32) {
            consume(getResult);
            result = topValue(resultType.kind);
            Location resultLocation = allocate(result);
            m_jit.move(getResultLocation.asGPRlo(), resultLocation.asGPR());
        } else
            result = getResult;

        switch (arrayGetKind) {
        case ExtGCOpType::ArrayGet:
            LOG_INSTRUCTION("ArrayGet", typeIndex, arrayref, index, RESULT(result));
            break;
        case ExtGCOpType::ArrayGetU:
            ASSERT(resultType.kind == TypeKind::I32);

            LOG_INSTRUCTION("ArrayGetU", typeIndex, arrayref, index, RESULT(result));
            break;
        case ExtGCOpType::ArrayGetS: {
            ASSERT(resultType.kind == TypeKind::I32);
            size_t elementSize = elementType.as<PackedType>() == PackedType::I8 ? sizeof(uint8_t) : sizeof(uint16_t);
            uint8_t bitShift = (sizeof(uint32_t) - elementSize) * 8;
            Location resultLocation = allocate(result);

            m_jit.lshift32(TrustedImm32(bitShift), resultLocation.asGPR());
            m_jit.rshift32(TrustedImm32(bitShift), resultLocation.asGPR());
            LOG_INSTRUCTION("ArrayGetS", typeIndex, arrayref, index, RESULT(result));
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return { };
        }
        return { };
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addArraySet(uint32_t typeIndex, ExpressionType arrayref, ExpressionType index, ExpressionType value)
    {
        if (arrayref.isConst()) {
            ASSERT(arrayref.asI64() == JSValue::encode(jsNull()));
            emitThrowException(ExceptionType::NullArraySet);
            return { };
        }

        Location arrayLocation = loadIfNecessary(arrayref);
        emitThrowOnNullReference(ExceptionType::NullArraySet, arrayLocation);

        if (index.isConst()) {
            m_jit.load32(MacroAssembler::Address(arrayLocation.asGPR(), JSWebAssemblyArray::offsetOfSize()), wasmScratchGPR);
            throwExceptionIf(ExceptionType::OutOfBoundsArraySet,
                m_jit.branch32(MacroAssembler::BelowOrEqual, wasmScratchGPR, TrustedImm32(index.asI32())));
        } else {
            Location indexLocation = loadIfNecessary(index);
            throwExceptionIf(ExceptionType::OutOfBoundsArraySet,
                m_jit.branch32(MacroAssembler::AboveOrEqual, indexLocation.asGPR(), MacroAssembler::Address(arrayLocation.asGPR(), JSWebAssemblyArray::offsetOfSize())));
        }

        emitArraySetUnchecked(typeIndex, arrayref, index, value);

        LOG_INSTRUCTION("ArraySet", typeIndex, arrayref, index, value);
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addArraySet(uint32_t typeIndex, ExpressionType arrayref, ExpressionType index, ExpressionType value)
    {
        if (arrayref.isConst()) {
            ASSERT(arrayref.asI64() == JSValue::encode(jsNull()));
            emitThrowException(ExceptionType::NullArraySet);
            return { };
        }

        Location arrayLocation = loadIfNecessary(arrayref);
        emitThrowOnNullReference(ExceptionType::NullArraySet, arrayLocation);

        if (index.isConst()) {
            m_jit.load32(MacroAssembler::Address(arrayLocation.asGPRlo(), JSWebAssemblyArray::offsetOfSize()), wasmScratchGPR);
            throwExceptionIf(ExceptionType::OutOfBoundsArraySet,
                m_jit.branch32(MacroAssembler::BelowOrEqual, wasmScratchGPR, TrustedImm32(index.asI32())));
        } else {
            Location indexLocation = loadIfNecessary(index);
            throwExceptionIf(ExceptionType::OutOfBoundsArraySet,
                m_jit.branch32(MacroAssembler::AboveOrEqual, indexLocation.asGPR(), MacroAssembler::Address(arrayLocation.asGPRlo(), JSWebAssemblyArray::offsetOfSize())));
        }

        emitArraySetUnchecked(typeIndex, arrayref, index, value);

        LOG_INSTRUCTION("ArraySet", typeIndex, arrayref, index, value);
        return { };
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayLen(ExpressionType arrayref, ExpressionType& result)
    {
        if (arrayref.isConst()) {
            ASSERT(arrayref.asI64() == JSValue::encode(jsNull()));
            emitThrowException(ExceptionType::NullArrayLen);
            result = Value::fromI32(0);
            LOG_INSTRUCTION("ArrayLen", arrayref, RESULT(result), "Exception");
            return { };
        }

        Location arrayLocation = loadIfNecessary(arrayref);
        consume(arrayref);
        emitThrowOnNullReference(ExceptionType::NullArrayLen, arrayLocation);

        result = topValue(TypeKind::I32);
        Location resultLocation = allocateWithHint(result, arrayLocation);
        m_jit.load32(MacroAssembler::Address(arrayLocation.asGPR(), JSWebAssemblyArray::offsetOfSize()), resultLocation.asGPR());

        LOG_INSTRUCTION("ArrayLen", arrayref, RESULT(result));
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addArrayLen(ExpressionType arrayref, ExpressionType& result)
    {
        if (arrayref.isConst()) {
            ASSERT(arrayref.asI64() == JSValue::encode(jsNull()));
            emitThrowException(ExceptionType::NullArrayLen);
            result = Value::fromI32(0);
            LOG_INSTRUCTION("ArrayLen", arrayref, RESULT(result), "Exception");
            return { };
        }

        Location arrayLocation = loadIfNecessary(arrayref);
        consume(arrayref);
        emitThrowOnNullReference(ExceptionType::NullArrayLen, arrayLocation);

        result = topValue(TypeKind::I32);
        Location resultLocation = allocateWithHint(result, arrayLocation);
        m_jit.load32(MacroAssembler::Address(arrayLocation.asGPRlo(), JSWebAssemblyArray::offsetOfSize()), resultLocation.asGPR());

        LOG_INSTRUCTION("ArrayLen", arrayref, RESULT(result));
        return { };
    }
#endif

#if USE(JSVALUE64)
    void BBQJIT::emitStructSet(GPRReg structGPR, const StructType& structType, uint32_t fieldIndex, Value value)
    {
        m_jit.loadPtr(MacroAssembler::Address(structGPR, JSWebAssemblyStruct::offsetOfPayload()), wasmScratchGPR);
        unsigned fieldOffset = *structType.offsetOfField(fieldIndex);
        RELEASE_ASSERT((std::numeric_limits<int32_t>::max() & fieldOffset) == fieldOffset);

        TypeKind kind = toValueKind(structType.field(fieldIndex).type.unpacked().kind);
        if (value.isConst()) {
            switch (kind) {
            case TypeKind::I32:
                if (structType.field(fieldIndex).type.is<PackedType>()) {
                    ScratchScope<1, 0> scratches(*this);
                    // If it's a packed type, we materialize the constant to ensure constant blinding.
                    emitMoveConst(value, Location::fromGPR(scratches.gpr(0)));
                    switch (structType.field(fieldIndex).type.as<PackedType>()) {
                    case PackedType::I8:
                        m_jit.store8(scratches.gpr(0), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                        break;
                    case PackedType::I16:
                        m_jit.store16(scratches.gpr(0), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                        break;
                    }
                    break;
                }
                m_jit.store32(MacroAssembler::Imm32(value.asI32()), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                break;
            case TypeKind::F32:
                m_jit.store32(MacroAssembler::Imm32(value.asI32()), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                break;
            case TypeKind::I64:
            case TypeKind::F64:
                m_jit.store64(MacroAssembler::Imm64(value.asI64()), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
            return;
        }

        Location valueLocation = loadIfNecessary(value);
        switch (kind) {
        case TypeKind::I32:
            if (structType.field(fieldIndex).type.is<PackedType>()) {
                switch (structType.field(fieldIndex).type.as<PackedType>()) {
                case PackedType::I8:
                    m_jit.store8(valueLocation.asGPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                    break;
                case PackedType::I16:
                    m_jit.store16(valueLocation.asGPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                    break;
                }
                break;
            }
            m_jit.store32(valueLocation.asGPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
            break;
        case TypeKind::I64:
            m_jit.store64(valueLocation.asGPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
            break;
        case TypeKind::F32:
            m_jit.storeFloat(valueLocation.asFPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
            break;
        case TypeKind::F64:
            m_jit.storeDouble(valueLocation.asFPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        consume(value);
    }
#else
    void BBQJIT::emitStructSet(GPRReg structGPR, const StructType& structType, uint32_t fieldIndex, Value value)
    {
        m_jit.loadPtr(MacroAssembler::Address(structGPR, JSWebAssemblyStruct::offsetOfPayload()), wasmScratchGPR);
        unsigned fieldOffset = *structType.offsetOfField(fieldIndex);
        RELEASE_ASSERT((std::numeric_limits<int32_t>::max() & fieldOffset) == fieldOffset);

        TypeKind kind = toValueKind(structType.field(fieldIndex).type.unpacked().kind);
        if (value.isConst()) {
            switch (kind) {
            case TypeKind::I32:
                if (structType.field(fieldIndex).type.is<PackedType>()) {
                    ScratchScope<1, 0> scratches(*this);
                    // If it's a packed type, we materialize the constant to ensure constant blinding.
                    emitMoveConst(value, Location::fromGPR(scratches.gpr(0)));
                    switch (structType.field(fieldIndex).type.as<PackedType>()) {
                    case PackedType::I8:
                        m_jit.store8(scratches.gpr(0), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                        break;
                    case PackedType::I16:
                        m_jit.store16(scratches.gpr(0), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                        break;
                    }
                    break;
                }
                m_jit.store32(MacroAssembler::Imm32(value.asI32()), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                break;
            case TypeKind::F32:
                m_jit.store32(MacroAssembler::Imm32(value.asI32()), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                break;
            case TypeKind::I64:
            case TypeKind::F64:
                m_jit.storePair32(TrustedImm32(value.asI64lo()), TrustedImm32(value.asI64hi()), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }
            return;
        }

        Location valueLocation = loadIfNecessary(value);
        switch (kind) {
        case TypeKind::I32:
            if (structType.field(fieldIndex).type.is<PackedType>()) {
                switch (structType.field(fieldIndex).type.as<PackedType>()) {
                case PackedType::I8:
                    m_jit.store8(valueLocation.asGPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                    break;
                case PackedType::I16:
                    m_jit.store16(valueLocation.asGPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
                    break;
                }
                break;
            }
            m_jit.store32(valueLocation.asGPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
            break;
        case TypeKind::I64:
            m_jit.storePair32(valueLocation.asGPRlo(), valueLocation.asGPRhi(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
            break;
        case TypeKind::F32:
            m_jit.storeFloat(valueLocation.asFPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
            break;
        case TypeKind::F64:
            m_jit.storeDouble(valueLocation.asFPR(), MacroAssembler::Address(wasmScratchGPR, fieldOffset));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        consume(value);
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addStructNewDefault(uint32_t typeIndex, ExpressionType& result)
    {

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
        };
        result = topValue(TypeKind::I64);
        emitCCall(operationWasmStructNewEmpty, arguments, result);

        // FIXME: What about OOM?

        const auto& structType = *m_info.typeSignatures[typeIndex]->expand().template as<StructType>();
        Location structLocation = allocate(result);
        for (StructFieldCount i = 0; i < structType.fieldCount(); ++i) {
            if (Wasm::isRefType(structType.field(i).type))
                emitStructSet(structLocation.asGPR(), structType, i, Value::fromRef(TypeKind::RefNull, JSValue::encode(jsNull())));
            else
                emitStructSet(structLocation.asGPR(), structType, i, Value::fromI64(0));
        }

        LOG_INSTRUCTION("StructNewDefault", typeIndex, RESULT(result));

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addStructNewDefault(uint32_t typeIndex, ExpressionType& result)
    {

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
        };
        result = topValue(TypeKind::I64);
        emitCCall(operationWasmStructNewEmpty, arguments, result);

        // FIXME: What about OOM?

        const auto& structType = *m_info.typeSignatures[typeIndex]->expand().template as<StructType>();
        Location structLocation = allocate(result);
        for (StructFieldCount i = 0; i < structType.fieldCount(); ++i) {
            if (Wasm::isRefType(structType.field(i).type))
                emitStructSet(structLocation.asGPRlo(), structType, i, Value::fromRef(TypeKind::RefNull, JSValue::encode(jsNull())));
            else
                emitStructSet(structLocation.asGPRlo(), structType, i, Value::fromI64(0));
        }

        LOG_INSTRUCTION("StructNewDefault", typeIndex, RESULT(result));

        return { };
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addStructNew(uint32_t typeIndex, Vector<Value>& args, Value& result)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
        };
        // Note: using topValue here would be wrong because args[0] would be clobbered by the result.
        Value allocationResult = Value::fromTemp(TypeKind::Structref, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + m_parser->expressionStack().size() + args.size());
        emitCCall(operationWasmStructNewEmpty, arguments, allocationResult);

        const auto& structType = *m_info.typeSignatures[typeIndex]->expand().template as<StructType>();
        Location allocationLocation = allocate(allocationResult);
        for (uint32_t i = 0; i < args.size(); ++i)
            emitStructSet(allocationLocation.asGPR(), structType, i, args[i]);

        result = topValue(TypeKind::Structref);
        Location resultLocation = allocate(result);
        emitMove(allocationResult, resultLocation);
        // If args.isEmpty() then allocationResult.asTemp() == result.asTemp() so we will consume our result.
        if (args.size())
            consume(allocationResult);

        LOG_INSTRUCTION("StructNew", typeIndex, args, RESULT(result));

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addStructNew(uint32_t typeIndex, Vector<Value>& args, Value& result)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(typeIndex),
        };
        // Note: using topValue here would be wrong because args[0] would be clobbered by the result.
        Value allocationResult = Value::fromTemp(TypeKind::Structref, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + m_parser->expressionStack().size() + args.size());
        emitCCall(operationWasmStructNewEmpty, arguments, allocationResult);

        const auto& structType = *m_info.typeSignatures[typeIndex]->expand().template as<StructType>();
        Location allocationLocation = allocate(allocationResult);
        for (uint32_t i = 0; i < args.size(); ++i)
            emitStructSet(allocationLocation.asGPRlo(), structType, i, args[i]);

        result = topValue(TypeKind::Structref);
        Location resultLocation = allocate(result);
        emitMove(allocationResult, resultLocation);
        // If args.isEmpty() then allocationResult.asTemp() == result.asTemp() so we will consume our result.
        if (args.size())
            consume(allocationResult);

        LOG_INSTRUCTION("StructNew", typeIndex, args, RESULT(result));

        return { };
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addStructGet(ExtGCOpType structGetKind, Value structValue, const StructType& structType, uint32_t fieldIndex, Value& result)
    {
        TypeKind resultKind = structType.field(fieldIndex).type.unpacked().kind;
        if (structValue.isConst()) {
            // This is the only constant struct currently possible.
            ASSERT(JSValue::decode(structValue.asRef()).isNull());
            emitThrowException(ExceptionType::NullStructGet);
            result = Value::fromRef(resultKind, 0);
            LOG_INSTRUCTION("StructGet", structValue, fieldIndex, "Exception");
            return { };
        }

        Location structLocation = allocate(structValue);
        emitThrowOnNullReference(ExceptionType::NullStructGet, structLocation);

        m_jit.loadPtr(MacroAssembler::Address(structLocation.asGPR(), JSWebAssemblyStruct::offsetOfPayload()), wasmScratchGPR);
        unsigned fieldOffset = *structType.offsetOfField(fieldIndex);
        RELEASE_ASSERT((std::numeric_limits<int32_t>::max() & fieldOffset) == fieldOffset);

        consume(structValue);
        result = topValue(resultKind);
        Location resultLocation = allocate(result);

        switch (result.type()) {
        case TypeKind::I32:
            if (structType.field(fieldIndex).type.is<PackedType>()) {
                switch (structType.field(fieldIndex).type.as<PackedType>()) {
                case PackedType::I8:
                    m_jit.load8(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asGPR());
                    break;
                case PackedType::I16:
                    m_jit.load16(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asGPR());
                    break;
                }
                switch (structGetKind) {
                case ExtGCOpType::StructGetU:
                    LOG_INSTRUCTION("StructGetU", structValue, fieldIndex, RESULT(result));
                    return { };
                case ExtGCOpType::StructGetS: {
                    size_t elementSize = structType.field(fieldIndex).type.as<PackedType>() == PackedType::I8 ? sizeof(uint8_t) : sizeof(uint16_t);
                    uint8_t bitShift = (sizeof(uint32_t) - elementSize) * 8;
                    m_jit.lshift32(TrustedImm32(bitShift), resultLocation.asGPR());
                    m_jit.rshift32(TrustedImm32(bitShift), resultLocation.asGPR());
                    LOG_INSTRUCTION("StructGetS", structValue, fieldIndex, RESULT(result));
                    return { };
                }
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    return { };
                }
            }
            m_jit.load32(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asGPR());
            break;
        case TypeKind::I64:
            m_jit.load64(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asGPR());
            break;
        case TypeKind::F32:
            m_jit.loadFloat(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asFPR());
            break;
        case TypeKind::F64:
            m_jit.loadDouble(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }

        LOG_INSTRUCTION("StructGet", structValue, fieldIndex, RESULT(result));
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addStructGet(ExtGCOpType structGetKind, Value structValue, const StructType& structType, uint32_t fieldIndex, Value& result)
    {
        TypeKind resultKind = structType.field(fieldIndex).type.unpacked().kind;
        if (structValue.isConst()) {
            // This is the only constant struct currently possible.
            ASSERT(JSValue::decode(structValue.asRef()).isNull());
            emitThrowException(ExceptionType::NullStructGet);
            result = Value::fromRef(resultKind, 0);
            LOG_INSTRUCTION("StructGet", structValue, fieldIndex, "Exception");
            return { };
        }

        Location structLocation = allocate(structValue);
        emitThrowOnNullReference(ExceptionType::NullStructGet, structLocation);

        m_jit.loadPtr(MacroAssembler::Address(structLocation.asGPRlo(), JSWebAssemblyStruct::offsetOfPayload()), wasmScratchGPR);
        unsigned fieldOffset = *structType.offsetOfField(fieldIndex);
        RELEASE_ASSERT((std::numeric_limits<int32_t>::max() & fieldOffset) == fieldOffset);

        consume(structValue);
        result = topValue(resultKind);
        Location resultLocation = allocate(result);

        switch (result.type()) {
        case TypeKind::I32:
            if (structType.field(fieldIndex).type.is<PackedType>()) {
                switch (structType.field(fieldIndex).type.as<PackedType>()) {
                case PackedType::I8:
                    m_jit.load8(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asGPR());
                    break;
                case PackedType::I16:
                    m_jit.load16(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asGPR());
                    break;
                }
                switch (structGetKind) {
                case ExtGCOpType::StructGetU:
                    LOG_INSTRUCTION("StructGetU", structValue, fieldIndex, RESULT(result));
                    return { };
                case ExtGCOpType::StructGetS: {
                    size_t elementSize = structType.field(fieldIndex).type.as<PackedType>() == PackedType::I8 ? sizeof(uint8_t) : sizeof(uint16_t);
                    uint8_t bitShift = (sizeof(uint32_t) - elementSize) * 8;
                    m_jit.lshift32(TrustedImm32(bitShift), resultLocation.asGPR());
                    m_jit.rshift32(TrustedImm32(bitShift), resultLocation.asGPR());
                    LOG_INSTRUCTION("StructGetS", structValue, fieldIndex, RESULT(result));
                    return { };
                }
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    return { };
                }
            }
            m_jit.load32(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asGPR());
            break;
        case TypeKind::I64:
            m_jit.loadPair32(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asGPRlo(), resultLocation.asGPRhi());
            break;
        case TypeKind::F32:
            m_jit.loadFloat(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asFPR());
            break;
        case TypeKind::F64:
            m_jit.loadDouble(MacroAssembler::Address(wasmScratchGPR, fieldOffset), resultLocation.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }

        LOG_INSTRUCTION("StructGet", structValue, fieldIndex, RESULT(result));
        return { };
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addStructSet(Value structValue, const StructType& structType, uint32_t fieldIndex, Value value)
    {
        if (structValue.isConst()) {
            // This is the only constant struct currently possible.
            ASSERT(JSValue::decode(structValue.asRef()).isNull());
            emitThrowException(ExceptionType::NullStructSet);
            LOG_INSTRUCTION("StructSet", structValue, fieldIndex, value, "Exception");
            return { };
        }

        Location structLocation = allocate(structValue);
        emitThrowOnNullReference(ExceptionType::NullStructSet, structLocation);

        emitStructSet(structLocation.asGPR(), structType, fieldIndex, value);
        LOG_INSTRUCTION("StructSet", structValue, fieldIndex, value);
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addStructSet(Value structValue, const StructType& structType, uint32_t fieldIndex, Value value)
    {
        if (structValue.isConst()) {
            // This is the only constant struct currently possible.
            ASSERT(JSValue::decode(structValue.asRef()).isNull());
            emitThrowException(ExceptionType::NullStructSet);
            LOG_INSTRUCTION("StructSet", structValue, fieldIndex, value, "Exception");
            return { };
        }

        Location structLocation = allocate(structValue);
        emitThrowOnNullReference(ExceptionType::NullStructSet, structLocation);

        emitStructSet(structLocation.asGPRlo(), structType, fieldIndex, value);
        LOG_INSTRUCTION("StructSet", structValue, fieldIndex, value);
        return { };
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefTest(ExpressionType reference, bool allowNull, int32_t heapType, ExpressionType& result)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            reference,
            Value::fromI32(allowNull),
            Value::fromI32(heapType),
        };
        result = topValue(TypeKind::I32);
        emitCCall(operationWasmRefTest, arguments, result);

        LOG_INSTRUCTION("RefTest", reference, allowNull, heapType, RESULT(result));

        return { };
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefCast(ExpressionType reference, bool allowNull, int32_t heapType, ExpressionType& result)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            reference,
            Value::fromI32(allowNull),
            Value::fromI32(heapType),
        };
        result = topValue(TypeKind::Ref);
        emitCCall(operationWasmRefCast, arguments, result);
        Location resultLocation = allocate(result);
        throwExceptionIf(ExceptionType::CastFailure, m_jit.branchTest64(MacroAssembler::Zero, resultLocation.asGPR()));

        LOG_INSTRUCTION("RefCast", reference, allowNull, heapType, RESULT(result));

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefCast(ExpressionType reference, bool allowNull, int32_t heapType, ExpressionType& result)
    {
        Vector<Value, 8> arguments = {
            instanceValue(),
            reference,
            Value::fromI32(allowNull),
            Value::fromI32(heapType),
        };
        result = topValue(TypeKind::Ref);
        emitCCall(operationWasmRefCast, arguments, result);
        Location resultLocation = allocate(result);
        throwExceptionIf(ExceptionType::CastFailure,
            m_jit.branch32(MacroAssembler::Equal, resultLocation.asGPRhi(), TrustedImm32(JSValue::EmptyValueTag)));

        LOG_INSTRUCTION("RefCast", reference, allowNull, heapType, RESULT(result));

        return { };
    }
#endif


    PartialResult WARN_UNUSED_RETURN BBQJIT::addAnyConvertExtern(ExpressionType reference, ExpressionType& result)
    {
        Vector<Value, 8> arguments = {
            reference
        };
        result = topValue(TypeKind::Anyref);
        emitCCall(&operationWasmAnyConvertExtern, arguments, result);
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addExternConvertAny(ExpressionType reference, ExpressionType& result)
    {
        result = reference;
        return { };
    }

    // Basic operators
    PartialResult WARN_UNUSED_RETURN BBQJIT::addSelect(Value condition, Value lhs, Value rhs, Value& result)
    {
        if (condition.isConst()) {
            Value src = condition.asI32() ? lhs : rhs;
            Location srcLocation;
            if (src.isConst())
                result = src;
            else {
                result = topValue(lhs.type());
                srcLocation = loadIfNecessary(src);
            }

            LOG_INSTRUCTION("Select", condition, lhs, rhs, RESULT(result));
            consume(condition);
            consume(lhs);
            consume(rhs);
            if (!result.isConst()) {
                Location resultLocation = allocate(result);
                emitMove(lhs.type(), srcLocation, resultLocation);
            }
        } else {
            Location conditionLocation = loadIfNecessary(condition);
            Location lhsLocation = Location::none(), rhsLocation = Location::none();

            // Ensure all non-constant parameters are loaded into registers.
            if (!lhs.isConst())
                lhsLocation = loadIfNecessary(lhs);
            if (!rhs.isConst())
                rhsLocation = loadIfNecessary(rhs);

            ASSERT(lhs.isConst() || lhsLocation.isRegister());
            ASSERT(rhs.isConst() || rhsLocation.isRegister());
            consume(lhs);
            consume(rhs);

            result = topValue(lhs.type());
            Location resultLocation = allocate(result);
            LOG_INSTRUCTION("Select", condition, lhs, lhsLocation, rhs, rhsLocation, RESULT(result));
            LOG_INDENT();

            bool inverted = false;

            // If the operands or the result alias, we want the matching one to be on top.
            if (rhsLocation == resultLocation) {
                std::swap(lhs, rhs);
                std::swap(lhsLocation, rhsLocation);
                inverted = true;
            }

            // If the condition location and the result alias, we want to make sure the condition is
            // preserved no matter what.
            if (conditionLocation == resultLocation) {
                m_jit.move(conditionLocation.asGPR(), wasmScratchGPR);
                conditionLocation = Location::fromGPR(wasmScratchGPR);
            }

            // Kind of gross isel, but it should handle all use/def aliasing cases correctly.
            if (lhs.isConst())
                emitMoveConst(lhs, resultLocation);
            else
                emitMove(lhs.type(), lhsLocation, resultLocation);
            Jump ifZero = m_jit.branchTest32(inverted ? ResultCondition::Zero : ResultCondition::NonZero, conditionLocation.asGPR(), conditionLocation.asGPR());
            consume(condition);
            if (rhs.isConst())
                emitMoveConst(rhs, resultLocation);
            else
                emitMove(rhs.type(), rhsLocation, resultLocation);
            ifZero.link(&m_jit);

            LOG_DEDENT();
        }
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Add(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I32Add", TypeKind::I32,
            BLOCK(Value::fromI32(lhs.asI32() + rhs.asI32())),
            BLOCK(
                m_jit.add32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(), resultLocation.asGPR());
                m_jit.add32(Imm32(ImmHelpers::imm(lhs, rhs).asI32()), resultLocation.asGPR());
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Add(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Add", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() + rhs.asI64())),
            BLOCK(
                m_jit.add64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(), resultLocation.asGPR());
                m_jit.add64(TrustedImm64(ImmHelpers::imm(lhs, rhs).asI64()), resultLocation.asGPR());
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Add(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Add", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() + rhs.asI64())),
            BLOCK(
                m_jit.add64(lhsLocation.asGPRhi(), lhsLocation.asGPRlo(), rhsLocation.asGPRhi(), rhsLocation.asGPRlo(), resultLocation.asGPRhi(), resultLocation.asGPRlo());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                m_jit.add64(lhsLocation.asGPRhi(), lhsLocation.asGPRlo(), rhsLocation.asGPRhi(), rhsLocation.asGPRlo(), resultLocation.asGPRhi(), resultLocation.asGPRlo());
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Add(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F32Add", TypeKind::F32,
            BLOCK(Value::fromF32(lhs.asF32() + rhs.asF32())),
            BLOCK(
                m_jit.addFloat(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                m_jit.addFloat(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Add(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F64Add", TypeKind::F64,
            BLOCK(Value::fromF64(lhs.asF64() + rhs.asF64())),
            BLOCK(
                m_jit.addDouble(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                m_jit.addDouble(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Sub(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I32Sub", TypeKind::I32,
            BLOCK(Value::fromI32(lhs.asI32() - rhs.asI32())),
            BLOCK(
                m_jit.sub32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst()) {
                    // Add a negative if rhs is a constant.
                    m_jit.move(lhsLocation.asGPR(), resultLocation.asGPR());
                    m_jit.add32(Imm32(-rhs.asI32()), resultLocation.asGPR());
                } else {
                    emitMoveConst(lhs, Location::fromGPR(wasmScratchGPR));
                    m_jit.sub32(wasmScratchGPR, rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Sub(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Sub", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() - rhs.asI64())),
            BLOCK(
                m_jit.sub64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst()) {
                    // Add a negative if rhs is a constant.
                    m_jit.move(lhsLocation.asGPR(), resultLocation.asGPR());
                    m_jit.add64(TrustedImm64(-rhs.asI64()), resultLocation.asGPR());
                } else {
                    emitMoveConst(lhs, Location::fromGPR(wasmScratchGPR));
                    m_jit.sub64(wasmScratchGPR, rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Sub(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Sub", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() - rhs.asI64())),
            BLOCK(
                m_jit.sub64(
                    lhsLocation.asGPRhi(), lhsLocation.asGPRlo(),
                    rhsLocation.asGPRhi(), rhsLocation.asGPRlo(),
                    resultLocation.asGPRhi(), resultLocation.asGPRlo()
                );
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                m_jit.sub64(
                    lhsLocation.asGPRhi(), lhsLocation.asGPRlo(),
                    rhsLocation.asGPRhi(), rhsLocation.asGPRlo(),
                    resultLocation.asGPRhi(), resultLocation.asGPRlo()
                );
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Sub(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F32Sub", TypeKind::F32,
            BLOCK(Value::fromF32(lhs.asF32() - rhs.asF32())),
            BLOCK(
                m_jit.subFloat(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                if (rhs.isConst()) {
                    // Add a negative if rhs is a constant.
                    emitMoveConst(Value::fromF32(-rhs.asF32()), Location::fromFPR(wasmScratchFPR));
                    m_jit.addFloat(lhsLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                } else {
                    emitMoveConst(lhs, Location::fromFPR(wasmScratchFPR));
                    m_jit.subFloat(wasmScratchFPR, rhsLocation.asFPR(), resultLocation.asFPR());
                }
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Sub(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F64Sub", TypeKind::F64,
            BLOCK(Value::fromF64(lhs.asF64() - rhs.asF64())),
            BLOCK(
                m_jit.subDouble(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                if (rhs.isConst()) {
                    // Add a negative if rhs is a constant.
                    emitMoveConst(Value::fromF64(-rhs.asF64()), Location::fromFPR(wasmScratchFPR));
                    m_jit.addDouble(lhsLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                } else {
                    emitMoveConst(lhs, Location::fromFPR(wasmScratchFPR));
                    m_jit.subDouble(wasmScratchFPR, rhsLocation.asFPR(), resultLocation.asFPR());
                }
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Mul(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I32Mul", TypeKind::I32,
            BLOCK(Value::fromI32(lhs.asI32() * rhs.asI32())),
            BLOCK(
                m_jit.mul32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.mul32(
                    Imm32(ImmHelpers::imm(lhs, rhs).asI32()),
                    ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(),
                    resultLocation.asGPR()
                );
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Mul(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Mul", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() * rhs.asI64())),
            BLOCK(
                m_jit.mul64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR(wasmScratchGPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR(wasmScratchGPR));
                m_jit.mul64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Mul(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Mul", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() * rhs.asI64())),
            BLOCK(
                ScratchScope<2, 0> scratches(*this, lhsLocation, rhsLocation, resultLocation);
                Location tmpHiLo = Location::fromGPR(scratches.gpr(0));
                Location tmpLoHi = Location::fromGPR(scratches.gpr(1));
                m_jit.mul32(lhsLocation.asGPRhi(), rhsLocation.asGPRlo(), tmpHiLo.asGPR());
                m_jit.mul32(lhsLocation.asGPRlo(), rhsLocation.asGPRhi(), tmpLoHi.asGPR());
                m_jit.uMull32(lhsLocation.asGPRlo(), rhsLocation.asGPRlo(), resultLocation.asGPRhi(),  resultLocation.asGPRlo());
                m_jit.add32(tmpHiLo.asGPR(), resultLocation.asGPRhi());
                m_jit.add32(tmpLoHi.asGPR(), resultLocation.asGPRhi());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR,  wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                ScratchScope<2, 0> scratches(*this, lhsLocation, rhsLocation, resultLocation);
                Location tmpHiLo = Location::fromGPR(scratches.gpr(0));
                Location tmpLoHi = Location::fromGPR(scratches.gpr(1));
                m_jit.mul32(lhsLocation.asGPRhi(), rhsLocation.asGPRlo(), tmpHiLo.asGPR());
                m_jit.mul32(lhsLocation.asGPRlo(), rhsLocation.asGPRhi(), tmpLoHi.asGPR());
                m_jit.uMull32(lhsLocation.asGPRlo(), rhsLocation.asGPRlo(), resultLocation.asGPRhi(),  resultLocation.asGPRlo());
                m_jit.add32(tmpHiLo.asGPR(), resultLocation.asGPRhi());
                m_jit.add32(tmpLoHi.asGPR(), resultLocation.asGPRhi());
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Mul(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F32Mul", TypeKind::F32,
            BLOCK(Value::fromF32(lhs.asF32() * rhs.asF32())),
            BLOCK(
                m_jit.mulFloat(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                m_jit.mulFloat(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Mul(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F64Mul", TypeKind::F64,
            BLOCK(Value::fromF64(lhs.asF64() * rhs.asF64())),
            BLOCK(
                m_jit.mulDouble(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                m_jit.mulDouble(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    template<typename Func>
    void BBQJIT::addLatePath(Func func)
    {
        m_latePaths.append(createSharedTask<void(BBQJIT&, CCallHelpers&)>(WTFMove(func)));
    }

    void BBQJIT::emitThrowException(ExceptionType type)
    {
        m_jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(type)), GPRInfo::argumentGPR1);
        auto jumpToExceptionStub = m_jit.jump();

        m_jit.addLinkTask([jumpToExceptionStub] (LinkBuffer& linkBuffer) {
            linkBuffer.link(jumpToExceptionStub, CodeLocationLabel<JITThunkPtrTag>(Thunks::singleton().stub(throwExceptionFromWasmThunkGenerator).code()));
        });
    }

    void BBQJIT::throwExceptionIf(ExceptionType type, Jump jump)
    {
        m_exceptions[static_cast<unsigned>(type)].append(jump);
    }

#if USE(JSVALUE64)
    void BBQJIT::emitThrowOnNullReference(ExceptionType type, Location ref)
    {
        throwExceptionIf(type, m_jit.branch64(MacroAssembler::Equal, ref.asGPR(), TrustedImm64(JSValue::encode(jsNull()))));
    }
#else
    void BBQJIT::emitThrowOnNullReference(ExceptionType type, Location ref)
    {
        throwExceptionIf(type, m_jit.branch32(MacroAssembler::Equal, ref.asGPRhi(), TrustedImm32(JSValue::NullTag)));
    }
#endif

#if USE(JSVALUE64)
#if CPU(X86_64)
    RegisterSet clobbersForDivX86()
    {
        static RegisterSet x86DivClobbers;
        static std::once_flag flag;
        std::call_once(
            flag,
            []() {
                RegisterSetBuilder builder;
                builder.add(X86Registers::eax, IgnoreVectors);
                builder.add(X86Registers::edx, IgnoreVectors);
                x86DivClobbers = builder.buildAndValidate();
            });
        return x86DivClobbers;
    }

#define PREPARE_FOR_MOD_OR_DIV \
    do { \
        for (JSC::Reg reg : clobbersForDivX86()) \
            clobber(reg); \
    } while (false); \
    ScratchScope<0, 0> scratches(*this, clobbersForDivX86())

    template<typename IntType, bool IsMod>
    void BBQJIT::emitModOrDiv(const Value& lhs, Location lhsLocation, const Value& rhs, Location rhsLocation, const Value&, Location resultLocation)
    {
        // FIXME: We currently don't do nearly as sophisticated instruction selection on Intel as we do on other platforms,
        // but there's no good reason we can't. We should probably port over the isel in the future if it seems to yield
        // dividends.

        constexpr bool isSigned = std::is_signed<IntType>();
        constexpr bool is32 = sizeof(IntType) == 4;

        ASSERT(lhsLocation.isRegister() || rhsLocation.isRegister());
        if (lhs.isConst())
            emitMoveConst(lhs, lhsLocation = Location::fromGPR(wasmScratchGPR));
        else if (rhs.isConst())
            emitMoveConst(rhs, rhsLocation = Location::fromGPR(wasmScratchGPR));
        ASSERT(lhsLocation.isRegister() && rhsLocation.isRegister());

        ASSERT(resultLocation.isRegister());
        ASSERT(lhsLocation.asGPR() != X86Registers::eax && lhsLocation.asGPR() != X86Registers::edx);
        ASSERT(rhsLocation.asGPR() != X86Registers::eax && lhsLocation.asGPR() != X86Registers::edx);

        ScratchScope<2, 0> scratches(*this, lhsLocation, rhsLocation, resultLocation);

        Jump toDiv, toEnd;

        Jump isZero = is32
            ? m_jit.branchTest32(ResultCondition::Zero, rhsLocation.asGPR())
            : m_jit.branchTest64(ResultCondition::Zero, rhsLocation.asGPR());
        throwExceptionIf(ExceptionType::DivisionByZero, isZero);
        if constexpr (isSigned) {
            if constexpr (is32)
                m_jit.compare32(RelationalCondition::Equal, rhsLocation.asGPR(), TrustedImm32(-1), scratches.gpr(0));
            else
                m_jit.compare64(RelationalCondition::Equal, rhsLocation.asGPR(), TrustedImm32(-1), scratches.gpr(0));
            if constexpr (is32)
                m_jit.compare32(RelationalCondition::Equal, lhsLocation.asGPR(), TrustedImm32(std::numeric_limits<int32_t>::min()), scratches.gpr(1));
            else {
                m_jit.move(TrustedImm64(std::numeric_limits<int64_t>::min()), scratches.gpr(1));
                m_jit.compare64(RelationalCondition::Equal, lhsLocation.asGPR(), scratches.gpr(1), scratches.gpr(1));
            }
            m_jit.and64(scratches.gpr(0), scratches.gpr(1));
            if constexpr (IsMod) {
                toDiv = m_jit.branchTest64(ResultCondition::Zero, scratches.gpr(1));
                // In this case, WASM doesn't want us to fault, but x86 will. So we set the result ourselves.
                if constexpr (is32)
                    m_jit.xor32(resultLocation.asGPR(), resultLocation.asGPR());
                else
                    m_jit.xor64(resultLocation.asGPR(), resultLocation.asGPR());
                toEnd = m_jit.jump();
            } else {
                Jump isNegativeOne = m_jit.branchTest64(ResultCondition::NonZero, scratches.gpr(1));
                throwExceptionIf(ExceptionType::IntegerOverflow, isNegativeOne);
            }
        }

        if (toDiv.isSet())
            toDiv.link(&m_jit);

        m_jit.move(lhsLocation.asGPR(), X86Registers::eax);

        if constexpr (is32 && isSigned) {
            m_jit.x86ConvertToDoubleWord32();
            m_jit.x86Div32(rhsLocation.asGPR());
        } else if constexpr (is32) {
            m_jit.xor32(X86Registers::edx, X86Registers::edx);
            m_jit.x86UDiv32(rhsLocation.asGPR());
        } else if constexpr (isSigned) {
            m_jit.x86ConvertToQuadWord64();
            m_jit.x86Div64(rhsLocation.asGPR());
        } else {
            m_jit.xor64(X86Registers::edx, X86Registers::edx);
            m_jit.x86UDiv64(rhsLocation.asGPR());
        }

        if constexpr (IsMod)
            m_jit.move(X86Registers::edx, resultLocation.asGPR());
        else
            m_jit.move(X86Registers::eax, resultLocation.asGPR());

        if (toEnd.isSet())
            toEnd.link(&m_jit);
    }
#else
#define PREPARE_FOR_MOD_OR_DIV

    template<typename IntType, bool IsMod>
    void BBQJIT::emitModOrDiv(const Value& lhs, Location lhsLocation, const Value& rhs, Location rhsLocation, const Value&, Location resultLocation)
    {
        constexpr bool isSigned = std::is_signed<IntType>();
        constexpr bool is32 = sizeof(IntType) == 4;

        ASSERT(lhsLocation.isRegister() || rhsLocation.isRegister());
        ASSERT(resultLocation.isRegister());

        bool checkedForZero = false, checkedForNegativeOne = false;
        if (rhs.isConst()) {
            int64_t divisor = is32 ? rhs.asI32() : rhs.asI64();
            if (!divisor) {
                emitThrowException(ExceptionType::DivisionByZero);
                return;
            }
            if (divisor == 1) {
                if constexpr (IsMod) {
                    // N % 1 == 0
                    if constexpr (is32)
                        m_jit.xor32(resultLocation.asGPR(), resultLocation.asGPR());
                    else
                        m_jit.xor64(resultLocation.asGPR(), resultLocation.asGPR());
                } else
                    m_jit.move(lhsLocation.asGPR(), resultLocation.asGPR());
                return;
            }
            if (divisor == -1) {
                // Check for INT_MIN / -1 case, and throw an IntegerOverflow exception if it occurs
                if (!IsMod && isSigned) {
                    Jump jump = is32
                        ? m_jit.branch32(RelationalCondition::Equal, lhsLocation.asGPR(), TrustedImm32(std::numeric_limits<int32_t>::min()))
                        : m_jit.branch64(RelationalCondition::Equal, lhsLocation.asGPR(), TrustedImm64(std::numeric_limits<int64_t>::min()));
                    throwExceptionIf(ExceptionType::IntegerOverflow, jump);
                }

                if constexpr (IsMod) {
                    // N % 1 == 0
                    if constexpr (is32)
                        m_jit.xor32(resultLocation.asGPR(), resultLocation.asGPR());
                    else
                        m_jit.xor64(resultLocation.asGPR(), resultLocation.asGPR());
                    return;
                }
                if constexpr (isSigned) {
                    if constexpr (is32)
                        m_jit.neg32(lhsLocation.asGPR(), resultLocation.asGPR());
                    else
                        m_jit.neg64(lhsLocation.asGPR(), resultLocation.asGPR());
                    return;
                }

                // Fall through to general case.
            } else if (isPowerOfTwo(divisor)) {
                if constexpr (IsMod) {
                    if constexpr (isSigned) {
                        // This constructs an extra operand with log2(divisor) bits equal to the sign bit of the dividend. If the dividend
                        // is positive, this is zero and adding it achieves nothing; but if the dividend is negative, this is equal to the
                        // divisor minus one, which is the exact amount of bias we need to get the correct result. Computing this for both
                        // positive and negative dividends lets us elide branching, but more importantly allows us to save a register by
                        // not needing an extra multiplySub at the end.
                        if constexpr (is32) {
                            m_jit.rshift32(lhsLocation.asGPR(), TrustedImm32(31), wasmScratchGPR);
                            m_jit.urshift32(wasmScratchGPR, TrustedImm32(32 - WTF::fastLog2(static_cast<unsigned>(divisor))), wasmScratchGPR);
                            m_jit.add32(wasmScratchGPR, lhsLocation.asGPR(), resultLocation.asGPR());
                        } else {
                            m_jit.rshift64(lhsLocation.asGPR(), TrustedImm32(63), wasmScratchGPR);
                            m_jit.urshift64(wasmScratchGPR, TrustedImm32(64 - WTF::fastLog2(static_cast<uint64_t>(divisor))), wasmScratchGPR);
                            m_jit.add64(wasmScratchGPR, lhsLocation.asGPR(), resultLocation.asGPR());
                        }

                        lhsLocation = resultLocation;
                    }

                    if constexpr (is32)
                        m_jit.and32(Imm32(static_cast<uint32_t>(divisor) - 1), lhsLocation.asGPR(), resultLocation.asGPR());
                    else
                        m_jit.and64(TrustedImm64(static_cast<uint64_t>(divisor) - 1), lhsLocation.asGPR(), resultLocation.asGPR());

                    if constexpr (isSigned) {
                        // The extra operand we computed is still in wasmScratchGPR - now we can subtract it from the result to get the
                        // correct answer.
                        if constexpr (is32)
                            m_jit.sub32(resultLocation.asGPR(), wasmScratchGPR, resultLocation.asGPR());
                        else
                            m_jit.sub64(resultLocation.asGPR(), wasmScratchGPR, resultLocation.asGPR());
                    }
                    return;
                }

                if constexpr (isSigned) {
                    // If we are doing signed division, we need to bias the dividend for negative numbers.
                    if constexpr (is32)
                        m_jit.add32(TrustedImm32(static_cast<int32_t>(divisor) - 1), lhsLocation.asGPR(), wasmScratchGPR);
                    else
                        m_jit.add64(TrustedImm64(divisor - 1), lhsLocation.asGPR(), wasmScratchGPR);

                    // moveConditionally seems to be faster than a branch here, even if it's well predicted.
                    if (is32)
                        m_jit.moveConditionally32(RelationalCondition::GreaterThanOrEqual, lhsLocation.asGPR(), TrustedImm32(0), lhsLocation.asGPR(), wasmScratchGPR, wasmScratchGPR);
                    else
                        m_jit.moveConditionally64(RelationalCondition::GreaterThanOrEqual, lhsLocation.asGPR(), TrustedImm32(0), lhsLocation.asGPR(), wasmScratchGPR, wasmScratchGPR);
                    lhsLocation = Location::fromGPR(wasmScratchGPR);
                }

                if constexpr (is32)
                    m_jit.rshift32(lhsLocation.asGPR(), m_jit.trustedImm32ForShift(Imm32(WTF::fastLog2(static_cast<unsigned>(divisor)))), resultLocation.asGPR());
                else
                    m_jit.rshift64(lhsLocation.asGPR(), TrustedImm32(WTF::fastLog2(static_cast<uint64_t>(divisor))), resultLocation.asGPR());

                return;
            }
            // TODO: try generating integer reciprocal instead.
            checkedForNegativeOne = true;
            checkedForZero = true;
            rhsLocation = Location::fromGPR(wasmScratchGPR);
            emitMoveConst(rhs, rhsLocation);
            // Fall through to register/register div.
        } else if (lhs.isConst()) {
            int64_t dividend = is32 ? lhs.asI32() : lhs.asI64();

            Jump isZero = is32
                ? m_jit.branchTest32(ResultCondition::Zero, rhsLocation.asGPR())
                : m_jit.branchTest64(ResultCondition::Zero, rhsLocation.asGPR());
            throwExceptionIf(ExceptionType::DivisionByZero, isZero);
            checkedForZero = true;

            if (!dividend) {
                if constexpr (is32)
                    m_jit.xor32(resultLocation.asGPR(), resultLocation.asGPR());
                else
                    m_jit.xor64(resultLocation.asGPR(), resultLocation.asGPR());
                return;
            }
            if (isSigned && !IsMod && dividend == std::numeric_limits<IntType>::min()) {
                Jump isNegativeOne = is32
                    ? m_jit.branch32(RelationalCondition::Equal, rhsLocation.asGPR(), TrustedImm32(-1))
                    : m_jit.branch64(RelationalCondition::Equal, rhsLocation.asGPR(), TrustedImm64(-1));
                throwExceptionIf(ExceptionType::IntegerOverflow, isNegativeOne);
            }
            checkedForNegativeOne = true;

            lhsLocation = Location::fromGPR(wasmScratchGPR);
            emitMoveConst(lhs, lhsLocation);
            // Fall through to register/register div.
        }

        if (!checkedForZero) {
            Jump isZero = is32
                ? m_jit.branchTest32(ResultCondition::Zero, rhsLocation.asGPR())
                : m_jit.branchTest64(ResultCondition::Zero, rhsLocation.asGPR());
            throwExceptionIf(ExceptionType::DivisionByZero, isZero);
        }

        ScratchScope<1, 0> scratches(*this, lhsLocation, rhsLocation, resultLocation);
        if (isSigned && !IsMod && !checkedForNegativeOne) {
            // The following code freely clobbers wasmScratchGPR. This would be a bug if either of our operands were
            // stored in wasmScratchGPR, which is the case if one of our operands is a constant - but in that case,
            // we should be able to rule out this check based on the value of that constant above.
            ASSERT(!lhs.isConst());
            ASSERT(!rhs.isConst());
            ASSERT(lhsLocation.asGPR() != wasmScratchGPR);
            ASSERT(rhsLocation.asGPR() != wasmScratchGPR);

            if constexpr (is32)
                m_jit.compare32(RelationalCondition::Equal, rhsLocation.asGPR(), TrustedImm32(-1), wasmScratchGPR);
            else
                m_jit.compare64(RelationalCondition::Equal, rhsLocation.asGPR(), TrustedImm32(-1), wasmScratchGPR);
            if constexpr (is32)
                m_jit.compare32(RelationalCondition::Equal, lhsLocation.asGPR(), TrustedImm32(std::numeric_limits<int32_t>::min()), scratches.gpr(0));
            else {
                m_jit.move(TrustedImm64(std::numeric_limits<int64_t>::min()), scratches.gpr(0));
                m_jit.compare64(RelationalCondition::Equal, lhsLocation.asGPR(), scratches.gpr(0), scratches.gpr(0));
            }
            m_jit.and64(wasmScratchGPR, scratches.gpr(0), wasmScratchGPR);
            Jump isNegativeOne = m_jit.branchTest64(ResultCondition::NonZero, wasmScratchGPR);
            throwExceptionIf(ExceptionType::IntegerOverflow, isNegativeOne);
        }

        GPRReg divResult = IsMod ? scratches.gpr(0) : resultLocation.asGPR();
        if (is32 && isSigned)
            m_jit.div32(lhsLocation.asGPR(), rhsLocation.asGPR(), divResult);
        else if (is32)
            m_jit.uDiv32(lhsLocation.asGPR(), rhsLocation.asGPR(), divResult);
        else if (isSigned)
            m_jit.div64(lhsLocation.asGPR(), rhsLocation.asGPR(), divResult);
        else
            m_jit.uDiv64(lhsLocation.asGPR(), rhsLocation.asGPR(), divResult);

        if (IsMod) {
            if (is32)
                m_jit.multiplySub32(divResult, rhsLocation.asGPR(), lhsLocation.asGPR(), resultLocation.asGPR());
            else
                m_jit.multiplySub64(divResult, rhsLocation.asGPR(), lhsLocation.asGPR(), resultLocation.asGPR());
        }
    }
#endif
#else // USE(JSVALUE32_64)
#define PREPARE_FOR_MOD_OR_DIV

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
#endif

    template<typename IntType>
    Value BBQJIT::checkConstantDivision(const Value& lhs, const Value& rhs)
    {
        constexpr bool is32 = sizeof(IntType) == 4;
        if (!(is32 ? int64_t(rhs.asI32()) : rhs.asI64())) {
            emitThrowException(ExceptionType::DivisionByZero);
            return is32 ? Value::fromI32(1) : Value::fromI64(1);
        }
        if ((is32 ? int64_t(rhs.asI32()) : rhs.asI64()) == -1
            && (is32 ? int64_t(lhs.asI32()) : lhs.asI64()) == std::numeric_limits<IntType>::min()
            && std::is_signed<IntType>()) {
            emitThrowException(ExceptionType::IntegerOverflow);
            return is32 ? Value::fromI32(1) : Value::fromI64(1);
        }
        return rhs;
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32DivS(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_MOD_OR_DIV;
        EMIT_BINARY(
            "I32DivS", TypeKind::I32,
            BLOCK(
                Value::fromI32(lhs.asI32() / checkConstantDivision<int32_t>(lhs, rhs).asI32())
            ),
            BLOCK(
                emitModOrDiv<int32_t, false>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            ),
            BLOCK(
                emitModOrDiv<int32_t, false>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64DivS(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_MOD_OR_DIV;
        EMIT_BINARY(
            "I64DivS", TypeKind::I64,
            BLOCK(
                Value::fromI64(lhs.asI64() / checkConstantDivision<int64_t>(lhs, rhs).asI64())
            ),
            BLOCK(
                emitModOrDiv<int64_t, false>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            ),
            BLOCK(
                emitModOrDiv<int64_t, false>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32DivU(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_MOD_OR_DIV;
        EMIT_BINARY(
            "I32DivU", TypeKind::I32,
            BLOCK(
                Value::fromI32(static_cast<uint32_t>(lhs.asI32()) / static_cast<uint32_t>(checkConstantDivision<int32_t>(lhs, rhs).asI32()))
            ),
            BLOCK(
                emitModOrDiv<uint32_t, false>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            ),
            BLOCK(
                emitModOrDiv<uint32_t, false>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64DivU(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_MOD_OR_DIV;
        EMIT_BINARY(
            "I64DivU", TypeKind::I64,
            BLOCK(
                Value::fromI64(static_cast<uint64_t>(lhs.asI64()) / static_cast<uint64_t>(checkConstantDivision<int64_t>(lhs, rhs).asI64()))
            ),
            BLOCK(
                emitModOrDiv<uint64_t, false>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            ),
            BLOCK(
                emitModOrDiv<uint64_t, false>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32RemS(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_MOD_OR_DIV;
        EMIT_BINARY(
            "I32RemS", TypeKind::I32,
            BLOCK(
                Value::fromI32(lhs.asI32() % checkConstantDivision<int32_t>(lhs, rhs).asI32())
            ),
            BLOCK(
                emitModOrDiv<int32_t, true>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            ),
            BLOCK(
                emitModOrDiv<int32_t, true>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64RemS(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_MOD_OR_DIV;
        EMIT_BINARY(
            "I64RemS", TypeKind::I64,
            BLOCK(
                Value::fromI64(lhs.asI64() % checkConstantDivision<int64_t>(lhs, rhs).asI64())
            ),
            BLOCK(
                emitModOrDiv<int64_t, true>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            ),
            BLOCK(
                emitModOrDiv<int64_t, true>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32RemU(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_MOD_OR_DIV;
        EMIT_BINARY(
            "I32RemU", TypeKind::I32,
            BLOCK(
                Value::fromI32(static_cast<uint32_t>(lhs.asI32()) % static_cast<uint32_t>(checkConstantDivision<int32_t>(lhs, rhs).asI32()))
            ),
            BLOCK(
                emitModOrDiv<uint32_t, true>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            ),
            BLOCK(
                emitModOrDiv<uint32_t, true>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64RemU(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_MOD_OR_DIV;
        EMIT_BINARY(
            "I64RemU", TypeKind::I64,
            BLOCK(
                Value::fromI64(static_cast<uint64_t>(lhs.asI64()) % static_cast<uint64_t>(checkConstantDivision<int64_t>(lhs, rhs).asI64()))
            ),
            BLOCK(
                emitModOrDiv<uint64_t, true>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            ),
            BLOCK(
                emitModOrDiv<uint64_t, true>(lhs, lhsLocation, rhs, rhsLocation, result, resultLocation);
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Div(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F32Div", TypeKind::F32,
            BLOCK(Value::fromF32(lhs.asF32() / rhs.asF32())),
            BLOCK(
                m_jit.divFloat(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                m_jit.divFloat(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Div(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F64Div", TypeKind::F64,
            BLOCK(Value::fromF64(lhs.asF64() / rhs.asF64())),
            BLOCK(
                m_jit.divDouble(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                m_jit.divDouble(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    template<typename FloatType, MinOrMax IsMinOrMax>
    void BBQJIT::emitFloatingPointMinOrMax(FPRReg left, FPRReg right, FPRReg result)
    {
        constexpr bool is32 = sizeof(FloatType) == 4;

#if CPU(ARM64)
            if constexpr (is32 && IsMinOrMax == MinOrMax::Min)
                m_jit.floatMin(left, right, result);
            else if constexpr (is32)
                m_jit.floatMax(left, right, result);
            else if constexpr (IsMinOrMax == MinOrMax::Min)
                m_jit.doubleMin(left, right, result);
            else
                m_jit.doubleMax(left, right, result);
            return;
#else
        // Entry
        Jump isEqual = is32
            ? m_jit.branchFloat(MacroAssembler::DoubleEqualAndOrdered, left, right)
            : m_jit.branchDouble(MacroAssembler::DoubleEqualAndOrdered, left, right);

        // Left is not equal to right
        Jump isLessThan = is32
            ? m_jit.branchFloat(MacroAssembler::DoubleLessThanAndOrdered, left, right)
            : m_jit.branchDouble(MacroAssembler::DoubleLessThanAndOrdered, left, right);

        // Left is not less than right
        Jump isGreaterThan = is32
            ? m_jit.branchFloat(MacroAssembler::DoubleGreaterThanAndOrdered, left, right)
            : m_jit.branchDouble(MacroAssembler::DoubleGreaterThanAndOrdered, left, right);

        // NaN
        if constexpr (is32)
            m_jit.addFloat(left, right, result);
        else
            m_jit.addDouble(left, right, result);
        Jump afterNaN = m_jit.jump();

        // Left is strictly greater than right
        isGreaterThan.link(&m_jit);
        auto isGreaterThanResult = IsMinOrMax == MinOrMax::Max ? left : right;
        m_jit.moveDouble(isGreaterThanResult, result);
        Jump afterGreaterThan = m_jit.jump();

        // Left is strictly less than right
        isLessThan.link(&m_jit);
        auto isLessThanResult = IsMinOrMax == MinOrMax::Max ? right : left;
        m_jit.moveDouble(isLessThanResult, result);
        Jump afterLessThan = m_jit.jump();

        // Left is equal to right
        isEqual.link(&m_jit);
        if constexpr (is32 && IsMinOrMax == MinOrMax::Max)
            m_jit.andFloat(left, right, result);
        else if constexpr (is32)
            m_jit.orFloat(left, right, result);
        else if constexpr (IsMinOrMax == MinOrMax::Max)
            m_jit.andDouble(left, right, result);
        else
            m_jit.orDouble(left, right, result);

        // Continuation
        afterNaN.link(&m_jit);
        afterGreaterThan.link(&m_jit);
        afterLessThan.link(&m_jit);
#endif
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Min(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F32Min", TypeKind::F32,
            BLOCK(Value::fromF32(std::min(lhs.asF32(), rhs.asF32()))),
            BLOCK(
                emitFloatingPointMinOrMax<float, MinOrMax::Min>(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                emitFloatingPointMinOrMax<float, MinOrMax::Min>(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Min(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F64Min", TypeKind::F64,
            BLOCK(Value::fromF64(std::min(lhs.asF64(), rhs.asF64()))),
            BLOCK(
                emitFloatingPointMinOrMax<double, MinOrMax::Min>(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                emitFloatingPointMinOrMax<double, MinOrMax::Min>(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Max(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F32Max", TypeKind::F32,
            BLOCK(Value::fromF32(std::max(lhs.asF32(), rhs.asF32()))),
            BLOCK(
                emitFloatingPointMinOrMax<float, MinOrMax::Max>(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                emitFloatingPointMinOrMax<float, MinOrMax::Max>(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Max(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F64Max", TypeKind::F64,
            BLOCK(Value::fromF64(std::max(lhs.asF64(), rhs.asF64()))),
            BLOCK(
                emitFloatingPointMinOrMax<double, MinOrMax::Max>(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                emitFloatingPointMinOrMax<double, MinOrMax::Max>(lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asFPR());
            )
        );
    }

    inline float BBQJIT::floatCopySign(float lhs, float rhs)
    {
        uint32_t lhsAsInt32 = bitwise_cast<uint32_t>(lhs);
        uint32_t rhsAsInt32 = bitwise_cast<uint32_t>(rhs);
        lhsAsInt32 &= 0x7fffffffu;
        rhsAsInt32 &= 0x80000000u;
        lhsAsInt32 |= rhsAsInt32;
        return bitwise_cast<float>(lhsAsInt32);
    }

    inline double BBQJIT::doubleCopySign(double lhs, double rhs)
    {
        uint64_t lhsAsInt64 = bitwise_cast<uint64_t>(lhs);
        uint64_t rhsAsInt64 = bitwise_cast<uint64_t>(rhs);
        lhsAsInt64 &= 0x7fffffffffffffffu;
        rhsAsInt64 &= 0x8000000000000000u;
        lhsAsInt64 |= rhsAsInt64;
        return bitwise_cast<double>(lhsAsInt64);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32And(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I32And", TypeKind::I32,
            BLOCK(Value::fromI32(lhs.asI32() & rhs.asI32())),
            BLOCK(
                m_jit.and32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(), resultLocation.asGPR());
                m_jit.and32(Imm32(ImmHelpers::imm(lhs, rhs).asI32()), resultLocation.asGPR());
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64And(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64And", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() & rhs.asI64())),
            BLOCK(
                m_jit.and64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(), resultLocation.asGPR());
                m_jit.and64(TrustedImm64(ImmHelpers::imm(lhs, rhs).asI64()), resultLocation.asGPR());
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64And(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64And", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() & rhs.asI64())),
            BLOCK(
                m_jit.and32(lhsLocation.asGPRhi(), rhsLocation.asGPRhi(), resultLocation.asGPRhi());
                m_jit.and32(lhsLocation.asGPRlo(), rhsLocation.asGPRlo(), resultLocation.asGPRlo());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPRhi(), resultLocation.asGPRhi());
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPRlo(), resultLocation.asGPRlo());
                m_jit.and32(TrustedImm32(static_cast<int32_t>(ImmHelpers::imm(lhs, rhs).asI64() >> 32)), resultLocation.asGPRhi());
                m_jit.and32(TrustedImm32(static_cast<int32_t>(ImmHelpers::imm(lhs, rhs).asI64() & 0xffffffff)), resultLocation.asGPRlo());
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Xor(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I32Xor", TypeKind::I32,
            BLOCK(Value::fromI32(lhs.asI32() ^ rhs.asI32())),
            BLOCK(
                m_jit.xor32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(), resultLocation.asGPR());
                m_jit.xor32(Imm32(ImmHelpers::imm(lhs, rhs).asI32()), resultLocation.asGPR());
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Xor(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Xor", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() ^ rhs.asI64())),
            BLOCK(
                m_jit.xor64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(), resultLocation.asGPR());
                m_jit.xor64(TrustedImm64(ImmHelpers::imm(lhs, rhs).asI64()), resultLocation.asGPR());
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Xor(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Xor", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() ^ rhs.asI64())),
            BLOCK(
                m_jit.xor32(lhsLocation.asGPRhi(), rhsLocation.asGPRhi(), resultLocation.asGPRhi());
                m_jit.xor32(lhsLocation.asGPRlo(), rhsLocation.asGPRlo(), resultLocation.asGPRlo());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPRhi(), resultLocation.asGPRhi());
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPRlo(), resultLocation.asGPRlo());
                m_jit.xor32(TrustedImm32(static_cast<int32_t>(ImmHelpers::imm(lhs, rhs).asI64() >> 32)), resultLocation.asGPRhi());
                m_jit.xor32(TrustedImm32(static_cast<int32_t>(ImmHelpers::imm(lhs, rhs).asI64() & 0xffffffff)), resultLocation.asGPRlo());
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Or(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I32Or", TypeKind::I32,
            BLOCK(Value::fromI32(lhs.asI32() | rhs.asI32())),
            BLOCK(
                m_jit.or32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(), resultLocation.asGPR());
                m_jit.or32(Imm32(ImmHelpers::imm(lhs, rhs).asI32()), resultLocation.asGPR());
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Or(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Or", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() | rhs.asI64())),
            BLOCK(
                m_jit.or64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPR(), resultLocation.asGPR());
                m_jit.or64(TrustedImm64(ImmHelpers::imm(lhs, rhs).asI64()), resultLocation.asGPR());
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Or(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "I64Or", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() | rhs.asI64())),
            BLOCK(
                m_jit.or32(lhsLocation.asGPRhi(), rhsLocation.asGPRhi(), resultLocation.asGPRhi());
                m_jit.or32(lhsLocation.asGPRlo(), rhsLocation.asGPRlo(), resultLocation.asGPRlo());
            ),
            BLOCK(
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPRhi(), resultLocation.asGPRhi());
                m_jit.move(ImmHelpers::regLocation(lhsLocation, rhsLocation).asGPRlo(), resultLocation.asGPRlo());
                m_jit.or32(TrustedImm32(static_cast<int32_t>(ImmHelpers::imm(lhs, rhs).asI64() >> 32)), resultLocation.asGPRhi());
                m_jit.or32(TrustedImm32(static_cast<int32_t>(ImmHelpers::imm(lhs, rhs).asI64() & 0xffffffff)), resultLocation.asGPRlo());
            )
        );
    }
#endif

#if CPU(X86_64)
#define PREPARE_FOR_SHIFT \
    do { \
        clobber(shiftRCX); \
    } while (false); \
    ScratchScope<0, 0> scratches(*this, Location::fromGPR(shiftRCX))
#else
#define PREPARE_FOR_SHIFT
#endif

    void BBQJIT::moveShiftAmountIfNecessary(Location& rhsLocation)
    {
        if constexpr (isX86()) {
            m_jit.move(rhsLocation.asGPR(), shiftRCX);
            rhsLocation = Location::fromGPR(shiftRCX);
        }
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Shl(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I32Shl", TypeKind::I32,
            BLOCK(Value::fromI32(lhs.asI32() << rhs.asI32())),
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.lshift32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.lshift32(lhsLocation.asGPR(), m_jit.trustedImm32ForShift(Imm32(rhs.asI32())), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, lhsLocation = Location::fromGPR(wasmScratchGPR));
                    m_jit.lshift32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Shl(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64Shl", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() << rhs.asI64())),
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.lshift64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.lshift64(lhsLocation.asGPR(), TrustedImm32(rhs.asI64()), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, lhsLocation = Location::fromGPR(wasmScratchGPR));
                    m_jit.lshift64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Shl(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64Shl", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() << rhs.asI64())),
            BLOCK(
                shiftI64Helper32(ShiftI64Helper32Op::Lshift, lhsLocation, rhsLocation, resultLocation);
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                shiftI64Helper32(ShiftI64Helper32Op::Lshift, lhsLocation, rhsLocation, resultLocation);
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32ShrS(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I32ShrS", TypeKind::I32,
            BLOCK(Value::fromI32(lhs.asI32() >> rhs.asI32())),
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.rshift32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.rshift32(lhsLocation.asGPR(), m_jit.trustedImm32ForShift(Imm32(rhs.asI32())), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, lhsLocation = Location::fromGPR(wasmScratchGPR));
                    m_jit.rshift32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ShrS(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64ShrS", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() >> rhs.asI64())),
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.rshift64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.rshift64(lhsLocation.asGPR(), TrustedImm32(rhs.asI64()), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, lhsLocation = Location::fromGPR(wasmScratchGPR));
                    m_jit.rshift64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ShrS(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64ShrS", TypeKind::I64,
            BLOCK(Value::fromI64(lhs.asI64() >> rhs.asI64())),
            BLOCK(
                shiftI64Helper32(ShiftI64Helper32Op::Rshift, lhsLocation, rhsLocation, resultLocation);
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                shiftI64Helper32(ShiftI64Helper32Op::Rshift, lhsLocation, rhsLocation, resultLocation);
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32ShrU(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I32ShrU", TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<uint32_t>(lhs.asI32()) >> static_cast<uint32_t>(rhs.asI32()))),
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.urshift32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.urshift32(lhsLocation.asGPR(), m_jit.trustedImm32ForShift(Imm32(rhs.asI32())), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, lhsLocation = Location::fromGPR(wasmScratchGPR));
                    m_jit.urshift32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ShrU(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64ShrU", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<uint64_t>(lhs.asI64()) >> static_cast<uint64_t>(rhs.asI64()))),
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.urshift64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.urshift64(lhsLocation.asGPR(), TrustedImm32(rhs.asI64()), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, lhsLocation = Location::fromGPR(wasmScratchGPR));
                    m_jit.urshift64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ShrU(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64ShrU", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<uint64_t>(lhs.asI64()) >> static_cast<uint64_t>(rhs.asI64()))),
            BLOCK(
                shiftI64Helper32(ShiftI64Helper32Op::Urshift, lhsLocation, rhsLocation, resultLocation);
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                shiftI64Helper32(ShiftI64Helper32Op::Urshift, lhsLocation, rhsLocation, resultLocation);
            )
        );
    }
#endif

#if USE(JSVALUE32_64)
    void BBQJIT::shiftI64Helper32(ShiftI64Helper32Op op, Location lhsLocation, Location rhsLocation, Location resultLocation)
    {
        auto shift = rhsLocation.asGPRlo();
        m_jit.and32(TrustedImm32(63), rhsLocation.asGPRlo(), shift);
        auto zero = m_jit.branch32(RelationalCondition::Equal, shift, TrustedImm32(0));
        auto aboveOrEqual32 = m_jit.branch32(RelationalCondition::AboveOrEqual, shift, TrustedImm32(32));
        // shift < 32
        ScratchScope<1, 0> scratches(*this, lhsLocation, rhsLocation, resultLocation);
        auto carry = scratches.gpr(0);
        m_jit.move(TrustedImm32(32), carry);
        m_jit.sub32(carry, shift, carry);
        if (op == ShiftI64Helper32Op::Lshift) {
            ASSERT(resultLocation.asGPRhi() != shift);
            ASSERT(resultLocation.asGPRhi() != lhsLocation.asGPRlo());
            m_jit.lshift32(lhsLocation.asGPRhi(), shift, resultLocation.asGPRhi());
            m_jit.urshift32(lhsLocation.asGPRlo(), carry, carry);
            m_jit.or32(carry, resultLocation.asGPRhi());
            m_jit.lshift32(lhsLocation.asGPRlo(), shift, resultLocation.asGPRlo());
        } else if (op == ShiftI64Helper32Op::Urshift) {
            m_jit.lshift32(lhsLocation.asGPRhi(), carry, carry);
            ASSERT(resultLocation.asGPRhi() != shift);
            ASSERT(resultLocation.asGPRhi() != lhsLocation.asGPRlo());
            m_jit.urshift32(lhsLocation.asGPRhi(), shift, resultLocation.asGPRhi());
            m_jit.urshift32(lhsLocation.asGPRlo(), shift, resultLocation.asGPRlo());
            m_jit.or32(carry, resultLocation.asGPRlo());
        } else if (op ==ShiftI64Helper32Op::Rshift) {
            m_jit.lshift32(lhsLocation.asGPRhi(), carry, carry);
            ASSERT(resultLocation.asGPRhi() != shift);
            ASSERT(resultLocation.asGPRhi() != lhsLocation.asGPRlo());
            m_jit.rshift32(lhsLocation.asGPRhi(), shift, resultLocation.asGPRhi());
            m_jit.urshift32(lhsLocation.asGPRlo(), shift, resultLocation.asGPRlo());
            m_jit.or32(carry, resultLocation.asGPRlo());
        }
        auto done = m_jit.jump();
        // shift >= 32
        aboveOrEqual32.link(&m_jit);
        m_jit.sub32(shift, TrustedImm32(32), shift);
        if (op == ShiftI64Helper32Op::Lshift) {
            m_jit.lshift32(lhsLocation.asGPRlo(), shift, resultLocation.asGPRhi());
            m_jit.xor32(resultLocation.asGPRlo(), resultLocation.asGPRlo());
        } else if (op == ShiftI64Helper32Op::Urshift) {
            m_jit.urshift32(lhsLocation.asGPRhi(), shift, resultLocation.asGPRlo());
            m_jit.xor32(resultLocation.asGPRhi(), resultLocation.asGPRhi());
        } else if (op ==ShiftI64Helper32Op::Rshift) {
            ASSERT(resultLocation.asGPRlo() != lhsLocation.asGPRhi());
            m_jit.rshift32(lhsLocation.asGPRhi(), shift, resultLocation.asGPRlo());
            m_jit.rshift32(lhsLocation.asGPRhi(), TrustedImm32(31), resultLocation.asGPRhi());
        }
        // shift == 0
        zero.link(&m_jit);
        done.link(&m_jit);
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Rotl(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I32Rotl", TypeKind::I32,
            BLOCK(Value::fromI32(B3::rotateLeft(lhs.asI32(), rhs.asI32()))),
#if CPU(X86_64)
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.rotateLeft32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.rotateLeft32(lhsLocation.asGPR(), m_jit.trustedImm32ForShift(Imm32(rhs.asI32())), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, resultLocation);
                    m_jit.rotateLeft32(resultLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
#else
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.neg32(rhsLocation.asGPR(), wasmScratchGPR);
                m_jit.rotateRight32(lhsLocation.asGPR(), wasmScratchGPR, resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.rotateRight32(lhsLocation.asGPR(), m_jit.trustedImm32ForShift(Imm32(-rhs.asI32())), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    m_jit.neg32(rhsLocation.asGPR(), wasmScratchGPR);
                    emitMoveConst(lhs, resultLocation);
                    m_jit.rotateRight32(resultLocation.asGPR(), wasmScratchGPR, resultLocation.asGPR());
                }
            )
#endif
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Rotl(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64Rotl", TypeKind::I64,
            BLOCK(Value::fromI64(B3::rotateLeft(lhs.asI64(), rhs.asI64()))),
#if CPU(X86_64)
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.rotateLeft64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.rotateLeft64(lhsLocation.asGPR(), TrustedImm32(rhs.asI32()), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, resultLocation);
                    m_jit.rotateLeft64(resultLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
#else
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.neg64(rhsLocation.asGPR(), wasmScratchGPR);
                m_jit.rotateRight64(lhsLocation.asGPR(), wasmScratchGPR, resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.rotateRight64(lhsLocation.asGPR(), TrustedImm32(-rhs.asI64()), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    m_jit.neg64(rhsLocation.asGPR(), wasmScratchGPR);
                    emitMoveConst(lhs, resultLocation);
                    m_jit.rotateRight64(resultLocation.asGPR(), wasmScratchGPR, resultLocation.asGPR());
                }
            )
#endif
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Rotl(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64Rotl", TypeKind::I64,
            BLOCK(Value::fromI64(B3::rotateLeft(lhs.asI64(), rhs.asI64()))),
            BLOCK(
                rotI64Helper32(RotI64Helper32Op::Left, lhsLocation, rhsLocation, resultLocation);
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                rotI64Helper32(RotI64Helper32Op::Left, lhsLocation, rhsLocation, resultLocation);
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Rotr(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I32Rotr", TypeKind::I32,
            BLOCK(Value::fromI32(B3::rotateRight(lhs.asI32(), rhs.asI32()))),
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.rotateRight32(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.rotateRight32(lhsLocation.asGPR(), m_jit.trustedImm32ForShift(Imm32(rhs.asI32())), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, Location::fromGPR(wasmScratchGPR));
                    m_jit.rotateRight32(wasmScratchGPR, rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Rotr(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64Rotr", TypeKind::I64,
            BLOCK(Value::fromI64(B3::rotateRight(lhs.asI64(), rhs.asI64()))),
            BLOCK(
                moveShiftAmountIfNecessary(rhsLocation);
                m_jit.rotateRight64(lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.rotateRight64(lhsLocation.asGPR(), TrustedImm32(rhs.asI64()), resultLocation.asGPR());
                else {
                    moveShiftAmountIfNecessary(rhsLocation);
                    emitMoveConst(lhs, Location::fromGPR(wasmScratchGPR));
                    m_jit.rotateRight64(wasmScratchGPR, rhsLocation.asGPR(), resultLocation.asGPR());
                }
            )
        );
    }
#else
PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Rotr(Value lhs, Value rhs, Value& result)
    {
        PREPARE_FOR_SHIFT;
        EMIT_BINARY(
            "I64Rotr", TypeKind::I64,
            BLOCK(Value::fromI64(B3::rotateRight(lhs.asI64(), rhs.asI64()))),
            BLOCK(
                rotI64Helper32(RotI64Helper32Op::Right, lhsLocation, rhsLocation, resultLocation);
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                rotI64Helper32(RotI64Helper32Op::Right, lhsLocation, rhsLocation, resultLocation);
            )
        );
    }
#endif

#if USE(JSVALUE32_64)
    void BBQJIT::rotI64Helper32(RotI64Helper32Op op, Location lhsLocation, Location rhsLocation, Location resultLocation)
    {
        // NB: this only works as long as result is allocated in lhsLocation!
        auto carry = rhsLocation.asGPRhi();
        auto shift = rhsLocation.asGPRlo();
        ScratchScope<2, 0> scratches(*this, lhsLocation, rhsLocation, resultLocation);
        auto tmpHi = scratches.gpr(0);
        auto tmpLo = scratches.gpr(1);
        m_jit.move(lhsLocation.asGPRhi(), resultLocation.asGPRhi());
        m_jit.move(lhsLocation.asGPRlo(), resultLocation.asGPRlo());
        m_jit.and32(TrustedImm32(63), shift, shift);
        auto rotate = m_jit.branch32(RelationalCondition::LessThan, shift, TrustedImm32(32));
        // swap
        m_jit.swap(resultLocation.asGPRhi(), resultLocation.asGPRlo());
        rotate.link(&m_jit);
        m_jit.and32(TrustedImm32(31), shift, shift);
        auto zero = m_jit.branch32(RelationalCondition::Equal, shift, TrustedImm32(0));
        // rotate
        m_jit.move(TrustedImm32(32), carry);
        m_jit.sub32(carry, shift, carry);
        if (op == RotI64Helper32Op::Left) {
            m_jit.urshift32(resultLocation.asGPRhi(), carry, tmpLo);
            m_jit.urshift32(resultLocation.asGPRlo(), carry, tmpHi);
            m_jit.lshift32(resultLocation.asGPRhi(), shift, resultLocation.asGPRhi());
            m_jit.lshift32(resultLocation.asGPRlo(), shift, resultLocation.asGPRlo());
        } else if (op == RotI64Helper32Op::Right) {
            m_jit.lshift32(resultLocation.asGPRhi(), carry, tmpLo);
            m_jit.lshift32(resultLocation.asGPRlo(), carry, tmpHi);
            m_jit.urshift32(resultLocation.asGPRhi(), shift, resultLocation.asGPRhi());
            m_jit.urshift32(resultLocation.asGPRlo(), shift, resultLocation.asGPRlo());
        }
        m_jit.or32(tmpHi, resultLocation.asGPRhi());
        m_jit.or32(tmpLo, resultLocation.asGPRlo());
        zero.link(&m_jit);
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Clz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32Clz", TypeKind::I32,
            BLOCK(Value::fromI32(WTF::clz(operand.asI32()))),
            BLOCK(
                m_jit.countLeadingZeros32(operandLocation.asGPR(), resultLocation.asGPR());
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Clz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Clz", TypeKind::I64,
            BLOCK(Value::fromI64(WTF::clz(operand.asI64()))),
            BLOCK(
                m_jit.countLeadingZeros64(operandLocation.asGPR(), resultLocation.asGPR());
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Clz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Clz", TypeKind::I64,
            BLOCK(Value::fromI64(WTF::clz(operand.asI64()))),
            BLOCK(
                m_jit.countLeadingZeros32(operandLocation.asGPRhi(), resultLocation.asGPRhi());
                Jump done = m_jit.branch32(RelationalCondition::LessThan, resultLocation.asGPRhi(), TrustedImm32(32));
                m_jit.countLeadingZeros32(operandLocation.asGPRlo(), resultLocation.asGPRhi());
                m_jit.add32(TrustedImm32(32), resultLocation.asGPRhi());
                done.link(&m_jit);
                m_jit.move(resultLocation.asGPRhi(), resultLocation.asGPRlo());
                m_jit.xor32(resultLocation.asGPRhi(), resultLocation.asGPRhi());
            )
        );
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Ctz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32Ctz", TypeKind::I32,
            BLOCK(Value::fromI32(WTF::ctz(operand.asI32()))),
            BLOCK(
                m_jit.countTrailingZeros32(operandLocation.asGPR(), resultLocation.asGPR());
            )
        );
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Ctz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Ctz", TypeKind::I64,
            BLOCK(Value::fromI64(WTF::ctz(operand.asI64()))),
            BLOCK(
                m_jit.countTrailingZeros64(operandLocation.asGPR(), resultLocation.asGPR());
            )
        );
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Ctz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Ctz", TypeKind::I64,
            BLOCK(Value::fromI64(WTF::ctz(operand.asI64()))),
            BLOCK(
                m_jit.countTrailingZeros32(operandLocation.asGPRlo(), resultLocation.asGPRlo());
                Jump done = m_jit.branch32(RelationalCondition::LessThan, resultLocation.asGPRlo(), TrustedImm32(32));
                m_jit.countTrailingZeros32(operandLocation.asGPRhi(), resultLocation.asGPRlo());
                m_jit.add32(TrustedImm32(32), resultLocation.asGPRlo());
                done.link(&m_jit);
                m_jit.xor32(resultLocation.asGPRhi(), resultLocation.asGPRhi());
            )
        );
    }
#endif

    PartialResult BBQJIT::emitCompareI32(const char* opcode, Value& lhs, Value& rhs, Value& result, RelationalCondition condition, bool (*comparator)(int32_t lhs, int32_t rhs))
    {
        EMIT_BINARY(
            opcode, TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(comparator(lhs.asI32(), rhs.asI32())))),
            BLOCK(
                m_jit.compare32(condition, lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                if (rhs.isConst())
                    m_jit.compare32(condition, lhsLocation.asGPR(), Imm32(rhs.asI32()), resultLocation.asGPR());
                else
                    m_jit.compare32(condition, Imm32(lhs.asI32()), rhsLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult BBQJIT::emitCompareI64(const char* opcode, Value& lhs, Value& rhs, Value& result, RelationalCondition condition, bool (*comparator)(int64_t lhs, int64_t rhs))
    {
        EMIT_BINARY(
            opcode, TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(comparator(lhs.asI64(), rhs.asI64())))),
            BLOCK(
                m_jit.compare64(condition, lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR(wasmScratchGPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR(wasmScratchGPR));
                m_jit.compare64(condition, lhsLocation.asGPR(), rhsLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult BBQJIT::emitCompareI64(const char* opcode, Value& lhs, Value& rhs, Value& result, RelationalCondition condition, bool (*comparator)(int64_t lhs, int64_t rhs))
    {
        EMIT_BINARY(
            opcode, TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(comparator(lhs.asI64(), rhs.asI64())))),
            BLOCK(
                compareI64Helper32(condition, lhsLocation, rhsLocation, resultLocation);
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                compareI64Helper32(condition, lhsLocation, rhsLocation, resultLocation);
            )
        )
    }

    void BBQJIT::compareI64Helper32(RelationalCondition condition, Location lhsLocation, Location rhsLocation, Location resultLocation)
    {
        if (condition == MacroAssembler::Equal || condition == MacroAssembler::NotEqual) {
            ScratchScope<1, 0> scratches(*this, lhsLocation, rhsLocation, resultLocation);
            auto tmp = Location::fromGPR(scratches.gpr(0));
            m_jit.move(TrustedImm32(condition == MacroAssembler::NotEqual), tmp.asGPR());
            auto compareLo = m_jit.branch32(RelationalCondition::NotEqual, lhsLocation.asGPRhi(), rhsLocation.asGPRhi());
            m_jit.compare32(condition, lhsLocation.asGPRlo(), rhsLocation.asGPRlo(), tmp.asGPR());
            compareLo.link(&m_jit);
            m_jit.move(tmp.asGPR(), resultLocation.asGPR());
        } else {
            auto compareLo = m_jit.branch32(RelationalCondition::Equal, lhsLocation.asGPRhi(), rhsLocation.asGPRhi());
            m_jit.compare32(condition, lhsLocation.asGPRhi(), rhsLocation.asGPRhi(), resultLocation.asGPR());
            auto done = m_jit.jump();
            compareLo.link(&m_jit);
            // Signed to unsigned, leave the rest alone
            RelationalCondition loCond = condition;
            switch (condition) {
            case MacroAssembler::GreaterThan:
                loCond = MacroAssembler::Above;
                break;
            case MacroAssembler::LessThan:
                loCond = MacroAssembler::Below;
                break;
            case MacroAssembler::GreaterThanOrEqual:
                loCond = MacroAssembler::AboveOrEqual;
                break;
            case MacroAssembler::LessThanOrEqual:
                loCond = MacroAssembler::BelowOrEqual;
                break;
            default:
                break;
            }
            m_jit.compare32(loCond, lhsLocation.asGPRlo(), rhsLocation.asGPRlo(), resultLocation.asGPR());
            done.link(&m_jit);
        }
    }
#endif

#define RELOP_AS_LAMBDA(op) [](auto lhs, auto rhs) -> auto { return lhs op rhs; }
#define TYPED_RELOP_AS_LAMBDA(type, op) [](auto lhs, auto rhs) -> auto { return static_cast<type>(lhs) op static_cast<type>(rhs); }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Eq(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32Eq", lhs, rhs, result, RelationalCondition::Equal, RELOP_AS_LAMBDA( == ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Eq(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64Eq", lhs, rhs, result, RelationalCondition::Equal, RELOP_AS_LAMBDA( == ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Ne(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32Ne", lhs, rhs, result, RelationalCondition::NotEqual, RELOP_AS_LAMBDA( != ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Ne(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64Ne", lhs, rhs, result, RelationalCondition::NotEqual, RELOP_AS_LAMBDA( != ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32LtS(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32LtS", lhs, rhs, result, RelationalCondition::LessThan, RELOP_AS_LAMBDA( < ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64LtS(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64LtS", lhs, rhs, result, RelationalCondition::LessThan, RELOP_AS_LAMBDA( < ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32LeS(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32LeS", lhs, rhs, result, RelationalCondition::LessThanOrEqual, RELOP_AS_LAMBDA( <= ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64LeS(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64LeS", lhs, rhs, result, RelationalCondition::LessThanOrEqual, RELOP_AS_LAMBDA( <= ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32GtS(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32GtS", lhs, rhs, result, RelationalCondition::GreaterThan, RELOP_AS_LAMBDA( > ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64GtS(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64GtS", lhs, rhs, result, RelationalCondition::GreaterThan, RELOP_AS_LAMBDA( > ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32GeS(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32GeS", lhs, rhs, result, RelationalCondition::GreaterThanOrEqual, RELOP_AS_LAMBDA( >= ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64GeS(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64GeS", lhs, rhs, result, RelationalCondition::GreaterThanOrEqual, RELOP_AS_LAMBDA( >= ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32LtU(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32LtU", lhs, rhs, result, RelationalCondition::Below, TYPED_RELOP_AS_LAMBDA(uint32_t, <));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64LtU(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64LtU", lhs, rhs, result, RelationalCondition::Below, TYPED_RELOP_AS_LAMBDA(uint64_t, <));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32LeU(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32LeU", lhs, rhs, result, RelationalCondition::BelowOrEqual, TYPED_RELOP_AS_LAMBDA(uint32_t, <=));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64LeU(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64LeU", lhs, rhs, result, RelationalCondition::BelowOrEqual, TYPED_RELOP_AS_LAMBDA(uint64_t, <=));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32GtU(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32GtU", lhs, rhs, result, RelationalCondition::Above, TYPED_RELOP_AS_LAMBDA(uint32_t, >));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64GtU(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64GtU", lhs, rhs, result, RelationalCondition::Above, TYPED_RELOP_AS_LAMBDA(uint64_t, >));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32GeU(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI32("I32GeU", lhs, rhs, result, RelationalCondition::AboveOrEqual, TYPED_RELOP_AS_LAMBDA(uint32_t, >=));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64GeU(Value lhs, Value rhs, Value& result)
    {
        return emitCompareI64("I64GeU", lhs, rhs, result, RelationalCondition::AboveOrEqual, TYPED_RELOP_AS_LAMBDA(uint64_t, >=));
    }

    PartialResult BBQJIT::emitCompareF32(const char* opcode, Value& lhs, Value& rhs, Value& result, DoubleCondition condition, bool (*comparator)(float lhs, float rhs))
    {
        EMIT_BINARY(
            opcode, TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(comparator(lhs.asF32(), rhs.asF32())))),
            BLOCK(
                m_jit.compareFloat(condition, lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asGPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                m_jit.compareFloat(condition, lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asGPR());
            )
        )
    }

    PartialResult BBQJIT::emitCompareF64(const char* opcode, Value& lhs, Value& rhs, Value& result, DoubleCondition condition, bool (*comparator)(double lhs, double rhs))
    {
        EMIT_BINARY(
            opcode, TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(comparator(lhs.asF64(), rhs.asF64())))),
            BLOCK(
                m_jit.compareDouble(condition, lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asGPR());
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromFPR(wasmScratchFPR);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromFPR(wasmScratchFPR));
                m_jit.compareDouble(condition, lhsLocation.asFPR(), rhsLocation.asFPR(), resultLocation.asGPR());
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Eq(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF32("F32Eq", lhs, rhs, result, DoubleCondition::DoubleEqualAndOrdered, RELOP_AS_LAMBDA( == ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Eq(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF64("F64Eq", lhs, rhs, result, DoubleCondition::DoubleEqualAndOrdered, RELOP_AS_LAMBDA( == ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Ne(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF32("F32Ne", lhs, rhs, result, DoubleCondition::DoubleNotEqualOrUnordered, RELOP_AS_LAMBDA( != ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Ne(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF64("F64Ne", lhs, rhs, result, DoubleCondition::DoubleNotEqualOrUnordered, RELOP_AS_LAMBDA( != ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Lt(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF32("F32Lt", lhs, rhs, result, DoubleCondition::DoubleLessThanAndOrdered, RELOP_AS_LAMBDA( < ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Lt(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF64("F64Lt", lhs, rhs, result, DoubleCondition::DoubleLessThanAndOrdered, RELOP_AS_LAMBDA( < ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Le(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF32("F32Le", lhs, rhs, result, DoubleCondition::DoubleLessThanOrEqualAndOrdered, RELOP_AS_LAMBDA( <= ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Le(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF64("F64Le", lhs, rhs, result, DoubleCondition::DoubleLessThanOrEqualAndOrdered, RELOP_AS_LAMBDA( <= ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Gt(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF32("F32Gt", lhs, rhs, result, DoubleCondition::DoubleGreaterThanAndOrdered, RELOP_AS_LAMBDA( > ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Gt(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF64("F64Gt", lhs, rhs, result, DoubleCondition::DoubleGreaterThanAndOrdered, RELOP_AS_LAMBDA( > ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Ge(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF32("F32Ge", lhs, rhs, result, DoubleCondition::DoubleGreaterThanOrEqualAndOrdered, RELOP_AS_LAMBDA( >= ));
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Ge(Value lhs, Value rhs, Value& result)
    {
        return emitCompareF64("F64Ge", lhs, rhs, result, DoubleCondition::DoubleGreaterThanOrEqualAndOrdered, RELOP_AS_LAMBDA( >= ));
    }

#undef RELOP_AS_LAMBDA
#undef TYPED_RELOP_AS_LAMBDA

#if USE(JSVALUE64)
    PartialResult BBQJIT::addI32WrapI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32WrapI64", TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(operand.asI64()))),
            BLOCK(
                m_jit.move(operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult BBQJIT::addI32WrapI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32WrapI64", TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(operand.asI64()))),
            BLOCK(
                m_jit.move(operandLocation.asGPRlo(), resultLocation.asGPR());
            )
        )
    }
#endif

    PartialResult BBQJIT::addI32Extend8S(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32Extend8S", TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(static_cast<int8_t>(operand.asI32())))),
            BLOCK(
                m_jit.signExtend8To32(operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Extend16S(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32Extend16S", TypeKind::I32,
            BLOCK(Value::fromI32(static_cast<int32_t>(static_cast<int16_t>(operand.asI32())))),
            BLOCK(
                m_jit.signExtend16To32(operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Extend8S(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Extend8S", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<int64_t>(static_cast<int8_t>(operand.asI64())))),
            BLOCK(
                m_jit.signExtend8To64(operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Extend8S(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Extend8S", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<int64_t>(static_cast<int8_t>(operand.asI64())))),
            BLOCK(
                GPRReg operandReg = operandLocation.isGPR() ? operandLocation.asGPR() : operandLocation.asGPRlo();
                m_jit.signExtend8To32(operandReg, resultLocation.asGPRlo());
                m_jit.rshift32(resultLocation.asGPRlo(), TrustedImm32(31), resultLocation.asGPRhi());
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Extend16S(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Extend16S", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<int64_t>(static_cast<int16_t>(operand.asI64())))),
            BLOCK(
                m_jit.signExtend16To64(operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Extend16S(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Extend16S", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<int64_t>(static_cast<int16_t>(operand.asI64())))),
            BLOCK(
                GPRReg operandReg = operandLocation.isGPR() ? operandLocation.asGPR() : operandLocation.asGPRlo();
                m_jit.signExtend16To32(operandReg, resultLocation.asGPRlo());
                m_jit.rshift32(resultLocation.asGPRlo(), TrustedImm32(31), resultLocation.asGPRhi());
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Extend32S(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Extend32S", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<int64_t>(static_cast<int32_t>(operand.asI64())))),
            BLOCK(
                m_jit.signExtend32To64(operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Extend32S(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Extend32S", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<int64_t>(static_cast<int32_t>(operand.asI64())))),
            BLOCK(
                GPRReg operandReg = operandLocation.isGPR() ? operandLocation.asGPR() : operandLocation.asGPRlo();
                m_jit.move(operandReg, resultLocation.asGPRlo());
                m_jit.rshift32(resultLocation.asGPRlo(), TrustedImm32(31), resultLocation.asGPRhi());
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ExtendSI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64ExtendSI32", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<int64_t>(operand.asI32()))),
            BLOCK(
                m_jit.signExtend32To64(operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ExtendSI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64ExtendSI32", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<int64_t>(operand.asI32()))),
            BLOCK(
                GPRReg operandReg = operandLocation.isGPR() ? operandLocation.asGPR() : operandLocation.asGPRlo();
                m_jit.move(operandReg, resultLocation.asGPRlo());
                m_jit.rshift32(resultLocation.asGPRlo(), TrustedImm32(31), resultLocation.asGPRhi());
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ExtendUI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64ExtendUI32", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<uint64_t>(static_cast<uint32_t>(operand.asI32())))),
            BLOCK(
                m_jit.zeroExtend32ToWord(operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ExtendUI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64ExtendUI32", TypeKind::I64,
            BLOCK(Value::fromI64(static_cast<uint64_t>(static_cast<uint32_t>(operand.asI32())))),
            BLOCK(
                GPRReg operandReg = operandLocation.isGPR() ? operandLocation.asGPR() : operandLocation.asGPRlo();
                m_jit.move(operandReg, resultLocation.asGPRlo());
                m_jit.xor32(resultLocation.asGPRhi(), resultLocation.asGPRhi());
            )
        )
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Eqz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32Eqz", TypeKind::I32,
            BLOCK(Value::fromI32(!operand.asI32())),
            BLOCK(
                m_jit.test32(ResultCondition::Zero, operandLocation.asGPR(), operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Eqz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Eqz", TypeKind::I32,
            BLOCK(Value::fromI32(!operand.asI64())),
            BLOCK(
                m_jit.test64(ResultCondition::Zero, operandLocation.asGPR(), operandLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Eqz(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Eqz", TypeKind::I32,
            BLOCK(Value::fromI32(!operand.asI64())),
            BLOCK(
                m_jit.or32(operandLocation.asGPRhi(), operandLocation.asGPRlo(), resultLocation.asGPR());
                m_jit.test32(ResultCondition::Zero, resultLocation.asGPR(), resultLocation.asGPR(), resultLocation.asGPR());
            )
        )
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32Popcnt(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32Popcnt", TypeKind::I32,
            BLOCK(Value::fromI32(std::popcount(static_cast<uint32_t>(operand.asI32())))),
            BLOCK(
                if (m_jit.supportsCountPopulation())
                    m_jit.countPopulation32(operandLocation.asGPR(), resultLocation.asGPR(), wasmScratchFPR);
                else {
                    // The EMIT_UNARY(...) macro will already assign result to the top value on the stack and give it a register,
                    // so it should be able to be passed in. However, this creates a somewhat nasty tacit dependency - emitCCall
                    // will bind result to the returnValueGPR, which will error if result is already bound to a different register
                    // (to avoid mucking with the register allocator state). It shouldn't currently error specifically because we
                    // only allocate caller-saved registers, which get flushed across the call regardless; however, if we add
                    // callee-saved register allocation to BBQJIT in the future, we could get very niche errors.
                    //
                    // We avoid this by consuming the result before passing it to emitCCall, which also saves us the mov for spilling.
                    consume(result);
                    auto arg = Value::pinned(TypeKind::I32, operandLocation);
                    emitCCall(&operationPopcount32, Vector<Value> { arg }, result);
                }
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64Popcnt(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64Popcnt", TypeKind::I64,
            BLOCK(Value::fromI64(std::popcount(static_cast<uint64_t>(operand.asI64())))),
            BLOCK(
                if (m_jit.supportsCountPopulation())
                    m_jit.countPopulation64(operandLocation.asGPR(), resultLocation.asGPR(), wasmScratchFPR);
                else {
                    // The EMIT_UNARY(...) macro will already assign result to the top value on the stack and give it a register,
                    // so it should be able to be passed in. However, this creates a somewhat nasty tacit dependency - emitCCall
                    // will bind result to the returnValueGPR, which will error if result is already bound to a different register
                    // (to avoid mucking with the register allocator state). It shouldn't currently error specifically because we
                    // only allocate caller-saved registers, which get flushed across the call regardless; however, if we add
                    // callee-saved register allocation to BBQJIT in the future, we could get very niche errors.
                    //
                    // We avoid this by consuming the result before passing it to emitCCall, which also saves us the mov for spilling.
                    consume(result);
                    auto arg = Value::pinned(TypeKind::I64, operandLocation);
                    emitCCall(&operationPopcount64, Vector<Value> { arg }, result);
                }
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32ReinterpretF32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I32ReinterpretF32", TypeKind::I32,
            BLOCK(Value::fromI32(bitwise_cast<int32_t>(operand.asF32()))),
            BLOCK(
                m_jit.moveFloatTo32(operandLocation.asFPR(), resultLocation.asGPR());
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ReinterpretF64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64ReinterpretF64", TypeKind::I64,
            BLOCK(Value::fromI64(bitwise_cast<int64_t>(operand.asF64()))),
            BLOCK(
                m_jit.moveDoubleTo64(operandLocation.asFPR(), resultLocation.asGPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64ReinterpretF64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "I64ReinterpretF64", TypeKind::I64,
            BLOCK(Value::fromI64(bitwise_cast<int64_t>(operand.asF64()))),
            BLOCK(
                m_jit.moveDoubleTo64(operandLocation.asFPR(), resultLocation.asGPRhi(), resultLocation.asGPRlo());
            )
        )
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32ReinterpretI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32ReinterpretI32", TypeKind::F32,
            BLOCK(Value::fromF32(bitwise_cast<float>(operand.asI32()))),
            BLOCK(
                m_jit.move32ToFloat(operandLocation.asGPR(), resultLocation.asFPR());
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ReinterpretI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ReinterpretI64", TypeKind::F64,
            BLOCK(Value::fromF64(bitwise_cast<double>(operand.asI64()))),
            BLOCK(
                m_jit.move64ToDouble(operandLocation.asGPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ReinterpretI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ReinterpretI64", TypeKind::F64,
            BLOCK(Value::fromF64(bitwise_cast<double>(operand.asI64()))),
            BLOCK(
                m_jit.moveIntsToDouble(operandLocation.asGPRlo(), operandLocation.asGPRhi(), resultLocation.asFPR());
            )
        )
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32DemoteF64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32DemoteF64", TypeKind::F32,
            BLOCK(Value::fromF32(operand.asF64())),
            BLOCK(
                m_jit.convertDoubleToFloat(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64PromoteF32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64PromoteF32", TypeKind::F64,
            BLOCK(Value::fromF64(operand.asF32())),
            BLOCK(
                m_jit.convertFloatToDouble(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32ConvertSI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32ConvertSI32", TypeKind::F32,
            BLOCK(Value::fromF32(operand.asI32())),
            BLOCK(
                m_jit.convertInt32ToFloat(operandLocation.asGPR(), resultLocation.asFPR());
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32ConvertUI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32ConvertUI32", TypeKind::F32,
            BLOCK(Value::fromF32(static_cast<uint32_t>(operand.asI32()))),
            BLOCK(
#if CPU(X86_64)
                ScratchScope<1, 0> scratches(*this);
                m_jit.zeroExtend32ToWord(operandLocation.asGPR(), wasmScratchGPR);
                m_jit.convertUInt64ToFloat(wasmScratchGPR, resultLocation.asFPR(), scratches.gpr(0));
#else
                m_jit.zeroExtend32ToWord(operandLocation.asGPR(), wasmScratchGPR);
                m_jit.convertUInt64ToFloat(wasmScratchGPR, resultLocation.asFPR());
#endif
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32ConvertUI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32ConvertUI32", TypeKind::F32,
            BLOCK(Value::fromF32(static_cast<uint32_t>(operand.asI32()))),
            BLOCK(
                m_jit.convertUInt32ToFloat(operandLocation.asGPR(), resultLocation.asFPR());
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32ConvertSI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32ConvertSI64", TypeKind::F32,
            BLOCK(Value::fromF32(operand.asI64())),
            BLOCK(
                m_jit.convertInt64ToFloat(operandLocation.asGPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32ConvertSI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32ConvertSI64", TypeKind::F32,
            BLOCK(Value::fromF32(operand.asI64())),
            BLOCK(
                auto arg = Value::pinned(TypeKind::I64, operandLocation);
                consume(result);
                emitCCall(Math::f32_convert_s_i64, Vector<Value> { arg }, result);
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32ConvertUI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32ConvertUI64", TypeKind::F32,
            BLOCK(Value::fromF32(static_cast<uint64_t>(operand.asI64()))),
            BLOCK(
#if CPU(X86_64)
                m_jit.convertUInt64ToFloat(operandLocation.asGPR(), resultLocation.asFPR(), wasmScratchGPR);
#else
                m_jit.convertUInt64ToFloat(operandLocation.asGPR(), resultLocation.asFPR());
#endif
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32ConvertUI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32ConvertUI64", TypeKind::F32,
            BLOCK(Value::fromF32(static_cast<uint64_t>(operand.asI64()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::I64, operandLocation);
                consume(result);
                emitCCall(Math::f32_convert_u_i64, Vector<Value> { arg }, result);
            )
        )
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ConvertSI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ConvertSI32", TypeKind::F64,
            BLOCK(Value::fromF64(operand.asI32())),
            BLOCK(
                m_jit.convertInt32ToDouble(operandLocation.asGPR(), resultLocation.asFPR());
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ConvertUI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ConvertUI32", TypeKind::F64,
            BLOCK(Value::fromF64(static_cast<uint32_t>(operand.asI32()))),
            BLOCK(
#if CPU(X86_64)
                ScratchScope<1, 0> scratches(*this);
                m_jit.zeroExtend32ToWord(operandLocation.asGPR(), wasmScratchGPR);
                m_jit.convertUInt64ToDouble(wasmScratchGPR, resultLocation.asFPR(), scratches.gpr(0));
#else
                m_jit.zeroExtend32ToWord(operandLocation.asGPR(), wasmScratchGPR);
                m_jit.convertUInt64ToDouble(wasmScratchGPR, resultLocation.asFPR());
#endif
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ConvertUI32(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ConvertUI32", TypeKind::F64,
            BLOCK(Value::fromF64(static_cast<uint32_t>(operand.asI32()))),
            BLOCK(
                m_jit.convertUInt32ToDouble(operandLocation.asGPR(), resultLocation.asFPR());
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ConvertSI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ConvertSI64", TypeKind::F64,
            BLOCK(Value::fromF64(operand.asI64())),
            BLOCK(
                m_jit.convertInt64ToDouble(operandLocation.asGPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ConvertSI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ConvertSI64", TypeKind::F64,
            BLOCK(Value::fromF64(operand.asI64())),
            BLOCK(
                auto arg = Value::pinned(TypeKind::I64, operandLocation);
                consume(result);
                emitCCall(Math::f64_convert_s_i64, Vector<Value> { arg }, result);
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ConvertUI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ConvertUI64", TypeKind::F64,
            BLOCK(Value::fromF64(static_cast<uint64_t>(operand.asI64()))),
            BLOCK(
#if CPU(X86_64)
                m_jit.convertUInt64ToDouble(operandLocation.asGPR(), resultLocation.asFPR(), wasmScratchGPR);
#else
                m_jit.convertUInt64ToDouble(operandLocation.asGPR(), resultLocation.asFPR());
#endif
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64ConvertUI64(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64ConvertUI64", TypeKind::F64,
            BLOCK(Value::fromF64(static_cast<uint64_t>(operand.asI64()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::I64, operandLocation);
                consume(result);
                emitCCall(Math::f64_convert_u_i64, Vector<Value> { arg }, result);
            )
        )
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Copysign(Value lhs, Value rhs, Value& result)
    {
        EMIT_BINARY(
            "F32Copysign", TypeKind::F32,
            BLOCK(Value::fromF32(floatCopySign(lhs.asF32(), rhs.asF32()))),
            BLOCK(
                // FIXME: Better than what we have in the Air backend, but still not great. I think
                // there's some vector instruction we can use to do this much quicker.

#if CPU(X86_64)
                m_jit.moveFloatTo32(lhsLocation.asFPR(), wasmScratchGPR);
                m_jit.and32(TrustedImm32(0x7fffffff), wasmScratchGPR);
                m_jit.move32ToFloat(wasmScratchGPR, wasmScratchFPR);
                m_jit.moveFloatTo32(rhsLocation.asFPR(), wasmScratchGPR);
                m_jit.and32(TrustedImm32(static_cast<int32_t>(0x80000000u)), wasmScratchGPR, wasmScratchGPR);
                m_jit.move32ToFloat(wasmScratchGPR, resultLocation.asFPR());
                m_jit.orFloat(resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
                m_jit.moveFloatTo32(rhsLocation.asFPR(), wasmScratchGPR);
                m_jit.and32(TrustedImm32(static_cast<int32_t>(0x80000000u)), wasmScratchGPR, wasmScratchGPR);
                m_jit.move32ToFloat(wasmScratchGPR, wasmScratchFPR);
                m_jit.absFloat(lhsLocation.asFPR(), lhsLocation.asFPR());
                m_jit.orFloat(lhsLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#endif
            ),
            BLOCK(
                if (lhs.isConst()) {
                    m_jit.moveFloatTo32(rhsLocation.asFPR(), wasmScratchGPR);
                    m_jit.and32(TrustedImm32(static_cast<int32_t>(0x80000000u)), wasmScratchGPR, wasmScratchGPR);
                    m_jit.move32ToFloat(wasmScratchGPR, wasmScratchFPR);

                    emitMoveConst(Value::fromF32(std::abs(lhs.asF32())), resultLocation);
                    m_jit.orFloat(resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                } else {
                    bool signBit = bitwise_cast<uint32_t>(rhs.asF32()) & 0x80000000u;
#if CPU(X86_64)
                    m_jit.moveDouble(lhsLocation.asFPR(), resultLocation.asFPR());
                    m_jit.move32ToFloat(TrustedImm32(0x7fffffff), wasmScratchFPR);
                    m_jit.andFloat(wasmScratchFPR, resultLocation.asFPR());
                    if (signBit) {
                        m_jit.xorFloat(wasmScratchFPR, wasmScratchFPR);
                        m_jit.subFloat(wasmScratchFPR, resultLocation.asFPR(), resultLocation.asFPR());
                    }
#else
                    m_jit.absFloat(lhsLocation.asFPR(), resultLocation.asFPR());
                    if (signBit)
                        m_jit.negateFloat(resultLocation.asFPR(), resultLocation.asFPR());
#endif
                }
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Copysign(Value lhs, Value rhs, Value& result)
    {
        if constexpr (isX86())
            clobber(shiftRCX);

        EMIT_BINARY(
            "F64Copysign", TypeKind::F64,
            BLOCK(Value::fromF64(doubleCopySign(lhs.asF64(), rhs.asF64()))),
            BLOCK(
                // FIXME: Better than what we have in the Air backend, but still not great. I think
                // there's some vector instruction we can use to do this much quicker.

#if CPU(X86_64)
                m_jit.moveDoubleTo64(lhsLocation.asFPR(), wasmScratchGPR);
                m_jit.and64(TrustedImm64(0x7fffffffffffffffll), wasmScratchGPR);
                m_jit.move64ToDouble(wasmScratchGPR, wasmScratchFPR);
                m_jit.moveDoubleTo64(rhsLocation.asFPR(), wasmScratchGPR);
                m_jit.urshift64(wasmScratchGPR, TrustedImm32(63), wasmScratchGPR);
                m_jit.lshift64(wasmScratchGPR, TrustedImm32(63), wasmScratchGPR);
                m_jit.move64ToDouble(wasmScratchGPR, resultLocation.asFPR());
                m_jit.orDouble(resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
                m_jit.moveDoubleTo64(rhsLocation.asFPR(), wasmScratchGPR);

                // Probably saves us a bit of space compared to reserving another register and
                // materializing a 64-bit constant.
                m_jit.urshift64(wasmScratchGPR, TrustedImm32(63), wasmScratchGPR);
                m_jit.lshift64(wasmScratchGPR, TrustedImm32(63), wasmScratchGPR);
                m_jit.move64ToDouble(wasmScratchGPR, wasmScratchFPR);

                m_jit.absDouble(lhsLocation.asFPR(), lhsLocation.asFPR());
                m_jit.orDouble(lhsLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#endif
            ),
            BLOCK(
                if (lhs.isConst()) {
                    m_jit.moveDoubleTo64(rhsLocation.asFPR(), wasmScratchGPR);
                    m_jit.urshift64(wasmScratchGPR, TrustedImm32(63), wasmScratchGPR);
                    m_jit.lshift64(wasmScratchGPR, TrustedImm32(63), wasmScratchGPR);
                    m_jit.move64ToDouble(wasmScratchGPR, wasmScratchFPR);

                    // Moving this constant clobbers wasmScratchGPR, but not wasmScratchFPR
                    emitMoveConst(Value::fromF64(std::abs(lhs.asF64())), resultLocation);
                    m_jit.orDouble(resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                } else {
                    bool signBit = bitwise_cast<uint64_t>(rhs.asF64()) & 0x8000000000000000ull;
#if CPU(X86_64)
                    m_jit.moveDouble(lhsLocation.asFPR(), resultLocation.asFPR());
                    m_jit.move64ToDouble(TrustedImm64(0x7fffffffffffffffll), wasmScratchFPR);
                    m_jit.andDouble(wasmScratchFPR, resultLocation.asFPR());
                    if (signBit) {
                        m_jit.xorDouble(wasmScratchFPR, wasmScratchFPR);
                        m_jit.subDouble(wasmScratchFPR, resultLocation.asFPR(), resultLocation.asFPR());
                    }
#else
                    m_jit.absDouble(lhsLocation.asFPR(), resultLocation.asFPR());
                    if (signBit)
                        m_jit.negateDouble(resultLocation.asFPR(), resultLocation.asFPR());
#endif
                }
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Copysign(Value lhs, Value rhs, Value& result)
    {
        if constexpr (isX86())
            clobber(shiftRCX);

        EMIT_BINARY(
            "F64Copysign", TypeKind::F64,
            BLOCK(Value::fromF64(doubleCopySign(lhs.asF64(), rhs.asF64()))),
            BLOCK(
                F64CopysignHelper32(lhsLocation, rhsLocation, resultLocation);
            ),
            BLOCK(
                ImmHelpers::immLocation(lhsLocation, rhsLocation) = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
                emitMoveConst(ImmHelpers::imm(lhs, rhs), Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2));
                F64CopysignHelper32(lhsLocation, rhsLocation, resultLocation);
            )
        )
    }

    void BBQJIT::F64CopysignHelper32(Location lhsLocation, Location rhsLocation, Location resultLocation)
    {
        ScratchScope<2, 0> scratches(*this);
        auto hi = scratches.gpr(1);
        auto lo = scratches.gpr(0);
        auto sign = wasmScratchGPR2;

        m_jit.moveDoubleHiTo32(rhsLocation.asFPR(), sign);
        m_jit.move(TrustedImm32(0x80000000), wasmScratchGPR);
        m_jit.and32(wasmScratchGPR, sign);

        m_jit.moveDoubleTo64(lhsLocation.asFPR(), hi, lo);
        m_jit.move(TrustedImm32(0x7fffffff), wasmScratchGPR);
        m_jit.and32(wasmScratchGPR, hi);

        m_jit.or32(sign, hi);
        m_jit.move64ToDouble(hi, lo, resultLocation.asFPR());
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Floor(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Floor", TypeKind::F32,
            BLOCK(Value::fromF32(Math::floorFloat(operand.asF32()))),
            BLOCK(
                m_jit.floorFloat(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Floor(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Floor", TypeKind::F32,
            BLOCK(Value::fromF32(Math::floorFloat(operand.asF32()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::F32, operandLocation);
                consume(result);
                emitCCall(Math::floorFloat, Vector<Value> { arg }, result);
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Floor(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Floor", TypeKind::F64,
            BLOCK(Value::fromF64(Math::floorDouble(operand.asF64()))),
            BLOCK(
                m_jit.floorDouble(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Floor(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Floor", TypeKind::F64,
            BLOCK(Value::fromF64(Math::floorDouble(operand.asF64()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::F64, operandLocation);
                consume(result);
                emitCCall(Math::floorDouble, Vector<Value> { arg }, result);
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Ceil(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Ceil", TypeKind::F32,
            BLOCK(Value::fromF32(Math::ceilFloat(operand.asF32()))),
            BLOCK(
                m_jit.ceilFloat(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Ceil(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Ceil", TypeKind::F32,
            BLOCK(Value::fromF32(Math::ceilFloat(operand.asF32()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::F32, operandLocation);
                consume(result);
                emitCCall(Math::ceilFloat, Vector<Value> { arg }, result);
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Ceil(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Ceil", TypeKind::F64,
            BLOCK(Value::fromF64(Math::ceilDouble(operand.asF64()))),
            BLOCK(
                m_jit.ceilDouble(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Ceil(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Ceil", TypeKind::F64,
            BLOCK(Value::fromF64(Math::ceilDouble(operand.asF64()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::F64, operandLocation);
                consume(result);
                emitCCall(Math::ceilDouble, Vector<Value> { arg }, result);
            )
        )
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Abs(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Abs", TypeKind::F32,
            BLOCK(Value::fromF32(std::abs(operand.asF32()))),
            BLOCK(
#if CPU(X86_64)
                m_jit.move32ToFloat(TrustedImm32(0x7fffffffll), wasmScratchFPR);
                m_jit.andFloat(operandLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
                m_jit.absFloat(operandLocation.asFPR(), resultLocation.asFPR());
#endif
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Abs(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Abs", TypeKind::F64,
            BLOCK(Value::fromF64(std::abs(operand.asF64()))),
            BLOCK(
#if CPU(X86_64)
                m_jit.move64ToDouble(TrustedImm64(0x7fffffffffffffffll), wasmScratchFPR);
                m_jit.andDouble(operandLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
                m_jit.absDouble(operandLocation.asFPR(), resultLocation.asFPR());
#endif
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Sqrt(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Sqrt", TypeKind::F32,
            BLOCK(Value::fromF32(Math::sqrtFloat(operand.asF32()))),
            BLOCK(
                m_jit.sqrtFloat(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Sqrt(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Sqrt", TypeKind::F64,
            BLOCK(Value::fromF64(Math::sqrtDouble(operand.asF64()))),
            BLOCK(
                m_jit.sqrtDouble(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Neg(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Neg", TypeKind::F32,
            BLOCK(Value::fromF32(-operand.asF32())),
            BLOCK(
#if CPU(X86_64)
                m_jit.moveFloatTo32(operandLocation.asFPR(), wasmScratchGPR);
                m_jit.xor32(TrustedImm32(bitwise_cast<uint32_t>(static_cast<float>(-0.0))), wasmScratchGPR);
                m_jit.move32ToFloat(wasmScratchGPR, resultLocation.asFPR());
#else
                m_jit.negateFloat(operandLocation.asFPR(), resultLocation.asFPR());
#endif
            )
        )
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Neg(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Neg", TypeKind::F64,
            BLOCK(Value::fromF64(-operand.asF64())),
            BLOCK(
#if CPU(X86_64)
                m_jit.moveDoubleTo64(operandLocation.asFPR(), wasmScratchGPR);
                m_jit.xor64(TrustedImm64(bitwise_cast<uint64_t>(static_cast<double>(-0.0))), wasmScratchGPR);
                m_jit.move64ToDouble(wasmScratchGPR, resultLocation.asFPR());
#else
                m_jit.negateDouble(operandLocation.asFPR(), resultLocation.asFPR());
#endif
            )
        )
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Nearest(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Nearest", TypeKind::F32,
            BLOCK(Value::fromF32(std::nearbyintf(operand.asF32()))),
            BLOCK(
                m_jit.roundTowardNearestIntFloat(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Nearest(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Nearest", TypeKind::F32,
            BLOCK(Value::fromF32(std::nearbyintf(operand.asF32()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::F32, operandLocation);
                consume(result);
                emitCCall(Math::f32_nearest, Vector<Value> { arg }, result);
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Nearest(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Nearest", TypeKind::F64,
            BLOCK(Value::fromF64(std::nearbyint(operand.asF64()))),
            BLOCK(
                m_jit.roundTowardNearestIntDouble(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Nearest(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Nearest", TypeKind::F64,
            BLOCK(Value::fromF64(std::nearbyint(operand.asF64()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::F64, operandLocation);
                consume(result);
                emitCCall(Math::f64_nearest, Vector<Value> { arg }, result);
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Trunc(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Trunc", TypeKind::F32,
            BLOCK(Value::fromF32(Math::truncFloat(operand.asF32()))),
            BLOCK(
                m_jit.roundTowardZeroFloat(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF32Trunc(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F32Trunc", TypeKind::F32,
            BLOCK(Value::fromF32(Math::truncFloat(operand.asF32()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::F32, operandLocation);
                consume(result);
                emitCCall(Math::truncFloat, Vector<Value> { arg }, result);
            )
        )
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Trunc(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Trunc", TypeKind::F64,
            BLOCK(Value::fromF64(Math::truncDouble(operand.asF64()))),
            BLOCK(
                m_jit.roundTowardZeroDouble(operandLocation.asFPR(), resultLocation.asFPR());
            )
        )
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addF64Trunc(Value operand, Value& result)
    {
        EMIT_UNARY(
            "F64Trunc", TypeKind::F64,
            BLOCK(Value::fromF64(Math::truncDouble(operand.asF64()))),
            BLOCK(
                auto arg = Value::pinned(TypeKind::F64, operandLocation);
                consume(result);
                emitCCall(Math::truncDouble, Vector<Value> { arg }, result);
            )
        )
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32TruncSF32(Value operand, Value& result)
    {
        return truncTrapping(OpType::I32TruncSF32, operand, result, Types::I32, Types::F32);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32TruncSF64(Value operand, Value& result)
    {
        return truncTrapping(OpType::I32TruncSF64, operand, result, Types::I32, Types::F64);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32TruncUF32(Value operand, Value& result)
    {
        return truncTrapping(OpType::I32TruncUF32, operand, result, Types::I32, Types::F32);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI32TruncUF64(Value operand, Value& result)
    {
        return truncTrapping(OpType::I32TruncUF64, operand, result, Types::I32, Types::F64);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64TruncSF32(Value operand, Value& result)
    {
        return truncTrapping(OpType::I64TruncSF32, operand, result, Types::I64, Types::F32);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64TruncSF64(Value operand, Value& result)
    {
        return truncTrapping(OpType::I64TruncSF64, operand, result, Types::I64, Types::F64);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64TruncUF32(Value operand, Value& result)
    {
        return truncTrapping(OpType::I64TruncUF32, operand, result, Types::I64, Types::F32);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addI64TruncUF64(Value operand, Value& result)
    {
        return truncTrapping(OpType::I64TruncUF64, operand, result, Types::I64, Types::F64);
    }

    // References

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefIsNull(Value operand, Value& result)
    {
        EMIT_UNARY(
            "RefIsNull", TypeKind::I32,
            BLOCK(Value::fromI32(operand.asRef() == JSValue::encode(jsNull()))),
            BLOCK(
                ASSERT(JSValue::encode(jsNull()) >= 0 && JSValue::encode(jsNull()) <= INT32_MAX);
                m_jit.compare64(RelationalCondition::Equal, operandLocation.asGPR(), TrustedImm32(static_cast<int32_t>(JSValue::encode(jsNull()))), resultLocation.asGPR());
            )
        );
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefIsNull(Value operand, Value& result)
    {
        EMIT_UNARY(
            "RefIsNull", TypeKind::I32,
            BLOCK(Value::fromI32(operand.asRef() == JSValue::encode(jsNull()))),
            BLOCK(
                m_jit.compare32(RelationalCondition::Equal, operandLocation.asGPRhi(), TrustedImm32(JSValue::NullTag), resultLocation.asGPR());
            )
        );
        return { };
    }
#endif

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefAsNonNull(Value value, Value& result)
    {
        Location valueLocation;
        if (value.isConst()) {
            valueLocation = Location::fromGPR(wasmScratchGPR);
            emitMoveConst(value, valueLocation);
        } else
            valueLocation = loadIfNecessary(value);
        ASSERT(valueLocation.isGPR());
        consume(value);

        result = topValue(TypeKind::Ref);
        Location resultLocation = allocate(result);
        ASSERT(JSValue::encode(jsNull()) >= 0 && JSValue::encode(jsNull()) <= INT32_MAX);
        throwExceptionIf(ExceptionType::NullRefAsNonNull, m_jit.branch64(RelationalCondition::Equal, valueLocation.asGPR(), TrustedImm32(static_cast<int32_t>(JSValue::encode(jsNull())))));
        emitMove(TypeKind::Ref, valueLocation, resultLocation);

        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefAsNonNull(Value value, Value& result)
    {
        Location valueLocation;
        if (value.isConst()) {
            valueLocation = Location::fromGPR2(wasmScratchGPR, wasmScratchGPR2);
            emitMoveConst(value, valueLocation);
        } else
            valueLocation = loadIfNecessary(value);
        ASSERT(valueLocation.isGPR2());
        consume(value);

        result = topValue(TypeKind::Ref);
        Location resultLocation = allocate(result);
        throwExceptionIf(ExceptionType::NullRefAsNonNull, m_jit.branch32(RelationalCondition::Equal, valueLocation.asGPRhi(), TrustedImm32(JSValue::NullTag)));
        emitMove(TypeKind::Ref, valueLocation, resultLocation);

        return { };
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefEq(Value ref0, Value ref1, Value& result)
    {
        return addI64Eq(ref0, ref1, result);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addRefFunc(uint32_t index, Value& result)
    {
        // FIXME: Emit this inline <https://bugs.webkit.org/show_bug.cgi?id=198506>.
        TypeKind returnType = Options::useWebAssemblyTypedFunctionReferences() ? TypeKind::Ref : TypeKind::Funcref;

        Vector<Value, 8> arguments = {
            instanceValue(),
            Value::fromI32(index)
        };
        result = topValue(returnType);
        emitCCall(&operationWasmRefFunc, arguments, result);

        return { };
    }

    void BBQJIT::emitEntryTierUpCheck()
    {
        if (!m_tierUp)
            return;

#if ENABLE(WEBASSEMBLY_OMGJIT)
        static_assert(GPRInfo::nonPreservedNonArgumentGPR0 == wasmScratchGPR);
        m_jit.move(TrustedImm64(bitwise_cast<uintptr_t>(&m_tierUp->m_counter)), wasmScratchGPR);
        Jump tierUp = m_jit.branchAdd32(CCallHelpers::PositiveOrZero, TrustedImm32(TierUpCount::functionEntryIncrement()), Address(wasmScratchGPR));
        MacroAssembler::Label tierUpResume = m_jit.label();
        auto functionIndex = m_functionIndex;
        addLatePath([tierUp, tierUpResume, functionIndex](BBQJIT& generator, CCallHelpers& jit) {
            tierUp.link(&jit);
            jit.move(TrustedImm32(functionIndex), GPRInfo::nonPreservedNonArgumentGPR0);
            MacroAssembler::Call call = jit.nearCall();
            jit.jump(tierUpResume);

            bool usesSIMD = generator.m_usesSIMD;
            jit.addLinkTask([=] (LinkBuffer& linkBuffer) {
                MacroAssembler::repatchNearCall(linkBuffer.locationOfNearCall<NoPtrTag>(call), CodeLocationLabel<JITThunkPtrTag>(Thunks::singleton().stub(triggerOMGEntryTierUpThunkGenerator(usesSIMD)).code()));
            });
        });
#else
        RELEASE_ASSERT_NOT_REACHED();
#endif
    }

    // Control flow
    ControlData WARN_UNUSED_RETURN BBQJIT::addTopLevel(BlockSignature signature)
    {
        if (UNLIKELY(Options::verboseBBQJITInstructions())) {
            auto nameSection = m_info.nameSection;
            auto functionIndexSpace = m_info.isImportedFunctionFromFunctionIndexSpace(m_functionIndex) ? m_functionIndex : m_functionIndex + m_info.importFunctionCount();
            std::pair<const Name*, RefPtr<NameSection>> name = nameSection->get(functionIndexSpace);
            dataLog("BBQ\tFunction ");
            if (name.first)
                dataLog(makeString(*name.first));
            else
                dataLog(m_functionIndex);
            dataLogLn(" ", *m_functionSignature);
            LOG_INDENT();
        }

        m_pcToCodeOriginMapBuilder.appendItem(m_jit.label(), PCToCodeOriginMapBuilder::defaultCodeOrigin());
        m_jit.emitFunctionPrologue();
        m_topLevel = ControlData(*this, BlockType::TopLevel, signature, 0);

        m_jit.move(CCallHelpers::TrustedImmPtr(CalleeBits::boxNativeCallee(&m_callee)), wasmScratchGPR);
        static_assert(CallFrameSlot::codeBlock + 1 == CallFrameSlot::callee);
        if constexpr (is32Bit()) {
            CCallHelpers::Address calleeSlot { GPRInfo::callFrameRegister, CallFrameSlot::callee * sizeof(Register) };
            m_jit.storePtr(wasmScratchGPR, calleeSlot.withOffset(PayloadOffset));
            m_jit.store32(CCallHelpers::TrustedImm32(JSValue::NativeCalleeTag), calleeSlot.withOffset(TagOffset));
            m_jit.storePtr(GPRInfo::wasmContextInstancePointer, CCallHelpers::addressFor(CallFrameSlot::codeBlock));
        } else
            m_jit.storePairPtr(GPRInfo::wasmContextInstancePointer, wasmScratchGPR, GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(CallFrameSlot::codeBlock * sizeof(Register)));

        m_frameSizeLabels.append(m_jit.moveWithPatch(TrustedImmPtr(nullptr), wasmScratchGPR));

        bool mayHaveExceptionHandlers = !m_hasExceptionHandlers || m_hasExceptionHandlers.value();
        if (mayHaveExceptionHandlers)
            m_jit.store32(CCallHelpers::TrustedImm32(PatchpointExceptionHandle::s_invalidCallSiteIndex), CCallHelpers::tagFor(CallFrameSlot::argumentCountIncludingThis));

        // Because we compile in a single pass, we always need to pessimistically check for stack underflow/overflow.
        static_assert(wasmScratchGPR == GPRInfo::nonPreservedNonArgumentGPR0);
        m_jit.subPtr(GPRInfo::callFrameRegister, wasmScratchGPR, wasmScratchGPR);

        MacroAssembler::JumpList overflow;
        overflow.append(m_jit.branchPtr(CCallHelpers::Above, wasmScratchGPR, GPRInfo::callFrameRegister));
        overflow.append(m_jit.branchPtr(CCallHelpers::Below, wasmScratchGPR, CCallHelpers::Address(GPRInfo::wasmContextInstancePointer, Instance::offsetOfSoftStackLimit())));
        m_jit.addLinkTask([overflow] (LinkBuffer& linkBuffer) {
            linkBuffer.link(overflow, CodeLocationLabel<JITThunkPtrTag>(Thunks::singleton().stub(throwStackOverflowFromWasmThunkGenerator).code()));
        });

        m_jit.move(wasmScratchGPR, MacroAssembler::stackPointerRegister);

        LocalOrTempIndex i = 0;
        for (; i < m_arguments.size(); ++i)
            flushValue(Value::fromLocal(m_parser->typeOfLocal(i).kind, i));

        // Zero all locals that aren't initialized by arguments.
        enum class ClearMode { Zero, JSNull };
        std::optional<int32_t> highest;
        std::optional<int32_t> lowest;
        auto flushZeroClear = [&]() {
            if (!lowest)
                return;
            size_t size = highest.value() - lowest.value();
            int32_t pointer = lowest.value();

            // Adjust pointer offset to be efficient for paired-stores.
            if (pointer & 4 && size >= 4) {
                m_jit.store32(TrustedImm32(0), Address(GPRInfo::callFrameRegister, pointer));
                pointer += 4;
                size -= 4;
            }

#if CPU(ARM64)
            if (pointer & 8 && size >= 8) {
                m_jit.store64(TrustedImm64(0), Address(GPRInfo::callFrameRegister, pointer));
                pointer += 8;
                size -= 8;
            }

            unsigned count = size / 16;
            for (unsigned i = 0; i < count; ++i) {
                m_jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, GPRInfo::callFrameRegister, TrustedImm32(pointer));
                pointer += 16;
                size -= 16;
            }
            if (size & 8) {
                m_jit.store64(TrustedImm64(0), Address(GPRInfo::callFrameRegister, pointer));
                pointer += 8;
                size -= 8;
            }
#else
            unsigned count = size / 8;
            for (unsigned i = 0; i < count; ++i) {
#if USE(JSVALUE64)
                m_jit.store64(TrustedImm64(0), Address(GPRInfo::callFrameRegister, pointer));
#else
                m_jit.store32(TrustedImm32(0), Address(GPRInfo::callFrameRegister, pointer));
                m_jit.store32(TrustedImm32(0), Address(GPRInfo::callFrameRegister, pointer+4));
#endif
                pointer += 8;
                size -= 8;
            }
#endif

            if (size & 4) {
                m_jit.store32(TrustedImm32(0), Address(GPRInfo::callFrameRegister, pointer));
                pointer += 4;
                size -= 4;
            }
            ASSERT(size == 0);

            highest = std::nullopt;
            lowest = std::nullopt;
        };

        auto clear = [&](ClearMode mode, TypeKind type, Location location) {
            if (mode == ClearMode::JSNull) {
                flushZeroClear();
                emitStoreConst(Value::fromI64(bitwise_cast<uint64_t>(JSValue::encode(jsNull()))), location);
                return;
            }
            if (!highest)
                highest = location.asStackOffset() + sizeOfType(type);
            lowest = location.asStackOffset();
        };

        JIT_COMMENT(m_jit, "initialize locals");
        for (; i < m_locals.size(); ++i) {
            TypeKind type = m_parser->typeOfLocal(i).kind;
            switch (type) {
            case TypeKind::I32:
            case TypeKind::I31ref:
            case TypeKind::F32:
            case TypeKind::F64:
            case TypeKind::I64:
            case TypeKind::Struct:
            case TypeKind::Rec:
            case TypeKind::Func:
            case TypeKind::Array:
            case TypeKind::Sub:
            case TypeKind::Subfinal:
            case TypeKind::V128:
                clear(ClearMode::Zero, type, m_locals[i]);
                break;
            case TypeKind::Externref:
            case TypeKind::Funcref:
            case TypeKind::Ref:
            case TypeKind::RefNull:
            case TypeKind::Structref:
            case TypeKind::Arrayref:
            case TypeKind::Eqref:
            case TypeKind::Anyref:
            case TypeKind::Nullref:
            case TypeKind::Nullfuncref:
            case TypeKind::Nullexternref:
                clear(ClearMode::JSNull, type, m_locals[i]);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
        flushZeroClear();
        JIT_COMMENT(m_jit, "initialize locals done");

        for (size_t i = 0; i < m_functionSignature->argumentCount(); i ++)
            m_topLevel.touch(i); // Ensure arguments are flushed to persistent locations when this block ends.

        emitEntryTierUpCheck();

        return m_topLevel;
    }

    bool BBQJIT::hasLoops() const
    {
        return m_compilation->bbqLoopEntrypoints.size();
    }

    MacroAssembler::Label BBQJIT::addLoopOSREntrypoint()
    {
        // To handle OSR entry into loops, we emit a second entrypoint, which sets up our call frame then calls an
        // operation to get the address of the loop we're trying to enter. Unlike the normal prologue, by the time
        // we emit this entry point, we:
        //  - Know the frame size, so we don't need to patch the constant.
        //  - Can omit the entry tier-up check, since this entry point is only reached when we initially tier up into a loop.
        //  - Don't need to zero our locals, since they are restored from the OSR entry scratch buffer anyway.
        auto label = m_jit.label();
        m_jit.emitFunctionPrologue();

        m_jit.move(CCallHelpers::TrustedImmPtr(CalleeBits::boxNativeCallee(&m_callee)), wasmScratchGPR);
        static_assert(CallFrameSlot::codeBlock + 1 == CallFrameSlot::callee);
        if constexpr (is32Bit()) {
            CCallHelpers::Address calleeSlot { GPRInfo::callFrameRegister, CallFrameSlot::callee * sizeof(Register) };
            m_jit.storePtr(wasmScratchGPR, calleeSlot.withOffset(PayloadOffset));
            m_jit.store32(CCallHelpers::TrustedImm32(JSValue::NativeCalleeTag), calleeSlot.withOffset(TagOffset));
            m_jit.storePtr(GPRInfo::wasmContextInstancePointer, CCallHelpers::addressFor(CallFrameSlot::codeBlock));
        } else
            m_jit.storePairPtr(GPRInfo::wasmContextInstancePointer, wasmScratchGPR, GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(CallFrameSlot::codeBlock * sizeof(Register)));

        int frameSize = m_frameSize + m_maxCalleeStackSize;
        int roundedFrameSize = WTF::roundUpToMultipleOf(stackAlignmentBytes(), frameSize);
#if USE(JSVALUE64)
        m_jit.subPtr(GPRInfo::callFrameRegister, TrustedImm32(roundedFrameSize), MacroAssembler::stackPointerRegister);
#else
        m_jit.subPtr(GPRInfo::callFrameRegister, TrustedImm32(roundedFrameSize), wasmScratchGPR);
        m_jit.move(wasmScratchGPR, MacroAssembler::stackPointerRegister);
#endif

        MacroAssembler::JumpList overflow;
        overflow.append(m_jit.branchPtr(CCallHelpers::Above, MacroAssembler::stackPointerRegister, GPRInfo::callFrameRegister));
        overflow.append(m_jit.branchPtr(CCallHelpers::Below, MacroAssembler::stackPointerRegister, CCallHelpers::Address(GPRInfo::wasmContextInstancePointer, Instance::offsetOfSoftStackLimit())));
        m_jit.addLinkTask([overflow] (LinkBuffer& linkBuffer) {
            linkBuffer.link(overflow, CodeLocationLabel<JITThunkPtrTag>(Thunks::singleton().stub(throwStackOverflowFromWasmThunkGenerator).code()));
        });

        // This operation shuffles around values on the stack, until everything is in the right place. Then,
        // it returns the address of the loop we're jumping to in wasmScratchGPR (so we don't interfere with
        // anything we just loaded from the scratch buffer into a register)
        m_jit.probe(tagCFunction<JITProbePtrTag>(operationWasmLoopOSREnterBBQJIT), m_tierUp, m_usesSIMD ? SavedFPWidth::SaveVectors : SavedFPWidth::DontSaveVectors);

        // We expect the loop address to be populated by the probe operation.
        static_assert(wasmScratchGPR == GPRInfo::nonPreservedNonArgumentGPR0);
        m_jit.farJump(wasmScratchGPR, WasmEntryPtrTag);
        return label;
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addBlock(BlockSignature signature, Stack& enclosingStack, ControlType& result, Stack& newStack)
    {
        result = ControlData(*this, BlockType::Block, signature, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + enclosingStack.size() - signature->as<FunctionSignature>()->argumentCount());
        currentControlData().flushAndSingleExit(*this, result, enclosingStack, true, false);

        LOG_INSTRUCTION("Block", *signature);
        LOG_INDENT();
        splitStack(signature, enclosingStack, newStack);
        result.startBlock(*this, newStack);
        return { };
    }

    B3::Type BBQJIT::toB3Type(Type type)
    {
        return Wasm::toB3Type(type);
    }

    B3::Type BBQJIT::toB3Type(TypeKind kind)
    {
        switch (kind) {
        case TypeKind::I31ref:
        case TypeKind::I32:
            return B3::Type(B3::Int32);
        case TypeKind::I64:
            return B3::Type(B3::Int64);
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Structref:
        case TypeKind::Arrayref:
        case TypeKind::Funcref:
        case TypeKind::Externref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            return B3::Type(B3::Int64);
        case TypeKind::F32:
            return B3::Type(B3::Float);
        case TypeKind::F64:
            return B3::Type(B3::Double);
        case TypeKind::V128:
            return B3::Type(B3::V128);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return B3::Void;
        }
    }

    B3::ValueRep BBQJIT::toB3Rep(Location location)
    {
        if (location.isRegister())
            return B3::ValueRep(location.isGPR() ? Reg(location.asGPR()) : Reg(location.asFPR()));
        if (location.isStack())
            return B3::ValueRep(ValueLocation::stack(location.asStackOffset()));

        RELEASE_ASSERT_NOT_REACHED();
        return B3::ValueRep();
    }

    StackMap BBQJIT::makeStackMap(const ControlData& data, Stack& enclosingStack)
    {
        unsigned numElements = m_locals.size() + data.enclosedHeight() + data.argumentLocations().size();

        StackMap stackMap(numElements);
        unsigned stackMapIndex = 0;
        for (unsigned i = 0; i < m_locals.size(); i ++)
            stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(m_locals[i]), toB3Type(m_localTypes[i]));

        if (Options::useWasmIPInt()) {
            // Do rethrow slots first because IPInt has them in a shadow stack.
            for (const ControlEntry& entry : m_parser->controlStack()) {
                for (unsigned i = 0; i < entry.controlData.implicitSlots(); i ++) {
                    Value exception = this->exception(entry.controlData);
                    stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(locationOf(exception)), B3::Int64); // Exceptions are EncodedJSValues, so they are always Int64
                }
            }

            for (const ControlEntry& entry : m_parser->controlStack()) {
                for (const TypedExpression& expr : entry.enclosedExpressionStack)
                    stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(locationOf(expr.value())), toB3Type(expr.type().kind));
            }

            for (const TypedExpression& expr : enclosingStack)
                stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(locationOf(expr.value())), toB3Type(expr.type().kind));
            for (unsigned i = 0; i < data.argumentLocations().size(); i ++)
                stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(data.argumentLocations()[i]), toB3Type(data.argumentType(i).kind));


        } else {
            for (const ControlEntry& entry : m_parser->controlStack()) {
                for (const TypedExpression& expr : entry.enclosedExpressionStack)
                    stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(locationOf(expr.value())), toB3Type(expr.type().kind));
                if (ControlData::isAnyCatch(entry.controlData)) {
                    for (unsigned i = 0; i < entry.controlData.implicitSlots(); i ++) {
                        Value exception = this->exception(entry.controlData);
                        stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(locationOf(exception)), B3::Int64); // Exceptions are EncodedJSValues, so they are always Int64
                    }
                }
            }
            for (const TypedExpression& expr : enclosingStack)
                stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(locationOf(expr.value())), toB3Type(expr.type().kind));
            for (unsigned i = 0; i < data.argumentLocations().size(); i ++)
                stackMap[stackMapIndex ++] = OSREntryValue(toB3Rep(data.argumentLocations()[i]), toB3Type(data.argumentType(i).kind));
        }

        RELEASE_ASSERT(stackMapIndex == numElements);
        m_osrEntryScratchBufferSize = std::max(m_osrEntryScratchBufferSize, numElements + BBQCallee::extraOSRValuesForLoopIndex);
        return stackMap;
    }

    void BBQJIT::emitLoopTierUpCheck(const ControlData& data, Stack& enclosingStack, unsigned loopIndex)
    {
        if (!m_tierUp)
            return;

#if ENABLE(WEBASSEMBLY_OMGJIT)
        ASSERT(m_tierUp->osrEntryTriggers().size() == loopIndex);
        m_tierUp->osrEntryTriggers().append(TierUpCount::TriggerReason::DontTrigger);

        unsigned outerLoops = m_outerLoops.isEmpty() ? UINT32_MAX : m_outerLoops.last();
        m_tierUp->outerLoops().append(outerLoops);
        m_outerLoops.append(loopIndex);

        static_assert(GPRInfo::nonPreservedNonArgumentGPR0 == wasmScratchGPR);
        m_jit.move(TrustedImmPtr(bitwise_cast<uintptr_t>(&m_tierUp->m_counter)), wasmScratchGPR);

        TierUpCount::TriggerReason* forceEntryTrigger = &(m_tierUp->osrEntryTriggers().last());
        static_assert(!static_cast<uint8_t>(TierUpCount::TriggerReason::DontTrigger), "the JIT code assumes non-zero means 'enter'");
        static_assert(sizeof(TierUpCount::TriggerReason) == 1, "branchTest8 assumes this size");

        Jump forceOSREntry = m_jit.branchTest8(ResultCondition::NonZero, CCallHelpers::AbsoluteAddress(forceEntryTrigger));
        Jump tierUp = m_jit.branchAdd32(ResultCondition::PositiveOrZero, TrustedImm32(TierUpCount::loopIncrement()), CCallHelpers::Address(wasmScratchGPR));
        MacroAssembler::Label tierUpResume = m_jit.label();

        OSREntryData& osrEntryData = m_tierUp->addOSREntryData(m_functionIndex, loopIndex, makeStackMap(data, enclosingStack));
        OSREntryData* osrEntryDataPtr = &osrEntryData;

        addLatePath([forceOSREntry, tierUp, tierUpResume, osrEntryDataPtr](BBQJIT& generator, CCallHelpers& jit) {
            forceOSREntry.link(&jit);
            tierUp.link(&jit);

            Probe::SavedFPWidth savedFPWidth = generator.m_usesSIMD ? Probe::SavedFPWidth::SaveVectors : Probe::SavedFPWidth::DontSaveVectors; // By the time we reach the late path, we should know whether or not the function uses SIMD.
            jit.probe(tagCFunction<JITProbePtrTag>(operationWasmTriggerOSREntryNow), osrEntryDataPtr, savedFPWidth);
            jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::nonPreservedNonArgumentGPR0).linkTo(tierUpResume, &jit);
            jit.farJump(GPRInfo::nonPreservedNonArgumentGPR0, WasmEntryPtrTag);
        });
#else
        UNUSED_PARAM(data);
        UNUSED_PARAM(enclosingStack);
        UNUSED_PARAM(loopIndex);
        RELEASE_ASSERT_NOT_REACHED();
#endif
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addLoop(BlockSignature signature, Stack& enclosingStack, ControlType& result, Stack& newStack, uint32_t loopIndex)
    {
        result = ControlData(*this, BlockType::Loop, signature, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + enclosingStack.size() - signature->as<FunctionSignature>()->argumentCount());
        currentControlData().flushAndSingleExit(*this, result, enclosingStack, true, false);

        LOG_INSTRUCTION("Loop", *signature);
        LOG_INDENT();
        splitStack(signature, enclosingStack, newStack);
        result.startBlock(*this, newStack);
        result.setLoopLabel(m_jit.label());

        RELEASE_ASSERT(m_compilation->bbqLoopEntrypoints.size() == loopIndex);
        m_compilation->bbqLoopEntrypoints.append(result.loopLabel());

        emitLoopTierUpCheck(result, enclosingStack, loopIndex);
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addIf(Value condition, BlockSignature signature, Stack& enclosingStack, ControlData& result, Stack& newStack)
    {
        // Here, we cannot use wasmScratchGPR since it is used for shuffling in flushAndSingleExit.
        static_assert(wasmScratchGPR == GPRInfo::nonPreservedNonArgumentGPR0);
        ScratchScope<1, 0> scratches(*this, RegisterSetBuilder::argumentGPRS());
        scratches.unbindPreserved();
        Location conditionLocation = Location::fromGPR(scratches.gpr(0));
        if (!condition.isConst())
            emitMove(condition, conditionLocation);
        consume(condition);

        result = ControlData(*this, BlockType::If, signature, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + enclosingStack.size() - signature->as<FunctionSignature>()->argumentCount());

        // Despite being conditional, if doesn't need to worry about diverging expression stacks at block boundaries, so it doesn't need multiple exits.
        currentControlData().flushAndSingleExit(*this, result, enclosingStack, true, false);

        LOG_INSTRUCTION("If", *signature, condition, conditionLocation);
        LOG_INDENT();
        splitStack(signature, enclosingStack, newStack);

        result.startBlock(*this, newStack);
        if (condition.isConst() && !condition.asI32())
            result.setIfBranch(m_jit.jump()); // Emit direct branch if we know the condition is false.
        else if (!condition.isConst()) // Otherwise, we only emit a branch at all if we don't know the condition statically.
            result.setIfBranch(m_jit.branchTest32(ResultCondition::Zero, conditionLocation.asGPR()));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addElse(ControlData& data, Stack& expressionStack)
    {
        data.flushAndSingleExit(*this, data, expressionStack, false, true);
        ControlData dataElse(*this, BlockType::Block, data.signature(), data.enclosedHeight());
        data.linkJumps(&m_jit);
        dataElse.addBranch(m_jit.jump());
        data.linkIfBranch(&m_jit); // Link specifically the conditional branch of the preceding If
        LOG_DEDENT();
        LOG_INSTRUCTION("Else");
        LOG_INDENT();

        // We don't care at this point about the values live at the end of the previous control block,
        // we just need the right number of temps for our arguments on the top of the stack.
        expressionStack.clear();
        while (expressionStack.size() < data.signature()->as<FunctionSignature>()->argumentCount()) {
            Type type = data.signature()->as<FunctionSignature>()->argumentType(expressionStack.size());
            expressionStack.constructAndAppend(type, Value::fromTemp(type.kind, dataElse.enclosedHeight() + dataElse.implicitSlots() + expressionStack.size()));
        }

        dataElse.startBlock(*this, expressionStack);
        data = dataElse;
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addElseToUnreachable(ControlData& data)
    {
        // We want to flush or consume all values on the stack to reset the allocator
        // state entering the else block.
        data.flushAtBlockBoundary(*this, 0, m_parser->expressionStack(), true);

        ControlData dataElse(*this, BlockType::Block, data.signature(), data.enclosedHeight());
        data.linkJumps(&m_jit);
        dataElse.addBranch(m_jit.jump()); // Still needed even when the parent was unreachable to avoid running code within the else block.
        data.linkIfBranch(&m_jit); // Link specifically the conditional branch of the preceding If
        LOG_DEDENT();
        LOG_INSTRUCTION("Else");
        LOG_INDENT();

        // We don't have easy access to the original expression stack we had entering the if block,
        // so we construct a local stack just to set up temp bindings as we enter the else.
        Stack expressionStack;
        auto functionSignature = dataElse.signature()->as<FunctionSignature>();
        for (unsigned i = 0; i < functionSignature->argumentCount(); i ++)
            expressionStack.constructAndAppend(functionSignature->argumentType(i), Value::fromTemp(functionSignature->argumentType(i).kind, dataElse.enclosedHeight() + dataElse.implicitSlots() + i));
        dataElse.startBlock(*this, expressionStack);
        data = dataElse;
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addTry(BlockSignature signature, Stack& enclosingStack, ControlType& result, Stack& newStack)
    {
        m_usesExceptions = true;
        ++m_tryCatchDepth;
        ++m_callSiteIndex;
        result = ControlData(*this, BlockType::Try, signature, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + enclosingStack.size() - signature->as<FunctionSignature>()->argumentCount());
        result.setTryInfo(m_callSiteIndex, m_callSiteIndex, m_tryCatchDepth);
        currentControlData().flushAndSingleExit(*this, result, enclosingStack, true, false);

        LOG_INSTRUCTION("Try", *signature);
        LOG_INDENT();
        splitStack(signature, enclosingStack, newStack);
        result.startBlock(*this, newStack);
        return { };
    }

#if USE(JSVALUE64)
    void BBQJIT::emitCatchPrologue()
    {
        m_frameSizeLabels.append(m_jit.moveWithPatch(TrustedImmPtr(nullptr), GPRInfo::nonPreservedNonArgumentGPR0));
        m_jit.subPtr(GPRInfo::callFrameRegister, GPRInfo::nonPreservedNonArgumentGPR0, MacroAssembler::stackPointerRegister);
        if (!!m_info.memory) {
            loadWebAssemblyGlobalState(wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister);
        }
        static_assert(noOverlap(GPRInfo::nonPreservedNonArgumentGPR0, GPRInfo::returnValueGPR, GPRInfo::returnValueGPR2));
    }
#else
    void BBQJIT::emitCatchPrologue()
    {
        m_frameSizeLabels.append(m_jit.moveWithPatch(TrustedImmPtr(nullptr), GPRInfo::nonPreservedNonArgumentGPR0));
        m_jit.subPtr(GPRInfo::callFrameRegister, GPRInfo::nonPreservedNonArgumentGPR0, wasmScratchGPR);
        m_jit.move(wasmScratchGPR, MacroAssembler::stackPointerRegister);

        static_assert(noOverlap(GPRInfo::nonPreservedNonArgumentGPR0, GPRInfo::returnValueGPR, GPRInfo::returnValueGPR2));
    }
#endif

#if USE(JSVALUE64)
    void BBQJIT::emitCatchAllImpl(ControlData& dataCatch)
    {
        m_catchEntrypoints.append(m_jit.label());
        emitCatchPrologue();
        bind(this->exception(dataCatch), Location::fromGPR(GPRInfo::returnValueGPR));
        Stack emptyStack { };
        dataCatch.startBlock(*this, emptyStack);
    }
#else
    void BBQJIT::emitCatchAllImpl(ControlData& dataCatch)
    {
        m_catchEntrypoints.append(m_jit.label());
        emitCatchPrologue();
        bind(this->exception(dataCatch), Location::fromGPR2(GPRInfo::returnValueGPR2, GPRInfo::returnValueGPR));
        Stack emptyStack { };
        dataCatch.startBlock(*this, emptyStack);
    }
#endif

#if USE(JSVALUE64)
    void BBQJIT::emitCatchImpl(ControlData& dataCatch, const TypeDefinition& exceptionSignature, ResultList& results)
    {
        m_catchEntrypoints.append(m_jit.label());
        emitCatchPrologue();
        bind(this->exception(dataCatch), Location::fromGPR(GPRInfo::returnValueGPR));
        Stack emptyStack { };
        dataCatch.startBlock(*this, emptyStack);

        if (exceptionSignature.as<FunctionSignature>()->argumentCount()) {
            ScratchScope<1, 0> scratches(*this);
            GPRReg bufferGPR = scratches.gpr(0);
            m_jit.loadPtr(Address(GPRInfo::returnValueGPR, JSWebAssemblyException::offsetOfPayload() + JSWebAssemblyException::Payload::offsetOfStorage()), bufferGPR);
            for (unsigned i = 0; i < exceptionSignature.as<FunctionSignature>()->argumentCount(); ++i) {
                Type type = exceptionSignature.as<FunctionSignature>()->argumentType(i);
                Value result = Value::fromTemp(type.kind, dataCatch.enclosedHeight() + dataCatch.implicitSlots() + i);
                Location slot = canonicalSlot(result);
                switch (type.kind) {
                case TypeKind::I32:
                    m_jit.load32(Address(bufferGPR, JSWebAssemblyException::Payload::Storage::offsetOfData() + i * sizeof(uint64_t)), wasmScratchGPR);
                    m_jit.store32(wasmScratchGPR, slot.asAddress());
                    break;
                case TypeKind::I31ref:
                case TypeKind::I64:
                case TypeKind::Ref:
                case TypeKind::RefNull:
                case TypeKind::Arrayref:
                case TypeKind::Structref:
                case TypeKind::Funcref:
                case TypeKind::Externref:
                case TypeKind::Eqref:
                case TypeKind::Anyref:
                case TypeKind::Nullref:
                case TypeKind::Nullfuncref:
                case TypeKind::Nullexternref:
                case TypeKind::Rec:
                case TypeKind::Sub:
                case TypeKind::Subfinal:
                case TypeKind::Array:
                case TypeKind::Struct:
                case TypeKind::Func: {
                    m_jit.load64(Address(bufferGPR, JSWebAssemblyException::Payload::Storage::offsetOfData() + i * sizeof(uint64_t)), wasmScratchGPR);
                    m_jit.store64(wasmScratchGPR, slot.asAddress());
                    break;
                }
                case TypeKind::F32:
                    m_jit.loadFloat(Address(bufferGPR, JSWebAssemblyException::Payload::Storage::offsetOfData() + i * sizeof(uint64_t)), wasmScratchFPR);
                    m_jit.storeFloat(wasmScratchFPR, slot.asAddress());
                    break;
                case TypeKind::F64:
                    m_jit.loadDouble(Address(bufferGPR, JSWebAssemblyException::Payload::Storage::offsetOfData() + i * sizeof(uint64_t)), wasmScratchFPR);
                    m_jit.storeDouble(wasmScratchFPR, slot.asAddress());
                    break;
                case TypeKind::V128:
                    materializeVectorConstant(v128_t { }, Location::fromFPR(wasmScratchFPR));
                    m_jit.storeVector(wasmScratchFPR, slot.asAddress());
                    break;
                case TypeKind::Void:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                bind(result, slot);
                results.append(result);
            }
        }
    }
#else
    void BBQJIT::emitCatchImpl(ControlData& dataCatch, const TypeDefinition& exceptionSignature, ResultList& results)
    {
        m_catchEntrypoints.append(m_jit.label());
        emitCatchPrologue();
        bind(this->exception(dataCatch), Location::fromGPR2(GPRInfo::returnValueGPR2, GPRInfo::returnValueGPR));
        Stack emptyStack { };
        dataCatch.startBlock(*this, emptyStack);

        if (exceptionSignature.as<FunctionSignature>()->argumentCount()) {
            ScratchScope<1, 0> scratches(*this);
            GPRReg bufferGPR = scratches.gpr(0);
            m_jit.loadPtr(Address(GPRInfo::returnValueGPR, JSWebAssemblyException::offsetOfPayload() + JSWebAssemblyException::Payload::offsetOfStorage()), bufferGPR);
            for (unsigned i = 0; i < exceptionSignature.as<FunctionSignature>()->argumentCount(); ++i) {
                Type type = exceptionSignature.as<FunctionSignature>()->argumentType(i);
                Value result = Value::fromTemp(type.kind, dataCatch.enclosedHeight() + dataCatch.implicitSlots() + i);
                Location slot = canonicalSlot(result);
                switch (type.kind) {
                case TypeKind::I32:
                    m_jit.load32(Address(bufferGPR, JSWebAssemblyException::Payload::Storage::offsetOfData() + i * sizeof(uint64_t)), wasmScratchGPR);
                    m_jit.store32(wasmScratchGPR, slot.asAddress());
                    break;
                case TypeKind::I31ref:
                case TypeKind::I64:
                case TypeKind::Ref:
                case TypeKind::RefNull:
                case TypeKind::Arrayref:
                case TypeKind::Structref:
                case TypeKind::Funcref:
                case TypeKind::Externref:
                case TypeKind::Eqref:
                case TypeKind::Anyref:
                case TypeKind::Nullref:
                case TypeKind::Nullfuncref:
                case TypeKind::Nullexternref:
                case TypeKind::Rec:
                case TypeKind::Sub:
                case TypeKind::Subfinal:
                case TypeKind::Array:
                case TypeKind::Struct:
                case TypeKind::Func: {
                    m_jit.loadPair32(Address(bufferGPR, JSWebAssemblyException::Payload::Storage::offsetOfData() + i * sizeof(uint64_t)), wasmScratchGPR, wasmScratchGPR2);
                    m_jit.storePair32(wasmScratchGPR, wasmScratchGPR2, slot.asAddress());
                    break;
                }
                case TypeKind::F32:
                    m_jit.loadFloat(Address(bufferGPR, JSWebAssemblyException::Payload::Storage::offsetOfData() + i * sizeof(uint64_t)), wasmScratchFPR);
                    m_jit.storeFloat(wasmScratchFPR, slot.asAddress());
                    break;
                case TypeKind::F64:
                    m_jit.loadDouble(Address(bufferGPR, JSWebAssemblyException::Payload::Storage::offsetOfData() + i * sizeof(uint64_t)), wasmScratchFPR);
                    m_jit.storeDouble(wasmScratchFPR, slot.asAddress());
                    break;
                case TypeKind::V128:
                    materializeVectorConstant(v128_t { }, Location::fromFPR(wasmScratchFPR));
                    m_jit.storeVector(wasmScratchFPR, slot.asAddress());
                    break;
                case TypeKind::Void:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
                bind(result, slot);
                results.append(result);
            }
        }
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCatch(unsigned exceptionIndex, const TypeDefinition& exceptionSignature, Stack& expressionStack, ControlType& data, ResultList& results)
    {
        m_usesExceptions = true;
        data.flushAndSingleExit(*this, data, expressionStack, false, true);
        ControlData dataCatch(*this, BlockType::Catch, data.signature(), data.enclosedHeight());
        dataCatch.setCatchKind(CatchKind::Catch);
        if (ControlData::isTry(data)) {
            ++m_callSiteIndex;
            data.setTryInfo(data.tryStart(), m_callSiteIndex, data.tryCatchDepth());
        }
        dataCatch.setTryInfo(data.tryStart(), data.tryEnd(), data.tryCatchDepth());

        data.delegateJumpsTo(dataCatch);
        dataCatch.addBranch(m_jit.jump());
        LOG_DEDENT();
        LOG_INSTRUCTION("Catch");
        LOG_INDENT();
        emitCatchImpl(dataCatch, exceptionSignature, results);
        data = WTFMove(dataCatch);
        m_exceptionHandlers.append({ HandlerType::Catch, data.tryStart(), data.tryEnd(), 0, m_tryCatchDepth, exceptionIndex });
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCatchToUnreachable(unsigned exceptionIndex, const TypeDefinition& exceptionSignature, ControlType& data, ResultList& results)
    {
        m_usesExceptions = true;
        ControlData dataCatch(*this, BlockType::Catch, data.signature(), data.enclosedHeight());
        dataCatch.setCatchKind(CatchKind::Catch);
        if (ControlData::isTry(data)) {
            ++m_callSiteIndex;
            data.setTryInfo(data.tryStart(), m_callSiteIndex, data.tryCatchDepth());
        }
        dataCatch.setTryInfo(data.tryStart(), data.tryEnd(), data.tryCatchDepth());

        data.delegateJumpsTo(dataCatch);
        LOG_DEDENT();
        LOG_INSTRUCTION("Catch");
        LOG_INDENT();
        emitCatchImpl(dataCatch, exceptionSignature, results);
        data = WTFMove(dataCatch);
        m_exceptionHandlers.append({ HandlerType::Catch, data.tryStart(), data.tryEnd(), 0, m_tryCatchDepth, exceptionIndex });
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCatchAll(Stack& expressionStack, ControlType& data)
    {
        m_usesExceptions = true;
        data.flushAndSingleExit(*this, data, expressionStack, false, true);
        ControlData dataCatch(*this, BlockType::Catch, data.signature(), data.enclosedHeight());
        dataCatch.setCatchKind(CatchKind::CatchAll);
        if (ControlData::isTry(data)) {
            ++m_callSiteIndex;
            data.setTryInfo(data.tryStart(), m_callSiteIndex, data.tryCatchDepth());
        }
        dataCatch.setTryInfo(data.tryStart(), data.tryEnd(), data.tryCatchDepth());

        data.delegateJumpsTo(dataCatch);
        dataCatch.addBranch(m_jit.jump());
        LOG_DEDENT();
        LOG_INSTRUCTION("CatchAll");
        LOG_INDENT();
        emitCatchAllImpl(dataCatch);
        data = WTFMove(dataCatch);
        m_exceptionHandlers.append({ HandlerType::CatchAll, data.tryStart(), data.tryEnd(), 0, m_tryCatchDepth, 0 });
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCatchAllToUnreachable(ControlType& data)
    {
        m_usesExceptions = true;
        ControlData dataCatch(*this, BlockType::Catch, data.signature(), data.enclosedHeight());
        dataCatch.setCatchKind(CatchKind::CatchAll);
        if (ControlData::isTry(data)) {
            ++m_callSiteIndex;
            data.setTryInfo(data.tryStart(), m_callSiteIndex, data.tryCatchDepth());
        }
        dataCatch.setTryInfo(data.tryStart(), data.tryEnd(), data.tryCatchDepth());

        data.delegateJumpsTo(dataCatch);
        LOG_DEDENT();
        LOG_INSTRUCTION("CatchAll");
        LOG_INDENT();
        emitCatchAllImpl(dataCatch);
        data = WTFMove(dataCatch);
        m_exceptionHandlers.append({ HandlerType::CatchAll, data.tryStart(), data.tryEnd(), 0, m_tryCatchDepth, 0 });
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addDelegate(ControlType& target, ControlType& data)
    {
        return addDelegateToUnreachable(target, data);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addDelegateToUnreachable(ControlType& target, ControlType& data)
    {
        unsigned depth = 0;
        if (ControlType::isTry(target))
            depth = target.tryCatchDepth();

        if (ControlData::isTry(data)) {
            ++m_callSiteIndex;
            data.setTryInfo(data.tryStart(), m_callSiteIndex, data.tryCatchDepth());
        }
        m_exceptionHandlers.append({ HandlerType::Delegate, data.tryStart(), m_callSiteIndex, 0, m_tryCatchDepth, depth });
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addThrow(unsigned exceptionIndex, Vector<ExpressionType>& arguments, Stack&)
    {
        Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf(stackAlignmentBytes(), arguments.size() * sizeof(uint64_t));
        m_maxCalleeStackSize = std::max<int>(calleeStackSize, m_maxCalleeStackSize);

        LOG_INSTRUCTION("Throw", arguments);

        for (unsigned i = 0; i < arguments.size(); i++) {
            Location stackLocation = Location::fromStackArgument(i * sizeof(uint64_t));
            Value argument = arguments[i];
            if (argument.type() == TypeKind::V128)
                emitMove(Value::fromF64(0), stackLocation);
            else
                emitMove(argument, stackLocation);
            consume(argument);
        }

        ++m_callSiteIndex;
        bool mayHaveExceptionHandlers = !m_hasExceptionHandlers || m_hasExceptionHandlers.value();
        if (mayHaveExceptionHandlers) {
            m_jit.store32(CCallHelpers::TrustedImm32(m_callSiteIndex), CCallHelpers::tagFor(CallFrameSlot::argumentCountIncludingThis));
            flushRegisters();
        }
        m_jit.move(GPRInfo::wasmContextInstancePointer, GPRInfo::argumentGPR0);
        emitThrowImpl(m_jit, exceptionIndex);

        return { };
    }

#if USE(JSVALUE64)
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRethrow(unsigned, ControlType& data)
    {
        LOG_INSTRUCTION("Rethrow", exception(data));

        ++m_callSiteIndex;
        bool mayHaveExceptionHandlers = !m_hasExceptionHandlers || m_hasExceptionHandlers.value();
        if (mayHaveExceptionHandlers) {
            m_jit.store32(CCallHelpers::TrustedImm32(m_callSiteIndex), CCallHelpers::tagFor(CallFrameSlot::argumentCountIncludingThis));
            flushRegisters();
        }
        emitMove(this->exception(data), Location::fromGPR(GPRInfo::argumentGPR1));
        m_jit.move(GPRInfo::wasmContextInstancePointer, GPRInfo::argumentGPR0);
        emitRethrowImpl(m_jit);
        return { };
    }
#else
    PartialResult WARN_UNUSED_RETURN BBQJIT::addRethrow(unsigned, ControlType& data)
    {
        LOG_INSTRUCTION("Rethrow", exception(data));

        ++m_callSiteIndex;
        bool mayHaveExceptionHandlers = !m_hasExceptionHandlers || m_hasExceptionHandlers.value();
        if (mayHaveExceptionHandlers) {
            m_jit.store32(CCallHelpers::TrustedImm32(m_callSiteIndex), CCallHelpers::tagFor(CallFrameSlot::argumentCountIncludingThis));
            flushRegisters();
        }
        emitMove(this->exception(data), Location::fromGPR2(GPRInfo::argumentGPR3, GPRInfo::argumentGPR2));
        m_jit.move(GPRInfo::wasmContextInstancePointer, GPRInfo::argumentGPR0);
        emitRethrowImpl(m_jit);
        return { };
    }
#endif

    void BBQJIT::prepareForExceptions()
    {
        ++m_callSiteIndex;
        bool mayHaveExceptionHandlers = !m_hasExceptionHandlers || m_hasExceptionHandlers.value();
        if (mayHaveExceptionHandlers) {
            m_jit.store32(CCallHelpers::TrustedImm32(m_callSiteIndex), CCallHelpers::tagFor(CallFrameSlot::argumentCountIncludingThis));
            flushRegistersForException();
        }
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addReturn(const ControlData& data, const Stack& returnValues)
    {
        CallInformation wasmCallInfo = wasmCallingConvention().callInformationFor(*data.signature(), CallRole::Callee);

        if (!wasmCallInfo.results.isEmpty()) {
            ASSERT(returnValues.size() >= wasmCallInfo.results.size());
            unsigned offset = returnValues.size() - wasmCallInfo.results.size();
            Vector<Value, 8> returnValuesForShuffle;
            Vector<Location, 8> returnLocationsForShuffle;
            for (unsigned i = 0; i < wasmCallInfo.results.size(); ++i) {
                returnValuesForShuffle.append(returnValues[offset + i]);
                returnLocationsForShuffle.append(Location::fromArgumentLocation(wasmCallInfo.results[i], returnValues[offset + i].type().kind));
            }
            emitShuffle(returnValuesForShuffle, returnLocationsForShuffle);
            LOG_INSTRUCTION("Return", returnLocationsForShuffle);
        } else
            LOG_INSTRUCTION("Return");

        for (const auto& value : returnValues)
            consume(value);

        const ControlData& enclosingBlock = !m_parser->controlStack().size() ? data : currentControlData();
        for (LocalOrTempIndex localIndex : enclosingBlock.m_touchedLocals) {
            Value local = Value::fromLocal(m_localTypes[localIndex], localIndex);
            if (locationOf(local).isRegister()) {
                // Flush all locals without emitting stores (since we're leaving anyway)
                unbind(local, locationOf(local));
                bind(local, canonicalSlot(local));
            }
        }

        m_jit.emitFunctionEpilogue();
        m_jit.ret();
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addBranch(ControlData& target, Value condition, Stack& results)
    {
        if (condition.isConst() && !condition.asI32()) // If condition is known to be false, this is a no-op.
            return { };

        Location conditionLocation = Location::fromGPR(wasmScratchGPR);
        if (!condition.isNone() && !condition.isConst())
            emitMove(condition, conditionLocation);
        consume(condition);

        if (condition.isNone())
            LOG_INSTRUCTION("Branch");
        else
            LOG_INSTRUCTION("Branch", condition, conditionLocation);

        if (condition.isConst() || condition.isNone()) {
            currentControlData().flushAndSingleExit(*this, target, results, false, condition.isNone());
            target.addBranch(m_jit.jump()); // We know condition is true, since if it was false we would have returned early.
        } else {
            currentControlData().flushAtBlockBoundary(*this, 0, results, condition.isNone());
            Jump ifNotTaken = m_jit.branchTest32(ResultCondition::Zero, wasmScratchGPR);
            currentControlData().addExit(*this, target.targetLocations(), results);
            target.addBranch(m_jit.jump());
            ifNotTaken.link(&m_jit);
            currentControlData().finalizeBlock(*this, target.targetLocations().size(), results, true);
        }

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSwitch(Value condition, const Vector<ControlData*>& targets, ControlData& defaultTarget, Stack& results)
    {
        ASSERT(condition.type() == TypeKind::I32);

        LOG_INSTRUCTION("BrTable", condition);

        if (!condition.isConst())
            emitMove(condition, Location::fromGPR(wasmScratchGPR));
        consume(condition);

        if (condition.isConst()) {
            // If we know the condition statically, we emit one direct branch to the known target.
            int targetIndex = condition.asI32();
            if (targetIndex >= 0 && targetIndex < static_cast<int>(targets.size())) {
                currentControlData().flushAndSingleExit(*this, *targets[targetIndex], results, false, true);
                targets[targetIndex]->addBranch(m_jit.jump());
            } else {
                currentControlData().flushAndSingleExit(*this, defaultTarget, results, false, true);
                defaultTarget.addBranch(m_jit.jump());
            }
            return { };
        }

        // Flush everything below the top N values.
        currentControlData().flushAtBlockBoundary(*this, defaultTarget.targetLocations().size(), results, true);

        constexpr unsigned minCasesForTable = 7;
        if (minCasesForTable <= targets.size()) {
            auto* jumpTable = m_callee.addJumpTable(targets.size());
            auto fallThrough = m_jit.branch32(RelationalCondition::AboveOrEqual, wasmScratchGPR, TrustedImm32(targets.size()));
            m_jit.zeroExtend32ToWord(wasmScratchGPR, wasmScratchGPR);
            if constexpr (is64Bit())
                m_jit.lshiftPtr(TrustedImm32(3), wasmScratchGPR);
            else
                m_jit.lshiftPtr(TrustedImm32(2), wasmScratchGPR);
            m_jit.addPtr(TrustedImmPtr(jumpTable->data()), wasmScratchGPR);
            m_jit.farJump(Address(wasmScratchGPR), JSSwitchPtrTag);

            auto labels = WTF::map(targets, [&](auto& target) {
                auto label = Box<CCallHelpers::Label>::create(m_jit.label());
                bool isCodeEmitted = currentControlData().addExit(*this, target->targetLocations(), results);
                if (isCodeEmitted)
                    target->addBranch(m_jit.jump());
                else {
                    // It is common that we do not need to emit anything before jumping to the target block.
                    // In that case, we put Box<Label> which will be filled later when the end of the block is linked.
                    // We put direct jump to that block in the link task.
                    target->addLabel(Box { label });
                }
                return label;
            });

            m_jit.addLinkTask([labels = WTFMove(labels), jumpTable](LinkBuffer& linkBuffer) {
                for (unsigned index = 0; index < labels.size(); ++index)
                    jumpTable->at(index) = linkBuffer.locationOf<JSSwitchPtrTag>(*labels[index]);
            });

            fallThrough.link(&m_jit);
        } else {
            Vector<int64_t> cases(targets.size(), [](size_t i) { return i; });

            BinarySwitch binarySwitch(wasmScratchGPR, cases, BinarySwitch::Int32);
            while (binarySwitch.advance(m_jit)) {
                unsigned value = binarySwitch.caseValue();
                unsigned index = binarySwitch.caseIndex();
                ASSERT_UNUSED(value, value == index);
                ASSERT(index < targets.size());
                currentControlData().addExit(*this, targets[index]->targetLocations(), results);
                targets[index]->addBranch(m_jit.jump());
            }

            binarySwitch.fallThrough().link(&m_jit);
        }
        currentControlData().addExit(*this, defaultTarget.targetLocations(), results);
        defaultTarget.addBranch(m_jit.jump());

        currentControlData().finalizeBlock(*this, defaultTarget.targetLocations().size(), results, false);
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::endBlock(ControlEntry& entry, Stack& stack)
    {
        return addEndToUnreachable(entry, stack, false);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addEndToUnreachable(ControlEntry& entry, Stack& stack, bool unreachable)
    {
        ControlData& entryData = entry.controlData;

        unsigned returnCount = entryData.signature()->as<FunctionSignature>()->returnCount();
        if (unreachable) {
            for (unsigned i = 0; i < returnCount; ++i) {
                Type type = entryData.signature()->as<FunctionSignature>()->returnType(i);
                entry.enclosedExpressionStack.constructAndAppend(type, Value::fromTemp(type.kind, entryData.enclosedHeight() + entryData.implicitSlots() + i));
            }
            for (const auto& binding : m_gprBindings) {
                if (!binding.isNone())
                    consume(binding.toValue());
            }
            for (const auto& binding : m_fprBindings) {
                if (!binding.isNone())
                    consume(binding.toValue());
            }
        } else {
            unsigned offset = stack.size() - returnCount;
            for (unsigned i = 0; i < returnCount; ++i)
                entry.enclosedExpressionStack.append(stack[i + offset]);
        }

        switch (entryData.blockType()) {
        case BlockType::TopLevel:
            entryData.flushAndSingleExit(*this, entryData, entry.enclosedExpressionStack, false, true, unreachable);
            entryData.linkJumps(&m_jit);
            for (unsigned i = 0; i < returnCount; ++i) {
                // Make sure we expect the stack values in the correct locations.
                if (!entry.enclosedExpressionStack[i].value().isConst()) {
                    Value& value = entry.enclosedExpressionStack[i].value();
                    value = Value::fromTemp(value.type(), i);
                    Location valueLocation = locationOf(value);
                    if (valueLocation.isRegister())
                        RELEASE_ASSERT(valueLocation == entryData.resultLocations()[i]);
                    else
                        bind(value, entryData.resultLocations()[i]);
                }
            }
            return addReturn(entryData, entry.enclosedExpressionStack);
        case BlockType::Loop:
            entryData.convertLoopToBlock();
            entryData.flushAndSingleExit(*this, entryData, entry.enclosedExpressionStack, false, true, unreachable);
            entryData.linkJumpsTo(entryData.loopLabel(), &m_jit);
            if (m_tierUp)
                m_outerLoops.takeLast();
            break;
        case BlockType::Try:
        case BlockType::Catch:
            --m_tryCatchDepth;
            entryData.flushAndSingleExit(*this, entryData, entry.enclosedExpressionStack, false, true, unreachable);
            entryData.linkJumps(&m_jit);
            break;
        default:
            entryData.flushAndSingleExit(*this, entryData, entry.enclosedExpressionStack, false, true, unreachable);
            entryData.linkJumps(&m_jit);
            break;
        }

        LOG_DEDENT();
        LOG_INSTRUCTION("End");

        currentControlData().resumeBlock(*this, entryData, entry.enclosedExpressionStack);

        return { };
    }

#if USE(JSVALUE64)
    int BBQJIT::alignedFrameSize(int frameSize)
    {
        return WTF::roundUpToMultipleOf(stackAlignmentBytes(), frameSize);
    }
#else
    int BBQJIT::alignedFrameSize(int frameSize)
    {
        // On armv7 account for misalignment due to of saved {FP, PC}
        constexpr int misalignment = 4 + 4;
        return WTF::roundUpToMultipleOf(stackAlignmentBytes(), frameSize + misalignment) - misalignment;
    }
#endif

    PartialResult WARN_UNUSED_RETURN BBQJIT::endTopLevel(BlockSignature, const Stack&)
    {
        int frameSize = m_frameSize + m_maxCalleeStackSize;
        CCallHelpers& jit = m_jit;
        m_jit.addLinkTask([frameSize, labels = WTFMove(m_frameSizeLabels), &jit, this](LinkBuffer& linkBuffer) {
            for (auto label : labels)
                jit.repatchPointer(linkBuffer.locationOf<NoPtrTag>(label), bitwise_cast<void*>(static_cast<uintptr_t>(alignedFrameSize(frameSize))));
        });

        LOG_DEDENT();
        LOG_INSTRUCTION("End");

        if (UNLIKELY(m_disassembler))
            m_disassembler->setEndOfOpcode(m_jit.label());

        for (const auto& latePath : m_latePaths)
            latePath->run(*this, m_jit);

        for (unsigned i = 0; i < numberOfExceptionTypes; ++i) {
            auto& jumps = m_exceptions[i];
            if (!jumps.empty()) {
                jumps.link(&jit);
                emitThrowException(static_cast<ExceptionType>(i));
            }
        }

        m_compilation->osrEntryScratchBufferSize = m_osrEntryScratchBufferSize;
        return { };
    }

    // Flush a value to its canonical slot.
    void BBQJIT::flushValue(Value value)
    {
        if (value.isConst() || value.isPinned())
            return;
        Location currentLocation = locationOf(value);
        Location slot = canonicalSlot(value);

        emitMove(value, slot);
        unbind(value, currentLocation);
        bind(value, slot);
    }

    void BBQJIT::restoreWebAssemblyContextInstance()
    {
        m_jit.loadPtr(Address(GPRInfo::callFrameRegister, CallFrameSlot::codeBlock * sizeof(Register)), GPRInfo::wasmContextInstancePointer);
    }

#if USE(JSVALUE64)
    void BBQJIT::restoreWebAssemblyGlobalState()
    {
        restoreWebAssemblyContextInstance();
        // FIXME: We should just store these registers on stack and load them.
        if (!!m_info.memory)
            loadWebAssemblyGlobalState(wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister);
    }
#else
    void BBQJIT::restoreWebAssemblyGlobalState()
    {
        restoreWebAssemblyContextInstance();
    }
#endif

    void BBQJIT::loadWebAssemblyGlobalState(GPRReg wasmBaseMemoryPointer, GPRReg wasmBoundsCheckingSizeRegister)
    {
        m_jit.loadPairPtr(GPRInfo::wasmContextInstancePointer, TrustedImm32(Instance::offsetOfCachedMemory()), wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister);
        m_jit.cageConditionallyAndUntag(Gigacage::Primitive, wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister, wasmScratchGPR, /* validateAuth */ true, /* mayBeNull */ false);
    }

#if USE(JSVALUE64)
    void BBQJIT::restoreWebAssemblyGlobalStateAfterWasmCall()
    {
        if (!!m_info.memory && (m_mode == MemoryMode::Signaling || m_info.memory.isShared())) {
            // If memory is signaling or shared, then memoryBase and memorySize will not change. This means that only thing we should check here is GPRInfo::wasmContextInstancePointer is the same or not.
            // Let's consider the case, this was calling a JS function. So it can grow / modify memory whatever. But memoryBase and memorySize are kept the same in this case.
            m_jit.loadPtr(Address(GPRInfo::callFrameRegister, CallFrameSlot::codeBlock * sizeof(Register)), wasmScratchGPR);
            Jump isSameInstanceAfter = m_jit.branchPtr(RelationalCondition::Equal, wasmScratchGPR, GPRInfo::wasmContextInstancePointer);
            m_jit.move(wasmScratchGPR, GPRInfo::wasmContextInstancePointer);
            m_jit.loadPairPtr(GPRInfo::wasmContextInstancePointer, TrustedImm32(Instance::offsetOfCachedMemory()), wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister);
            m_jit.cageConditionallyAndUntag(Gigacage::Primitive, wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister, wasmScratchGPR, /* validateAuth */ true, /* mayBeNull */ false);
            isSameInstanceAfter.link(&m_jit);
        } else
            restoreWebAssemblyGlobalState();
    }
#else
    void BBQJIT::restoreWebAssemblyGlobalStateAfterWasmCall()
    {
        restoreWebAssemblyGlobalState();
    }
#endif

    void BBQJIT::flushRegistersForException()
    {
        // Flush all locals.
        for (RegisterBinding& binding : m_gprBindings) {
            if (binding.toValue().isLocal())
                flushValue(binding.toValue());
        }

        for (RegisterBinding& binding : m_fprBindings) {
            if (binding.toValue().isLocal())
                flushValue(binding.toValue());
        }
    }

    void BBQJIT::flushRegisters()
    {
        // Just flush everything.
        for (RegisterBinding& binding : m_gprBindings) {
            if (!binding.toValue().isNone())
                flushValue(binding.toValue());
        }

        for (RegisterBinding& binding : m_fprBindings) {
            if (!binding.toValue().isNone())
                flushValue(binding.toValue());
        }
    }

    template<size_t N>
    void BBQJIT::saveValuesAcrossCallAndPassArguments(const Vector<Value, N>& arguments, const CallInformation& callInfo, const TypeDefinition& signature)
    {
        // First, we resolve all the locations of the passed arguments, before any spillage occurs. For constants,
        // we store their normal values; for all other values, we store pinned values with their current location.
        // We'll directly use these when passing parameters, since no other instructions we emit here should
        // overwrite registers currently occupied by values.

        auto resolvedArguments = WTF::map<N>(arguments, [&](auto& argument) {
            auto value = argument.isConst() ? argument : Value::pinned(argument.type(), locationOf(argument));
            // Like other value uses, we count this as a use here, and end the lifetimes of any temps we passed.
            // This saves us the work of having to spill them to their canonical slots.
            consume(argument);
            return value;
        });

        // At this point in the program, argumentLocations doesn't represent the state of the register allocator.
        // We need to be careful not to allocate any new registers before passing them to the function, since that
        // could clobber the registers we assume still contain the argument values!

        // Next, for all values currently still occupying a caller-saved register, we flush them to their canonical slot.
        for (Reg reg : m_callerSaves) {
            RegisterBinding binding = reg.isGPR() ? m_gprBindings[reg.gpr()] : m_fprBindings[reg.fpr()];
            if (!binding.toValue().isNone())
                flushValue(binding.toValue());
        }

        // Additionally, we flush anything currently bound to a register we're going to use for parameter passing. I
        // think these will be handled by the caller-save logic without additional effort, but it doesn't hurt to be
        // careful.
        for (size_t i = 0; i < callInfo.params.size(); ++i) {
            auto type = signature.as<FunctionSignature>()->argumentType(i);
            Location paramLocation = Location::fromArgumentLocation(callInfo.params[i], type.kind);
            if (paramLocation.isRegister()) {
                RegisterBinding binding;
                if (paramLocation.isGPR())
                    binding = m_gprBindings[paramLocation.asGPR()];
                else if (paramLocation.isFPR())
                    binding = m_fprBindings[paramLocation.asFPR()];
#if USE(JSVALUE32_64)
                else if (paramLocation.isGPR2())
                    binding = m_gprBindings[paramLocation.asGPRhi()];
#endif
                if (!binding.toValue().isNone())
                    flushValue(binding.toValue());
            }
        }

        // Finally, we parallel-move arguments to the parameter locations.
        WTF::Vector<Location, N> parameterLocations;
        parameterLocations.reserveInitialCapacity(callInfo.params.size());
        for (unsigned i = 0; i < callInfo.params.size(); i++) {
            auto type = signature.as<FunctionSignature>()->argumentType(i);
            auto parameterLocation = Location::fromArgumentLocation(callInfo.params[i], type.kind);
            parameterLocations.append(parameterLocation);
        }

        emitShuffle(resolvedArguments, parameterLocations);
    }

    void BBQJIT::restoreValuesAfterCall(const CallInformation& callInfo)
    {
        UNUSED_PARAM(callInfo);
        // Caller-saved values shouldn't actually need to be restored here, the register allocator will restore them lazily
        // whenever they are next used.
    }

    template<size_t N>
    void BBQJIT::returnValuesFromCall(Vector<Value, N>& results, const FunctionSignature& functionType, const CallInformation& callInfo)
    {
        for (size_t i = 0; i < callInfo.results.size(); i ++) {
            Value result = Value::fromTemp(functionType.returnType(i).kind, currentControlData().enclosedHeight() + currentControlData().implicitSlots() + m_parser->expressionStack().size() + i);
            Location returnLocation = Location::fromArgumentLocation(callInfo.results[i], result.type());
            if (returnLocation.isRegister()) {
                RegisterBinding currentBinding;
                if (returnLocation.isGPR())
                    currentBinding = m_gprBindings[returnLocation.asGPR()];
                else if (returnLocation.isFPR())
                    currentBinding = m_fprBindings[returnLocation.asFPR()];
#if USE(JSVALUE32_64)
                else if (returnLocation.isGPR2())
                    currentBinding = m_gprBindings[returnLocation.asGPRhi()];
#endif
                if (currentBinding.isScratch()) {
                    // FIXME: This is a total hack and could cause problems. We assume scratch registers (allocated by a ScratchScope)
                    // will never be live across a call. So far, this is probably true, but it's fragile. Probably the fix here is to
                    // exclude all possible return value registers from ScratchScope so we can guarantee there's never any interference.
                    currentBinding = RegisterBinding::none();
                    if (returnLocation.isGPR()) {
                        ASSERT(m_validGPRs.contains(returnLocation.asGPR(), IgnoreVectors));
                        m_gprSet.add(returnLocation.asGPR(), IgnoreVectors);
#if USE(JSVALUE32_64)
                    } else if (returnLocation.isGPR2()) {
                        ASSERT(m_validGPRs.contains(returnLocation.asGPRlo(), IgnoreVectors));
                        ASSERT(m_validGPRs.contains(returnLocation.asGPRhi(), IgnoreVectors));
                        m_gprSet.add(returnLocation.asGPRlo(), IgnoreVectors);
                        m_gprSet.add(returnLocation.asGPRhi(), IgnoreVectors);
#endif
                    } else {
                        ASSERT(m_validFPRs.contains(returnLocation.asFPR(), Width::Width128));
                        m_fprSet.add(returnLocation.asFPR(), Width::Width128);
                    }
                }
            }
            bind(result, returnLocation);
            results.append(result);
        }
    }

    template<typename Func, size_t N>
    void BBQJIT::emitCCall(Func function, const Vector<Value, N>& arguments)
    {
        // Currently, we assume the Wasm calling convention is the same as the C calling convention
        Vector<Type, 16> resultTypes;
        auto argumentTypes = WTF::map<16>(arguments, [](auto& value) {
            return Type { value.type(), 0u };
        });
        RefPtr<TypeDefinition> functionType = TypeInformation::typeDefinitionForFunction(resultTypes, argumentTypes);
#if CPU(ARM64) || CPU(X86_64)
        CallInformation callInfo = wasmCallingConvention().callInformationFor(*functionType, CallRole::Caller);
#elif CPU(ARM)
        CallInformation callInfo = cCallingConventionArmThumb2().callInformationFor(*functionType, CallRole::Caller);
#endif
        Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf(stackAlignmentBytes(), callInfo.headerAndArgumentStackSizeInBytes);
        m_maxCalleeStackSize = std::max<int>(calleeStackSize, m_maxCalleeStackSize);

        // Prepare wasm operation calls.
        m_jit.prepareWasmCallOperation(GPRInfo::wasmContextInstancePointer);

        // Preserve caller-saved registers and other info
        prepareForExceptions();
        saveValuesAcrossCallAndPassArguments(arguments, callInfo, *functionType);

        // Materialize address of native function and call register
        void* taggedFunctionPtr = tagCFunctionPtr<void*, OperationPtrTag>(function);
        m_jit.move(TrustedImmPtr(bitwise_cast<uintptr_t>(taggedFunctionPtr)), wasmScratchGPR);
        m_jit.call(wasmScratchGPR, OperationPtrTag);
    }

    template<typename Func, size_t N>
    void BBQJIT::emitCCall(Func function, const Vector<Value, N>& arguments, Value& result)
    {
        ASSERT(result.isTemp());

        // Currently, we assume the Wasm calling convention is the same as the C calling convention
        Vector<Type, 16> resultTypes = { Type { result.type(), 0u } };
        auto argumentTypes = WTF::map<16>(arguments, [](auto& value) {
            return Type { value.type(), 0u };
        });

        RefPtr<TypeDefinition> functionType = TypeInformation::typeDefinitionForFunction(resultTypes, argumentTypes);
#if CPU(ARM64) || CPU(X86_64)
        CallInformation callInfo = wasmCallingConvention().callInformationFor(*functionType, CallRole::Caller);
#elif CPU(ARM)
        CallInformation callInfo = cCallingConventionArmThumb2().callInformationFor(*functionType, CallRole::Caller);
#endif
        Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf(stackAlignmentBytes(), callInfo.headerAndArgumentStackSizeInBytes);
        m_maxCalleeStackSize = std::max<int>(calleeStackSize, m_maxCalleeStackSize);

        // Prepare wasm operation calls.
        m_jit.prepareWasmCallOperation(GPRInfo::wasmContextInstancePointer);

        // Preserve caller-saved registers and other info
        prepareForExceptions();
        saveValuesAcrossCallAndPassArguments(arguments, callInfo, *functionType);

        // Materialize address of native function and call register
        void* taggedFunctionPtr = tagCFunctionPtr<void*, OperationPtrTag>(function);
        m_jit.move(TrustedImmPtr(bitwise_cast<uintptr_t>(taggedFunctionPtr)), wasmScratchGPR);
        m_jit.call(wasmScratchGPR, OperationPtrTag);

        Location resultLocation;
        switch (result.type()) {
        case TypeKind::I32:
#if USE(JSVALUE32_64)
            resultLocation = Location::fromGPR(GPRInfo::returnValueGPR);
            break;
#endif
        case TypeKind::I31ref:
        case TypeKind::I64:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::Funcref:
        case TypeKind::Externref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
        case TypeKind::Rec:
        case TypeKind::Sub:
        case TypeKind::Subfinal:
        case TypeKind::Array:
        case TypeKind::Struct:
        case TypeKind::Func: {
#if USE(JSVALUE64)
            resultLocation = Location::fromGPR(GPRInfo::returnValueGPR);
#else
            resultLocation = Location::fromGPR2(GPRInfo::returnValueGPR2, GPRInfo::returnValueGPR);
#endif
            ASSERT(m_validGPRs.contains(GPRInfo::returnValueGPR, IgnoreVectors));
            break;
        }
        case TypeKind::F32:
        case TypeKind::F64: {
            resultLocation = Location::fromFPR(FPRInfo::returnValueFPR);
            ASSERT(m_validFPRs.contains(FPRInfo::returnValueFPR, Width::Width128));
            break;
        }
        case TypeKind::V128: {
            resultLocation = Location::fromFPR(FPRInfo::returnValueFPR);
            ASSERT(m_validFPRs.contains(FPRInfo::returnValueFPR, Width::Width128));
            break;
        }
        case TypeKind::Void:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }

        RegisterBinding currentBinding;
        if (resultLocation.isGPR())
            currentBinding = m_gprBindings[resultLocation.asGPR()];
        else if (resultLocation.isFPR())
            currentBinding = m_fprBindings[resultLocation.asFPR()];
#if USE(JSVALUE32_64)
        else if (resultLocation.isGPR2())
            currentBinding = m_gprBindings[resultLocation.asGPRhi()];
#endif
        RELEASE_ASSERT(!currentBinding.isScratch());

        bind(result, resultLocation);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCall(unsigned functionIndex, const TypeDefinition& signature, Vector<Value>& arguments, ResultList& results, CallType callType)
    {
        UNUSED_PARAM(callType); // TODO: handle tail calls
        const FunctionSignature& functionType = *signature.as<FunctionSignature>();
        CallInformation callInfo = wasmCallingConvention().callInformationFor(signature, CallRole::Caller);
        Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf(stackAlignmentBytes(), callInfo.headerAndArgumentStackSizeInBytes);
        m_maxCalleeStackSize = std::max<int>(calleeStackSize, m_maxCalleeStackSize);

        // Preserve caller-saved registers and other info
        prepareForExceptions();
        saveValuesAcrossCallAndPassArguments(arguments, callInfo, signature);

        if (m_info.isImportedFunctionFromFunctionIndexSpace(functionIndex)) {
            static_assert(sizeof(Instance::ImportFunctionInfo) * maxImports < std::numeric_limits<int32_t>::max());
            RELEASE_ASSERT(Instance::offsetOfImportFunctionStub(functionIndex) < std::numeric_limits<int32_t>::max());
            m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, Instance::offsetOfImportFunctionStub(functionIndex)), wasmScratchGPR);

            m_jit.call(wasmScratchGPR, WasmEntryPtrTag);
        } else {
            // Emit the call.
            Vector<UnlinkedWasmToWasmCall>* unlinkedWasmToWasmCalls = &m_unlinkedWasmToWasmCalls;
            CCallHelpers::Call call = m_jit.threadSafePatchableNearCall();
            m_jit.addLinkTask([unlinkedWasmToWasmCalls, call, functionIndex] (LinkBuffer& linkBuffer) {
                unlinkedWasmToWasmCalls->append({ linkBuffer.locationOfNearCall<WasmEntryPtrTag>(call), functionIndex });
            });
        }

        // Push return value(s) onto the expression stack
        returnValuesFromCall(results, functionType, callInfo);

        if (m_info.callCanClobberInstance(functionIndex) || m_info.isImportedFunctionFromFunctionIndexSpace(functionIndex))
            restoreWebAssemblyGlobalStateAfterWasmCall();

        LOG_INSTRUCTION("Call", functionIndex, arguments, "=> ", results);

        return { };
    }

    void BBQJIT::emitIndirectCall(const char* opcode, const Value& calleeIndex, GPRReg calleeInstance, GPRReg calleeCode, GPRReg jsCalleeAnchor, const TypeDefinition& signature, Vector<Value>& arguments, ResultList& results, CallType callType)
    {
        // TODO: Support tail calls
        UNUSED_PARAM(jsCalleeAnchor);
        RELEASE_ASSERT(callType == CallType::Call);
        ASSERT(!RegisterSetBuilder::argumentGPRS().contains(calleeCode, IgnoreVectors));

        const auto& callingConvention = wasmCallingConvention();
        CallInformation wasmCalleeInfo = callingConvention.callInformationFor(signature, CallRole::Caller);
        Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf(stackAlignmentBytes(), wasmCalleeInfo.headerAndArgumentStackSizeInBytes);
        m_maxCalleeStackSize = std::max<int>(calleeStackSize, m_maxCalleeStackSize);

        // Do a context switch if needed.
        Jump isSameInstanceBefore = m_jit.branchPtr(RelationalCondition::Equal, calleeInstance, GPRInfo::wasmContextInstancePointer);
        m_jit.move(calleeInstance, GPRInfo::wasmContextInstancePointer);
#if USE(JSVALUE64)
        loadWebAssemblyGlobalState(wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister);
#endif
        isSameInstanceBefore.link(&m_jit);

        // Since this can switch instance, we need to keep JSWebAssemblyInstance anchored in the stack.
        m_jit.storePtr(jsCalleeAnchor, Location::fromArgumentLocation(wasmCalleeInfo.thisArgument, TypeKind::Void).asAddress());

        m_jit.loadPtr(Address(calleeCode), calleeCode);
        prepareForExceptions();
        saveValuesAcrossCallAndPassArguments(arguments, wasmCalleeInfo, signature); // Keep in mind that this clobbers wasmScratchGPR and wasmScratchFPR.

        // Why can we still call calleeCode after saveValuesAcrossCallAndPassArguments? This is because we ensured that calleeCode is GPRInfo::nonPreservedNonArgumentGPR1,
        // and any argument locations will not include GPRInfo::nonPreservedNonArgumentGPR1.
        m_jit.call(calleeCode, WasmEntryPtrTag);
        returnValuesFromCall(results, *signature.as<FunctionSignature>(), wasmCalleeInfo);

        restoreWebAssemblyGlobalStateAfterWasmCall();

        LOG_INSTRUCTION(opcode, calleeIndex, arguments, "=> ", results);
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCallIndirect(unsigned tableIndex, const TypeDefinition& originalSignature, Vector<Value>& args, ResultList& results, CallType callType)
    {
        Value calleeIndex = args.takeLast();
        const TypeDefinition& signature = originalSignature.expand();
        ASSERT(signature.as<FunctionSignature>()->argumentCount() == args.size());
        ASSERT(m_info.tableCount() > tableIndex);
        ASSERT(m_info.tables[tableIndex].type() == TableElementType::Funcref);

        Location calleeIndexLocation;
        GPRReg calleeInstance;
        GPRReg calleeCode;
        GPRReg jsCalleeAnchor;

        {
            ScratchScope<1, 0> calleeCodeScratch(*this, RegisterSetBuilder::argumentGPRS());
            calleeCode = calleeCodeScratch.gpr(0);
            calleeCodeScratch.unbindPreserved();

            {
                ScratchScope<2, 0> scratches(*this);

                if (calleeIndex.isConst())
                    emitMoveConst(calleeIndex, calleeIndexLocation = Location::fromGPR(scratches.gpr(1)));
                else
                    calleeIndexLocation = loadIfNecessary(calleeIndex);

                GPRReg callableFunctionBufferLength = calleeCode;
                GPRReg callableFunctionBuffer = scratches.gpr(0);

                ASSERT(tableIndex < m_info.tableCount());

                int numImportFunctions = m_info.importFunctionCount();
                m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, Instance::offsetOfTablePtr(numImportFunctions, tableIndex)), callableFunctionBufferLength);
                auto& tableInformation = m_info.table(tableIndex);
                if (tableInformation.maximum() && tableInformation.maximum().value() == tableInformation.initial()) {
                    if (!tableInformation.isImport())
                        m_jit.addPtr(TrustedImm32(FuncRefTable::offsetOfFunctionsForFixedSizedTable()), callableFunctionBufferLength, callableFunctionBuffer);
                    else
                        m_jit.loadPtr(Address(callableFunctionBufferLength, FuncRefTable::offsetOfFunctions()), callableFunctionBuffer);
                    m_jit.move(TrustedImm32(tableInformation.initial()), callableFunctionBufferLength);
                } else {
                    m_jit.loadPtr(Address(callableFunctionBufferLength, FuncRefTable::offsetOfFunctions()), callableFunctionBuffer);
                    m_jit.load32(Address(callableFunctionBufferLength, FuncRefTable::offsetOfLength()), callableFunctionBufferLength);
                }
                ASSERT(calleeIndexLocation.isGPR());

                consume(calleeIndex);

                // Check the index we are looking for is valid.
                throwExceptionIf(ExceptionType::OutOfBoundsCallIndirect, m_jit.branch32(RelationalCondition::AboveOrEqual, calleeIndexLocation.asGPR(), callableFunctionBufferLength));

                // Neither callableFunctionBuffer nor callableFunctionBufferLength are used before any of these
                // are def'd below, so we can reuse the registers and save some pressure.
                calleeInstance = scratches.gpr(0);
                jsCalleeAnchor = scratches.gpr(1);

                static_assert(sizeof(TypeIndex) == sizeof(void*));
                GPRReg calleeSignatureIndex = wasmScratchGPR;

                // Compute the offset in the table index space we are looking for.
                if (hasOneBitSet(sizeof(FuncRefTable::Function))) {
                    m_jit.zeroExtend32ToWord(calleeIndexLocation.asGPR(), calleeIndexLocation.asGPR());
#if CPU(ARM64)
                    m_jit.addLeftShift64(callableFunctionBuffer, calleeIndexLocation.asGPR(), TrustedImm32(getLSBSet(sizeof(FuncRefTable::Function))), calleeSignatureIndex);
#elif CPU(ARM)
                    m_jit.lshift32(calleeIndexLocation.asGPR(), TrustedImm32(getLSBSet(sizeof(FuncRefTable::Function))), calleeSignatureIndex);
                    m_jit.addPtr(callableFunctionBuffer, calleeSignatureIndex);
#else
                    m_jit.lshift64(calleeIndexLocation.asGPR(), TrustedImm32(getLSBSet(sizeof(FuncRefTable::Function))), calleeSignatureIndex);
                    m_jit.addPtr(callableFunctionBuffer, calleeSignatureIndex);
#endif
                } else {
                    m_jit.move(TrustedImmPtr(sizeof(FuncRefTable::Function)), calleeSignatureIndex);
#if CPU(ARM64)
                    m_jit.multiplyAddZeroExtend32(calleeIndexLocation.asGPR(), calleeSignatureIndex, callableFunctionBuffer, calleeSignatureIndex);
#elif CPU(ARM)
                    m_jit.mul32(calleeIndexLocation.asGPR(), calleeSignatureIndex);
                    m_jit.addPtr(callableFunctionBuffer, calleeSignatureIndex);
#else
                    m_jit.zeroExtend32ToWord(calleeIndexLocation.asGPR(), calleeIndexLocation.asGPR());
                    m_jit.mul64(calleeIndexLocation.asGPR(), calleeSignatureIndex);
                    m_jit.addPtr(callableFunctionBuffer, calleeSignatureIndex);
#endif
                }

                // FIXME: This seems wasteful to do two checks just for a nicer error message.
                // We should move just to use a single branch and then figure out what
                // error to use in the exception handler.

#if USE(JSVALUE64)
                ASSERT(static_cast<ptrdiff_t>(FuncRefTable::Function::offsetOfInstance() + sizeof(void*)) == FuncRefTable::Function::offsetOfValue());
                m_jit.loadPairPtr(calleeSignatureIndex, TrustedImm32(FuncRefTable::Function::offsetOfInstance()), calleeInstance, jsCalleeAnchor);
                ASSERT(static_cast<ptrdiff_t>(WasmToWasmImportableFunction::offsetOfSignatureIndex() + sizeof(void*)) == WasmToWasmImportableFunction::offsetOfEntrypointLoadLocation());
                m_jit.loadPairPtr(calleeSignatureIndex, TrustedImm32(FuncRefTable::Function::offsetOfFunction() + WasmToWasmImportableFunction::offsetOfSignatureIndex()), calleeSignatureIndex, calleeCode);
#else
                m_jit.loadPtr(Address(calleeSignatureIndex, FuncRefTable::Function::offsetOfFunction() + WasmToWasmImportableFunction::offsetOfEntrypointLoadLocation()), calleeCode); // Pointer to callee code.
                m_jit.loadPtr(Address(calleeSignatureIndex, FuncRefTable::Function::offsetOfInstance()), calleeInstance);
                m_jit.loadPtr(Address(calleeSignatureIndex, FuncRefTable::Function::offsetOfValue()), jsCalleeAnchor);
                m_jit.loadPtr(Address(calleeSignatureIndex, FuncRefTable::Function::offsetOfFunction() + WasmToWasmImportableFunction::offsetOfSignatureIndex()), calleeSignatureIndex);
#endif

                throwExceptionIf(ExceptionType::NullTableEntry, m_jit.branchTestPtr(ResultCondition::Zero, calleeSignatureIndex, calleeSignatureIndex));
                throwExceptionIf(ExceptionType::BadSignature, m_jit.branchPtr(RelationalCondition::NotEqual, calleeSignatureIndex, TrustedImmPtr(TypeInformation::get(originalSignature))));
            }
        }
        emitIndirectCall("CallIndirect", calleeIndex, calleeInstance, calleeCode, jsCalleeAnchor, signature, args, results, callType);
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCallRef(const TypeDefinition& originalSignature, Vector<Value>& args, ResultList& results)
    {
        Value callee = args.takeLast();
        const TypeDefinition& signature = originalSignature.expand();
        ASSERT(signature.as<FunctionSignature>()->argumentCount() == args.size());

        CallInformation callInfo = wasmCallingConvention().callInformationFor(signature, CallRole::Caller);
        Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf(stackAlignmentBytes(), callInfo.headerAndArgumentStackSizeInBytes);
        m_maxCalleeStackSize = std::max<int>(calleeStackSize, m_maxCalleeStackSize);

        GPRReg calleePtr;
        GPRReg calleeInstance;
        GPRReg calleeCode;
        GPRReg jsCalleeAnchor;
        {
            ScratchScope<1, 0> calleeCodeScratch(*this, RegisterSetBuilder::argumentGPRS());
            calleeCode = calleeCodeScratch.gpr(0);
            calleeCodeScratch.unbindPreserved();

#if USE(JSVALUE64)
            ScratchScope<2, 0> otherScratches(*this);

            Location calleeLocation;
            if (callee.isConst()) {
                ASSERT(callee.asI64() == JSValue::encode(jsNull()));
                // This is going to throw anyway. It's suboptimial but probably won't happen in practice anyway.
                emitMoveConst(callee, calleeLocation = Location::fromGPR(otherScratches.gpr(0)));
            } else
                calleeLocation = loadIfNecessary(callee);
            emitThrowOnNullReference(ExceptionType::NullReference, calleeLocation);

            calleePtr = calleeLocation.asGPR();
            calleeInstance = otherScratches.gpr(0);
            jsCalleeAnchor = otherScratches.gpr(1);
#elif USE(JSVALUE32_64)
            ScratchScope<3, 0> otherScratches(*this);

            Location calleeLocation;
            if (callee.isConst()) {
                ASSERT(callee.asI64() == JSValue::encode(jsNull()));
                // This is going to throw anyway. It's suboptimial but probably won't happen in practice anyway.
                emitMoveConst(callee, calleeLocation = Location::fromGPR2(otherScratches.gpr(1), otherScratches.gpr(0)));
            } else
                calleeLocation = loadIfNecessary(callee);
            emitThrowOnNullReference(ExceptionType::NullReference, calleeLocation);

            calleePtr = calleeLocation.asGPRlo();
            calleeInstance = otherScratches.gpr(1);
            jsCalleeAnchor = otherScratches.gpr(2);
#endif

            m_jit.loadPtr(MacroAssembler::Address(calleePtr, WebAssemblyFunctionBase::offsetOfInstance()), jsCalleeAnchor);
            m_jit.loadPtr(MacroAssembler::Address(calleePtr, WebAssemblyFunctionBase::offsetOfEntrypointLoadLocation()), calleeCode);
            m_jit.loadPtr(MacroAssembler::Address(jsCalleeAnchor, JSWebAssemblyInstance::offsetOfInstance()), calleeInstance);

        }

        emitIndirectCall("CallRef", callee, calleeInstance, calleeCode, jsCalleeAnchor, signature, args, results, CallType::Call);
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addUnreachable()
    {
        LOG_INSTRUCTION("Unreachable");
        emitThrowException(ExceptionType::Unreachable);
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addCrash()
    {
        m_jit.breakpoint();
        return { };
    }

    ALWAYS_INLINE void BBQJIT::willParseOpcode()
    {
        m_pcToCodeOriginMapBuilder.appendItem(m_jit.label(), CodeOrigin(BytecodeIndex(m_parser->currentOpcodeStartingOffset())));
        if (UNLIKELY(m_disassembler))
            m_disassembler->setOpcode(m_jit.label(), m_parser->currentOpcode(), m_parser->currentOpcodeStartingOffset());
    }

    ALWAYS_INLINE void BBQJIT::didParseOpcode()
    {
    }

    // SIMD

#if USE(JSVALUE64)

    void BBQJIT::notifyFunctionUsesSIMD()
    {
        m_usesSIMD = true;
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoad(ExpressionType pointer, uint32_t uoffset, ExpressionType& result)
    {
        result = emitCheckAndPrepareAndMaterializePointerApply(pointer, uoffset, bytesForWidth(Width::Width128), [&](auto location) -> Value {
            consume(pointer);
            Value result = topValue(TypeKind::V128);
            Location resultLocation = allocate(result);
            m_jit.loadVector(location, resultLocation.asFPR());
            LOG_INSTRUCTION("V128Load", pointer, uoffset, RESULT(result));
            return result;
        });
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDStore(ExpressionType value, ExpressionType pointer, uint32_t uoffset)
    {
        emitCheckAndPrepareAndMaterializePointerApply(pointer, uoffset, bytesForWidth(Width::Width128), [&](auto location) -> void {
            Location valueLocation = loadIfNecessary(value);
            consume(pointer);
            consume(value);
            m_jit.storeVector(valueLocation.asFPR(), location);
            LOG_INSTRUCTION("V128Store", pointer, uoffset, value, valueLocation);
        });
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDSplat(SIMDLane lane, ExpressionType value, ExpressionType& result)
    {
        Location valueLocation;
        if (value.isConst()) {
            auto moveZeroToVector = [&] () -> PartialResult {
                result = topValue(TypeKind::V128);
                Location resultLocation = allocate(result);
                m_jit.moveZeroToVector(resultLocation.asFPR());
                LOG_INSTRUCTION("VectorSplat", lane, value, valueLocation, RESULT(result));
                return { };
            };

            auto moveOnesToVector = [&] () -> PartialResult {
                result = topValue(TypeKind::V128);
                Location resultLocation = allocate(result);
#if CPU(X86_64)
                m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::Unsigned }, resultLocation.asFPR(), resultLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
#else
                m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::Unsigned }, resultLocation.asFPR(), resultLocation.asFPR(), resultLocation.asFPR());
#endif
                LOG_INSTRUCTION("VectorSplat", lane, value, valueLocation, RESULT(result));
                return { };
            };

            switch (lane) {
            case SIMDLane::i8x16:
            case SIMDLane::i16x8:
            case SIMDLane::i32x4:
            case SIMDLane::f32x4: {
                // In theory someone could encode only the bottom bits for the i8x16/i16x8 cases but that would
                // require more bytes in the wasm encoding than just encoding 0/-1, so we don't worry about that.
                if (!value.asI32())
                    return moveZeroToVector();
                if (value.asI32() == -1)
                    return moveOnesToVector();
                break;
            }
            case SIMDLane::i64x2:
            case SIMDLane::f64x2: {
                if (!value.asI64())
                    return moveZeroToVector();
                if (value.asI64() == -1)
                    return moveOnesToVector();
                break;
            }

            default:
                RELEASE_ASSERT_NOT_REACHED();
                break;

            }

            if (value.isFloat()) {
                ScratchScope<0, 1> scratches(*this);
                valueLocation = Location::fromFPR(scratches.fpr(0));
            } else {
                ScratchScope<1, 0> scratches(*this);
                valueLocation = Location::fromGPR(scratches.gpr(0));
            }
            emitMoveConst(value, valueLocation);
        } else
            valueLocation = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);
        if (valueLocation.isGPR())
            m_jit.vectorSplat(lane, valueLocation.asGPR(), resultLocation.asFPR());
        else
            m_jit.vectorSplat(lane, valueLocation.asFPR(), resultLocation.asFPR());

        LOG_INSTRUCTION("VectorSplat", lane, value, valueLocation, RESULT(result));
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDShuffle(v128_t imm, ExpressionType a, ExpressionType b, ExpressionType& result)
    {
#if CPU(X86_64)
        ScratchScope<0, 1> scratches(*this);
#elif CPU(ARM64)
        // We need these adjacent registers for the tbl instruction, so we clobber and preserve them in this scope here.
        clobber(ARM64Registers::q28);
        clobber(ARM64Registers::q29);
        ScratchScope<0, 0> scratches(*this, Location::fromFPR(ARM64Registers::q28), Location::fromFPR(ARM64Registers::q29));
#endif
        Location aLocation = loadIfNecessary(a);
        Location bLocation = loadIfNecessary(b);
        consume(a);
        consume(b);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("VectorShuffle", a, aLocation, b, bLocation, RESULT(result));

        if constexpr (isX86()) {
            v128_t leftImm = imm;
            v128_t rightImm = imm;
            for (unsigned i = 0; i < 16; ++i) {
                if (leftImm.u8x16[i] > 15)
                    leftImm.u8x16[i] = 0xFF; // Force OOB
                if (rightImm.u8x16[i] < 16 || rightImm.u8x16[i] > 31)
                    rightImm.u8x16[i] = 0xFF; // Force OOB
            }
            // Store each byte (w/ index < 16) of `a` to result
            // and zero clear each byte (w/ index > 15) in result.
            materializeVectorConstant(leftImm, Location::fromFPR(scratches.fpr(0)));
            m_jit.vectorSwizzle(aLocation.asFPR(), scratches.fpr(0), scratches.fpr(0));

            // Store each byte (w/ index - 16 >= 0) of `b` to result2
            // and zero clear each byte (w/ index - 16 < 0) in result2.
            materializeVectorConstant(rightImm, Location::fromFPR(wasmScratchFPR));
            m_jit.vectorSwizzle(bLocation.asFPR(), wasmScratchFPR, wasmScratchFPR);
            m_jit.vectorOr(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, scratches.fpr(0), wasmScratchFPR, resultLocation.asFPR());
            return { };
        }

#if CPU(ARM64)
        materializeVectorConstant(imm, Location::fromFPR(wasmScratchFPR));
        if (unsigned(aLocation.asFPR()) + 1 != unsigned(bLocation.asFPR())) {
            m_jit.moveVector(aLocation.asFPR(), ARM64Registers::q28);
            m_jit.moveVector(bLocation.asFPR(), ARM64Registers::q29);
            aLocation = Location::fromFPR(ARM64Registers::q28);
            bLocation = Location::fromFPR(ARM64Registers::q29);
        }
        m_jit.vectorSwizzle2(aLocation.asFPR(), bLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
        UNREACHABLE_FOR_PLATFORM();
#endif

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDShift(SIMDLaneOperation op, SIMDInfo info, ExpressionType src, ExpressionType shift, ExpressionType& result)
    {
#if CPU(X86_64)
        // Clobber and preserve RCX on x86, since we need it to do shifts.
        clobber(shiftRCX);
        ScratchScope<2, 2> scratches(*this, Location::fromGPR(shiftRCX));
#endif
        Location srcLocation = loadIfNecessary(src);
        Location shiftLocation;
        if (shift.isConst()) {
            shiftLocation = Location::fromGPR(wasmScratchGPR);
            emitMoveConst(shift, shiftLocation);
        } else
            shiftLocation = loadIfNecessary(shift);
        consume(src);
        consume(shift);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        int32_t mask = (elementByteSize(info.lane) * CHAR_BIT) - 1;

        LOG_INSTRUCTION("Vector", op, src, srcLocation, shift, shiftLocation, RESULT(result));

#if CPU(ARM64)
        m_jit.and32(Imm32(mask), shiftLocation.asGPR(), wasmScratchGPR);
        if (op == SIMDLaneOperation::Shr) {
            // ARM64 doesn't have a version of this instruction for right shift. Instead, if the input to
            // left shift is negative, it's a right shift by the absolute value of that amount.
            m_jit.neg32(wasmScratchGPR);
        }
        m_jit.vectorSplatInt8(wasmScratchGPR, wasmScratchFPR);
        if (info.signMode == SIMDSignMode::Signed)
            m_jit.vectorSshl(info, srcLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
        else
            m_jit.vectorUshl(info, srcLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
        ASSERT(isX86());
        m_jit.move(shiftLocation.asGPR(), wasmScratchGPR);
        m_jit.and32(Imm32(mask), wasmScratchGPR);

        if (op == SIMDLaneOperation::Shr && info.signMode == SIMDSignMode::Signed && info.lane == SIMDLane::i64x2) {
            // x86 has no SIMD 64-bit signed right shift instruction, so we scalarize it here.
            m_jit.move(wasmScratchGPR, shiftRCX);
            m_jit.vectorExtractLaneInt64(TrustedImm32(0), srcLocation.asFPR(), scratches.gpr(0));
            m_jit.vectorExtractLaneInt64(TrustedImm32(1), srcLocation.asFPR(), scratches.gpr(1));
            m_jit.rshift64(shiftRCX, scratches.gpr(0));
            m_jit.rshift64(shiftRCX, scratches.gpr(1));
            m_jit.vectorSplatInt64(scratches.gpr(0), resultLocation.asFPR());
            m_jit.vectorReplaceLaneInt64(TrustedImm32(1), scratches.gpr(1), resultLocation.asFPR());
            return { };
        }

        // Unlike ARM, x86 expects the shift provided as a *scalar*, stored in the lower 64 bits of a vector register.
        // So, we don't need to splat the shift amount like we do on ARM.
        m_jit.move64ToDouble(wasmScratchGPR, wasmScratchFPR);

        // 8-bit shifts are pretty involved to implement on Intel, so they get their own instruction type with extra temps.
        if (op == SIMDLaneOperation::Shl && info.lane == SIMDLane::i8x16) {
            m_jit.vectorUshl8(srcLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR(), scratches.fpr(0), scratches.fpr(1));
            return { };
        }
        if (op == SIMDLaneOperation::Shr && info.lane == SIMDLane::i8x16) {
            if (info.signMode == SIMDSignMode::Signed)
                m_jit.vectorSshr8(srcLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR(), scratches.fpr(0), scratches.fpr(1));
            else
                m_jit.vectorUshr8(srcLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR(), scratches.fpr(0), scratches.fpr(1));
            return { };
        }

        if (op == SIMDLaneOperation::Shl)
            m_jit.vectorUshl(info, srcLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
        else if (info.signMode == SIMDSignMode::Signed)
            m_jit.vectorSshr(info, srcLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
        else
            m_jit.vectorUshr(info, srcLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#endif
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDExtmul(SIMDLaneOperation op, SIMDInfo info, ExpressionType left, ExpressionType right, ExpressionType& result)
    {
        ASSERT(info.signMode != SIMDSignMode::None);

        Location leftLocation = loadIfNecessary(left);
        Location rightLocation = loadIfNecessary(right);
        consume(left);
        consume(right);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("Vector", op, left, leftLocation, right, rightLocation, RESULT(result));

        ScratchScope<0, 1> scratches(*this, leftLocation, rightLocation, resultLocation);
        FPRReg scratchFPR = scratches.fpr(0);
        if (op == SIMDLaneOperation::ExtmulLow) {
            m_jit.vectorExtendLow(info, leftLocation.asFPR(), scratchFPR);
            m_jit.vectorExtendLow(info, rightLocation.asFPR(), resultLocation.asFPR());
        } else {
            ASSERT(op == SIMDLaneOperation::ExtmulHigh);
            m_jit.vectorExtendHigh(info, leftLocation.asFPR(), scratchFPR);
            m_jit.vectorExtendHigh(info, rightLocation.asFPR(), resultLocation.asFPR());
        }
        emitVectorMul(info, Location::fromFPR(scratchFPR), resultLocation, resultLocation);

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoadSplat(SIMDLaneOperation op, ExpressionType pointer, uint32_t uoffset, ExpressionType& result)
    {
        Width width;
        switch (op) {
        case SIMDLaneOperation::LoadSplat8:
            width = Width::Width8;
            break;
        case SIMDLaneOperation::LoadSplat16:
            width = Width::Width16;
            break;
        case SIMDLaneOperation::LoadSplat32:
            width = Width::Width32;
            break;
        case SIMDLaneOperation::LoadSplat64:
            width = Width::Width64;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        Location pointerLocation = emitCheckAndPreparePointer(pointer, uoffset, bytesForWidth(width));
        Address address = materializePointer(pointerLocation, uoffset);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("Vector", op, pointer, pointerLocation, uoffset, RESULT(result));

        switch (op) {
#if CPU(X86_64)
        case SIMDLaneOperation::LoadSplat8:
            m_jit.vectorLoad8Splat(address, resultLocation.asFPR(), wasmScratchFPR);
            break;
#else
        case SIMDLaneOperation::LoadSplat8:
            m_jit.vectorLoad8Splat(address, resultLocation.asFPR());
            break;
#endif
        case SIMDLaneOperation::LoadSplat16:
            m_jit.vectorLoad16Splat(address, resultLocation.asFPR());
            break;
        case SIMDLaneOperation::LoadSplat32:
            m_jit.vectorLoad32Splat(address, resultLocation.asFPR());
            break;
        case SIMDLaneOperation::LoadSplat64:
            m_jit.vectorLoad64Splat(address, resultLocation.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoadLane(SIMDLaneOperation op, ExpressionType pointer, ExpressionType vector, uint32_t uoffset, uint8_t lane, ExpressionType& result)
    {
        Width width;
        switch (op) {
        case SIMDLaneOperation::LoadLane8:
            width = Width::Width8;
            break;
        case SIMDLaneOperation::LoadLane16:
            width = Width::Width16;
            break;
        case SIMDLaneOperation::LoadLane32:
            width = Width::Width32;
            break;
        case SIMDLaneOperation::LoadLane64:
            width = Width::Width64;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        Location pointerLocation = emitCheckAndPreparePointer(pointer, uoffset, bytesForWidth(width));
        Address address = materializePointer(pointerLocation, uoffset);

        Location vectorLocation = loadIfNecessary(vector);
        consume(vector);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("Vector", op, pointer, pointerLocation, uoffset, RESULT(result));

        m_jit.moveVector(vectorLocation.asFPR(), resultLocation.asFPR());
        switch (op) {
        case SIMDLaneOperation::LoadLane8:
            m_jit.vectorLoad8Lane(address, TrustedImm32(lane), resultLocation.asFPR());
            break;
        case SIMDLaneOperation::LoadLane16:
            m_jit.vectorLoad16Lane(address, TrustedImm32(lane), resultLocation.asFPR());
            break;
        case SIMDLaneOperation::LoadLane32:
            m_jit.vectorLoad32Lane(address, TrustedImm32(lane), resultLocation.asFPR());
            break;
        case SIMDLaneOperation::LoadLane64:
            m_jit.vectorLoad64Lane(address, TrustedImm32(lane), resultLocation.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDStoreLane(SIMDLaneOperation op, ExpressionType pointer, ExpressionType vector, uint32_t uoffset, uint8_t lane)
    {
        Width width;
        switch (op) {
        case SIMDLaneOperation::StoreLane8:
            width = Width::Width8;
            break;
        case SIMDLaneOperation::StoreLane16:
            width = Width::Width16;
            break;
        case SIMDLaneOperation::StoreLane32:
            width = Width::Width32;
            break;
        case SIMDLaneOperation::StoreLane64:
            width = Width::Width64;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        Location pointerLocation = emitCheckAndPreparePointer(pointer, uoffset, bytesForWidth(width));
        Address address = materializePointer(pointerLocation, uoffset);

        Location vectorLocation = loadIfNecessary(vector);
        consume(vector);

        LOG_INSTRUCTION("Vector", op, vector, vectorLocation, pointer, pointerLocation, uoffset);

        switch (op) {
        case SIMDLaneOperation::StoreLane8:
            m_jit.vectorStore8Lane(vectorLocation.asFPR(), address, TrustedImm32(lane));
            break;
        case SIMDLaneOperation::StoreLane16:
            m_jit.vectorStore16Lane(vectorLocation.asFPR(), address, TrustedImm32(lane));
            break;
        case SIMDLaneOperation::StoreLane32:
            m_jit.vectorStore32Lane(vectorLocation.asFPR(), address, TrustedImm32(lane));
            break;
        case SIMDLaneOperation::StoreLane64:
            m_jit.vectorStore64Lane(vectorLocation.asFPR(), address, TrustedImm32(lane));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoadExtend(SIMDLaneOperation op, ExpressionType pointer, uint32_t uoffset, ExpressionType& result)
    {
        SIMDLane lane;
        SIMDSignMode signMode;

        switch (op) {
        case SIMDLaneOperation::LoadExtend8U:
            lane = SIMDLane::i16x8;
            signMode = SIMDSignMode::Unsigned;
            break;
        case SIMDLaneOperation::LoadExtend8S:
            lane = SIMDLane::i16x8;
            signMode = SIMDSignMode::Signed;
            break;
        case SIMDLaneOperation::LoadExtend16U:
            lane = SIMDLane::i32x4;
            signMode = SIMDSignMode::Unsigned;
            break;
        case SIMDLaneOperation::LoadExtend16S:
            lane = SIMDLane::i32x4;
            signMode = SIMDSignMode::Signed;
            break;
        case SIMDLaneOperation::LoadExtend32U:
            lane = SIMDLane::i64x2;
            signMode = SIMDSignMode::Unsigned;
            break;
        case SIMDLaneOperation::LoadExtend32S:
            lane = SIMDLane::i64x2;
            signMode = SIMDSignMode::Signed;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        result = emitCheckAndPrepareAndMaterializePointerApply(pointer, uoffset, sizeof(double), [&](auto location) -> Value {
            consume(pointer);
            Value result = topValue(TypeKind::V128);
            Location resultLocation = allocate(result);

            LOG_INSTRUCTION("Vector", op, pointer, uoffset, RESULT(result));

            m_jit.loadDouble(location, resultLocation.asFPR());
            m_jit.vectorExtendLow(SIMDInfo { lane, signMode }, resultLocation.asFPR(), resultLocation.asFPR());

            return result;
        });
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoadPad(SIMDLaneOperation op, ExpressionType pointer, uint32_t uoffset, ExpressionType& result)
    {
        result = emitCheckAndPrepareAndMaterializePointerApply(pointer, uoffset, op == SIMDLaneOperation::LoadPad32 ? sizeof(float) : sizeof(double), [&](auto location) -> Value {
            consume(pointer);
            Value result = topValue(TypeKind::V128);
            Location resultLocation = allocate(result);

            LOG_INSTRUCTION("Vector", op, pointer, uoffset, RESULT(result));

            if (op == SIMDLaneOperation::LoadPad32)
                m_jit.loadFloat(location, resultLocation.asFPR());
            else {
                ASSERT(op == SIMDLaneOperation::LoadPad64);
                m_jit.loadDouble(location, resultLocation.asFPR());
            }
            return result;
        });
        return { };
    }

    void BBQJIT::materializeVectorConstant(v128_t value, Location result)
    {
        if (!value.u64x2[0] && !value.u64x2[1])
            m_jit.moveZeroToVector(result.asFPR());
        else if (value.u64x2[0] == 0xffffffffffffffffull && value.u64x2[1] == 0xffffffffffffffffull)
#if CPU(X86_64)
            m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::Unsigned }, result.asFPR(), result.asFPR(), result.asFPR(), wasmScratchFPR);
#else
            m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::Unsigned }, result.asFPR(), result.asFPR(), result.asFPR());
#endif
        else
            m_jit.materializeVector(value, result.asFPR());
    }

    ExpressionType WARN_UNUSED_RETURN BBQJIT::addConstant(v128_t value)
    {
        // We currently don't track constant Values for V128s, since folding them seems like a lot of work that might not be worth it.
        // Maybe we can look into this eventually?
        Value temp = topValue(TypeKind::V128);
        Location tempLocation = allocate(temp);
        materializeVectorConstant(value, tempLocation);
        LOG_INSTRUCTION("V128Const", value, RESULT(temp));
        return temp;
    }

    // SIMD generated

    PartialResult WARN_UNUSED_RETURN BBQJIT::addExtractLane(SIMDInfo info, uint8_t lane, Value value, Value& result)
    {
        Location valueLocation = loadIfNecessary(value);
        consume(value);

        result = topValue(simdScalarType(info.lane).kind);
        Location resultLocation = allocate(result);
        LOG_INSTRUCTION("VectorExtractLane", info.lane, lane, value, valueLocation, RESULT(result));

        if (scalarTypeIsFloatingPoint(info.lane))
            m_jit.vectorExtractLane(info.lane, TrustedImm32(lane), valueLocation.asFPR(), resultLocation.asFPR());
        else
            m_jit.vectorExtractLane(info.lane, info.signMode, TrustedImm32(lane), valueLocation.asFPR(), resultLocation.asGPR());
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addReplaceLane(SIMDInfo info, uint8_t lane, ExpressionType vector, ExpressionType scalar, ExpressionType& result)
    {
        Location vectorLocation = loadIfNecessary(vector);
        Location scalarLocation;
        if (scalar.isConst()) {
            scalarLocation = scalar.isFloat() ? Location::fromFPR(wasmScratchFPR) : Location::fromGPR(wasmScratchGPR);
            emitMoveConst(scalar, scalarLocation);
        } else
            scalarLocation = loadIfNecessary(scalar);
        consume(vector);
        consume(scalar);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        if (scalarLocation == resultLocation) {
            m_jit.moveVector(scalarLocation.asFPR(), wasmScratchFPR);
            scalarLocation = Location::fromFPR(wasmScratchFPR);
        }

        LOG_INSTRUCTION("VectorReplaceLane", info.lane, lane, vector, vectorLocation, scalar, scalarLocation, RESULT(result));

        m_jit.moveVector(vectorLocation.asFPR(), resultLocation.asFPR());
        if (scalarLocation.isFPR())
            m_jit.vectorReplaceLane(info.lane, TrustedImm32(lane), scalarLocation.asFPR(), resultLocation.asFPR());
        else
            m_jit.vectorReplaceLane(info.lane, TrustedImm32(lane), scalarLocation.asGPR(), resultLocation.asFPR());
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDI_V(SIMDLaneOperation op, SIMDInfo info, ExpressionType value, ExpressionType& result)
    {
        Location valueLocation = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::I32);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("Vector", op, value, valueLocation, RESULT(result));

        switch (op) {
        case SIMDLaneOperation::Bitmask:
#if CPU(ARM64)
            if (info.lane == SIMDLane::i64x2) {
                // This might look bad, but remember: every bit of information we destroy contributes to the heat death of the universe.
                m_jit.vectorSshr8(SIMDInfo { SIMDLane::i64x2, SIMDSignMode::None }, valueLocation.asFPR(), TrustedImm32(63), wasmScratchFPR);
                m_jit.vectorUnzipEven(SIMDInfo { SIMDLane::i8x16, SIMDSignMode::None }, wasmScratchFPR, wasmScratchFPR, wasmScratchFPR);
                m_jit.moveDoubleTo64(wasmScratchFPR, wasmScratchGPR);
                m_jit.rshift64(wasmScratchGPR, TrustedImm32(31), wasmScratchGPR);
                m_jit.and32(Imm32(0b11), wasmScratchGPR, resultLocation.asGPR());
                return { };
            }

            {
                v128_t towerOfPower { };
                switch (info.lane) {
                case SIMDLane::i32x4:
                    for (unsigned i = 0; i < 4; ++i)
                        towerOfPower.u32x4[i] = 1 << i;
                    break;
                case SIMDLane::i16x8:
                    for (unsigned i = 0; i < 8; ++i)
                        towerOfPower.u16x8[i] = 1 << i;
                    break;
                case SIMDLane::i8x16:
                    for (unsigned i = 0; i < 8; ++i)
                        towerOfPower.u8x16[i] = 1 << i;
                    for (unsigned i = 0; i < 8; ++i)
                        towerOfPower.u8x16[i + 8] = 1 << i;
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                }

                // FIXME: this is bad, we should load
                materializeVectorConstant(towerOfPower, Location::fromFPR(wasmScratchFPR));
            }

            {
                ScratchScope<0, 1> scratches(*this, valueLocation, resultLocation);

                m_jit.vectorSshr8(info, valueLocation.asFPR(), TrustedImm32(elementByteSize(info.lane) * 8 - 1), scratches.fpr(0));
                m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, scratches.fpr(0), wasmScratchFPR, scratches.fpr(0));

                if (info.lane == SIMDLane::i8x16) {
                    m_jit.vectorExtractPair(SIMDInfo { SIMDLane::i8x16, SIMDSignMode::None }, TrustedImm32(8), scratches.fpr(0), scratches.fpr(0), wasmScratchFPR);
                    m_jit.vectorZipUpper(SIMDInfo { SIMDLane::i8x16, SIMDSignMode::None }, scratches.fpr(0), wasmScratchFPR, scratches.fpr(0));
                    info.lane = SIMDLane::i16x8;
                }

                m_jit.vectorHorizontalAdd(info, scratches.fpr(0), scratches.fpr(0));
                m_jit.moveFloatTo32(scratches.fpr(0), resultLocation.asGPR());
            }
#else
            ASSERT(isX86());
            m_jit.vectorBitmask(info, valueLocation.asFPR(), resultLocation.asGPR(), wasmScratchFPR);
#endif
            return { };
        case JSC::SIMDLaneOperation::AnyTrue:
#if CPU(ARM64)
            m_jit.vectorUnsignedMax(SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, valueLocation.asFPR(), wasmScratchFPR);
            m_jit.moveFloatTo32(wasmScratchFPR, resultLocation.asGPR());
            m_jit.test32(ResultCondition::NonZero, resultLocation.asGPR(), resultLocation.asGPR(), resultLocation.asGPR());
#else
            m_jit.vectorAnyTrue(valueLocation.asFPR(), resultLocation.asGPR());
#endif
            return { };
        case JSC::SIMDLaneOperation::AllTrue:
#if CPU(ARM64)
            ASSERT(scalarTypeIsIntegral(info.lane));
            switch (info.lane) {
            case SIMDLane::i64x2:
                m_jit.compareIntegerVectorWithZero(RelationalCondition::NotEqual, info, valueLocation.asFPR(), wasmScratchFPR);
                m_jit.vectorUnsignedMin(SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, wasmScratchFPR, wasmScratchFPR);
                break;
            case SIMDLane::i32x4:
            case SIMDLane::i16x8:
            case SIMDLane::i8x16:
                m_jit.vectorUnsignedMin(info, valueLocation.asFPR(), wasmScratchFPR);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }

            m_jit.moveFloatTo32(wasmScratchFPR, wasmScratchGPR);
            m_jit.test32(ResultCondition::NonZero, wasmScratchGPR, wasmScratchGPR, resultLocation.asGPR());
#else
            ASSERT(isX86());
            m_jit.vectorAllTrue(info, valueLocation.asFPR(), resultLocation.asGPR(), wasmScratchFPR);
#endif
            return { };
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return { };
        }
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDV_V(SIMDLaneOperation op, SIMDInfo info, ExpressionType value, ExpressionType& result)
    {
        Location valueLocation = loadIfNecessary(value);
        consume(value);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("Vector", op, value, valueLocation, RESULT(result));

        switch (op) {
        case JSC::SIMDLaneOperation::Demote:
            m_jit.vectorDemote(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::Promote:
            m_jit.vectorPromote(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::Abs:
#if CPU(X86_64)
            if (info.lane == SIMDLane::i64x2) {
                m_jit.vectorAbsInt64(valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
                return { };
            }
            if (scalarTypeIsFloatingPoint(info.lane)) {
                if (info.lane == SIMDLane::f32x4) {
                    m_jit.move32ToFloat(TrustedImm32(0x7fffffff), wasmScratchFPR);
                    m_jit.vectorSplatFloat32(wasmScratchFPR, wasmScratchFPR);
                } else {
                    m_jit.move64ToDouble(TrustedImm64(0x7fffffffffffffffll), wasmScratchFPR);
                    m_jit.vectorSplatFloat64(wasmScratchFPR, wasmScratchFPR);
                }
                m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, valueLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                return { };
            }
#endif
            m_jit.vectorAbs(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::Popcnt:
#if CPU(X86_64)
            {
                ScratchScope<0, 1> scratches(*this, valueLocation, resultLocation);
                ASSERT(info.lane == SIMDLane::i8x16);

                // x86_64 does not natively support vector lanewise popcount, so we emulate it using multiple
                // masks.

                v128_t bottomNibbleConst;
                v128_t popcntConst;
                bottomNibbleConst.u64x2[0] = 0x0f0f0f0f0f0f0f0f;
                bottomNibbleConst.u64x2[1] = 0x0f0f0f0f0f0f0f0f;
                popcntConst.u64x2[0] = 0x0302020102010100;
                popcntConst.u64x2[1] = 0x0403030203020201;

                materializeVectorConstant(bottomNibbleConst, Location::fromFPR(scratches.fpr(0)));
                m_jit.vectorAndnot(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, valueLocation.asFPR(), scratches.fpr(0), wasmScratchFPR);
                m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, valueLocation.asFPR(), scratches.fpr(0), resultLocation.asFPR());
                m_jit.vectorUshr8(SIMDInfo { SIMDLane::i16x8, SIMDSignMode::None }, wasmScratchFPR, TrustedImm32(4), wasmScratchFPR);

                materializeVectorConstant(popcntConst, Location::fromFPR(scratches.fpr(0)));
                m_jit.vectorSwizzle(scratches.fpr(0), resultLocation.asFPR(), resultLocation.asFPR());
                m_jit.vectorSwizzle(scratches.fpr(0), wasmScratchFPR, wasmScratchFPR);
                m_jit.vectorAdd(SIMDInfo { SIMDLane::i8x16, SIMDSignMode::None }, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
            }
#else
            m_jit.vectorPopcnt(info, valueLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case JSC::SIMDLaneOperation::Ceil:
            m_jit.vectorCeil(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::Floor:
            m_jit.vectorFloor(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::Trunc:
            m_jit.vectorTrunc(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::Nearest:
            m_jit.vectorNearest(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::Sqrt:
            m_jit.vectorSqrt(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::ExtaddPairwise:
#if CPU(X86_64)
            if (info.lane == SIMDLane::i16x8 && info.signMode == SIMDSignMode::Unsigned) {
                m_jit.vectorExtaddPairwiseUnsignedInt16(valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
                return { };
            }
            m_jit.vectorExtaddPairwise(info, valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchGPR, wasmScratchFPR);
#else
            m_jit.vectorExtaddPairwise(info, valueLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case JSC::SIMDLaneOperation::Convert:
#if CPU(X86_64)
            if (info.signMode == SIMDSignMode::Unsigned) {
                m_jit.vectorConvertUnsigned(valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
                return { };
            }
#endif
            m_jit.vectorConvert(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::ConvertLow:
#if CPU(X86_64)
            if (info.signMode == SIMDSignMode::Signed)
                m_jit.vectorConvertLowSignedInt32(valueLocation.asFPR(), resultLocation.asFPR());
            else
                m_jit.vectorConvertLowUnsignedInt32(valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchGPR, wasmScratchFPR);
#else
            m_jit.vectorConvertLow(info, valueLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case JSC::SIMDLaneOperation::ExtendHigh:
            m_jit.vectorExtendHigh(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::ExtendLow:
            m_jit.vectorExtendLow(info, valueLocation.asFPR(), resultLocation.asFPR());
            return { };
        case JSC::SIMDLaneOperation::TruncSat:
        case JSC::SIMDLaneOperation::RelaxedTruncSat:
#if CPU(X86_64)
            switch (info.lane) {
            case SIMDLane::f64x2:
                if (info.signMode == SIMDSignMode::Signed)
                    m_jit.vectorTruncSatSignedFloat64(valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchGPR, wasmScratchFPR);
                else
                    m_jit.vectorTruncSatUnsignedFloat64(valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchGPR, wasmScratchFPR);
                break;
            case SIMDLane::f32x4: {
                ScratchScope<0, 1> scratches(*this, valueLocation, resultLocation);
                if (info.signMode == SIMDSignMode::Signed)
                    m_jit.vectorTruncSat(info, valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchGPR, wasmScratchFPR, scratches.fpr(0));
                else
                    m_jit.vectorTruncSatUnsignedFloat32(valueLocation.asFPR(), resultLocation.asFPR(), wasmScratchGPR, wasmScratchFPR, scratches.fpr(0));
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
#else
            m_jit.vectorTruncSat(info, valueLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case JSC::SIMDLaneOperation::Not: {
#if CPU(X86_64)
            ScratchScope<0, 1> scratches(*this, valueLocation, resultLocation);
            m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, wasmScratchFPR, wasmScratchFPR, wasmScratchFPR, scratches.fpr(0));
            m_jit.vectorXor(info, valueLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
            m_jit.vectorNot(info, valueLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        }
        case JSC::SIMDLaneOperation::Neg:
#if CPU(X86_64)
            switch (info.lane) {
            case SIMDLane::i8x16:
            case SIMDLane::i16x8:
            case SIMDLane::i32x4:
            case SIMDLane::i64x2:
                // For integers, we can negate by subtracting our input from zero.
                m_jit.moveZeroToVector(wasmScratchFPR);
                m_jit.vectorSub(info, wasmScratchFPR, valueLocation.asFPR(), resultLocation.asFPR());
                break;
            case SIMDLane::f32x4:
                // For floats, we unfortunately have to flip the sign bit using XOR.
                m_jit.move32ToFloat(TrustedImm32(-0x80000000), wasmScratchFPR);
                m_jit.vectorSplatFloat32(wasmScratchFPR, wasmScratchFPR);
                m_jit.vectorXor(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, valueLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                break;
            case SIMDLane::f64x2:
                m_jit.move64ToDouble(TrustedImm64(-0x8000000000000000ll), wasmScratchFPR);
                m_jit.vectorSplatFloat64(wasmScratchFPR, wasmScratchFPR);
                m_jit.vectorXor(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, valueLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
#else
            m_jit.vectorNeg(info, valueLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return { };
        }
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDBitwiseSelect(ExpressionType left, ExpressionType right, ExpressionType selector, ExpressionType& result)
    {
        Location leftLocation = loadIfNecessary(left);
        Location rightLocation = loadIfNecessary(right);
        Location selectorLocation = loadIfNecessary(selector);
        consume(left);
        consume(right);
        consume(selector);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("VectorBitwiseSelect", left, leftLocation, right, rightLocation, selector, selectorLocation, RESULT(result));

#if CPU(X86_64)
        m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, leftLocation.asFPR(), selectorLocation.asFPR(), wasmScratchFPR);
        m_jit.vectorAndnot(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, rightLocation.asFPR(), selectorLocation.asFPR(), resultLocation.asFPR());
        m_jit.vectorOr(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
        m_jit.moveVector(selectorLocation.asFPR(), wasmScratchFPR);
        m_jit.vectorBitwiseSelect(leftLocation.asFPR(), rightLocation.asFPR(), wasmScratchFPR);
        m_jit.moveVector(wasmScratchFPR, resultLocation.asFPR());
#endif
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDRelOp(SIMDLaneOperation op, SIMDInfo info, ExpressionType left, ExpressionType right, B3::Air::Arg relOp, ExpressionType& result)
    {
        Location leftLocation = loadIfNecessary(left);
        Location rightLocation = loadIfNecessary(right);
        consume(left);
        consume(right);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("Vector", op, left, leftLocation, right, rightLocation, RESULT(result));

        if (scalarTypeIsFloatingPoint(info.lane)) {
            m_jit.compareFloatingPointVector(relOp.asDoubleCondition(), info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        }

#if CPU(X86_64)
        // On Intel, the best codegen for a bitwise-complement of an integer vector is to
        // XOR with a vector of all ones. This is necessary here since Intel also doesn't
        // directly implement most relational conditions between vectors: the cases below
        // are best emitted as inversions of conditions that are supported.
        switch (relOp.asRelationalCondition()) {
        case MacroAssembler::NotEqual: {
            ScratchScope<0, 1> scratches(*this, leftLocation, rightLocation, resultLocation);
            m_jit.compareIntegerVector(RelationalCondition::Equal, info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
            m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, wasmScratchFPR, wasmScratchFPR, wasmScratchFPR, scratches.fpr(0));
            m_jit.vectorXor(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
            break;
        }
        case MacroAssembler::Above: {
            ScratchScope<0, 1> scratches(*this, leftLocation, rightLocation, resultLocation);
            m_jit.compareIntegerVector(RelationalCondition::BelowOrEqual, info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
            m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, wasmScratchFPR, wasmScratchFPR, wasmScratchFPR, scratches.fpr(0));
            m_jit.vectorXor(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
            break;
        }
        case MacroAssembler::Below: {
            ScratchScope<0, 1> scratches(*this, leftLocation, rightLocation, resultLocation);
            m_jit.compareIntegerVector(RelationalCondition::AboveOrEqual, info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
            m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, wasmScratchFPR, wasmScratchFPR, wasmScratchFPR, scratches.fpr(0));
            m_jit.vectorXor(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
            break;
        }
        case MacroAssembler::GreaterThanOrEqual:
            if (info.lane == SIMDLane::i64x2) {
                // Note: rhs and lhs are reversed here, we are semantically negating LessThan. GreaterThan is
                // just better supported on AVX.
                ScratchScope<0, 1> scratches(*this, leftLocation, rightLocation, resultLocation);
                m_jit.compareIntegerVector(RelationalCondition::GreaterThan, info, rightLocation.asFPR(), leftLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
                m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, wasmScratchFPR, wasmScratchFPR, wasmScratchFPR, scratches.fpr(0));
                m_jit.vectorXor(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
            } else
                m_jit.compareIntegerVector(relOp.asRelationalCondition(), info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
            break;
        case MacroAssembler::LessThanOrEqual:
            if (info.lane == SIMDLane::i64x2) {
                ScratchScope<0, 1> scratches(*this, leftLocation, rightLocation, resultLocation);
                m_jit.compareIntegerVector(RelationalCondition::GreaterThan, info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
                m_jit.compareIntegerVector(RelationalCondition::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, wasmScratchFPR, wasmScratchFPR, wasmScratchFPR, scratches.fpr(0));
                m_jit.vectorXor(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
            } else
                m_jit.compareIntegerVector(relOp.asRelationalCondition(), info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
            break;
        default:
            m_jit.compareIntegerVector(relOp.asRelationalCondition(), info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
        }
#else
        m_jit.compareIntegerVector(relOp.asRelationalCondition(), info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#endif
        return { };
    }

    void BBQJIT::emitVectorMul(SIMDInfo info, Location left, Location right, Location result)
    {
        if (info.lane == SIMDLane::i64x2) {
            // Multiplication of 64-bit ints isn't natively supported on ARM or Intel (at least the ones we're targeting)
            // so we scalarize it instead.
            ScratchScope<1, 0> scratches(*this);
            GPRReg dataScratchGPR = scratches.gpr(0);
            m_jit.vectorExtractLaneInt64(TrustedImm32(0), left.asFPR(), wasmScratchGPR);
            m_jit.vectorExtractLaneInt64(TrustedImm32(0), right.asFPR(), dataScratchGPR);
            m_jit.mul64(wasmScratchGPR, dataScratchGPR, wasmScratchGPR);
            m_jit.vectorSplatInt64(wasmScratchGPR, wasmScratchFPR);
            m_jit.vectorExtractLaneInt64(TrustedImm32(1), left.asFPR(), wasmScratchGPR);
            m_jit.vectorExtractLaneInt64(TrustedImm32(1), right.asFPR(), dataScratchGPR);
            m_jit.mul64(wasmScratchGPR, dataScratchGPR, wasmScratchGPR);
            m_jit.vectorReplaceLaneInt64(TrustedImm32(1), wasmScratchGPR, wasmScratchFPR);
            m_jit.moveVector(wasmScratchFPR, result.asFPR());
        } else
            m_jit.vectorMul(info, left.asFPR(), right.asFPR(), result.asFPR());
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::fixupOutOfBoundsIndicesForSwizzle(Location a, Location b, Location result)
    {
        ASSERT(isX86());
        // Let each byte mask be 112 (0x70) then after VectorAddSat
        // each index > 15 would set the saturated index's bit 7 to 1,
        // whose corresponding byte will be zero cleared in VectorSwizzle.
        // https://github.com/WebAssembly/simd/issues/93
        v128_t mask;
        mask.u64x2[0] = 0x7070707070707070;
        mask.u64x2[1] = 0x7070707070707070;
        materializeVectorConstant(mask, Location::fromFPR(wasmScratchFPR));
        m_jit.vectorAddSat(SIMDInfo { SIMDLane::i8x16, SIMDSignMode::Unsigned }, wasmScratchFPR, b.asFPR(), wasmScratchFPR);
        m_jit.vectorSwizzle(a.asFPR(), wasmScratchFPR, result.asFPR());
        return { };
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDV_VV(SIMDLaneOperation op, SIMDInfo info, ExpressionType left, ExpressionType right, ExpressionType& result)
    {
        Location leftLocation = loadIfNecessary(left);
        Location rightLocation = loadIfNecessary(right);
        consume(left);
        consume(right);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("Vector", op, left, leftLocation, right, rightLocation, RESULT(result));

        switch (op) {
        case SIMDLaneOperation::And:
            m_jit.vectorAnd(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::Andnot:
            m_jit.vectorAndnot(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::AvgRound:
            m_jit.vectorAvgRound(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::DotProduct:
#if CPU(ARM64)
            m_jit.vectorDotProduct(leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
#else
            m_jit.vectorDotProduct(leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case SIMDLaneOperation::Add:
            m_jit.vectorAdd(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::Mul:
            emitVectorMul(info, leftLocation, rightLocation, resultLocation);
            return { };
        case SIMDLaneOperation::MulSat:
#if CPU(X86_64)
            m_jit.vectorMulSat(leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchGPR, wasmScratchFPR);
#else
            m_jit.vectorMulSat(leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case SIMDLaneOperation::Sub:
            m_jit.vectorSub(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::Div:
            m_jit.vectorDiv(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::Pmax:
#if CPU(ARM64)
            m_jit.vectorPmax(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
#else
            m_jit.vectorPmax(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case SIMDLaneOperation::Pmin:
#if CPU(ARM64)
            m_jit.vectorPmin(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
#else
            m_jit.vectorPmin(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case SIMDLaneOperation::Or:
            m_jit.vectorOr(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::Swizzle:
            if constexpr (isX86())
                return fixupOutOfBoundsIndicesForSwizzle(leftLocation, rightLocation, resultLocation);
            m_jit.vectorSwizzle(leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::RelaxedSwizzle:
            m_jit.vectorSwizzle(leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::Xor:
            m_jit.vectorXor(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::Narrow:
            m_jit.vectorNarrow(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
            return { };
        case SIMDLaneOperation::AddSat:
            m_jit.vectorAddSat(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::SubSat:
            m_jit.vectorSubSat(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
            return { };
        case SIMDLaneOperation::Max:
#if CPU(X86_64)
            if (scalarTypeIsFloatingPoint(info.lane)) {
                // Intel's vectorized maximum instruction has slightly different semantics to the WebAssembly vectorized
                // minimum instruction, namely in terms of signed zero values and propagating NaNs. VectorPmax implements
                // a fast version of this instruction that compiles down to a single op, without conforming to the exact
                // semantics. In order to precisely implement VectorMax, we need to do extra work on Intel to check for
                // the necessary edge cases.

                // Compute result in both directions.
                m_jit.vectorPmax(info, rightLocation.asFPR(), leftLocation.asFPR(), wasmScratchFPR);
                m_jit.vectorPmax(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());

                // Check for discrepancies by XORing the two results together.
                m_jit.vectorXor(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, wasmScratchFPR, resultLocation.asFPR(), resultLocation.asFPR());

                // OR results, propagating the sign bit for negative zeroes, and NaNs.
                m_jit.vectorOr(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, wasmScratchFPR, resultLocation.asFPR(), wasmScratchFPR);

                // Propagate discrepancies in the sign bit.
                m_jit.vectorSub(info, wasmScratchFPR, resultLocation.asFPR(), wasmScratchFPR);

                // Canonicalize NaNs by checking for unordered values and clearing payload if necessary.
                m_jit.compareFloatingPointVectorUnordered(info, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                m_jit.vectorUshr8(SIMDInfo { info.lane == SIMDLane::f32x4 ? SIMDLane::i32x4 : SIMDLane::i64x2, SIMDSignMode::None }, resultLocation.asFPR(), TrustedImm32(info.lane == SIMDLane::f32x4 ? 10 : 13), resultLocation.asFPR());
                m_jit.vectorAndnot(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, wasmScratchFPR, resultLocation.asFPR(), resultLocation.asFPR());
            } else
                m_jit.vectorMax(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#else
            m_jit.vectorMax(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        case SIMDLaneOperation::Min:
#if CPU(X86_64)
            if (scalarTypeIsFloatingPoint(info.lane)) {
                // Intel's vectorized minimum instruction has slightly different semantics to the WebAssembly vectorized
                // minimum instruction, namely in terms of signed zero values and propagating NaNs. VectorPmin implements
                // a fast version of this instruction that compiles down to a single op, without conforming to the exact
                // semantics. In order to precisely implement VectorMin, we need to do extra work on Intel to check for
                // the necessary edge cases.

                // Compute result in both directions.
                m_jit.vectorPmin(info, rightLocation.asFPR(), leftLocation.asFPR(), wasmScratchFPR);
                m_jit.vectorPmin(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());

                // OR results, propagating the sign bit for negative zeroes, and NaNs.
                m_jit.vectorOr(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, wasmScratchFPR, resultLocation.asFPR(), wasmScratchFPR);

                // Canonicalize NaNs by checking for unordered values and clearing payload if necessary.
                m_jit.compareFloatingPointVectorUnordered(info, resultLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
                m_jit.vectorOr(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, wasmScratchFPR, resultLocation.asFPR(), wasmScratchFPR);
                m_jit.vectorUshr8(SIMDInfo { info.lane == SIMDLane::f32x4 ? SIMDLane::i32x4 : SIMDLane::i64x2, SIMDSignMode::None }, resultLocation.asFPR(), TrustedImm32(info.lane == SIMDLane::f32x4 ? 10 : 13), resultLocation.asFPR());
                m_jit.vectorAndnot(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, wasmScratchFPR, resultLocation.asFPR(), resultLocation.asFPR());
            } else
                m_jit.vectorMin(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#else
            m_jit.vectorMin(info, leftLocation.asFPR(), rightLocation.asFPR(), resultLocation.asFPR());
#endif
            return { };
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return { };
        }
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDRelaxedFMA(SIMDLaneOperation op, SIMDInfo info, ExpressionType mul1, ExpressionType mul2, ExpressionType addend, ExpressionType& result)
    {
        Location mul1Location = loadIfNecessary(mul1);
        Location mul2Location = loadIfNecessary(mul2);
        Location addendLocation = loadIfNecessary(addend);
        consume(mul1);
        consume(mul2);
        consume(addend);

        result = topValue(TypeKind::V128);
        Location resultLocation = allocate(result);

        LOG_INSTRUCTION("VectorRelaxedMAdd", mul1, mul1Location, mul2, mul2Location, addend, addendLocation, RESULT(result));

        if (op == SIMDLaneOperation::RelaxedMAdd) {
#if CPU(X86_64)
            m_jit.vectorMul(info, mul1Location.asFPR(), mul2Location.asFPR(), wasmScratchFPR);
            m_jit.vectorAdd(info, wasmScratchFPR, addendLocation.asFPR(), resultLocation.asFPR());
#else
            m_jit.vectorFusedMulAdd(info, mul1Location.asFPR(), mul2Location.asFPR(), addendLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
#endif
        } else if (op == SIMDLaneOperation::RelaxedNMAdd) {
#if CPU(X86_64)
            m_jit.vectorMul(info, mul1Location.asFPR(), mul2Location.asFPR(), wasmScratchFPR);
            m_jit.vectorSub(info, addendLocation.asFPR(), wasmScratchFPR, resultLocation.asFPR());
#else
            m_jit.vectorFusedNegMulAdd(info, mul1Location.asFPR(), mul2Location.asFPR(), addendLocation.asFPR(), resultLocation.asFPR(), wasmScratchFPR);
#endif
        } else
            RELEASE_ASSERT_NOT_REACHED();
        return { };
    }

#else // USE(JSVALUE64) elif USE(JSVALUE32_64)

    void BBQJIT::notifyFunctionUsesSIMD()
    {
        m_usesSIMD = false;
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoad(ExpressionType, uint32_t, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDStore(ExpressionType, ExpressionType, uint32_t)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDSplat(SIMDLane, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDShuffle(v128_t, ExpressionType, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDShift(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDExtmul(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoadSplat(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoadLane(SIMDLaneOperation, ExpressionType, ExpressionType, uint32_t, uint8_t, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDStoreLane(SIMDLaneOperation, ExpressionType, ExpressionType, uint32_t, uint8_t)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoadExtend(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDLoadPad(SIMDLaneOperation, ExpressionType, uint32_t, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    void BBQJIT::materializeVectorConstant(v128_t, Location)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    ExpressionType BBQJIT::addConstant(v128_t)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addExtractLane(SIMDInfo, uint8_t, Value, Value&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addReplaceLane(SIMDInfo, uint8_t, ExpressionType, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDI_V(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDV_V(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDBitwiseSelect(ExpressionType, ExpressionType, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDRelOp(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, B3::Air::Arg, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDV_VV(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

    PartialResult WARN_UNUSED_RETURN BBQJIT::addSIMDRelaxedFMA(SIMDLaneOperation, SIMDInfo, ExpressionType, ExpressionType, ExpressionType, ExpressionType&)
    {
        UNREACHABLE_FOR_PLATFORM();
    }

#endif // USE(JSVALUE32_64)

    void BBQJIT::dump(const ControlStack&, const Stack*) { }
    void BBQJIT::didFinishParsingLocals() { }
    void BBQJIT::didPopValueFromStack(ExpressionType, String) { }

    void BBQJIT::finalize()
    {
        if (UNLIKELY(m_disassembler))
            m_disassembler->setEndOfCode(m_jit.label());
    }

    Vector<UnlinkedHandlerInfo>&& BBQJIT::takeExceptionHandlers()
    {
        return WTFMove(m_exceptionHandlers);
    }

    Vector<CCallHelpers::Label>&& BBQJIT::takeCatchEntrypoints()
    {
        return WTFMove(m_catchEntrypoints);
    }

    Box<PCToCodeOriginMapBuilder> BBQJIT::takePCToCodeOriginMapBuilder()
    {
        if (m_pcToCodeOriginMapBuilder.didBuildMapping())
            return Box<PCToCodeOriginMapBuilder>::create(WTFMove(m_pcToCodeOriginMapBuilder));
        return nullptr;
    }

    std::unique_ptr<BBQDisassembler> BBQJIT::takeDisassembler()
    {
        return WTFMove(m_disassembler);
    }

    bool BBQJIT::isScratch(Location loc)
    {
        return (loc.isGPR() && loc.asGPR() == wasmScratchGPR) || (loc.isFPR() && loc.asFPR() == wasmScratchFPR);
    }

#if USE(JSVALUE64)
    void BBQJIT::emitStoreConst(Value constant, Location loc)
    {
        LOG_INSTRUCTION("Store", constant, RESULT(loc));

        ASSERT(constant.isConst());
        ASSERT(loc.isMemory());

        switch (constant.type()) {
        case TypeKind::I32:
        case TypeKind::F32:
            m_jit.store32(Imm32(constant.asI32()), loc.asAddress());
            break;
        case TypeKind::Ref:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::RefNull:
        case TypeKind::Externref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.store64(TrustedImm64(constant.asRef()), loc.asAddress());
            break;
        case TypeKind::I64:
        case TypeKind::F64:
            m_jit.store64(Imm64(constant.asI64()), loc.asAddress());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented constant typekind.");
            break;
        }
    }
#else
    void BBQJIT::emitStoreConst(Value constant, Location loc)
    {
        LOG_INSTRUCTION("Store", constant, RESULT(loc));

        ASSERT(constant.isConst());
        ASSERT(loc.isMemory());

        switch (constant.type()) {
        case TypeKind::I32:
        case TypeKind::F32:
            m_jit.store32(Imm32(constant.asI32()), loc.asAddress());
            break;
        case TypeKind::Ref:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::RefNull:
        case TypeKind::Externref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.storePair32(TrustedImm32(constant.asI64lo()), TrustedImm32(constant.asI64hi()), loc.asAddress());
            break;
        case TypeKind::I64:
        case TypeKind::F64:
            m_jit.storePair32(TrustedImm32(constant.asI64lo()), TrustedImm32(constant.asI64hi()), loc.asAddress());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented constant typekind.");
            break;
        }
    }
#endif

#if USE(JSVALUE64)
    void BBQJIT::emitMoveConst(Value constant, Location loc)
    {
        ASSERT(constant.isConst());

        if (loc.isMemory())
            return emitStoreConst(constant, loc);

        ASSERT(loc.isRegister());
        ASSERT(loc.isFPR() == constant.isFloat());

        if (!isScratch(loc))
            LOG_INSTRUCTION("Move", constant, RESULT(loc));

        switch (constant.type()) {
        case TypeKind::I32:
            m_jit.move(Imm32(constant.asI32()), loc.asGPR());
            break;
        case TypeKind::I64:
            m_jit.move(Imm64(constant.asI64()), loc.asGPR());
            break;
        case TypeKind::Ref:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::RefNull:
        case TypeKind::Externref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.move(TrustedImm64(constant.asRef()), loc.asGPR());
            break;
        case TypeKind::F32:
            m_jit.moveFloat(Imm32(constant.asI32()), loc.asFPR());
            break;
        case TypeKind::F64:
            m_jit.moveDouble(Imm64(constant.asI64()), loc.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented constant typekind.");
            break;
        }
    }
#else
    void BBQJIT::emitMoveConst(Value constant, Location loc)
    {
        ASSERT(constant.isConst());

        if (loc.isMemory())
            return emitStoreConst(constant, loc);

        ASSERT(loc.isRegister());
        ASSERT(loc.isFPR() == constant.isFloat());

        if (!isScratch(loc))
            LOG_INSTRUCTION("Move", constant, RESULT(loc));

        switch (constant.type()) {
        case TypeKind::I32:
            m_jit.move(Imm32(constant.asI32()), loc.asGPR());
            break;
        case TypeKind::I64:
            m_jit.move(Imm32(constant.asI64hi()), loc.asGPRhi());
            m_jit.move(Imm32(constant.asI64lo()), loc.asGPRlo());
            break;
        case TypeKind::Ref:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::RefNull:
        case TypeKind::Externref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.move(Imm32(constant.asI64hi()), loc.asGPRhi());
            m_jit.move(Imm32(constant.asI64lo()), loc.asGPRlo());
            break;
        case TypeKind::F32:
            m_jit.move(Imm32(constant.asI32()), wasmScratchGPR);
            m_jit.move32ToFloat(wasmScratchGPR, loc.asFPR());
            break;
        case TypeKind::F64:
            m_jit.move(Imm32(constant.asI64hi()), wasmScratchGPR2);
            m_jit.move(Imm32(constant.asI64lo()), wasmScratchGPR);
            m_jit.move64ToDouble(wasmScratchGPR2, wasmScratchGPR, loc.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented constant typekind.");
            break;
        }
    }
#endif

#if USE(JSVALUE64)
    void BBQJIT::emitStore(TypeKind type, Location src, Location dst)
    {
        ASSERT(dst.isMemory());
        ASSERT(src.isRegister());

        switch (type) {
        case TypeKind::I32:
        case TypeKind::I31ref:
            m_jit.store32(src.asGPR(), dst.asAddress());
            break;
        case TypeKind::I64:
            m_jit.store64(src.asGPR(), dst.asAddress());
            break;
        case TypeKind::F32:
            m_jit.storeFloat(src.asFPR(), dst.asAddress());
            break;
        case TypeKind::F64:
            m_jit.storeDouble(src.asFPR(), dst.asAddress());
            break;
        case TypeKind::Externref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.store64(src.asGPR(), dst.asAddress());
            break;
        case TypeKind::V128:
            m_jit.storeVector(src.asFPR(), dst.asAddress());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented type kind store.");
        }
    }
#else
    void BBQJIT::emitStore(TypeKind type, Location src, Location dst)
    {
        ASSERT(dst.isMemory());
        ASSERT(src.isRegister());

        switch (type) {
        case TypeKind::I32:
            m_jit.store32(src.asGPR(), dst.asAddress());
            break;
        case TypeKind::I31ref:
            m_jit.store32(src.asGPRlo(), dst.asAddress());
            m_jit.store32(TrustedImm32(JSValue::Int32Tag), dst.asAddress().withOffset(4));
            break;
        case TypeKind::I64:
            m_jit.storePair32(src.asGPRlo(), src.asGPRhi(), dst.asAddress());
            break;
        case TypeKind::F32:
            m_jit.storeFloat(src.asFPR(), dst.asAddress());
            break;
        case TypeKind::F64:
            m_jit.storeDouble(src.asFPR(), dst.asAddress());
            break;
        case TypeKind::Externref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.storePair32(src.asGPRlo(), src.asGPRhi(), dst.asAddress());
            break;
        case TypeKind::V128:
            m_jit.storeVector(src.asFPR(), dst.asAddress());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented type kind store.");
        }
    }
#endif

    void BBQJIT::emitStore(Value src, Location dst)
    {
        if (src.isConst())
            return emitStoreConst(src, dst);

        LOG_INSTRUCTION("Store", src, RESULT(dst));
        emitStore(src.type(), locationOf(src), dst);
    }

#if USE(JSVALUE64)
    void BBQJIT::emitMoveMemory(TypeKind type, Location src, Location dst)
    {
        ASSERT(dst.isMemory());
        ASSERT(src.isMemory());

        if (src == dst)
            return;

        switch (type) {
        case TypeKind::I32:
        case TypeKind::I31ref:
        case TypeKind::F32:
            m_jit.transfer32(src.asAddress(), dst.asAddress());
            break;
        case TypeKind::I64:
            m_jit.transfer64(src.asAddress(), dst.asAddress());
            break;
        case TypeKind::F64:
            m_jit.loadDouble(src.asAddress(), wasmScratchFPR);
            m_jit.storeDouble(wasmScratchFPR, dst.asAddress());
            break;
        case TypeKind::Externref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Funcref:
        case TypeKind::Structref:
        case TypeKind::Arrayref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.transfer64(src.asAddress(), dst.asAddress());
            break;
        case TypeKind::V128: {
            Address srcAddress = src.asAddress();
            Address dstAddress = dst.asAddress();
            m_jit.loadVector(srcAddress, wasmScratchFPR);
            m_jit.storeVector(wasmScratchFPR, dstAddress);
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented type kind move.");
        }
    }
#else
    void BBQJIT::emitMoveMemory(TypeKind type, Location src, Location dst)
    {
        ASSERT(dst.isMemory());
        ASSERT(src.isMemory());

        if (src == dst)
            return;

        switch (type) {
        case TypeKind::I32:
            m_jit.transfer32(src.asAddress(), dst.asAddress());
            break;
        case TypeKind::I31ref:
            m_jit.transfer32(src.asAddress(), dst.asAddress());
            m_jit.store32(TrustedImm32(JSValue::Int32Tag), dst.asAddress().withOffset(4));
            break;
        case TypeKind::F32:
            m_jit.transfer32(src.asAddress(), dst.asAddress());
            break;
        case TypeKind::I64:
            m_jit.transfer32(src.asAddress().withOffset(0), dst.asAddress().withOffset(0));
            m_jit.transfer32(src.asAddress().withOffset(4), dst.asAddress().withOffset(4));
            break;
        case TypeKind::F64:
            m_jit.loadDouble(src.asAddress(), wasmScratchFPR);
            m_jit.storeDouble(wasmScratchFPR, dst.asAddress());
            break;
        case TypeKind::Externref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Funcref:
        case TypeKind::Structref:
        case TypeKind::Arrayref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.transfer32(src.asAddress().withOffset(0), dst.asAddress().withOffset(0));
            m_jit.transfer32(src.asAddress().withOffset(4), dst.asAddress().withOffset(4));
            break;
        case TypeKind::V128: {
            Address srcAddress = src.asAddress();
            Address dstAddress = dst.asAddress();
            m_jit.loadVector(srcAddress, wasmScratchFPR);
            m_jit.storeVector(wasmScratchFPR, dstAddress);
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented type kind move.");
        }
    }
#endif

    void BBQJIT::emitMoveMemory(Value src, Location dst)
    {
        LOG_INSTRUCTION("Move", src, RESULT(dst));
        emitMoveMemory(src.type(), locationOf(src), dst);
    }

#if USE(JSVALUE64)
    void BBQJIT::emitMoveRegister(TypeKind type, Location src, Location dst)
    {
        ASSERT(dst.isRegister());
        ASSERT(src.isRegister());

        if (src == dst)
            return;

        switch (type) {
        case TypeKind::I32:
        case TypeKind::I31ref:
        case TypeKind::I64:
        case TypeKind::Externref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.move(src.asGPR(), dst.asGPR());
            break;
        case TypeKind::F32:
        case TypeKind::F64:
            m_jit.moveDouble(src.asFPR(), dst.asFPR());
            break;
        case TypeKind::V128:
            m_jit.moveVector(src.asFPR(), dst.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented type kind move.");
        }
    }
#else
    void BBQJIT::emitMoveRegister(TypeKind type, Location src, Location dst)
    {
        ASSERT(dst.isRegister());
        ASSERT(src.isRegister());

        if (src == dst)
            return;

        switch (type) {
        case TypeKind::I32:
            m_jit.move(src.asGPR(), dst.asGPR());
            break;
        case TypeKind::I31ref:
        case TypeKind::I64:
        case TypeKind::Externref:
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            ASSERT(dst.asGPRlo() != src.asGPRhi());
            m_jit.move(src.asGPRlo(), dst.asGPRlo());
            m_jit.move(src.asGPRhi(), dst.asGPRhi());
            break;
        case TypeKind::F32:
        case TypeKind::F64:
            m_jit.moveDouble(src.asFPR(), dst.asFPR());
            break;
        case TypeKind::V128:
            m_jit.moveVector(src.asFPR(), dst.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented type kind move.");
        }
    }
#endif

    void BBQJIT::emitMoveRegister(Value src, Location dst)
    {
        if (!isScratch(locationOf(src)) && !isScratch(dst))
            LOG_INSTRUCTION("Move", src, RESULT(dst));

        emitMoveRegister(src.type(), locationOf(src), dst);
    }

#if USE(JSVALUE64)
    void BBQJIT::emitLoad(TypeKind type, Location src, Location dst)
    {
        ASSERT(dst.isRegister());
        ASSERT(src.isMemory());

        switch (type) {
        case TypeKind::I32:
        case TypeKind::I31ref:
            m_jit.load32(src.asAddress(), dst.asGPR());
            break;
        case TypeKind::I64:
            m_jit.load64(src.asAddress(), dst.asGPR());
            break;
        case TypeKind::F32:
            m_jit.loadFloat(src.asAddress(), dst.asFPR());
            break;
        case TypeKind::F64:
            m_jit.loadDouble(src.asAddress(), dst.asFPR());
            break;
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Externref:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.load64(src.asAddress(), dst.asGPR());
            break;
        case TypeKind::V128:
            m_jit.loadVector(src.asAddress(), dst.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented type kind load.");
        }
    }
#else
    void BBQJIT::emitLoad(TypeKind type, Location src, Location dst)
    {
        ASSERT(dst.isRegister());
        ASSERT(src.isMemory());

        switch (type) {
        case TypeKind::I32:
            m_jit.load32(src.asAddress(), dst.asGPR());
            break;
        case TypeKind::I31ref:
        case TypeKind::I64:
            m_jit.loadPair32(src.asAddress(), dst.asGPRlo(), dst.asGPRhi());
            break;
        case TypeKind::F32:
            m_jit.loadFloat(src.asAddress(), dst.asFPR());
            break;
        case TypeKind::F64:
            m_jit.loadDouble(src.asAddress(), dst.asFPR());
            break;
        case TypeKind::Ref:
        case TypeKind::RefNull:
        case TypeKind::Externref:
        case TypeKind::Funcref:
        case TypeKind::Arrayref:
        case TypeKind::Structref:
        case TypeKind::Eqref:
        case TypeKind::Anyref:
        case TypeKind::Nullref:
        case TypeKind::Nullfuncref:
        case TypeKind::Nullexternref:
            m_jit.loadPair32(src.asAddress(), dst.asGPRlo(), dst.asGPRhi());
            break;
        case TypeKind::V128:
            m_jit.loadVector(src.asAddress(), dst.asFPR());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Unimplemented type kind load.");
        }
    }
#endif

    void BBQJIT::emitLoad(Value src, Location dst)
    {
        if (!isScratch(dst))
            LOG_INSTRUCTION("Load", src, RESULT(dst));

        emitLoad(src.type(), locationOf(src), dst);
    }

    void BBQJIT::emitMove(TypeKind type, Location src, Location dst)
    {
        if (src.isRegister()) {
            if (dst.isMemory())
                emitStore(type, src, dst);
            else
                emitMoveRegister(type, src, dst);
        } else {
            if (dst.isMemory())
                emitMoveMemory(type, src, dst);
            else
                emitLoad(type, src, dst);
        }
    }

    void BBQJIT::emitMove(Value src, Location dst)
    {
        if (src.isConst()) {
            if (dst.isMemory())
                emitStoreConst(src, dst);
            else
                emitMoveConst(src, dst);
        } else {
            Location srcLocation = locationOf(src);
            emitMove(src.type(), srcLocation, dst);
        }
    }

#if USE(JSVALUE64)
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
            if (locationOf(srcVector[i]) == dst) {
                switch (statusVector[i]) {
                case ShuffleStatus::ToMove:
                    emitShuffleMove(srcVector, dstVector, statusVector, i);
                    break;
                case ShuffleStatus::BeingMoved: {
                    Location temp = srcVector[i].isFloat() ? Location::fromFPR(wasmScratchFPR) : Location::fromGPR(wasmScratchGPR);
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
#else
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
#endif

    template<size_t N, typename OverflowHandler>
    void BBQJIT::emitShuffle(Vector<Value, N, OverflowHandler>& srcVector, Vector<Location, N, OverflowHandler>& dstVector)
    {
        ASSERT(srcVector.size() == dstVector.size());

        if (srcVector.size() == 1) {
            emitMove(srcVector[0], dstVector[0]);
            return;
        }

        // For multi-value return, a parallel move might be necessary. This is comparatively complex
        // and slow, so we limit it to this slow path.
        Vector<ShuffleStatus, N, OverflowHandler> statusVector(srcVector.size(), ShuffleStatus::ToMove);
        for (unsigned i = 0; i < srcVector.size(); i ++) {
            if (statusVector[i] == ShuffleStatus::ToMove)
                emitShuffleMove(srcVector, dstVector, statusVector, i);
        }
    }

    ControlData& BBQJIT::currentControlData()
    {
        return m_parser->controlStack().last().controlData;
    }

#if USE(JSVALUE64)
    void BBQJIT::setLRUKey(Location location, LocalOrTempIndex key)
    {
        if (location.isGPR())
            m_gprLRU.increaseKey(location.asGPR(), key);
        else if (location.isFPR())
            m_fprLRU.increaseKey(location.asFPR(), key);
    }
#else
    void BBQJIT::setLRUKey(Location location, LocalOrTempIndex key)
    {
        if (location.isGPR()) {
            m_gprLRU.increaseKey(location.asGPR(), key);
        } else if (location.isFPR()) {
            m_fprLRU.increaseKey(location.asFPR(), key);
        } else if (location.isGPR2()) {
            m_gprLRU.increaseKey(location.asGPRhi(), key);
            m_gprLRU.increaseKey(location.asGPRlo(), key);
        }
    }
#endif

    void BBQJIT::increaseKey(Location location)
    {
        setLRUKey(location, m_lastUseTimestamp ++);
    }

    Location BBQJIT::bind(Value value)
    {
        if (value.isPinned())
            return value.asPinned();

        Location reg = allocateRegister(value);
        increaseKey(reg);
        return bind(value, reg);
    }

    Location BBQJIT::allocate(Value value)
    {
        return BBQJIT::allocateWithHint(value, Location::none());
    }

    Location BBQJIT::allocateWithHint(Value value, Location hint)
    {
        if (value.isPinned())
            return value.asPinned();

        // It's possible, in some circumstances, that the value in question was already assigned to a register.
        // The most common example of this is returned values (that use registers from the calling convention) or
        // values passed between control blocks. In these cases, we just leave it in its register unmoved.
        Location existingLocation = locationOf(value);
        if (existingLocation.isRegister()) {
            ASSERT(value.isFloat() == existingLocation.isFPR());
            return existingLocation;
        }

        Location reg = hint;
        if (reg.kind() == Location::None
            || value.isFloat() != reg.isFPR()
            || (reg.isGPR() && !m_gprSet.contains(reg.asGPR(), IgnoreVectors))
#if USE(JSVALUE32_64)
            || (reg.isGPR2() && !typeNeedsGPR2(value.type()))
            || (reg.isGPR() && typeNeedsGPR2(value.type()))
#endif
            || (reg.isFPR() && !m_fprSet.contains(reg.asFPR(), Width::Width128)))
            reg = allocateRegister(value);
        increaseKey(reg);
        if (value.isLocal())
            currentControlData().touch(value.asLocal());
        if (UNLIKELY(Options::verboseBBQJITAllocation()))
            dataLogLn("BBQ\tAllocated ", value, " with type ", makeString(value.type()), " to ", reg);
        return bind(value, reg);
    }

    Location BBQJIT::locationOfWithoutBinding(Value value)
    {
        // Used internally in bind() to avoid infinite recursion.
        if (value.isPinned())
            return value.asPinned();
        if (value.isLocal()) {
            ASSERT(value.asLocal() < m_locals.size());
            return m_locals[value.asLocal()];
        }
        if (value.isTemp()) {
            if (value.asTemp() >= m_temps.size())
                return Location::none();
            return m_temps[value.asTemp()];
        }
        return Location::none();
    }

    Location BBQJIT::locationOf(Value value)
    {
        if (value.isTemp()) {
            if (value.asTemp() >= m_temps.size() || m_temps[value.asTemp()].isNone())
                bind(value, canonicalSlot(value));
            return m_temps[value.asTemp()];
        }
        return locationOfWithoutBinding(value);
    }

    Location BBQJIT::loadIfNecessary(Value value)
    {
        ASSERT(!value.isPinned()); // We should not load or move pinned values.
        ASSERT(!value.isConst()); // We should not be materializing things we know are constants.
        if (UNLIKELY(Options::verboseBBQJITAllocation()))
            dataLogLn("BBQ\tLoading value ", value, " if necessary");
        Location loc = locationOf(value);
        if (loc.isMemory()) {
            if (UNLIKELY(Options::verboseBBQJITAllocation()))
                dataLogLn("BBQ\tLoading local ", value, " to ", loc);
            loc = allocateRegister(value); // Find a register to store this value. Might spill older values if we run out.
            emitLoad(value, loc); // Generate the load instructions to move the value into the register.
            increaseKey(loc);
            if (value.isLocal())
                currentControlData().touch(value.asLocal());
            bind(value, loc); // Bind the value to the register in the register/local/temp bindings.
        } else
            increaseKey(loc);
        ASSERT(loc.isRegister());
        return loc;
    }

    void BBQJIT::consume(Value value)
    {
        // Called whenever a value is popped from the expression stack. Mostly, we
        // use this to release the registers temporaries are bound to.
        Location location = locationOf(value);
        if (value.isTemp() && location != canonicalSlot(value))
            unbind(value, location);
    }

#if USE(JSVALUE64)
    Location BBQJIT::allocateRegister(TypeKind type)
    {
        if (isFloatingPointType(type))
            return Location::fromFPR(m_fprSet.isEmpty() ? evictFPR() : nextFPR());
        return Location::fromGPR(m_gprSet.isEmpty() ? evictGPR() : nextGPR());
    }

#else
    Location BBQJIT::allocateRegister(TypeKind type)
    {
        if (isFloatingPointType(type))
            return Location::fromFPR(m_fprSet.isEmpty() ? evictFPR() : nextFPR());
        if (typeNeedsGPR2(type)) {
            GPRReg hi, lo;

            do {
                // we loop here until we can get _two_ register from m_gprSet
                // this wouldn't be necessary except that evictGPR modifies m_gprSet and nextGPR _doesn't_

                if (m_gprSet.isEmpty()) {
                    evictGPR();
                    continue;
                }

                auto iter = m_gprSet.begin();
                ASSERT(iter != m_gprSet.end());
                hi = (*iter).gpr();
                ++iter;
                if (iter == m_gprSet.end()) {
                    m_gprLRU.lock(hi);
                    evictGPR();
                    m_gprLRU.unlock(hi);
                    continue;
                }
                lo = (*iter).gpr();

                return Location::fromGPR2(hi, lo);
            } while (1);
        }
        return Location::fromGPR(m_gprSet.isEmpty() ? evictGPR() : nextGPR());
    }
#endif

    Location BBQJIT::allocateRegister(Value value)
    {
        return allocateRegister(value.type());
    }

    Location BBQJIT::bind(Value value, Location loc)
    {
        // Bind a value to a location. Doesn't touch the LRU, but updates the register set
        // and local/temp tables accordingly.

        // Return early if we are already bound to the chosen location. Mostly this avoids
        // spurious assertion failures.
        Location currentLocation = locationOfWithoutBinding(value);
        if (currentLocation == loc)
            return currentLocation;

        // Technically, it could be possible to bind from a register to another register. But
        // this risks (and is usually caused by) messing up the allocator state. So we check
        // for it here.
        ASSERT(!currentLocation.isRegister());

        if (loc.isRegister()) {
            if (value.isFloat()) {
                ASSERT(m_fprSet.contains(loc.asFPR(), Width::Width128));
                m_fprSet.remove(loc.asFPR());
                m_fprBindings[loc.asFPR()] = RegisterBinding::fromValue(value);
#if USE(JSVALUE32_64)
            } else if (loc.isGPR2()) {
                ASSERT(m_gprBindings[loc.asGPRlo()].isNone());
                ASSERT(m_gprBindings[loc.asGPRhi()].isNone());
                ASSERT(m_gprSet.contains(loc.asGPRlo(), IgnoreVectors));
                ASSERT(m_gprSet.contains(loc.asGPRhi(), IgnoreVectors));
                m_gprSet.remove(loc.asGPRlo());
                m_gprSet.remove(loc.asGPRhi());
                auto binding = RegisterBinding::fromValue(value);
                m_gprBindings[loc.asGPRlo()] = binding;
                m_gprBindings[loc.asGPRhi()] = binding;
#endif
            } else {
                ASSERT(m_gprSet.contains(loc.asGPR(), IgnoreVectors));
                m_gprSet.remove(loc.asGPR());
                m_gprBindings[loc.asGPR()] = RegisterBinding::fromValue(value);
            }
        }
        if (value.isLocal())
            m_locals[value.asLocal()] = loc;
        else if (value.isTemp()) {
            if (m_temps.size() <= value.asTemp())
                m_temps.resize(value.asTemp() + 1);
            m_temps[value.asTemp()] = loc;
        }

        if (UNLIKELY(Options::verboseBBQJITAllocation()))
            dataLogLn("BBQ\tBound value ", value, " to ", loc);

        return loc;
    }

    void BBQJIT::unbind(Value value, Location loc)
    {
        // Unbind a value from a location. Doesn't touch the LRU, but updates the register set
        // and local/temp tables accordingly.
        if (loc.isFPR()) {
            ASSERT(m_validFPRs.contains(loc.asFPR(), Width::Width128));
            m_fprSet.add(loc.asFPR(), Width::Width128);
            m_fprBindings[loc.asFPR()] = RegisterBinding::none();
        } else if (loc.isGPR()) {
            ASSERT(m_validGPRs.contains(loc.asGPR(), IgnoreVectors));
            m_gprSet.add(loc.asGPR(), IgnoreVectors);
            m_gprBindings[loc.asGPR()] = RegisterBinding::none();
#if USE(JSVALUE32_64)
        } else if (loc.isGPR2()) {
            m_gprSet.add(loc.asGPRlo(), IgnoreVectors);
            m_gprSet.add(loc.asGPRhi(), IgnoreVectors);
            m_gprBindings[loc.asGPRlo()] = RegisterBinding::none();
            m_gprBindings[loc.asGPRhi()] = RegisterBinding::none();
#endif
        }
        if (value.isLocal())
            m_locals[value.asLocal()] = m_localSlots[value.asLocal()];
        else if (value.isTemp())
            m_temps[value.asTemp()] = Location::none();

        if (UNLIKELY(Options::verboseBBQJITAllocation()))
            dataLogLn("BBQ\tUnbound value ", value, " from ", loc);
    }

    GPRReg BBQJIT::nextGPR()
    {
        auto next = m_gprSet.begin();
        ASSERT(next != m_gprSet.end());
        GPRReg reg = (*next).gpr();
        ASSERT(m_gprBindings[reg].m_kind == RegisterBinding::None);
        return reg;
    }

    FPRReg BBQJIT::nextFPR()
    {
        auto next = m_fprSet.begin();
        ASSERT(next != m_fprSet.end());
        FPRReg reg = (*next).fpr();
        ASSERT(m_fprBindings[reg].m_kind == RegisterBinding::None);
        return reg;
    }

    GPRReg BBQJIT::evictGPR()
    {
        auto lruGPR = m_gprLRU.findMin();
        auto lruBinding = m_gprBindings[lruGPR];

        if (UNLIKELY(Options::verboseBBQJITAllocation()))
            dataLogLn("BBQ\tEvicting GPR ", MacroAssembler::gprName(lruGPR), " currently bound to ", lruBinding);
        flushValue(lruBinding.toValue());

        ASSERT(m_gprSet.contains(lruGPR, IgnoreVectors));
        ASSERT(m_gprBindings[lruGPR].m_kind == RegisterBinding::None);
        return lruGPR;
    }

    FPRReg BBQJIT::evictFPR()
    {
        auto lruFPR = m_fprLRU.findMin();
        auto lruBinding = m_fprBindings[lruFPR];

        if (UNLIKELY(Options::verboseBBQJITAllocation()))
            dataLogLn("BBQ\tEvicting FPR ", MacroAssembler::fprName(lruFPR), " currently bound to ", lruBinding);
        flushValue(lruBinding.toValue());

        ASSERT(m_fprSet.contains(lruFPR, Width::Width128));
        ASSERT(m_fprBindings[lruFPR].m_kind == RegisterBinding::None);
        return lruFPR;
    }

    // We use this to free up specific registers that might get clobbered by an instruction.

    void BBQJIT::clobber(GPRReg gpr)
    {
        if (m_validGPRs.contains(gpr, IgnoreVectors) && !m_gprSet.contains(gpr, IgnoreVectors)) {
            RegisterBinding& binding = m_gprBindings[gpr];
            if (UNLIKELY(Options::verboseBBQJITAllocation()))
                dataLogLn("BBQ\tClobbering GPR ", MacroAssembler::gprName(gpr), " currently bound to ", binding);
            RELEASE_ASSERT(!binding.isNone() && !binding.isScratch()); // We could probably figure out how to handle this, but let's just crash if it happens for now.
            flushValue(binding.toValue());
        }
    }

    void BBQJIT::clobber(FPRReg fpr)
    {
        if (m_validFPRs.contains(fpr, Width::Width128) && !m_fprSet.contains(fpr, Width::Width128)) {
            RegisterBinding& binding = m_fprBindings[fpr];
            if (UNLIKELY(Options::verboseBBQJITAllocation()))
                dataLogLn("BBQ\tClobbering FPR ", MacroAssembler::fprName(fpr), " currently bound to ", binding);
            RELEASE_ASSERT(!binding.isNone() && !binding.isScratch()); // We could probably figure out how to handle this, but let's just crash if it happens for now.
            flushValue(binding.toValue());
        }
    }

    void BBQJIT::clobber(JSC::Reg reg)
    {
        reg.isGPR() ? clobber(reg.gpr()) : clobber(reg.fpr());
    }

    Location BBQJIT::canonicalSlot(Value value)
    {
        ASSERT(value.isLocal() || value.isTemp());
        if (value.isLocal())
            return m_localSlots[value.asLocal()];

        LocalOrTempIndex tempIndex = value.asTemp();
        int slotOffset = WTF::roundUpToMultipleOf<tempSlotSize>(m_localStorage) + (tempIndex + 1) * tempSlotSize;
        if (m_frameSize < slotOffset)
            m_frameSize = slotOffset;
        return Location::fromStack(-slotOffset);
    }

    Location BBQJIT::allocateStack(Value value)
    {
        // Align stack for value size.
        m_frameSize = WTF::roundUpToMultipleOf(value.size(), m_frameSize);
        m_frameSize += value.size();
        return Location::fromStack(-m_frameSize);
    }

    constexpr static int tempSlotSize = 16; // Size of the stack slot for a stack temporary. Currently the size of the largest possible temporary (a v128).

} // namespace JSC::Wasm::BBQJITImpl

Expected<std::unique_ptr<InternalFunction>, String> parseAndCompileBBQ(CompilationContext& compilationContext, BBQCallee& callee, const FunctionData& function, const TypeDefinition& signature, Vector<UnlinkedWasmToWasmCall>& unlinkedWasmToWasmCalls, const ModuleInformation& info, MemoryMode mode, uint32_t functionIndex, std::optional<bool> hasExceptionHandlers, unsigned loopIndexForOSREntry, TierUpCount* tierUp)
{
    CompilerTimingScope totalTime("BBQ", "Total BBQ");

    Thunks::singleton().stub(catchInWasmThunkGenerator);

    auto result = makeUnique<InternalFunction>();

    compilationContext.wasmEntrypointJIT = makeUnique<CCallHelpers>();

    BBQJIT irGenerator(*compilationContext.wasmEntrypointJIT, signature, callee, function, functionIndex, info, unlinkedWasmToWasmCalls, mode, result.get(), hasExceptionHandlers, loopIndexForOSREntry, tierUp);
    FunctionParser<BBQJIT> parser(irGenerator, function.data.data(), function.data.size(), signature, info);
    WASM_FAIL_IF_HELPER_FAILS(parser.parse());

    if (irGenerator.hasLoops())
        result->bbqSharedLoopEntrypoint = irGenerator.addLoopOSREntrypoint();

    irGenerator.finalize();

    result->exceptionHandlers = irGenerator.takeExceptionHandlers();
    compilationContext.catchEntrypoints = irGenerator.takeCatchEntrypoints();
    compilationContext.pcToCodeOriginMapBuilder = irGenerator.takePCToCodeOriginMapBuilder();
    compilationContext.bbqDisassembler = irGenerator.takeDisassembler();

    return result;
}

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY_OMGJIT)
