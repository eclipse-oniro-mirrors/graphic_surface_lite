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

/*
 * Single process full-flow example for surface_lite (SURFACE_LITE_SINGLE_PROCESS=1):
 * a camera HAL producer thread handing driver-owned buffers to a consumer
 * (graphic framework) thread of the same process.
 *
 * Covered scenarios:
 *  S0  basics: surface id allocation, producer proxy by id, unknown id, internal
 *      allocation unavailability, format round-trips, fresh wrapper chunk defaults
 *  S1  RGB single segment: GetChunk(0) identity; queued (FLUSH) Detach synchronous
 *  S2  NV12 ring: dual plane + out-of-order driver delivery sorted by the HAL
 *      adapter, skip/range SetChunk rejection, legacy view invalidation, extra
 *      data surviving release, ring re-attach reuse, idle (RELEASE) Detach
 *  S3  ring buffer crossing two segments + H264 format: Detach in ACQUIRED state
 *      returns SURFACE_ERROR_BUFFER_IN_USE, pending recycle table,
 *      OnBufferReleased -> hit -> detach + driver memory return (delta-spec 5.2)
 *
 * The example deliberately uses only kits interfaces plus the two innerkits
 * headers needed for error codes and the id registry (buffer_common.h,
 * surface_utils.h).
 */

#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "buffer_common.h"      // innerkits: SURFACE_ERROR_BUFFER_IN_USE etc.
#include "surface.h"            // kits
#include "surface_utils.h"      // innerkits: id registry (teardown assertion)
#include "surface_type.h"       // kits: format constants

namespace OHOS {
namespace {
constexpr int32_t FRAME_WIDTH = 320;
constexpr int32_t FRAME_HEIGHT = 240;
constexpr uint32_t Y_SIZE = FRAME_WIDTH * FRAME_HEIGHT;             // 76800
constexpr uint32_t UV_SIZE = FRAME_WIDTH * FRAME_HEIGHT / 2;        // 38400
constexpr uint32_t NV12_TOTAL = Y_SIZE + UV_SIZE;
constexpr uint32_t DRIVER_BUF_ID_KEY = 1001;   // HAL-private extra data key
constexpr int RING_NUM = 3;
constexpr int RING_FRAMES = 6;

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            printf("[FAIL] %s (line %d)\n", msg, __LINE__);              \
        }                                                                 \
    } while (0)

/* ---------------------------------------------------------------- demo sync */
pthread_mutex_t g_demoLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_demoCond = PTHREAD_COND_INITIALIZER;
bool g_available = false;   // OnBufferAvailable fired
bool g_released = false;    // OnBufferReleased fired

void DemoSignal(bool* flag)
{
    pthread_mutex_lock(&g_demoLock);
    *flag = true;
    pthread_cond_broadcast(&g_demoCond);
    pthread_mutex_unlock(&g_demoLock);
}

void DemoWait(bool* flag)
{
    pthread_mutex_lock(&g_demoLock);
    while (!*flag) {
        pthread_cond_wait(&g_demoCond, &g_demoLock);
    }
    *flag = false;
    pthread_mutex_unlock(&g_demoLock);
}

/* ------------------------------------------------------------- demo HAL ctx */
struct DriverSlot {
    void* mem;          // simulated driver memory (aligned)
    size_t size;
    bool freed;
};

struct HalCtx {
    Surface* producer;  // producer proxy surface
    DriverSlot rgbSlot;
    DriverSlot ringSlots[RING_NUM];
    DriverSlot ring2Slot;
    SurfaceBuffer* rgbWrapper = nullptr;
    SurfaceBuffer* ringWrappers[RING_NUM] = { nullptr, nullptr, nullptr };
    SurfaceBuffer* ring2Wrapper = nullptr;
    uint64_t ring2Mid = 0;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    SurfaceBuffer* pendingRecycle[RING_NUM + 2] = { nullptr };  // "待回收表"
    int pendingCount = 0;
    int releasedRingCount = 0;
    int detachedCount = 0;
};

HalCtx g_hal;

