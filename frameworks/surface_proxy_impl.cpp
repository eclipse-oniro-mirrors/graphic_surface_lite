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

#include "surface_utils.h"

#include "buffer_common.h"
#include "buffer_queue_producer.h"
#include "surface_buffer_impl.h"

namespace OHOS {
namespace {
/**
 * @brief Producer-side surface proxy for single process mode.
 *
 * The proxy holds only the consumer surface id (never a raw producer pointer).
 * Every call is routed through SurfaceUtils::WithProducer, which holds the
 * registry lock: in-flight calls complete before the consumer can unregister
 * or destroy the queue, and calls issued afterwards fail cleanly with
 * SURFACE_ERROR_NOT_READY instead of dangling. Destruction order no longer
 * decides safety.
 */
class SurfaceProxyImpl : public Surface {
public:
    explicit SurfaceProxyImpl(uint64_t targetId) : targetId_(targetId) {}

    ~SurfaceProxyImpl() override
    {
        SurfaceUtils::GetInstance()->RemoveProxy(targetId_);
    }

    uint64_t GetSurfaceId() const override
    {
        return targetId_;
    }

    void SetQueueSize(uint8_t queueSize) override
    {
        WithProducer([&](BufferProducer* p) { p->SetQueueSize(queueSize); });
    }

    uint8_t GetQueueSize() override
    {
        uint8_t queueSize = 0;
        WithProducer([&](BufferProducer* p) { queueSize = p->GetQueueSize(); });
        return queueSize;
    }

    void SetWidthAndHeight(uint32_t width, uint32_t height) override
    {
        WithProducer([&](BufferProducer* p) { p->SetWidthAndHeight(width, height); });
    }

    uint32_t GetWidth() override
    {
        uint32_t width = 0;
        WithProducer([&](BufferProducer* p) { width = p->GetWidth(); });
        return width;
    }

    uint32_t GetHeight() override
    {
        uint32_t height = 0;
        WithProducer([&](BufferProducer* p) { height = p->GetHeight(); });
        return height;
    }

    void SetFormat(uint32_t format) override
    {
        WithProducer([&](BufferProducer* p) { p->SetFormat(format); });
    }

    uint32_t GetFormat() override
    {
        uint32_t format = 0;
        WithProducer([&](BufferProducer* p) { format = p->GetFormat(); });
        return format;
    }

    void SetStrideAlignment(uint32_t strideAlignment) override
    {
        WithProducer([&](BufferProducer* p) { p->SetStrideAlignment(strideAlignment); });
    }

    uint32_t GetStrideAlignment() override
    {
        uint32_t alignment = 0;
        WithProducer([&](BufferProducer* p) { alignment = p->GetStrideAlignment(); });
        return alignment;
    }

    uint32_t GetStride() override
    {
        uint32_t stride = 0;
        WithProducer([&](BufferProducer* p) { stride = p->GetStride(); });
        return stride;
    }

    void SetSize(uint32_t size) override
    {
        WithProducer([&](BufferProducer* p) { p->SetSize(size); });
    }

    uint32_t GetSize() override
    {
        uint32_t size = 0;
        WithProducer([&](BufferProducer* p) { size = p->GetSize(); });
        return size;
    }

    void SetUsage(uint32_t usage) override
    {
        WithProducer([&](BufferProducer* p) { p->SetUsage(usage); });
    }

    uint32_t GetUsage() override
    {
        uint32_t usage = 0;
        WithProducer([&](BufferProducer* p) { usage = p->GetUsage(); });
        return usage;
    }

    void SetUserData(const std::string& key, const std::string& value) override
    {
        WithProducer([&](BufferProducer* p) { p->SetUserData(key, value); });
    }

    std::string GetUserData(const std::string& key) override
    {
        std::string value;
        WithProducer([&](BufferProducer* p) { value = p->GetUserData(key); });
        return value;
    }

    SurfaceBuffer* RequestBuffer(uint8_t wait) override
    {
        SurfaceBufferImpl* buffer = nullptr;
        WithProducer([&](BufferProducer* p) { buffer = p->RequestBuffer(wait); });
        return buffer;
    }

    int32_t FlushBuffer(SurfaceBuffer* buffer) override
    {
        RETURN_VAL_IF_FAIL(buffer != nullptr, SURFACE_ERROR_INVALID_PARAM);
        int32_t ret = SURFACE_ERROR_NOT_READY;
        WithProducer([&](BufferProducer* p) { ret = p->FlushBuffer(reinterpret_cast<SurfaceBufferImpl*>(buffer)); });
        return ret;
    }

    SurfaceBuffer* AcquireBuffer() override
    {
        // Producer-side proxy: no consumer end lives here.
        return nullptr;
    }

    bool ReleaseBuffer(SurfaceBuffer* buffer) override
    {
        (void)buffer;
        // Producer-side proxy: no consumer end lives here.
        return false;
    }

    void CancelBuffer(SurfaceBuffer* buffer) override
    {
        RETURN_IF_FAIL(buffer != nullptr);
        WithProducer([&](BufferProducer* p) { p->Cancel(reinterpret_cast<SurfaceBufferImpl*>(buffer)); });
    }

    void RegisterConsumerListener(IBufferConsumerListener& listener) override
    {
        WithProducer([&](BufferProducer* p) {
            reinterpret_cast<BufferQueueProducer*>(p)->RegisterConsumerListener(listener);
        });
    }

    void UnregisterConsumerListener() override
    {
        WithProducer([&](BufferProducer* p) {
            reinterpret_cast<BufferQueueProducer*>(p)->UnregisterConsumerListener();
        });
    }

    bool IsQueueFull() const override
    {
        bool full = true;
        WithProducer([&](BufferProducer* p) { full = reinterpret_cast<BufferQueueProducer*>(p)->IsQueueFull(); });
        return full;
    }

    int32_t AttachBuffer(SurfaceBuffer* buffer) override
    {
        RETURN_VAL_IF_FAIL(buffer != nullptr, SURFACE_ERROR_INVALID_PARAM);
        int32_t ret = SURFACE_ERROR_NOT_READY;
        WithProducer([&](BufferProducer* p) { ret = p->AttachBuffer(reinterpret_cast<SurfaceBufferImpl*>(buffer)); });
        return ret;
    }

    void RegisterProducerListener(IProducerListener& listener) override
    {
        WithProducer([&](BufferProducer* p) { p->RegisterProducerListener(listener); });
    }

    void UnregisterProducerListener() override
    {
        WithProducer([&](BufferProducer* p) { p->UnregisterProducerListener(); });
    }

private:
    template <typename F>
    int32_t WithProducer(F fn) const
    {
        return SurfaceUtils::GetInstance()->WithProducer(targetId_, fn);
    }

    uint64_t targetId_;
};
} // namespace

Surface* SurfaceUtils::CreateSurfaceProxy(uint64_t surfaceId)
{
    if (!SurfaceUtils::GetInstance()->AddProxy(surfaceId)) {
        GRAPHIC_LOGE("CreateSurfaceProxyImpl failed: surface id %llu is unknown.",
                     static_cast<unsigned long long>(surfaceId));
        return nullptr;
    }
    return new SurfaceProxyImpl(surfaceId);
}
} // namespace OHOS
