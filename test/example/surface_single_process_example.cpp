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
 * consumer of the same process. Covered scenarios:
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
 *  S8  mid-traffic consumer death: producer thread running while the consumer
 *      surface is destroyed (death notify, queue deletes the in-flight
 *      wrapper, producer calls fail cleanly, driver memory from own records)
 *  S9  mid-traffic producer stop: producer thread exits first, the queued
 *      frame is still consumable, consumer drains and releases everything
 *  S10 one-way attach: ownership transfer — queue disposes at release (after
 *      the observational callback) or at consumer destruction; rejection
 *      paths (no release function / no frame / no listener)
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
    if (posix_memalign(&slot->mem, 64, size) != 0) { // 64: driver buffer alignment (bytes)
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

void DriverSlotRelease(const GraphicFrame* frame, void* context)
{
    (void)frame;
    DriverSlot* slot = static_cast<DriverSlot*>(context);
    if (slot == nullptr || slot->mem == nullptr) {
        return;
    }
    free(slot->mem);
    slot->mem = nullptr;
    slot->freed = true;
}

class ConsumerListener : public IBufferConsumerListener {
public:
    void OnBufferAvailable() override
    {
        Signal(&g_available);
    }
};

class EmptyListener : public IProducerListener {
public:
    void OnBufferReleased(SurfaceBuffer* buffer) override
    {
        (void)buffer;
    }

    void OnConsumerSurfaceDestroyed() override {}
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
            // Retire = destroy only: the release function returns the memory.
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
EmptyListener g_emptyListener;
ProducerListener g_producerListener;

class OneWayListener : public IProducerListener {
public:
    void OnBufferReleased(SurfaceBuffer* buffer) override
    {
        releasedCount++;
        // The wrapper is still readable inside the callback (destroyed right after).
        int32_t id = -1;
        if (buffer->GetInt32(DRIVER_BUF_ID_KEY, id) == 0) {
            lastId = id;
        }
    }

    void OnConsumerSurfaceDestroyed() override
    {
        deathNotified = true;
    }

    int releasedCount = 0;
    int lastId = -1;
    bool deathNotified = false;
};

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
    frame.drvHandle = slotIdx;
    frame.context = &g_hal.ringSlots[slotIdx];
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
    CHECK(g_consumerSurface->AcquireBuffer() == nullptr, "S0: acquire on empty queue returns null");
    CHECK(!g_producerSurface->IsQueueFull(), "S0: queue not full before attach");
    pthread_create(&g_producerThread, nullptr, [](void*)->void* { return nullptr; }, nullptr);
}

void RunFreshWrapperChecks()
{
    SurfaceBuffer* fresh = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
    CHECK(fresh != nullptr, "S0: CreateSurfaceBuffer");
    fresh->SetSize(1024); // 1024: test buffer size (bytes)
    CHECK(fresh->GetTotalDataSize() == 1024, "S0: fresh wrapper total size follows SetSize");
    GraphicFrame outFrame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    CHECK(fresh->GetFrame(outFrame) == 0, "S0: GetFrame on fresh wrapper");
    CHECK(outFrame.chunkCount == 1, "S0: fresh wrapper chunk count defaults to 1");
    CHECK(outFrame.format == GRAPHIC_FRAME_FORMAT_NONE, "S0: fresh wrapper format defaults to NONE");
    CHECK(outFrame.chunks[0].virAddr == nullptr && outFrame.chunks[0].size == 1024,
          "S0: synthesized chunk content");
    // Closed format domain (SetFrame rejection paths).
    GraphicFrame badFrame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    badFrame.chunks[0] = MakeChunk(nullptr, 16); // 16: dummy segment size (bytes)
    badFrame.format = static_cast<GraphicFrameFormat>(GRAPHIC_FRAME_FORMAT_H264 + 1);
    CHECK(fresh->SetFrame(badFrame) == -1, "S0: out-of-domain format rejected");
    badFrame.format = GRAPHIC_FRAME_FORMAT_JPEG;
    badFrame.chunkCount = 0;
    CHECK(fresh->SetFrame(badFrame) == -1, "S0: zero chunk count rejected");
    badFrame.chunkCount = static_cast<uint8_t>(GRAPHIC_FRAME_MAX_CHUNK_NUM + 1);
    CHECK(fresh->SetFrame(badFrame) == -1, "S0: chunk count overflow rejected");
    // Valid JPEG frame round trip; a rejected write leaves the stored tag unchanged.
    GraphicFrame jpegFrame = { .format = GRAPHIC_FRAME_FORMAT_JPEG, .chunkCount = 1 };
    jpegFrame.chunks[0] = MakeChunk(nullptr, 16); // 16: dummy segment size (bytes)
    CHECK(fresh->SetFrame(jpegFrame) == 0, "S0: JPEG frame round trip set");
    GraphicFrame outJpeg = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    CHECK(fresh->GetFrame(outJpeg) == 0 && outJpeg.format == GRAPHIC_FRAME_FORMAT_JPEG,
          "S0: JPEG frame round trip get");
    CHECK(fresh->SetFrame(badFrame) == -1, "S0: rejected write leaves tag unchanged");
    CHECK(fresh->GetFrame(outJpeg) == 0 && outJpeg.format == GRAPHIC_FRAME_FORMAT_JPEG,
          "S0: tag unchanged after rejection");
    // Clear the tag back to NONE (S1 re-points this wrapper to RGB memory).
    GraphicFrame noneFrame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    noneFrame.chunks[0] = MakeChunk(nullptr, 16); // 16: dummy segment size (bytes)
    CHECK(fresh->SetFrame(noneFrame) == 0, "S0: clear tag back to NONE");
    CHECK(fresh->GetFrame(outJpeg) == 0 && outJpeg.format == GRAPHIC_FRAME_FORMAT_NONE,
          "S0: tag cleared");
    CHECK(!g_consumerSurface->ReleaseBuffer(fresh), "S0: release unattached buffer rejected");
    SurfaceBuffer* spare = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
    CHECK(spare != nullptr, "S0: spare wrapper");
    CHECK(SurfaceBuffer::DestroySurfaceBuffer(spare) == SURFACE_ERROR_OK,
          "S0: destroy never-attached wrapper");
    g_hal.rgbWrapper = fresh;
}

void RunRgbIdentity()
{
    AllocDriverMem(&g_hal.rgbSlot, 64 * 1024); // 65536: wearable RGB frame size (bytes)
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    frame.chunkCount = 1;
    frame.chunks[0] = MakeChunk(g_hal.rgbSlot.mem, g_hal.rgbSlot.size);
    frame.drvHandle = 0; // 0: RGB slot handle
    frame.context = &g_hal.rgbSlot;
    CHECK(g_hal.rgbWrapper->SetFrame(frame) == 0, "S1: SetFrame single segment");
    g_hal.rgbWrapper->SetSize(static_cast<uint32_t>(g_hal.rgbSlot.size));
    CHECK(g_producerSurface->AttachBuffer(g_hal.rgbWrapper, true) == SURFACE_ERROR_OK, "S1: attach RGB buffer");
    CHECK(g_producerSurface->AttachBuffer(g_hal.rgbWrapper, true) == SURFACE_ERROR_INVALID_REQUEST,
          "S1: double attach while queued rejected");
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
        g_hal.ringWrappers[i] = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
        CHECK(g_hal.ringWrappers[i] != nullptr, "S2: CreateSurfaceBuffer");
    }
    for (int frameNo = 0; frameNo < RING_FRAMES; frameNo++) {
        int slot = frameNo % RING_NUM;
        GraphicFrame frame = MakeNv12Frame(slot);
        CHECK(g_hal.ringWrappers[slot]->SetFrame(frame) == 0, "S2: SetFrame NV12");
        CHECK(g_hal.ringWrappers[slot]->SetInt32(DRIVER_BUF_ID_KEY, frameNo) == 0,
              "S2: stamp frame id before attach");
        CHECK(g_producerSurface->AttachBuffer(g_hal.ringWrappers[slot], true) == SURFACE_ERROR_OK,
              "S2: AttachBuffer");
        Wait(&g_available);
        SurfaceBuffer* buf = g_consumerSurface->AcquireBuffer();
        CHECK(buf != nullptr, "S2: AcquireBuffer");
        GraphicFrame vf = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
        CHECK(buf->GetFrame(vf) == 0, "S2: GetFrame");
        CHECK(vf.chunkCount == 2, "S2: NV12 chunk count is 2");
        CHECK(vf.format == GRAPHIC_FRAME_FORMAT_NV12, "S2: per-frame NV12 format");
        CHECK(vf.width == FRAME_WIDTH && vf.height == FRAME_HEIGHT, "S2: frame dimensions round trip");
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
        CHECK(g_hal.ringSlots[i].freed, "S2: release function returned driver memory");
    }
}