void AllocDriverMem(DriverSlot* slot, size_t size)
{
    slot->size = 0;
    slot->mem = nullptr;
    slot->freed = false;
    if (posix_memalign(&slot->mem, 64, size) != 0) {
        // Simulated driver allocation failure: keep the slot consistent and terminate.
        printf("[FAIL] AllocDriverMem: posix_memalign(%zu) failed\n", size);
        exit(1);
    }
    slot->size = size;
    memset(slot->mem, 0xAA, size);  // driver "fills" the frame
}

uint64_t SimPhyAddr(void* addr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(addr));
}

int FindWrapperSlot(SurfaceBuffer* wrapper)
{
    if (wrapper == g_hal.ring2Wrapper) {
        return RING_NUM + 1;
    }
    for (int i = 0; i < RING_NUM; i++) {
        if (wrapper == g_hal.ringWrappers[i]) {
            return i;
        }
    }
    if (wrapper == g_hal.rgbWrapper) {
        return RING_NUM;  // rgb slot
    }
    return -1;
}

void ReturnDriverMemory(SurfaceBuffer* wrapper)
{
    int slotIdx = FindWrapperSlot(wrapper);
    if (slotIdx < 0) {
        return;
    }
    if (slotIdx == RING_NUM) {
        free(g_hal.rgbSlot.mem);
        g_hal.rgbSlot.mem = nullptr;
        g_hal.rgbSlot.freed = true;
    } else if (slotIdx == RING_NUM + 1) {
        free(g_hal.ring2Slot.mem);
        g_hal.ring2Slot.mem = nullptr;
        g_hal.ring2Slot.freed = true;
    } else {
        free(g_hal.ringSlots[slotIdx].mem);
        g_hal.ringSlots[slotIdx].mem = nullptr;
        g_hal.ringSlots[slotIdx].freed = true;
    }
}

/* --------------------------------------------------------------- listeners */
class DemoConsumerListener : public IBufferConsumerListener {
public:
    void OnBufferAvailable() override
    {
        DemoSignal(&g_available);
    }
};

class DemoProducerListener : public IProducerListener {
public:
    void OnBufferReleased(SurfaceBuffer* buffer) override
    {
        // Runs in the consumer thread (release context), outside the queue lock:
        // re-entering the surface here is safe.
        pthread_mutex_lock(&g_hal.lock);
        int idx = -1;
        for (int i = 0; i < g_hal.pendingCount; i++) {
            if (g_hal.pendingRecycle[i] == buffer) {
                idx = i;
                break;
            }
        }
        if (idx >= 0) {
            for (int i = idx; i < g_hal.pendingCount - 1; i++) {
                g_hal.pendingRecycle[i] = g_hal.pendingRecycle[i + 1];
            }
            g_hal.pendingRecycle[--g_hal.pendingCount] = nullptr;
            pthread_mutex_unlock(&g_hal.lock);
            // Hit: the buffer was in the pending recycle table because DetachBuffer
            // returned IN_USE earlier. It is idle now: detach synchronously and
            // return the driver memory.
            int32_t ret = g_hal.producer->DetachBuffer(buffer);
            CHECK(ret == SURFACE_ERROR_OK, "OnBufferReleased: pending detach must succeed");
            ReturnDriverMemory(buffer);
        } else {
            // Miss: keep for ring reuse; the producer loop re-attaches it.
            g_hal.releasedRingCount++;
            pthread_mutex_unlock(&g_hal.lock);
        }
        DemoSignal(&g_released);
    }
};

DemoConsumerListener g_consumerListener;
DemoProducerListener g_producerListener;

/* --------------------------------------------------- producer (HAL) thread */
enum JobType {
    JOB_NONE = 0,
    JOB_S1_SETUP_ATTACH,
    JOB_S1_FLUSH_DETACH,
    JOB_S2_ATTACH,      // arg = frame number; slot = arg % RING_NUM (0 = first frame)
    JOB_S2_DETACH_ALL,
    JOB_S3_SETUP_ATTACH,
    JOB_S3_DETACH_INUSE,
    JOB_QUIT,
};

struct Job {
    JobType type = JOB_NONE;
    int arg = 0;
    int32_t result = -1;
};

pthread_mutex_t g_jobLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_jobCond = PTHREAD_COND_INITIALIZER;
Job g_job;

