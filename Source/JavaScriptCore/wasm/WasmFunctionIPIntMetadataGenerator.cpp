
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
#include "WasmFunctionIPIntMetadataGenerator.h"

#include <numeric>
#include <wtf/TZoneMallocInlines.h>

#if ENABLE(WEBASSEMBLY)

namespace JSC {

namespace Wasm {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FunctionIPIntMetadataGenerator);

unsigned FunctionIPIntMetadataGenerator::addSignature(const TypeDefinition& signature)
{
    unsigned index = m_signatures.size();
    m_signatures.append(&signature);
    return index;
}

void FunctionIPIntMetadataGenerator::addBlankSpace(uint32_t size)
{
    m_metadata.grow(m_metadata.size() + size);
}

void FunctionIPIntMetadataGenerator::addRawValue(uint64_t value)
{
    auto size = m_metadata.size();
    m_metadata.grow(m_metadata.size() + 8);
    WRITE_TO_METADATA(m_metadata.data() + size, value, uint64_t);
}

void FunctionIPIntMetadataGenerator::addLength(uint32_t length)
{
    size_t size = m_metadata.size();
    m_metadata.grow(size + 1);
    WRITE_TO_METADATA(m_metadata.data() + size, length, uint8_t);
}

void FunctionIPIntMetadataGenerator::addLEB128ConstantInt32AndLength(uint32_t value, uint32_t length)
{
    size_t size = m_metadata.size();
    m_metadata.grow(size + 5);
    WRITE_TO_METADATA(m_metadata.data() + size, length, uint8_t);
    WRITE_TO_METADATA(m_metadata.data() + size + 1, value, uint32_t);
}

void FunctionIPIntMetadataGenerator::addCondensedLocalIndexAndLength(uint32_t index, uint32_t length)
{
    size_t size = m_metadata.size();
    if (length == 2) {
        m_metadata.grow(size + 1);
        WRITE_TO_METADATA(m_metadata.data() + size, 0, uint8_t);
    } else {
        m_metadata.grow(size + 5);
        WRITE_TO_METADATA(m_metadata.data() + size, length, uint8_t);
        WRITE_TO_METADATA(m_metadata.data() + size + 1, index, uint32_t);
    }
}

void FunctionIPIntMetadataGenerator::addLEB128ConstantAndLengthForType(Type type, uint64_t value, uint32_t length)
{
    if (type.isI32()) {
        size_t size = m_metadata.size();
        if (length == 2) {
            m_metadata.grow(size + 1);
            WRITE_TO_METADATA(m_metadata.data() + size, (value >> 7) & 1, uint8_t);
        } else {
            m_metadata.grow(size + 5);
            WRITE_TO_METADATA(m_metadata.data() + size, static_cast<uint8_t>(length), uint8_t);
            WRITE_TO_METADATA(m_metadata.data() + size + 1, static_cast<uint32_t>(value), uint32_t);
        }
    } else if (type.isI64()) {
        size_t size = m_metadata.size();
        m_metadata.grow(size + 9);
        WRITE_TO_METADATA(m_metadata.data() + size, static_cast<uint8_t>(length), uint8_t);
        WRITE_TO_METADATA(m_metadata.data() + size + 1, static_cast<uint64_t>(value), uint64_t);
    }  else if (type.isFuncref()) {
        size_t size = m_metadata.size();
        m_metadata.grow(size + 5);
        WRITE_TO_METADATA(m_metadata.data() + size, static_cast<uint8_t>(length), uint8_t);
        WRITE_TO_METADATA(m_metadata.data() + size + 1, static_cast<uint32_t>(value), uint32_t);
    } else if (!type.isF32() && !type.isF64())
        ASSERT_NOT_IMPLEMENTED_YET();
}

void FunctionIPIntMetadataGenerator::addLEB128V128Constant(v128_t value, uint32_t length)
{
    size_t size = m_metadata.size();
    m_metadata.grow(size + 17);
    WRITE_TO_METADATA(m_metadata.data() + size, static_cast<uint8_t>(length), uint8_t);
    WRITE_TO_METADATA(m_metadata.data() + size + 1, value, v128_t);
}

} }

#endif // ENABLE(WEBASSEMBLY)