void RunRingCrossingInUse()
{
    AllocDriverMem(&g_hal.ring2Slot, Y_SIZE);
    g_hal.ring2Mid = Y_SIZE / 2;
    SurfaceBuffer* wrapper = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
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
    frame.drvHandle = 0; // 0: ring2 slot handle
    frame.context = &g_hal.ring2Slot;
    CHECK(wrapper->SetFrame(frame) == 0, "S3: SetFrame ring-crossing H264");
    g_hal.ring2Wrapper = wrapper;
    CHECK(g_producerSurface->AttachBuffer(wrapper, true) == SURFACE_ERROR_OK, "S3: AttachBuffer");
    Wait(&g_available);
    SurfaceBuffer* ring2 = g_consumerSurface->AcquireBuffer();
    CHECK(ring2 != nullptr, "S3: AcquireBuffer");
    GraphicFrame s3 = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    CHECK(ring2->GetFrame(s3) == 0, "S3: GetFrame");
    CHECK(s3.chunkCount == 2, "S3: two segments");
    CHECK(s3.format == GRAPHIC_FRAME_FORMAT_H264, "S3: per-frame H264 format");
    CHECK(s3.width == FRAME_WIDTH && s3.height == FRAME_HEIGHT, "S3: frame dimensions round trip");
    CHECK(s3.chunks[0].size + s3.chunks[1].size == Y_SIZE, "S3: segments cover the frame");
    CHECK(s3.chunks[0].virAddr == static_cast<char*>(g_hal.ring2Slot.mem) + g_hal.ring2Mid,
          "S3: head segment starts at mid");
    CHECK(s3.chunks[1].virAddr == g_hal.ring2Slot.mem, "S3: tail segment wraps to base");
    CHECK(ring2->GetVirAddr() == nullptr, "S3: legacy view invalidated");
    CHECK(ring2->GetTotalDataSize() == Y_SIZE, "S3: total data size covers segments");
    CHECK(SurfaceBuffer::DestroySurfaceBuffer(ring2) == SURFACE_ERROR_BUFFER_IN_USE,
          "S3: destroy acquired wrapper rejected");
    CHECK(g_producerSurface->AttachBuffer(g_hal.ring2Wrapper, true) == SURFACE_ERROR_BUFFER_IN_USE,
          "S3: double attach while acquired rejected");
    pthread_mutex_lock(&g_hal.lock);
    g_hal.retireTable[g_hal.retireCount++] = g_hal.ring2Wrapper;
    pthread_mutex_unlock(&g_hal.lock);
    g_consumerSurface->ReleaseBuffer(ring2);
    Wait(&g_released);
    CHECK(g_hal.retireCount == 0, "S3: retire table drained");
    CHECK(g_hal.ring2Slot.freed, "S3: driver memory returned by release function");
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
    CHECK(proxy->AttachBuffer(nullptr, false) == SURFACE_ERROR_INVALID_PARAM,
          "S5a: arg checks still run before routing");
    {
        // A valid wrapper still routes to NOT_READY when the consumer is gone.
        SurfaceBuffer* deadProbe = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
        CHECK(deadProbe != nullptr, "S5a: dead probe wrapper");
        GraphicFrame df = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
        df.chunks[0] = MakeChunk(nullptr, 16); // 16: dummy segment size (bytes)
        CHECK(deadProbe->SetFrame(df) == 0, "S5a: dead probe frame");
        CHECK(proxy->AttachBuffer(deadProbe, false) == SURFACE_ERROR_NOT_READY,
              "S5a: attach after consumer death rejected");
        CHECK(SurfaceBuffer::DestroySurfaceBuffer(deadProbe) == SURFACE_ERROR_OK, "S5a: cleanup probe");
    }
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
    AllocDriverMem(&g_hal.ring2Slot, 16 * 1024); // 16384: S5b probe buffer size (bytes)
    g_hal.deathNotified = false;
    SurfaceBuffer* wrapper = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
    CHECK(wrapper != nullptr, "S5b: probe wrapper");
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    frame.chunkCount = 1;
    frame.chunks[0] = MakeChunk(g_hal.ring2Slot.mem, g_hal.ring2Slot.size);
    frame.drvHandle = 0; // 0: probe slot handle
    frame.context = &g_hal.ring2Slot;
    CHECK(wrapper->SetFrame(frame) == 0, "S5b: probe SetFrame");
    wrapper->SetSize(static_cast<uint32_t>(g_hal.ring2Slot.size));
    CHECK(proxy->AttachBuffer(wrapper, false) == SURFACE_ERROR_OK, "S5b: attach probe wrapper");
    delete consumer;
    CHECK(g_hal.deathNotified, "S5b: OnConsumerSurfaceDestroyed fired");
    CHECK(g_hal.ring2Slot.freed, "S5b: release function returned driver memory");
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
    // in-flight wrapper itself and the release function returns the memory.
    AllocDriverMem(&g_hal.ring2Slot, 8 * 1024); // 8192: S5c probe buffer size (bytes)
    SurfaceBuffer* wrapper = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
    CHECK(wrapper != nullptr, "S5c: probe wrapper");
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    frame.chunks[0] = MakeChunk(g_hal.ring2Slot.mem, g_hal.ring2Slot.size);
    frame.drvHandle = 0; // 0: probe slot handle
    frame.context = &g_hal.ring2Slot;
    CHECK(wrapper->SetFrame(frame) == 0, "S5c: probe SetFrame");
    wrapper->SetSize(static_cast<uint32_t>(g_hal.ring2Slot.size));
    CHECK(proxy->AttachBuffer(wrapper, false) == SURFACE_ERROR_OK, "S5c: attach in-flight wrapper");
    delete consumer;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(id) == nullptr, "S5c: unregistered");
    CHECK(g_hal.ring2Slot.freed, "S5c: release function returned driver memory");
    delete proxy;
}

