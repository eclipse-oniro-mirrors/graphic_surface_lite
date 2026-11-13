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

#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "buffer_common.h"
#include "surface.h"
#include "surface_buffer.h"
#include "surface_type.h"
#include "surface_utils.h"

/*
 * Single process full-flow example for surface_lite (SURFACE_LITE_SINGLE_PROCESS=1):
 * a camera HAL producer handing driver-owned buffers to the graphic framework
 * consumer of the same process. Scenario list mirrors the LITESURFACE-001
 * codespec record:
 *  S0  basics: surface id, producer proxy, unknown id, SP request unavailable,
 *      format round trips, fresh wrapper view + SetFrame domain validation
 *  S1  RGB single segment: queued destroy rejected, retire-on-release,
 *      driver memory returned on the release callback
 *  S2  NV12 ring: dual plane frames, extra data across release cycles,
 *      total size summation, ring reuse, destroy after idle
 *  S3  ring crossing two segments + H264 tag: destroy in ACQUIRE rejected,
 *      retire-on-release through OnBufferReleased
 *  S4  teardown: proxy keeps the queue alive, consumer unregisters
 *  S5a destruction order probe: consumer deleted first, calls fail cleanly
 *  S5b death notify: OnConsumerSurfaceDestroyed with a wrapper in flight
 *  S5c in-flight teardown without a producer listener (queue deletes wrappers)
 *  S6  flow control: IsQueueFull at queueSize, attach beyond queueSize rejected
 *  S7  concurrency: real producer thread vs consumer thread
 */

namespace OHOS {
namespace {
constexpr int32_t FRAME_WIDTH = 320;
constexpr int32_t FRAME_HEIGHT = 240;
constexpr uint32_t Y_SIZE = FRAME_WIDTH * FRAME_HEIGHT;
constexpr uint32_t UV_SIZE = FRAME_WIDTH * FRAME_HEIGHT / 2;
constexpr uint32_t NV12_TOTAL = Y_SIZE + UV_SIZE;
constexpr uint32_t DRIVER_BUF_ID_KEY = 1001;
constexpr int RING_NUM = 3;
constexpr int RING_FRAMES = 6;

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { g_pass++; } else { g_fail++; printf("[FAIL] %s (line %d)\n", msg, __LINE__); } \
    } while (0)

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
bool g_available = false;
bool g_released = false;

void Signal(bool* flag)
{
    pthread_mutex_lock(&g_lock);
    *flag = true;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void Wait(bool* flag)
{
    pthread_mutex_lock(&g_lock);
    while (!*flag) {
        pthread_cond_wait(&g_cond, &g_lock);
    }
    *flag = false;
    pthread_mutex_unlock(&g_lock);
}

struct DriverSlot {
    void* mem;
    size_t size;
    bool freed;
};

struct HalCtx {
    Surface* producer;
    DriverSlot rgbSlot;
    DriverSlot ringSlots[RING_NUM];
    DriverSlot ring2Slot;
    uint64_t ring2Mid;
    SurfaceBuffer* rgbWrapper = nullptr;
    SurfaceBuffer* ringWrappers[RING_NUM] = { nullptr, nullptr, nullptr };
    SurfaceBuffer* ring2Wrapper = nullptr;
    SurfaceBuffer* retireTable[RING_NUM + 2] = { nullptr };
    int retireCount = 0;
    int releasedCount = 0;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    bool deathNotified = false;
};

HalCtx g_hal;
Surface* g_consumerSurface = nullptr;
Surface* g_producerSurface = nullptr;
uint64_t g_surfaceId = 0;
pthread_t g_producerThread;

void AllocDriverMem(DriverSlot* slot, size_t size)
{
    slot->size = 0;
    slot->mem = nullptr;
    slot->freed = false;
    if (posix_memalign(&slot->mem, 64, size) != 0) {
        printf("[FAIL] AllocDriverMem failed\n");
        exit(1);
    }
    slot->size = size;
    memset(slot->mem, 0xAA, size);
}

uint64_t SimPhyAddr(void* addr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(addr));
}

GraphicFrameChunk MakeChunk(void* mem, size_t size)
{
    GraphicFrameChunk chunk;
    chunk.virAddr = mem;
    chunk.phyAddr = SimPhyAddr(mem);
    chunk.size = static_cast<uint32_t>(size);
    chunk.stride = static_cast<uint32_t>(size);
    return chunk;
}

void ReturnDriverMem(SurfaceBuffer* wrapper)
{
    if (wrapper == g_hal.ring2Wrapper) {
        free(g_hal.ring2Slot.mem);
        g_hal.ring2Slot.mem = nullptr;
        g_hal.ring2Slot.freed = true;
    } else if (wrapper == g_hal.rgbWrapper) {
        free(g_hal.rgbSlot.mem);
        g_hal.rgbSlot.mem = nullptr;
        g_hal.rgbSlot.freed = true;
    } else {
        for (int i = 0; i < RING_NUM; i++) {
            if (wrapper == g_hal.ringWrappers[i]) {
                free(g_hal.ringSlots[i].mem);
                g_hal.ringSlots[i].mem = nullptr;
                g_hal.ringSlots[i].freed = true;
                break;
            }
        }
    }
}

class ConsumerListener : public IBufferConsumerListener {
public:
    void OnBufferAvailable() override
    {
        Signal(&g_available);
    }
};

class ProducerListener : public IProducerListener {
public:
    void OnBufferReleased(SurfaceBuffer* buffer) override
    {
        pthread_mutex_lock(&g_hal.lock);
        int idx = -1;
        for (int i = 0; i < g_hal.retireCount; i++) {
            if (g_hal.retireTable[i] == buffer) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) {
            for (int i = idx; i < g_hal.retireCount - 1; i++) {
                g_hal.retireTable[i] = g_hal.retireTable[i + 1];
            }
            g_hal.retireTable[--g_hal.retireCount] = nullptr;
            pthread_mutex_unlock(&g_hal.lock);
            ReturnDriverMem(buffer);
            CHECK(SurfaceBuffer::DestroySurfaceBuffer(buffer) == SURFACE_ERROR_OK,
                  "OnBufferReleased: destroy retired wrapper");
        } else {
            g_hal.releasedCount++;
            pthread_mutex_unlock(&g_hal.lock);
        }
        Signal(&g_released);
    }