void SubmitJob(JobType type, int arg, int32_t* result)
{
    pthread_mutex_lock(&g_jobLock);
    g_job.type = type;
    g_job.arg = arg;
    g_job.result = -1;
    pthread_cond_broadcast(&g_jobCond);
    while (g_job.type != JOB_NONE) {
        pthread_cond_wait(&g_jobCond, &g_jobLock);
    }
    if (result != nullptr) {
        *result = g_job.result;
    }
    pthread_mutex_unlock(&g_jobLock);
}

SurfaceDataChunk MakeChunk(void* mem, size_t size, uint8_t planeId)
{
    SurfaceDataChunk chunk;
    chunk.virAddr = mem;
    chunk.phyAddr = SimPhyAddr(mem);
    chunk.size = static_cast<uint32_t>(size);
    chunk.stride = static_cast<uint32_t>(size);
    chunk.planeId = planeId;
    return chunk;
}

int32_t SetupNv12Wrapper(int slotIdx, int frameNo)
{
    // Driver delivers the UV segment first (out of order). The HAL adapter
    // sorts segments by plane id and writes them in ascending order.
    if (g_hal.ringWrappers[slotIdx] == nullptr) {
        AllocDriverMem(&g_hal.ringSlots[slotIdx], Y_SIZE + UV_SIZE);
        void* yMem = g_hal.ringSlots[slotIdx].mem;
        void* uvMem = static_cast<char*>(g_hal.ringSlots[slotIdx].mem) + Y_SIZE;
        SurfaceBuffer* wrapper = SurfaceBuffer::CreateSurfaceBuffer();
        CHECK(wrapper != nullptr, "S2: CreateSurfaceBuffer");
        SurfaceDataChunk uvChunk = MakeChunk(uvMem, UV_SIZE, 1);
        // Skip rejection: index 1 with count 0 must fail.
        CHECK(wrapper->SetChunk(1, uvChunk) == -1, "S2: SetChunk skip must be rejected");
        // Range rejection: index >= SURFACE_MAX_CHUNK_NUM must fail.
        CHECK(wrapper->SetChunk(SURFACE_MAX_CHUNK_NUM, uvChunk) == -1,
              "S2: SetChunk out of range must be rejected");
        SurfaceDataChunk yChunk = MakeChunk(yMem, Y_SIZE, 0);
        CHECK(wrapper->SetChunk(0, yChunk) == 0, "S2: SetChunk plane0");
        CHECK(wrapper->SetChunk(1, uvChunk) == 0, "S2: SetChunk plane1 after sort");
        wrapper->SetSize(NV12_TOTAL);  // must not be rejected by capacity check
        wrapper->SetBufferFormat(SURFACE_BUFFER_FORMAT_NV12);  // per-frame format tag
        wrapper->SetInt32(DRIVER_BUF_ID_KEY, frameNo);
        g_hal.ringWrappers[slotIdx] = wrapper;
    } else {
        g_hal.ringWrappers[slotIdx]->SetInt32(DRIVER_BUF_ID_KEY, frameNo);
    }
    return g_hal.producer->AttachBuffer(g_hal.ringWrappers[slotIdx]);
}

/* -------------------------------------------------- producer (HAL) handlers */
int32_t HandleRgbSetupAttach()
{
    // Single-segment RGB frame: chunk 0 identity view.
    SurfaceDataChunk chunk = MakeChunk(g_hal.rgbSlot.mem, g_hal.rgbSlot.size, 0);
    int32_t ret = g_hal.rgbWrapper->SetChunk(0, chunk);  // overwrite fresh wrapper slot
    CHECK(ret == 0, "S1: SetChunk overwrite single segment");
    g_hal.rgbWrapper->SetSize(static_cast<uint32_t>(g_hal.rgbSlot.size));
    // RGB is out of the closed four-format domain: the tag stays NONE
    // (surface-level format applies), demonstrating the default path.
    return g_hal.producer->AttachBuffer(g_hal.rgbWrapper);
}

int32_t HandleRgbFlushDetach()
{
    // Buffer is queued (FLUSH) and has not been acquired: detach takes effect
    // synchronously.
    int32_t ret = g_hal.producer->DetachBuffer(g_hal.rgbWrapper);
    if (ret == SURFACE_ERROR_OK) {
        ReturnDriverMemory(g_hal.rgbWrapper);
    }
    return ret;
}