void RunFlowControl()
{
    Surface* consumer = Surface::CreateSurface();
    CHECK(consumer != nullptr, "S6: spare consumer");
    consumer->SetQueueSize(2); // 2: flow control triggers with the third attach
    Surface* proxy = Surface::CreateSurfaceAsProducer(consumer->GetSurfaceId());
    CHECK(proxy != nullptr, "S6: spare proxy");
    proxy->RegisterProducerListener(g_emptyListener);
    SurfaceBuffer* wrappers[3] = { nullptr, nullptr, nullptr };
    for (int i = 0; i < 3; i++) {
        wrappers[i] = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
        CHECK(wrappers[i] != nullptr, "S6: wrapper");
        GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
        frame.chunks[0] = MakeChunk(nullptr, 1024); // 1024: dummy frame size (bytes)
        CHECK(wrappers[i]->SetFrame(frame) == 0, "S6: SetFrame");
        wrappers[i]->SetSize(1024);
    }
    CHECK(proxy->AttachBuffer(wrappers[0], true) == SURFACE_ERROR_OK, "S6: attach 1/2");
    CHECK(proxy->AttachBuffer(wrappers[1], true) == SURFACE_ERROR_OK, "S6: attach 2/2");
    CHECK(proxy->IsQueueFull(), "S6: queue full at queueSize");
    CHECK(proxy->AttachBuffer(wrappers[2], true) == SURFACE_ERROR_INVALID_REQUEST,
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
        frame.width = FRAME_WIDTH;
        frame.height = FRAME_HEIGHT;
        frame.chunks[0] = MakeChunk(g_concurrent.slots[slot].mem, Y_SIZE);
        frame.chunks[1] = MakeChunk(static_cast<char*>(g_concurrent.slots[slot].mem) + Y_SIZE, UV_SIZE);
        frame.drvHandle = slot;
        frame.context = &g_concurrent.slots[slot];
        if (g_concurrent.wrappers[slot]->SetFrame(frame) != 0 ||
            g_concurrent.wrappers[slot]->SetInt32(DRIVER_BUF_ID_KEY, frameNo) != 0 ||
            g_concurrent.proxy->AttachBuffer(g_concurrent.wrappers[slot], true) != SURFACE_ERROR_OK) {
            g_concurrent.producerFailures++;
            Signal(&g_available);
            return nullptr;
        }
        Signal(&g_available);
        Wait(&g_released); // ring reuse: wait until the consumer releases this slot
    }
    return nullptr;
}