    void OnConsumerSurfaceDestroyed() override
    {
        g_hal.deathNotified = true;
    }
};

ConsumerListener g_consumerListener;
ProducerListener g_producerListener;

GraphicFrame MakeNv12Frame(int slotIdx)
{
    void* yMem = g_hal.ringSlots[slotIdx].mem;
    void* uvMem = static_cast<char*>(g_hal.ringSlots[slotIdx].mem) + Y_SIZE;
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    frame.format = GRAPHIC_FRAME_FORMAT_NV12;
    frame.width = FRAME_WIDTH;
    frame.height = FRAME_HEIGHT;
    frame.chunkCount = 2;
    frame.chunks[0] = MakeChunk(yMem, Y_SIZE);
    frame.chunks[1] = MakeChunk(uvMem, UV_SIZE);
    return frame;
}

void RunBasics()
{
    g_consumerSurface = Surface::CreateSurface();
    CHECK(g_consumerSurface != nullptr, "S0: CreateSurface");
    g_surfaceId = g_consumerSurface->GetSurfaceId();
    CHECK(g_surfaceId != 0, "S0: auto allocated surface id");
    g_consumerSurface->RegisterConsumerListener(g_consumerListener);
    g_consumerSurface->SetQueueSize(RING_NUM);
    CHECK(g_consumerSurface->GetQueueSize() == RING_NUM, "S0: queue size set to ring depth");
    g_consumerSurface->SetFormat(IMAGE_PIXEL_FORMAT_NV21);
    CHECK(g_consumerSurface->GetFormat() == IMAGE_PIXEL_FORMAT_NV21, "S0: NV21 round trip");
    g_consumerSurface->SetFormat(IMAGE_PIXEL_FORMAT_NV12);
    CHECK(g_consumerSurface->GetFormat() == IMAGE_PIXEL_FORMAT_NV12, "S0: NV12 round trip");
    g_producerSurface = Surface::CreateSurfaceAsProducer(g_surfaceId);
    CHECK(g_producerSurface != nullptr, "S0: CreateSurfaceAsProducer by id");
    CHECK(g_producerSurface->GetSurfaceId() == g_surfaceId, "S0: proxy reports consumer id");
    CHECK(Surface::CreateSurfaceAsProducer(g_surfaceId + 99999) == nullptr, "S0: unknown id rejected");
    g_producerSurface->RegisterProducerListener(g_producerListener);
    CHECK(g_producerSurface->RequestBuffer() == nullptr, "S0: RequestBuffer unavailable in SP mode");
    CHECK(!g_producerSurface->IsQueueFull(), "S0: queue not full before attach");
    pthread_create(&g_producerThread, nullptr, [](void*)->void* { return nullptr; }, nullptr);
}

void RunFreshWrapperChecks()
{
    SurfaceBuffer* fresh = SurfaceBuffer::CreateSurfaceBuffer();
    CHECK(fresh != nullptr, "S0: CreateSurfaceBuffer");
    fresh->SetSize(1024);
    CHECK(fresh->GetTotalDataSize() == 1024, "S0: fresh wrapper total size follows SetSize");
    GraphicFrame outFrame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    CHECK(fresh->GetFrame(outFrame) == 0, "S0: GetFrame on fresh wrapper");
    CHECK(outFrame.chunkCount == 1, "S0: fresh wrapper chunk count defaults to 1");
    CHECK(outFrame.format == GRAPHIC_FRAME_FORMAT_NONE, "S0: fresh wrapper format defaults to NONE");
    CHECK(outFrame.chunks[0].virAddr == nullptr && outFrame.chunks[0].size == 1024,
          "S0: synthesized chunk content");
    // Closed format domain (SetFrame rejection paths).
    GraphicFrame badFrame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    badFrame.chunks[0] = MakeChunk(nullptr, 16);
    badFrame.format = static_cast<GraphicFrameFormat>(GRAPHIC_FRAME_FORMAT_H264 + 1);
    CHECK(fresh->SetFrame(badFrame) == -1, "S0: out-of-domain format rejected");
    badFrame.format = GRAPHIC_FRAME_FORMAT_JPEG;
    badFrame.chunkCount = 0;
    CHECK(fresh->SetFrame(badFrame) == -1, "S0: zero chunk count rejected");
    badFrame.chunkCount = static_cast<uint8_t>(GRAPHIC_FRAME_MAX_CHUNK_NUM + 1);
    CHECK(fresh->SetFrame(badFrame) == -1, "S0: chunk count overflow rejected");
    // Valid JPEG frame round trip; a rejected write leaves the stored tag unchanged.
    GraphicFrame jpegFrame = { .format = GRAPHIC_FRAME_FORMAT_JPEG, .chunkCount = 1 };
    jpegFrame.chunks[0] = MakeChunk(nullptr, 16);
    CHECK(fresh->SetFrame(jpegFrame) == 0, "S0: JPEG frame round trip set");
    GraphicFrame outJpeg = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    CHECK(fresh->GetFrame(outJpeg) == 0 && outJpeg.format == GRAPHIC_FRAME_FORMAT_JPEG,
          "S0: JPEG frame round trip get");
    CHECK(fresh->SetFrame(badFrame) == -1, "S0: rejected write leaves tag unchanged");
    CHECK(fresh->GetFrame(outJpeg) == 0 && outJpeg.format == GRAPHIC_FRAME_FORMAT_JPEG,
          "S0: tag unchanged after rejection");
    // Clear the tag back to NONE (S1 re-points this wrapper to RGB memory).
    GraphicFrame noneFrame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    noneFrame.chunks[0] = MakeChunk(nullptr, 16);
    CHECK(fresh->SetFrame(noneFrame) == 0, "S0: clear tag back to NONE");
    CHECK(fresh->GetFrame(outJpeg) == 0 && outJpeg.format == GRAPHIC_FRAME_FORMAT_NONE,
          "S0: tag cleared");
    SurfaceBuffer* spare = SurfaceBuffer::CreateSurfaceBuffer();
    CHECK(spare != nullptr, "S0: spare wrapper");
    CHECK(SurfaceBuffer::DestroySurfaceBuffer(spare) == SURFACE_ERROR_OK,
          "S0: destroy never-attached wrapper");
    g_hal.rgbWrapper = fresh;
}

void RunRgbIdentity()
{
    AllocDriverMem(&g_hal.rgbSlot, 64 * 1024);
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    frame.chunkCount = 1;
    frame.chunks[0] = MakeChunk(g_hal.rgbSlot.mem, g_hal.rgbSlot.size);
    CHECK(g_hal.rgbWrapper->SetFrame(frame) == 0, "S1: SetFrame single segment");
    g_hal.rgbWrapper->SetSize(static_cast<uint32_t>(g_hal.rgbSlot.size));
    CHECK(g_producerSurface->AttachBuffer(g_hal.rgbWrapper) == SURFACE_ERROR_OK, "S1: attach RGB buffer");
    CHECK(SurfaceBuffer::DestroySurfaceBuffer(g_hal.rgbWrapper) == SURFACE_ERROR_BUFFER_IN_USE,
          "S1: destroy queued wrapper rejected");
    CHECK(!g_producerSurface->IsQueueFull(), "S1: queue has slot after 1 attach");
    Wait(&g_available);
    SurfaceBuffer* rgb = g_consumerSurface->AcquireBuffer();
    CHECK(rgb != nullptr, "S1: acquire RGB frame");
    GraphicFrame s1Frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    CHECK(rgb->GetFrame(s1Frame) == 0, "S1: GetFrame");
    CHECK(s1Frame.chunkCount == 1, "S1: single chunk count");
    CHECK(s1Frame.chunks[0].virAddr == rgb->GetVirAddr(), "S1: chunk 0 identical to legacy view");
    pthread_mutex_lock(&g_hal.lock);
    g_hal.retireTable[g_hal.retireCount++] = g_hal.rgbWrapper;
    pthread_mutex_unlock(&g_hal.lock);
    g_consumerSurface->ReleaseBuffer(rgb);
    Wait(&g_released);
    CHECK(g_hal.retireCount == 0, "S1: retire table drained");
    CHECK(g_hal.rgbSlot.freed, "S1: driver memory returned");
}

void RunNv12Ring()
{
    for (int i = 0; i < RING_NUM; i++) {
        AllocDriverMem(&g_hal.ringSlots[i], Y_SIZE + UV_SIZE);
        g_hal.ringWrappers[i] = SurfaceBuffer::CreateSurfaceBuffer();
        CHECK(g_hal.ringWrappers[i] != nullptr, "S2: CreateSurfaceBuffer");
    }
    for (int frameNo = 0; frameNo < RING_FRAMES; frameNo++) {
        int slot = frameNo % RING_NUM;
        GraphicFrame frame = MakeNv12Frame(slot);
        CHECK(g_hal.ringWrappers[slot]->SetFrame(frame) == 0, "S2: SetFrame NV12");
        CHECK(g_hal.ringWrappers[slot]->SetInt32(DRIVER_BUF_ID_KEY, frameNo) == 0,
              "S2: stamp frame id before attach");
        CHECK(g_producerSurface->AttachBuffer(g_hal.ringWrappers[slot]) == SURFACE_ERROR_OK, "S2: AttachBuffer");
        Wait(&g_available);
        SurfaceBuffer* buf = g_consumerSurface->AcquireBuffer();
        CHECK(buf != nullptr, "S2: AcquireBuffer");
        GraphicFrame vf = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
        CHECK(buf->GetFrame(vf) == 0, "S2: GetFrame");
        CHECK(vf.chunkCount == 2, "S2: NV12 chunk count is 2");
        CHECK(vf.format == GRAPHIC_FRAME_FORMAT_NV12, "S2: per-frame NV12 format");
        CHECK(vf.chunks[0].size == Y_SIZE, "S2: plane0 size");
        CHECK(vf.chunks[1].size == UV_SIZE, "S2: plane1 size");
        CHECK(buf->GetTotalDataSize() == NV12_TOTAL, "S2: total data size sums planes");
        int32_t bufId = -1;
        CHECK(buf->GetInt32(DRIVER_BUF_ID_KEY, bufId) == 0 && bufId == frameNo,
              "S2: extra data survives release cycles (ring slot re-pointed)");
        g_consumerSurface->ReleaseBuffer(buf);
        Wait(&g_released);
    }
    CHECK(g_hal.releasedCount == RING_FRAMES, "S2: OnBufferReleased fired per frame");
    for (int i = 0; i < RING_NUM; i++) {
        CHECK(SurfaceBuffer::DestroySurfaceBuffer(g_hal.ringWrappers[i]) == SURFACE_ERROR_OK,
              "S2: destroy idle wrapper");
        free(g_hal.ringSlots[i].mem);
        g_hal.ringSlots[i].mem = nullptr;
        g_hal.ringSlots[i].freed = true;
    }
}

void RunRingCrossingInUse()
{
    AllocDriverMem(&g_hal.ring2Slot, Y_SIZE);
    g_hal.ring2Mid = Y_SIZE / 2;
    SurfaceBuffer* wrapper = SurfaceBuffer::CreateSurfaceBuffer();
    CHECK(wrapper != nullptr, "S3: CreateSurfaceBuffer");
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    frame.format = GRAPHIC_FRAME_FORMAT_H264;
    frame.width = FRAME_WIDTH;
    frame.height = FRAME_HEIGHT;
    frame.chunkCount = 2;
    frame.chunks[0].virAddr = static_cast<char*>(g_hal.ring2Slot.mem) + g_hal.ring2Mid;
    frame.chunks[0].phyAddr = SimPhyAddr(frame.chunks[0].virAddr);
    frame.chunks[0].size = Y_SIZE - static_cast<uint32_t>(g_hal.ring2Mid);
    frame.chunks[0].stride = frame.chunks[0].size;
    frame.chunks[1].virAddr = g_hal.ring2Slot.mem;
    frame.chunks[1].phyAddr = SimPhyAddr(frame.chunks[1].virAddr);
    frame.chunks[1].size = static_cast<uint32_t>(g_hal.ring2Mid);
    frame.chunks[1].stride = frame.chunks[1].size;
    CHECK(wrapper->SetFrame(frame) == 0, "S3: SetFrame ring-crossing H264");
    g_hal.ring2Wrapper = wrapper;
    CHECK(g_producerSurface->AttachBuffer(wrapper) == SURFACE_ERROR_OK, "S3: AttachBuffer");
    Wait(&g_available);
    SurfaceBuffer* ring2 = g_consumerSurface->AcquireBuffer();
    CHECK(ring2 != nullptr, "S3: AcquireBuffer");
    GraphicFrame s3 = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    CHECK(ring2->GetFrame(s3) == 0, "S3: GetFrame");
    CHECK(s3.chunkCount == 2, "S3: two segments");
    CHECK(s3.format == GRAPHIC_FRAME_FORMAT_H264, "S3: per-frame H264 format");
    CHECK(s3.chunks[0].size + s3.chunks[1].size == Y_SIZE, "S3: segments cover the frame");
    CHECK(s3.chunks[0].virAddr == static_cast<char*>(g_hal.ring2Slot.mem) + g_hal.ring2Mid,
          "S3: head segment starts at mid");
    CHECK(s3.chunks[1].virAddr == g_hal.ring2Slot.mem, "S3: tail segment wraps to base");
    CHECK(ring2->GetVirAddr() == nullptr, "S3: legacy view invalidated");
    CHECK(ring2->GetTotalDataSize() == Y_SIZE, "S3: total data size covers segments");
    CHECK(SurfaceBuffer::DestroySurfaceBuffer(ring2) == SURFACE_ERROR_BUFFER_IN_USE,
          "S3: destroy acquired wrapper rejected");
    pthread_mutex_lock(&g_hal.lock);
    g_hal.retireTable[g_hal.retireCount++] = g_hal.ring2Wrapper;
    pthread_mutex_unlock(&g_hal.lock);
    g_consumerSurface->ReleaseBuffer(ring2);
    Wait(&g_released);
    CHECK(g_hal.retireCount == 0, "S3: retire table drained");
    CHECK(g_hal.ring2Slot.freed, "S3: driver memory returned via OnBufferReleased");
}

void RunReverseOrderProbe()
{
    Surface* consumer = Surface::CreateSurface();
    CHECK(consumer != nullptr, "S5a: spare consumer");
    uint64_t id = consumer->GetSurfaceId();
    Surface* proxy = Surface::CreateSurfaceAsProducer(id);
    CHECK(proxy != nullptr, "S5a: spare proxy");
    CHECK(proxy->GetQueueSize() == 1, "S5a: routed call while consumer lives");
    delete consumer;
    CHECK(proxy->GetQueueSize() == 0, "S5a: routed call fails after consumer death");
    CHECK(proxy->RequestBuffer() == nullptr, "S5a: request fails after death");
    CHECK(proxy->AttachBuffer(nullptr) == SURFACE_ERROR_INVALID_PARAM, "S5a: arg checks still run before routing");
    delete proxy;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(id) == nullptr, "S5a: unregistered");
}

