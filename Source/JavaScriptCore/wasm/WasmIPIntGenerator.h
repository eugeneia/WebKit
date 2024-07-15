/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#if ENABLE(WEBASSEMBLY)

#include "WasmCallingConvention.h"
#include <wtf/Expected.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace Wasm {

class FunctionIPIntMetadataGenerator;
class TypeDefinition;
struct ModuleInformation;

Expected<std::unique_ptr<FunctionIPIntMetadataGenerator>, String> parseAndCompileMetadata(std::span<const uint8_t>, const TypeDefinition&, ModuleInformation&, uint32_t functionIndex);

} // namespace JSC::Wasm

namespace IPInt {

#pragma pack(1)

// mINT: mini-interpreter / minimalist interpreter (whichever floats your boat)

// Metadata structure for calls:

struct MDCallCommonCall {
    uint16_t stackSlots; // 2B for number of arguments on stack (to set up callee frame)
};

enum class MINTCall : uint8_t {
    a = 0x0,          // 0x00 - 0x07: push into a0, a1, ...
    fa = 0x8,         // 0x08 - 0x0b: push into fa0, fa1, ...
    stackzero = 0xc,  // 0x0c: pop stack value, push onto stack[0]
    stackeight = 0xd, // 0x0d: pop stack value, add another 16B for params, push onto stack[8]
    gap = 0xe,        // 0x0e: add another 16B for params
    call = 0xf        // 0x0f: stop
};

// Metadata structure for returns:

struct MDCallCommonReturn {
    uint16_t stackSlots;    // 2B for number of arguments on stack (to clean up current call frame)
    uint16_t argumentCount; // 2B for number of arguments (to take off arguments)
};

enum class MINTReturn : uint8_t {
    r = 0x0,     // 0x00 - 0x07: r0 - r7
    fr = 0x8,    // 0x08 - 0x0b: fr0 - fr3
    stack = 0xc, // 0x0c: stack
    end = 0xd    // 0x0d: end
};

// Metadata structure for calls:

struct MDCall {
    // Function index:
    uint8_t length;         // 1B for instruction length
    uint32_t functionIndex; // 4B for decoded index
};

// Metadata structure for indirect calls:

struct MDCallIndirect {
    // Function index:
    uint8_t length;       // 1B for length
    uint32_t tableIndex;  // 4B for table index
    uint32_t typeIndex;   // 4B for type index
    uint32_t functionRef; // 4B for function ref (set by interpreter)
    CallFrame* callFrame; // pointer to call frame (set by interpreter)
};

// Helpers

struct MDCallHeader {
    struct MDCall call;
    struct MDCallCommonCall common;
};

struct MDCallIndirectHeader {
    struct MDCallIndirect indirect;
    struct MDCallCommonCall common;
};

#pragma pack()

} // namespace JSC::IPInt

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY[:w