void DestroyRingWrappers(SurfaceBuffer* wrappers[], DriverSlot slots[], int skipIdx)
{
    for (int i = 0; i < RING_NUM; i++) {
        if (i != skipIdx) {
            CHECK(SurfaceBuffer::DestroySurfaceBuffer(wrappers[i]) == SURFACE_ERROR_OK,
                  "destroy idle wrapper");
        }
        // All driver memory goes back through the release function.
        CHECK(slots[i].freed, "release function returned driver memory");
    }
}

void VerifyConcurrentFrame(SurfaceBuffer* buf, int frameNo)
{
    GraphicFrame vf = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
    CHECK(buf->GetFrame(vf) == 0, "S7: GetFrame");
    CHECK(vf.chunkCount == 2 && vf.format == GRAPHIC_FRAME_FORMAT_NV12, "S7: NV12 frame view");
    CHECK(vf.width == FRAME_WIDTH && vf.height == FRAME_HEIGHT, "S7: frame dimensions round trip");
    CHECK(buf->GetTotalDataSize() == NV12_TOTAL, "S7: total data size sums planes");
    int32_t bufId = -1;
    CHECK(buf->GetInt32(DRIVER_BUF_ID_KEY, bufId) == 0 && bufId == frameNo,
          "S7: frame id handed over across threads");
}