void RunDeathNotifyProbe()
{
    Surface* consumer = Surface::CreateSurface();
    CHECK(consumer != nullptr, "S5b: spare consumer");
    uint64_t id = consumer->GetSurfaceId();
    Surface* proxy = Surface::CreateSurfaceAsProducer(id);
    CHECK(proxy != nullptr, "S5b: spare proxy");
    proxy->RegisterProducerListener(g_producerListener);
    AllocDriverMem(&g_hal.ring2Slot, 16 * 1024);
    g_hal.deathNotified = false;
    SurfaceBuffer* wrapper = SurfaceBuffer::CreateSurfaceBuffer();
    CHECK(wrapper != nullptr, "S5b: probe wrapper");
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    frame.chunkCount = 1;
    frame.chunks[0] = MakeChunk(g_hal.ring2Slot.mem, g_hal.ring2Slot.size);
    CHECK(wrapper->SetFrame(frame) == 0, "S5b: probe SetFrame");
    wrapper->SetSize(static_cast<uint32_t>(g_hal.ring2Slot.size));
    CHECK(proxy->AttachBuffer(wrapper) == SURFACE_ERROR_OK, "S5b: attach probe wrapper");
    delete consumer;
    CHECK(g_hal.deathNotified, "S5b: OnConsumerSurfaceDestroyed fired");
    free(g_hal.ring2Slot.mem);
    g_hal.ring2Slot.mem = nullptr;
    g_hal.ring2Slot.freed = true;
    delete proxy;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(id) == nullptr, "S5b: unregistered");
}