int32_t HandleRingDetachAll()
{
    // Stream stop: all ring wrappers are idle (RELEASE).
    int32_t ret = SURFACE_ERROR_OK;
    for (int i = 0; i < RING_NUM; i++) {
        int32_t r = g_hal.producer->DetachBuffer(g_hal.ringWrappers[i]);
        CHECK(r == SURFACE_ERROR_OK, "S2: idle detach at stream stop");
        if (r != SURFACE_ERROR_OK) {
            ret = r;
        }
        ReturnDriverMemory(g_hal.ringWrappers[i]);
        g_hal.detachedCount++;
    }
    return ret;
}

int32_t HandleRingCrossingSetupAttach()
{
    // Ring buffer frame crossing two segments of one driver block: logical data
    // = [mid..end] (chunk 0) followed by [0..mid) (chunk 1).
    AllocDriverMem(&g_hal.ring2Slot, Y_SIZE);
    g_hal.ring2Mid = Y_SIZE / 2;
    SurfaceBuffer* wrapper = SurfaceBuffer::CreateSurfaceBuffer();
    CHECK(wrapper != nullptr, "S3: CreateSurfaceBuffer");
    SurfaceDataChunk head;
    head.virAddr = static_cast<char*>(g_hal.ring2Slot.mem) + g_hal.ring2Mid;
    head.phyAddr = SimPhyAddr(head.virAddr);
    head.size = Y_SIZE - static_cast<uint32_t>(g_hal.ring2Mid);
    head.stride = head.size;
    head.planeId = 0;
    SurfaceDataChunk tail;
    tail.virAddr = g_hal.ring2Slot.mem;
    tail.phyAddr = SimPhyAddr(tail.virAddr);
    tail.size = static_cast<uint32_t>(g_hal.ring2Mid);
    tail.stride = tail.size;
    tail.planeId = 0;
    CHECK(wrapper->SetChunk(0, head) == 0, "S3: SetChunk head segment");
    CHECK(wrapper->SetChunk(1, tail) == 0, "S3: SetChunk tail segment");
    wrapper->SetSize(Y_SIZE);
    wrapper->SetBufferFormat(SURFACE_BUFFER_FORMAT_H264);  // per-frame format tag
    wrapper->SetInt32(DRIVER_BUF_ID_KEY, 42);
    g_hal.ring2Wrapper = wrapper;
    return g_hal.producer->AttachBuffer(wrapper);
}

int32_t HandleRingCrossingDetachInUse()
{
    // Consumer still holds the buffer (ACQUIRED): expect IN_USE and record it
    // in the pending recycle table.
    int32_t ret = g_hal.producer->DetachBuffer(g_hal.ring2Wrapper);
    if (ret == SURFACE_ERROR_BUFFER_IN_USE) {
        pthread_mutex_lock(&g_hal.lock);
        g_hal.pendingRecycle[g_hal.pendingCount++] = g_hal.ring2Wrapper;
        pthread_mutex_unlock(&g_hal.lock);
    }
    return ret;
}

void* ProducerThread(void*)
{
    for (;;) {
        pthread_mutex_lock(&g_jobLock);
        while (g_job.type == JOB_NONE) {
            pthread_cond_wait(&g_jobCond, &g_jobLock);
        }
        JobType type = g_job.type;
        int arg = g_job.arg;
        pthread_mutex_unlock(&g_jobLock);
        int32_t ret = -1;
        switch (type) {
            case JOB_S1_SETUP_ATTACH:
                ret = HandleRgbSetupAttach();
                break;
            case JOB_S1_FLUSH_DETACH:
                ret = HandleRgbFlushDetach();
                break;
            case JOB_S2_ATTACH:
                ret = SetupNv12Wrapper(arg % RING_NUM, arg);
                break;
            case JOB_S2_DETACH_ALL:
                ret = HandleRingDetachAll();
                break;
            case JOB_S3_SETUP_ATTACH:
                ret = HandleRingCrossingSetupAttach();
                break;
            case JOB_S3_DETACH_INUSE:
                ret = HandleRingCrossingDetachInUse();
                break;
            default:
                break;
        }
        pthread_mutex_lock(&g_jobLock);
        g_job.result = ret;
        g_job.type = JOB_NONE;
        pthread_cond_broadcast(&g_jobCond);
        pthread_mutex_unlock(&g_jobLock);
        if (type == JOB_QUIT) {
            break;
        }
    }
    return nullptr;
}