int ConsumeReleasedFrames(Surface* consumer, int count)
{
    int consumed = 0;
    for (int frameNo = 0; frameNo < count; frameNo++) {
        Wait(&g_available);
        SurfaceBuffer* buf = consumer->AcquireBuffer();
        if (buf == nullptr) {
            g_fail++;
            printf("[FAIL] consumer acquire (line %d)\n", __LINE__);
            return consumed;
        }
        consumer->ReleaseBuffer(buf);
        consumed++;
        Signal(&g_released);
    }
    return consumed;
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
    // Reusable attach requires a listener (return channel); it stays silent so
    // the manual handshake remains 1:1.
    g_concurrent.proxy->RegisterProducerListener(g_emptyListener);
    for (int i = 0; i < RING_NUM; i++) {
        AllocDriverMem(&g_concurrent.slots[i], Y_SIZE + UV_SIZE);
        g_concurrent.wrappers[i] = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
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
        VerifyConcurrentFrame(buf, frameNo);
        g_concurrent.consumer->ReleaseBuffer(buf);
        consumed++;
        Signal(&g_released);
    }
    pthread_join(producer, nullptr);
    CHECK(g_concurrent.producerFailures == 0, "S7: producer thread attach loop clean");
    CHECK(consumed == RING_FRAMES, "S7: all frames consumed across threads");
    uint64_t concurrentId = g_concurrent.consumer->GetSurfaceId();
    DestroyRingWrappers(g_concurrent.wrappers, g_concurrent.slots, -1);
    delete g_concurrent.proxy;
    delete g_concurrent.consumer;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(concurrentId) == nullptr,
          "S7: unregistered");
}

struct ConcurrentDeathCtx {
    Surface* consumer;
    Surface* proxy;
    DriverSlot slots[RING_NUM];
    SurfaceBuffer* wrappers[RING_NUM] = { nullptr, nullptr, nullptr };
    bool abort;
    int produced;
};

ConcurrentDeathCtx g_deathCtx;

void* ConcurrentDeathProducer(void*)
{
    // Producer thread for the mid-traffic consumer-death scenario: the abort
    // flag is only read inside the handshake, so a dead wrapper pointer is
    // never touched again after the consumer died.
    const int total = 8; // 8: frames the death producer wants to produce (death expected at frame 5)
    for (int frameNo = 0; frameNo < total; frameNo++) {
        if (g_deathCtx.abort) {
            break;
        }
        int slot = frameNo % RING_NUM;
        GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NV12, .chunkCount = 2 };
        frame.width = FRAME_WIDTH;
        frame.height = FRAME_HEIGHT;
        frame.chunks[0] = MakeChunk(g_deathCtx.slots[slot].mem, Y_SIZE);
        frame.chunks[1] = MakeChunk(static_cast<char*>(g_deathCtx.slots[slot].mem) + Y_SIZE, UV_SIZE);
        frame.drvHandle = slot;
        frame.context = &g_deathCtx.slots[slot];
        if (g_deathCtx.wrappers[slot]->SetFrame(frame) != 0 ||
            g_deathCtx.proxy->AttachBuffer(g_deathCtx.wrappers[slot], true) != SURFACE_ERROR_OK) {
            break;
        }
        g_deathCtx.produced++;
        Signal(&g_available);
        Wait(&g_released);
    }
    return nullptr;
}

class ConcurrentDeathListener : public IProducerListener {
public:
    void OnBufferReleased(SurfaceBuffer* buffer) override
    {
        (void)buffer;
        releasedCount++;
    }

    void OnConsumerSurfaceDestroyed() override
    {
        deathNotified = true;
    }

    int releasedCount = 0;
    bool deathNotified = false;
};