void RunInFlightNoListenerProbe()
{
    Surface* consumer = Surface::CreateSurface();
    CHECK(consumer != nullptr, "S5c: spare consumer");
    uint64_t id = consumer->GetSurfaceId();
    Surface* proxy = Surface::CreateSurfaceAsProducer(id);
    CHECK(proxy != nullptr, "S5c: spare proxy");
    // No producer listener registered: the queue destructor deletes the
    // in-flight wrapper itself (K warning path) and the driver memory is
    // returned from the HAL's own records afterwards.
    AllocDriverMem(&g_hal.ring2Slot, 8 * 1024);
    SurfaceBuffer* wrapper = SurfaceBuffer::CreateSurfaceBuffer();
    CHECK(wrapper != nullptr, "S5c: probe wrapper");
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    frame.chunks[0] = MakeChunk(g_hal.ring2Slot.mem, g_hal.ring2Slot.size);
    CHECK(wrapper->SetFrame(frame) == 0, "S5c: probe SetFrame");
    wrapper->SetSize(static_cast<uint32_t>(g_hal.ring2Slot.size));
    CHECK(proxy->AttachBuffer(wrapper) == SURFACE_ERROR_OK, "S5c: attach in-flight wrapper");
    delete consumer;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(id) == nullptr, "S5c: unregistered");
    free(g_hal.ring2Slot.mem);
    g_hal.ring2Slot.mem = nullptr;
    g_hal.ring2Slot.freed = true;
    delete proxy;
}