/* --------------------------------------------------------- consumer checks */
void VerifyNv12Frame(SurfaceBuffer* buffer, int frameNo)
{
    CHECK(buffer->GetChunkCount() == 2, "S2: NV12 chunk count is 2");
    SurfaceDataChunk yChunk;
    SurfaceDataChunk uvChunk;
    CHECK(buffer->GetChunk(0, yChunk) == 0, "S2: GetChunk(0)");
    CHECK(buffer->GetChunk(1, uvChunk) == 0, "S2: GetChunk(1)");
    CHECK(yChunk.size == Y_SIZE, "S2: plane0 size");
    CHECK(uvChunk.size == UV_SIZE, "S2: plane1 size");
    CHECK(yChunk.planeId == 0 && uvChunk.planeId == 1, "S2: plane ids");
    // Multi-segment: legacy view must be invalidated.
    CHECK(buffer->GetVirAddr() == nullptr, "S2: legacy virAddr invalidated");
    CHECK(buffer->GetPhyAddr() == 0, "S2: legacy phyAddr invalidated");
    CHECK(buffer->GetSize() == 0, "S2: legacy size invalidated");
    CHECK(buffer->GetTotalDataSize() == NV12_TOTAL, "S2: total data size");
    CHECK(buffer->GetBufferFormat() == SURFACE_BUFFER_FORMAT_NV12, "S2: per-frame NV12 format tag");
    // Extra data (driver handle) survives release cycles with the wrapper.
    int32_t bufId = 0;
    CHECK(buffer->GetInt32(DRIVER_BUF_ID_KEY, bufId) == 0, "S2: extra data readable");
    CHECK(bufId == frameNo, "S2: extra data value (ring slot re-pointed)");
    CHECK(buffer->GetChunk(2, yChunk) == -1, "S2: GetChunk beyond count rejected");
}

/* --------------------------------------------------------------- scenarios */
Surface* g_consumerSurface = nullptr;
Surface* g_producerSurface = nullptr;
uint64_t g_surfaceId = 0;
pthread_t g_producerThread;

void RunBasics()
{
    g_consumerSurface = Surface::CreateSurface();
    CHECK(g_consumerSurface != nullptr, "S0: CreateSurface");
    g_surfaceId = g_consumerSurface->GetSurfaceId();
    CHECK(g_surfaceId != 0, "S0: auto allocated surface id");
    g_consumerSurface->RegisterConsumerListener(g_consumerListener);

    // Surface-level format round trips (NV12/NV21 existing constants).
    g_consumerSurface->SetFormat(IMAGE_PIXEL_FORMAT_NV21);
    CHECK(g_consumerSurface->GetFormat() == IMAGE_PIXEL_FORMAT_NV21, "S0: NV21 round trip");
    g_consumerSurface->SetFormat(IMAGE_PIXEL_FORMAT_NV12);
    CHECK(g_consumerSurface->GetFormat() == IMAGE_PIXEL_FORMAT_NV12, "S0: NV12 round trip");

    g_producerSurface = Surface::CreateSurfaceAsProducer(g_surfaceId);
    CHECK(g_producerSurface != nullptr, "S0: CreateSurfaceAsProducer by id");
    CHECK(Surface::CreateSurfaceAsProducer(g_surfaceId + 12345) == nullptr,
          "S0: unknown surface id rejected");
    CHECK(SurfaceUtils::GetInstance()->GetSurface(g_surfaceId) == g_consumerSurface,
          "S0: registry returns the consumer surface");
    g_producerSurface->RegisterProducerListener(g_producerListener);
    // Single process: internal allocation is unavailable (V10).
    CHECK(g_producerSurface->RequestBuffer() == nullptr, "S0: RequestBuffer unavailable in SP mode");
}