void RunConcurrentConsumerDeath()
{
    g_deathCtx = ConcurrentDeathCtx();
    ConcurrentDeathListener listener;
    g_deathCtx.consumer = Surface::CreateSurface();
    CHECK(g_deathCtx.consumer != nullptr, "S8: spare consumer");
    g_deathCtx.consumer->SetQueueSize(RING_NUM);
    uint64_t id = g_deathCtx.consumer->GetSurfaceId();
    g_deathCtx.proxy = Surface::CreateSurfaceAsProducer(id);
    CHECK(g_deathCtx.proxy != nullptr, "S8: spare proxy");
    g_deathCtx.proxy->RegisterProducerListener(listener);
    for (int i = 0; i < RING_NUM; i++) {
        AllocDriverMem(&g_deathCtx.slots[i], Y_SIZE + UV_SIZE);
        g_deathCtx.wrappers[i] = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
        CHECK(g_deathCtx.wrappers[i] != nullptr, "S8: wrapper");
    }
    pthread_t producer;
    pthread_create(&producer, nullptr, ConcurrentDeathProducer, nullptr);
    const int consumedBeforeDeath = 4; // 4: frames consumed before the teardown
    int consumed = ConsumeReleasedFrames(g_deathCtx.consumer, consumedBeforeDeath);
    // Take the next frame's available signal but do NOT consume the frame:
    // frame 4 is queued (FLUSH) and the producer thread is blocked on the
    // released handshake when the teardown below runs.
    Wait(&g_available);
    // Mid-traffic teardown of the consumer: death notify fires inside delete,
    // the queue deletes the in-flight wrapper, and the producer must exit on
    // the abort flag without touching the dead wrapper again. Driver memory of
    // the in-flight frame is returned from the producer's own records.
    g_deathCtx.abort = true;
    delete g_deathCtx.consumer;
    Signal(&g_released);
    pthread_join(producer, nullptr);
    CHECK(listener.deathNotified, "S8: OnConsumerSurfaceDestroyed fired mid-traffic");
    CHECK(g_deathCtx.produced == consumedBeforeDeath + 1, "S8: one frame was in flight at teardown");
    CHECK(listener.releasedCount == consumedBeforeDeath, "S8: OnBufferReleased per consumed frame");
    CHECK(consumed == consumedBeforeDeath, "S8: frames consumed before teardown");
    int deadSlot = consumedBeforeDeath % RING_NUM; // the queued frame is frame 'consumedBeforeDeath'
    // The queued wrapper (deadSlot) was already deleted by the queue
    // destructor; both paths return the driver memory through the
    // release function.
    DestroyRingWrappers(g_deathCtx.wrappers, g_deathCtx.slots, deadSlot);
    delete g_deathCtx.proxy;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(id) == nullptr, "S8: unregistered");
}

struct ConcurrentStopCtx {
    Surface* proxy;
    DriverSlot slots[RING_NUM];
    SurfaceBuffer* wrappers[RING_NUM] = { nullptr, nullptr, nullptr };
    int produced;
};

ConcurrentStopCtx g_stopCtx;

void* ConcurrentStopProducer(void*)
{
    // Stream stop from the producer side mid-traffic: attach frames 0..3 and
    // exit without waiting for frame 3 to be consumed (it stays queued).
    const int producedFrames = 4; // 4: frames produced before the producer thread exits
    for (int frameNo = 0; frameNo < producedFrames; frameNo++) {
        int slot = frameNo % RING_NUM;
        GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NV12, .chunkCount = 2 };
        frame.width = FRAME_WIDTH;
        frame.height = FRAME_HEIGHT;
        frame.chunks[0] = MakeChunk(g_stopCtx.slots[slot].mem, Y_SIZE);
        frame.chunks[1] = MakeChunk(static_cast<char*>(g_stopCtx.slots[slot].mem) + Y_SIZE, UV_SIZE);
        frame.drvHandle = slot;
        frame.context = &g_stopCtx.slots[slot];
        if (g_stopCtx.wrappers[slot]->SetFrame(frame) != 0 ||
            g_stopCtx.proxy->AttachBuffer(g_stopCtx.wrappers[slot], true) != SURFACE_ERROR_OK) {
            return nullptr;
        }
        g_stopCtx.produced++;
        Signal(&g_available);
        if (frameNo + 1 < producedFrames) {
            Wait(&g_released);
        }
    }
    return nullptr;
}