void RunFlowControl()
{
    Surface* consumer = Surface::CreateSurface();
    CHECK(consumer != nullptr, "S6: spare consumer");
    consumer->SetQueueSize(2);
    Surface* proxy = Surface::CreateSurfaceAsProducer(consumer->GetSurfaceId());
    CHECK(proxy != nullptr, "S6: spare proxy");
    SurfaceBuffer* wrappers[3] = { nullptr, nullptr, nullptr };
    for (int i = 0; i < 3; i++) {
        wrappers[i] = SurfaceBuffer::CreateSurfaceBuffer();
        CHECK(wrappers[i] != nullptr, "S6: wrapper");
        GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
        frame.chunks[0] = MakeChunk(nullptr, 1024);
        CHECK(wrappers[i]->SetFrame(frame) == 0, "S6: SetFrame");
        wrappers[i]->SetSize(1024);
    }
    CHECK(proxy->AttachBuffer(wrappers[0]) == SURFACE_ERROR_OK, "S6: attach 1/2");
    CHECK(proxy->AttachBuffer(wrappers[1]) == SURFACE_ERROR_OK, "S6: attach 2/2");
    CHECK(proxy->IsQueueFull(), "S6: queue full at queueSize");
    CHECK(proxy->AttachBuffer(wrappers[2]) == SURFACE_ERROR_INVALID_REQUEST,
          "S6: attach beyond queueSize rejected");
    for (int i = 0; i < 2; i++) {
        SurfaceBuffer* buf = consumer->AcquireBuffer();
        CHECK(buf != nullptr, "S6: acquire drains the queue");
        consumer->ReleaseBuffer(buf);
    }
    CHECK(!proxy->IsQueueFull(), "S6: queue not full after drain");
    for (int i = 0; i < 3; i++) {
        CHECK(SurfaceBuffer::DestroySurfaceBuffer(wrappers[i]) == SURFACE_ERROR_OK,
              "S6: destroy idle wrapper");
    }
    delete proxy;
    delete consumer;
}

