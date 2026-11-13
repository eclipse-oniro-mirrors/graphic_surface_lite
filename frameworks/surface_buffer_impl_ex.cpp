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

namespace OHOS {
namespace {
/**
 * @brief Buffer wrapper for externally allocated (driver-owned) memory.
 *
 * Differences from the framework-allocated SurfaceBufferImpl:
 * - SetSize bypasses the capacity check (bufferData_.size is always 0 for
 *   external buffers; capacity correctness is the HAL's responsibility).
 * - GetVirAddr/GetPhyAddr/GetSize follow the three-branch unified view:
 *   0 = fall back to base (legacy view), 1 = identical to chunks[0], >1 = invalid.
 * - SetFrame/GetFrame provide the real segment storage (ordered fill, skip
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

    int32_t SetFrame(const GraphicFrame& frame) override
    {
        if (frame.format > GRAPHIC_FRAME_FORMAT_H264) {
            GRAPHIC_LOGD("Invalid frame format %u.", static_cast<uint32_t>(frame.format));
            return -1;
        }
        if (frame.chunkCount == 0 || frame.chunkCount > GRAPHIC_FRAME_MAX_CHUNK_NUM) {
            GRAPHIC_LOGD("Invalid chunk count %u.", frame.chunkCount);
            return -1;
        }
        chunkCount_ = frame.chunkCount;
        for (uint8_t i = 0; i < frame.chunkCount; i++) {
            chunks_[i] = frame.chunks[i];
        }
        bufferFormat_ = frame.format;
        return 0;
    }

    int32_t GetFrame(GraphicFrame& frame) const override
    {
        frame.format = bufferFormat_;
        frame.width = 0;
        frame.height = 0;
        frame.chunkCount = chunkCount_ > 0 ? chunkCount_ : 1;
        if (chunkCount_ == 0) {
            // Legacy synthesized single-chunk view.
            frame.chunks[0].virAddr = SurfaceBufferImpl::GetVirAddr();
            frame.chunks[0].phyAddr = SurfaceBufferImpl::GetPhyAddr();
            frame.chunks[0].size = SurfaceBufferImpl::GetSize();
            frame.chunks[0].stride = 0;
        } else {
            for (uint8_t i = 0; i < chunkCount_; i++) {
                frame.chunks[i] = chunks_[i];
            }
        }
        return 0;
    }

    uint32_t GetTotalDataSize() const override
    {
        if (chunkCount_ == 0) {
            return SurfaceBufferImpl::GetTotalDataSize();
        }
        uint32_t total = 0;
        for (uint8_t i = 0; i < chunkCount_; i++) {
            if (total > UINT32_MAX - chunks_[i].size) {
                GRAPHIC_LOGD("GetTotalDataSize overflow, saturating.");
                return UINT32_MAX;
            }
            total += chunks_[i].size;
        }
        return total;
    }

private:
    GraphicFrameChunk chunks_[GRAPHIC_FRAME_MAX_CHUNK_NUM] = {};
    uint8_t chunkCount_ = 0;
};
} // namespace

SurfaceBuffer* SurfaceBuffer::CreateSurfaceBuffer()
{
    SurfaceBuffer* buffer = new (std::nothrow) SurfaceBufferImplEx();
    if (buffer == nullptr) {
        GRAPHIC_LOGE("CreateSurfaceBuffer failed: out of memory.");
    }
    return buffer;
}

int32_t SurfaceBuffer::DestroySurfaceBuffer(SurfaceBuffer* buffer)
{
    if (buffer == nullptr) {
        GRAPHIC_LOGE("DestroySurfaceBuffer failed: buffer is null.");
        return SURFACE_ERROR_INVALID_PARAM;
    }
    SurfaceBufferImpl* impl = static_cast<SurfaceBufferImpl*>(buffer);
    if (impl->GetState() != BUFFER_STATE_NONE) {
        GRAPHIC_LOGI("DestroySurfaceBuffer rejected: buffer is owned by a queue, release it first.");
        return SURFACE_ERROR_BUFFER_IN_USE;
    }
    delete impl;
    return SURFACE_ERROR_OK;
}
} // namespace OHOS