void RunFreshWrapperChecks()
{
    // Fresh wrapper defaults: legacy synthesized single-chunk view + closed
    // format domain validation.
    SurfaceBuffer* fresh = SurfaceBuffer::CreateSurfaceBuffer();
    CHECK(fresh != nullptr, "S0: CreateSurfaceBuffer");
    CHECK(fresh->GetChunkCount() == 1, "S0: fresh wrapper chunk count defaults to 1");
    fresh->SetSize(1024);  // must not be silently rejected (capacity check bypassed)
    CHECK(fresh->GetTotalDataSize() == 1024, "S0: fresh wrapper total size follows SetSize");
    SurfaceDataChunk synth;
    CHECK(fresh->GetChunk(0, synth) == 0, "S0: fresh wrapper synthesizes chunk 0");
    CHECK(synth.virAddr == nullptr && synth.size == 1024, "S0: synthesized chunk content");
    CHECK(fresh->GetBufferFormat() == SURFACE_BUFFER_FORMAT_NONE, "S0: fresh wrapper format defaults to NONE");
    CHECK(fresh->SetBufferFormat(SURFACE_BUFFER_FORMAT_JPEG) == 0, "S0: set JPEG tag");
    CHECK(fresh->GetBufferFormat() == SURFACE_BUFFER_FORMAT_JPEG, "S0: JPEG tag round trip");
    CHECK(fresh->SetBufferFormat(SURFACE_BUFFER_FORMAT_H264) == 0, "S0: set H264 tag");
    CHECK(fresh->GetBufferFormat() == SURFACE_BUFFER_FORMAT_H264, "S0: H264 tag round trip");
    CHECK(fresh->SetBufferFormat(999) == -1, "S0: out-of-domain format rejected");
    CHECK(fresh->SetBufferFormat(IMAGE_PIXEL_FORMAT_NV12) == -1, "S0: ImageFormat value rejected (closed domain)");
    CHECK(fresh->GetBufferFormat() == SURFACE_BUFFER_FORMAT_H264, "S0: rejected writes leave tag unchanged");
    CHECK(fresh->SetBufferFormat(SURFACE_BUFFER_FORMAT_NONE) == 0, "S0: clear tag back to NONE");
    CHECK(fresh->GetBufferFormat() == SURFACE_BUFFER_FORMAT_NONE, "S0: tag cleared");
    g_hal.producer = g_producerSurface;
    g_hal.rgbWrapper = fresh;  // reused by S1 (attached there, never leaked)
    pthread_create(&g_producerThread, nullptr, ProducerThread, nullptr);
}

void RunRgbFlushDetach()
{
    AllocDriverMem(&g_hal.rgbSlot, 64 * 1024);  // wearable RGB frame
    int32_t ret = -1;
    SubmitJob(JOB_S1_SETUP_ATTACH, 0, &ret);
    CHECK(ret == SURFACE_ERROR_OK, "S1: attach RGB single-segment buffer");
    g_available = false;  // not consumed on purpose
    SubmitJob(JOB_S1_FLUSH_DETACH, 0, &ret);
    CHECK(ret == SURFACE_ERROR_OK, "S1: queued (FLUSH) detach is synchronous");
    CHECK(g_hal.rgbSlot.freed, "S1: driver memory returned by HAL call stack");
}