struct ConcurrentCtx {
    Surface* consumer;
    Surface* proxy;
    DriverSlot slots[RING_NUM];
    SurfaceBuffer* wrappers[RING_NUM] = { nullptr, nullptr, nullptr };
    int producerFailures;
};

ConcurrentCtx g_concurrent;

void* ConcurrentProducerThread(void*)
{
    // Real producer thread (delta-spec 5.1): SetFrame + attach happen on the
    // HAL call stack while the consumer acquires/releases on its own thread.
    for (int frameNo = 0; frameNo < RING_FRAMES; frameNo++) {
        int slot = frameNo % RING_NUM;
        GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NV12, .chunkCount = 2 };
        frame.chunks[0] = MakeChunk(g_concurrent.slots[slot].mem, Y_SIZE);
        frame.chunks[1] = MakeChunk(static_cast<char*>(g_concurrent.slots[slot].mem) + Y_SIZE, UV_SIZE);
        if (g_concurrent.wrappers[slot]->SetFrame(frame) != 0 ||
            g_concurrent.wrappers[slot]->SetInt32(DRIVER_BUF_ID_KEY, frameNo) != 0 ||
            g_concurrent.proxy->AttachBuffer(g_concurrent.wrappers[slot]) != SURFACE_ERROR_OK) {
            g_concurrent.producerFailures++;
            Signal(&g_available);
            return nullptr;
        }
        Signal(&g_available);
        Wait(&g_released); // ring reuse: wait until the consumer releases this slot
    }
    return nullptr;
}