void RunConcurrentProducerStop()
{
    g_stopCtx = ConcurrentStopCtx();
    Surface* consumer = Surface::CreateSurface();
    CHECK(consumer != nullptr, "S9: spare consumer");
    consumer->SetQueueSize(RING_NUM);
    uint64_t id = consumer->GetSurfaceId();
    g_stopCtx.proxy = Surface::CreateSurfaceAsProducer(id);
    CHECK(g_stopCtx.proxy != nullptr, "S9: spare proxy");
    // Reusable attach requires a listener (return channel).
    g_stopCtx.proxy->RegisterProducerListener(g_emptyListener);
    for (int i = 0; i < RING_NUM; i++) {
        AllocDriverMem(&g_stopCtx.slots[i], Y_SIZE + UV_SIZE);
        g_stopCtx.wrappers[i] = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
        CHECK(g_stopCtx.wrappers[i] != nullptr, "S9: wrapper");
    }
    pthread_t producer;
    pthread_create(&producer, nullptr, ConcurrentStopProducer, nullptr);
    int consumed = ConsumeReleasedFrames(consumer, 3); // 3: frames consumed before the producer exits
    // The producer exits right after attaching the last frame: the queued
    // frame must still be consumable after its thread is gone.
    pthread_join(producer, nullptr);
    CHECK(g_stopCtx.produced == 4, "S9: producer attached all frames before exiting");
    Wait(&g_available);
    SurfaceBuffer* tail = consumer->AcquireBuffer();
    CHECK(tail != nullptr, "S9: queued frame consumed after producer exit");
    if (tail != nullptr) {
        GraphicFrame vf = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 0 };
        CHECK(tail->GetFrame(vf) == 0 && vf.chunkCount == 2 && vf.format == GRAPHIC_FRAME_FORMAT_NV12,
              "S9: queued frame intact after producer exit");
        consumer->ReleaseBuffer(tail);
        consumed++;
    }
    CHECK(consumed == 4, "S9: all frames drained across the producer stop");
    DestroyRingWrappers(g_stopCtx.wrappers, g_stopCtx.slots, -1);
    delete g_stopCtx.proxy;
    delete consumer;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(id) == nullptr, "S9: unregistered");
}

void OneWayRejectionProbes(Surface* proxy, const GraphicFrame& frame)
{
    // One-way wrapper without a release function.
    SurfaceBuffer* noHook = SurfaceBuffer::CreateSurfaceBuffer(nullptr);
    CHECK(noHook != nullptr, "S10: wrapper without release function");
    CHECK(noHook->SetFrame(frame) == 0, "S10: frame on no-hook wrapper");
    CHECK(proxy->AttachBuffer(noHook, false) == SURFACE_ERROR_INVALID_REQUEST,
          "S10: one-way without release function rejected");
    CHECK(SurfaceBuffer::DestroySurfaceBuffer(noHook) == SURFACE_ERROR_OK, "S10: cleanup no-hook wrapper");

    // Wrapper without a frame description.
    SurfaceBuffer* noFrame = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
    CHECK(noFrame != nullptr, "S10: wrapper without frame");
    CHECK(proxy->AttachBuffer(noFrame, false) == SURFACE_ERROR_INVALID_REQUEST,
          "S10: attach without frame rejected");
    CHECK(SurfaceBuffer::DestroySurfaceBuffer(noFrame) == SURFACE_ERROR_OK, "S10: cleanup no-frame wrapper");

    // Reusable attach without a listener (separate surface), with real
    // credentials: the self-destroy after the failed attach returns memory.
    AllocDriverMem(&g_hal.ring2Slot, 4 * 1024); // 4096: real-credentials probe buffer size (bytes)
    Surface* bareConsumer = Surface::CreateSurface();
    Surface* bareProxy = Surface::CreateSurfaceAsProducer(bareConsumer->GetSurfaceId());
    SurfaceBuffer* bare = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
    CHECK(bare != nullptr, "S10: bare wrapper");
    GraphicFrame realFrame = frame;
    realFrame.drvHandle = 3; // 3: probe slot handle
    realFrame.context = &g_hal.ring2Slot;
    CHECK(bare->SetFrame(realFrame) == 0, "S10: frame on bare wrapper");
    CHECK(bareProxy->AttachBuffer(bare, true) == SURFACE_ERROR_NOT_READY,
          "S10: reusable without listener rejected");
    CHECK(SurfaceBuffer::DestroySurfaceBuffer(bare) == SURFACE_ERROR_OK, "S10: cleanup bare wrapper");
    CHECK(g_hal.ring2Slot.freed, "S10: self-destroy after failed attach returns memory");
    delete bareProxy;
    delete bareConsumer;
}

