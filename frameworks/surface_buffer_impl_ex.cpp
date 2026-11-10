/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdint>
#include <new>
#include "surface_buffer_impl.h"
#include "surface_type.h"

namespace OHOS {
namespace {
/**
 * @brief Buffer wrapper for externally allocated (driver-owned) memory.
 *
 * Differences from the framework-allocated SurfaceBufferImpl:
 * - SetSize bypasses the capacity check (bufferData_.size is always 0 for
 *   external buffers; capacity correctness is the HAL's responsibility).
 * - GetVirAddr/GetPhyAddr/GetSize follow the three-branch unified view:
 *   0 = fall back to base (legacy view), 1 = identical to chunk[0], >1 = invalid.
 * - SetChunk/GetChunk provide the real segment storage (ordered fill, skip
 *   rejected, overwrite allowed for ring reuse re-pointing).
 *
 * Defined in an anonymous namespace on purpose: no new header is introduced,
 * only SurfaceBuffer* is exposed to the HAL.
 */
class SurfaceBufferImplEx : public SurfaceBufferImpl {
public:
    SurfaceBufferImplEx() = default;
    ~SurfaceBufferImplEx() override = default;

    void SetSize(uint32_t size) override
    {
        SetSizeUnchecked(size);
    }

    void* GetVirAddr() const override
    {
        if (chunkCount_ > 1) {
            return nullptr;
        }
        if (chunkCount_ == 1) {
            return chunks_[0].virAddr;
        }
        return SurfaceBufferImpl::GetVirAddr();
    }

    uint64_t GetPhyAddr() const override
    {
        if (chunkCount_ > 1) {
            return 0;
        }
        if (chunkCount_ == 1) {
            return chunks_[0].phyAddr;
        }
        return SurfaceBufferImpl::GetPhyAddr();
    }

    uint32_t GetSize() const override
    {
        if (chunkCount_ > 1) {
            return 0;
        }
        if (chunkCount_ == 1) {
            return chunks_[0].size;
        }
        return SurfaceBufferImpl::GetSize();
    }

    int32_t SetChunk(uint8_t index, const SurfaceDataChunk& chunk) override
    {
        if (index >= SURFACE_MAX_CHUNK_NUM) {
            GRAPHIC_LOGD("Chunk index %u is out of range.", index);
            return -1;
        }
        if (index > chunkCount_) {
            GRAPHIC_LOGD("Chunk index %u skipped, current count is %u.", index, chunkCount_);
            return -1;
        }
        chunks_[index] = chunk;
        if (index == chunkCount_) {
            chunkCount_ = static_cast<uint8_t>(index + 1);
        }
        return 0;
    }

    int32_t GetChunk(uint8_t index, SurfaceDataChunk& chunk) const override
    {
        if (chunkCount_ == 0) {
            if (index != 0) {
                GRAPHIC_LOGD("Chunk index %u is out of range.", index);
                return -1;
            }
            chunk = GetVirtualChunk();
            return 0;
        }
        if (index >= chunkCount_) {
            GRAPHIC_LOGD("Chunk index %u is out of range.", index);
            return -1;
        }
        chunk = chunks_[index];
        return 0;
    }

    uint32_t GetChunkCount() const override
    {
        return chunkCount_ > 0 ? chunkCount_ : 1;
    }

    uint32_t GetTotalDataSize() const override
    {
        if (chunkCount_ == 0) {
            return SurfaceBufferImpl::GetTotalDataSize();
        }
        uint32_t total = 0;
        for (uint8_t i = 0; i < chunkCount_; i++) {
            if (total > UINT32_MAX - chunks_[i].size) {
                // Saturate instead of silently wrapping around on bogus segment sizes.
                GRAPHIC_LOGD("GetTotalDataSize overflow, saturating.");
                return UINT32_MAX;
            }
            total += chunks_[i].size;
        }
        return total;
    }

private:
    SurfaceDataChunk chunks_[SURFACE_MAX_CHUNK_NUM] = {};
    uint8_t chunkCount_ = 0;
};
} // namespace

SurfaceBuffer* SurfaceBuffer::CreateSurfaceBuffer()
{
    return new (std::nothrow) SurfaceBufferImplEx();
}
} // namespace OHOS