void RunNv12Ring()
{
    int32_t ret = -1;
    g_released = false;
    SubmitJob(JOB_S2_ATTACH, 0, &ret);
    CHECK(ret == SURFACE_ERROR_OK, "S2: first NV12 attach");
    for (int frameNo = 1; frameNo < RING_FRAMES; frameNo++) {
        DemoWait(&g_available);
        SurfaceBuffer* buffer = g_consumerSurface->AcquireBuffer();
        CHECK(buffer != nullptr, "S2: acquire NV12 frame");
        if (buffer == nullptr) {
            break;
        }
        // The acquire in iteration N consumes the frame attached in iteration N-1
        // (strict producer-consumer ping-pong).
        VerifyNv12Frame(buffer, frameNo - 1);
        g_consumerSurface->ReleaseBuffer(buffer);
        DemoWait(&g_released);
        // Ring reuse: producer re-attaches the idle wrapper of this slot.
        SubmitJob(JOB_S2_ATTACH, frameNo, &ret);
        CHECK(ret == SURFACE_ERROR_OK, "S2: re-attach idle wrapper (ring reuse)");
    }
    DemoWait(&g_available);
    SurfaceBuffer* lastFrame = g_consumerSurface->AcquireBuffer();
    CHECK(lastFrame != nullptr, "S2: acquire last NV12 frame");
    VerifyNv12Frame(lastFrame, RING_FRAMES - 1);
    g_consumerSurface->ReleaseBuffer(lastFrame);
    DemoWait(&g_released);
    CHECK(g_hal.releasedRingCount == RING_FRAMES, "S2: OnBufferReleased fired per frame");
    SubmitJob(JOB_S2_DETACH_ALL, 0, &ret);
    CHECK(ret == SURFACE_ERROR_OK, "S2: detach all idle ring wrappers");
    CHECK(g_hal.detachedCount == RING_NUM, "S2: all ring wrappers detached");
    for (int i = 0; i < RING_NUM; i++) {
        CHECK(g_hal.ringSlots[i].freed, "S2: ring driver memory returned");
    }
}

void RunRingCrossingInUse()
{
    int32_t ret = -1;
    SubmitJob(JOB_S3_SETUP_ATTACH, 0, &ret);
    CHECK(ret == SURFACE_ERROR_OK, "S3: attach ring-crossing H264 frame");
    DemoWait(&g_available);
    SurfaceBuffer* ring2 = g_consumerSurface->AcquireBuffer();
    CHECK(ring2 != nullptr, "S3: acquire ring-crossing frame");
    CHECK(ring2->GetChunkCount() == 2, "S3: two segments");
    SurfaceDataChunk c0;
    SurfaceDataChunk c1;
    CHECK(ring2->GetChunk(0, c0) == 0 && ring2->GetChunk(1, c1) == 0, "S3: read segments");
    CHECK(c0.planeId == 0 && c1.planeId == 0, "S3: same plane, order by convention");
    CHECK(c0.size + c1.size == Y_SIZE, "S3: segments cover the frame");
    CHECK(c0.virAddr == static_cast<char*>(g_hal.ring2Slot.mem) + g_hal.ring2Mid,
          "S3: head segment starts at mid");
    CHECK(c1.virAddr == g_hal.ring2Slot.mem, "S3: tail segment wraps to base");
    CHECK(ring2->GetTotalDataSize() == Y_SIZE, "S3: total data size");
    CHECK(ring2->GetVirAddr() == nullptr, "S3: legacy view invalidated");
    CHECK(ring2->GetBufferFormat() == SURFACE_BUFFER_FORMAT_H264, "S3: per-frame H264 format tag");
    SubmitJob(JOB_S3_DETACH_INUSE, 0, &ret);
    CHECK(ret == SURFACE_ERROR_BUFFER_IN_USE, "S3: detach in ACQUIRED returns IN_USE");
    CHECK(g_hal.pendingCount == 1, "S3: buffer recorded in pending recycle table");
    g_released = false;
    g_consumerSurface->ReleaseBuffer(ring2);
    DemoWait(&g_released);
    // OnBufferReleased hit the pending table: detached + driver memory returned.
    CHECK(g_hal.pendingCount == 0, "S3: pending table drained");
    CHECK(g_hal.ring2Slot.freed, "S3: driver memory returned via OnBufferReleased");
}

void RunTeardown()
{
    SubmitJob(JOB_QUIT, 0, nullptr);
    pthread_join(g_producerThread, nullptr);
    delete g_producerSurface;   // proxy: shares the queue, does not delete it
    delete g_consumerSurface;  // owns the queue; attachList_ must be empty by now
    CHECK(SurfaceUtils::GetInstance()->GetSurface(g_surfaceId) == nullptr,
          "S4: surface unregistered on destruction");
}

int Run()
{
    RunBasics();
    RunFreshWrapperChecks();
    RunRgbFlushDetach();
    RunNv12Ring();
    RunRingCrossingInUse();
    RunTeardown();

    printf("\n==== surface_lite single process example: %d passed, %d failed ====\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
} // namespace
} // namespace OHOS

int main()
{
    return OHOS::Run();
}