void OneWayDisposeFlow(Surface* consumer, Surface* proxy, OneWayListener& listener)
{
    // One-way attach, consume, release — the queue disposes after the
    // observational callback.
    AllocDriverMem(&g_hal.ring2Slot, 8 * 1024); // 8192: one-way probe buffer size (bytes)
    SurfaceBuffer* w1 = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
    CHECK(w1 != nullptr, "S10: one-way wrapper");
    GraphicFrame f1 = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    f1.chunks[0] = MakeChunk(g_hal.ring2Slot.mem, g_hal.ring2Slot.size);
    f1.drvHandle = 1; // 1: one-way probe slot handle
    f1.context = &g_hal.ring2Slot;
    CHECK(w1->SetFrame(f1) == 0, "S10: one-way SetFrame");
    w1->SetSize(static_cast<uint32_t>(g_hal.ring2Slot.size));
    CHECK(w1->SetInt32(DRIVER_BUF_ID_KEY, 42) == 0, "S10: stamp frame id"); // 42: stamped test frame id
    CHECK(proxy->AttachBuffer(w1, false) == SURFACE_ERROR_OK, "S10: one-way attach");
    SurfaceBuffer* buf = consumer->AcquireBuffer();
    CHECK(buf != nullptr, "S10: acquire one-way frame");
    consumer->ReleaseBuffer(buf);
    CHECK(listener.releasedCount == 1 && listener.lastId == 42,
          "S10: callback before dispose, wrapper readable inside");
    CHECK(g_hal.ring2Slot.freed, "S10: release function returned driver memory");
}

void OneWayUnconsumedFlow(Surface* consumer, Surface* proxy, OneWayListener& listener)
{
    // A one-way frame left in the queue at consumer destruction.
    AllocDriverMem(&g_hal.ring2Slot, 8 * 1024); // 8192: unconsumed probe buffer size (bytes)
    SurfaceBuffer* w2 = SurfaceBuffer::CreateSurfaceBuffer(DriverSlotRelease);
    CHECK(w2 != nullptr, "S10: unconsumed wrapper");
    GraphicFrame f2 = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    f2.chunks[0] = MakeChunk(g_hal.ring2Slot.mem, g_hal.ring2Slot.size);
    f2.drvHandle = 2; // 2: unconsumed probe slot handle
    f2.context = &g_hal.ring2Slot;
    CHECK(w2->SetFrame(f2) == 0, "S10: second SetFrame");
    w2->SetSize(static_cast<uint32_t>(g_hal.ring2Slot.size));
    CHECK(proxy->AttachBuffer(w2, false) == SURFACE_ERROR_OK, "S10: attach unconsumed wrapper");
    listener.deathNotified = false;
    delete consumer;
    CHECK(listener.deathNotified, "S10: death notify with one-way in flight");
    CHECK(g_hal.ring2Slot.freed, "S10: queue destruction returned driver memory");
}

void RunOneWayScenario()
{
    OneWayListener listener;
    Surface* consumer = Surface::CreateSurface();
    CHECK(consumer != nullptr, "S10: spare consumer");
    consumer->SetQueueSize(2); // 2: allows one attach plus one rejection probe
    uint64_t id = consumer->GetSurfaceId();
    Surface* proxy = Surface::CreateSurfaceAsProducer(id);
    CHECK(proxy != nullptr, "S10: spare proxy");
    proxy->RegisterProducerListener(listener);
    GraphicFrame frame = { .format = GRAPHIC_FRAME_FORMAT_NONE, .chunkCount = 1 };
    frame.chunks[0] = MakeChunk(nullptr, 16); // 16: dummy segment size (bytes)
    OneWayRejectionProbes(proxy, frame);
    OneWayDisposeFlow(consumer, proxy, listener);
    OneWayUnconsumedFlow(consumer, proxy, listener);
    delete proxy;
    CHECK(SurfaceUtils::GetInstance()->GetSurface(id) == nullptr, "S10: unregistered");
}

void RunTeardown()
{
    pthread_join(g_producerThread, nullptr);
    // All in-flight wrappers are retired: unregistering is safe now.
    g_producerSurface->UnregisterProducerListener();
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
    RunConcurrentConsumerDeath();
    RunConcurrentProducerStop();
    RunOneWayScenario();
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