void RunConcurrentRing()
{
    g_concurrent = ConcurrentCtx();
    g_concurrent.consumer = Surface::CreateSurface();
    CHECK(g_concurrent.consumer != nullptr, "S7: spare consumer");
    g_concurrent.consumer->SetQueueSize(RING_NUM);
    // No consumer listener here on purpose: OnBufferAvailable would signal
    // g_available in addition to the producer's own signal and let the
    // consumer lap the producer (bool-flag handshake must stay 1:1). The
    // listener path itself is already covered by S1-S3.
    g_concurrent.proxy = Surface::CreateSurfaceAsProducer(g_concurrent.consumer->GetSurfaceId());
    CHECK(g_concurrent.proxy != nullptr, "S7: spare proxy");
    for (int i = 0; i < RING_NUM; i++) {
        AllocDriverMem(&g_concurrent.slots[i], Y_SIZE + UV_SIZE);
        g_concurrent.wrappers[i] = SurfaceBuffer::CreateSurfaceBuffer();
        CHECK(g_concurrent.wrappers[i] != nullptr, "S7: wrapper");
    }
    pthread_t producer;
    pthread_create(&producer, nullptr, ConcurrentProducerThread, nullptr);
    int consumed = 0;
    for (int frameNo = 0; frameNo < RING_FRAMES; frameNo++) {
        Wait(&g_available);
        SurfaceBuffer* buf = g_concurrent.consumer->AcquireBuffer();
        if (buf == nullptr) {
            // Producer thread stopped early (attach failure): do not wait again.
            g_fail++;
            printf("[FAIL] S7: consumer thread acquire (line %d)\n", __LINE__);
            break;
        }
        GraphicFrame vf = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
        CHECK(buf->GetFrame(vf) == 0, "S7: GetFrame");
        CHECK(vf.chunkCount == 2 && vf.format == GRAPHIC_FRAME_FORMAT_NV12, "S7: NV12 frame view");
        CHECK(buf->GetTotalDataSize() == NV12_TOTAL, "S7: total data size sums planes");
        int32_t bufId = -1;
        CHECK(buf->GetInt32(DRIVER_BUF_ID_KEY, bufId) == 0 && bufId == frameNo,
              "S7: frame id handed over across threads");
        g_concurrent.consumer->ReleaseBuffer(buf);
        consumed++;
        Signal(&g_released);
    }
    pthread_join(producer, nullptr);
    CHECK(g_concurrent.producerFailures == 0, "S7: producer thread attach loop clean");
    CHECK(consumed == RING_FRAMES, "S7: all frames consumed across threads");
    uint64_t concurrentId = g_concurrent.consumer->GetSurfaceId();
    for (int i = 0; i < RING_NUM; i++) {
        CHECK(SurfaceBuffer::DestroySurfaceBuffer(g_concurrent.wrappers[i]) == SURFACE_ERROR_OK,
              "S7: destroy idle wrapper");
        free(g_concurrent.slots[i].mem);
        g_concurrent.slots[i].mem = nullptr;
        g_concurrent.slots[i].freed = true;
    }
    delete g_concurrent.proxy;
    delete g_concurrent.consumer;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(concurrentId) == nullptr,
          "S7: unregistered");
}

void RunTeardown()
{
    pthread_join(g_producerThread, nullptr);
    delete g_producerSurface;
    delete g_consumerSurface;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(g_surfaceId) == nullptr,
          "S4: surface unregistered on destruction");
}

int Run()
{
    RunBasics();
    RunFreshWrapperChecks();
    RunRgbIdentity();
    RunNv12Ring();
    RunRingCrossingInUse();
    RunFlowControl();
    RunReverseOrderProbe();
    RunDeathNotifyProbe();
    RunInFlightNoListenerProbe();
    RunConcurrentRing();
    RunTeardown();
    printf("\n==== surface_lite single process example: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
} // namespace
} // namespace OHOS

int main()
{
    return OHOS::Run();
}
