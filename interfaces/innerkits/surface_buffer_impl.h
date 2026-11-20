/*
 * Copyright (c) 2020-2022 Huawei Device Co., Ltd.
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

#ifndef GRAPHIC_LITE_SURFACE_BUFFER_IMPL_H
#define GRAPHIC_LITE_SURFACE_BUFFER_IMPL_H

#include <map>
#include <pthread.h>
#include "buffer_common.h"
#ifndef SURFACE_LITE_SINGLE_PROCESS
#include "ipc_skeleton.h"
#endif
#include "surface_buffer.h"

namespace OHOS {
enum BufferState {
    BUFFER_STATE_NONE = 0,
    BUFFER_STATE_REQUEST,
    BUFFER_STATE_FLUSH,
    BUFFER_STATE_ACQUIRE,
    BUFFER_STATE_RELEASE
};

enum BufferDataType {
    BUFFER_DATA_TYPE_NONE,
    BUFFER_DATA_TYPE_INT_32,
    BUFFER_DATA_TYPE_INT_64,
    BUFFER_DATA_TYPE_MAX,
};

struct SurfaceBufferHandle {
    int32_t key;
    uint64_t phyAddr;
    int32_t stride;       /* the stride of memory */
    uint32_t reserveFds;  /* the number of reserved fd value */
    uint32_t reserveInts; /* the number of reserved integer value */
    bool operator == (const SurfaceBufferHandle &rHandle) const
    {
        return ((key == rHandle.key)
            && (phyAddr == rHandle.phyAddr));
    }
};

struct SurfaceBufferData {
    SurfaceBufferHandle handle;
    uint32_t size;
    uint32_t usage;
    uint8_t deletePending;
    BufferState state;
    void* virAddr;
    bool operator == (const SurfaceBufferData &rData) const
    {
        return handle == rData.handle;
    }
};

typedef struct {
    void* value;
    uint8_t size;
    uint8_t type;
} ExtraData;

/**
 * @brief Buffer class. Provide shared memory for graphic and multi media to use.
 */
class SurfaceBufferImpl : public SurfaceBuffer {
public:
    /**
     * @brief Buffer Constructor.
     */
    SurfaceBufferImpl();

    /**
     * @brief Buffer Destructor.
     */
    ~SurfaceBufferImpl();

    /**
     * @brief Get buffer key, for shared virtual memory.
     * @returns The buffer key.
     */
    int32_t GetKey() const
    {
        return bufferData_.handle.key;
    }

    /**
     * @brief Set buffer key, for shared virtual memory.
     * @param [in] The buffer key
     */
    void SetKey(int32_t key)
    {
        bufferData_.handle.key = key;
    }

    /**
     * @brief Get buffer phyAddr, for shared physical memory.
     * @returns The buffer phyAddr.
     */
    uint64_t GetPhyAddr() const override
    {
        return bufferData_.handle.phyAddr;
    }

    /**
     * @brief Set buffer phyAddr, for shared physical memory.
     * @param [in] The buffer phyAddr
     */
    void SetPhyAddr(uint64_t phyAddr)
    {
        bufferData_.handle.phyAddr = phyAddr;
    }

    /**
     * @brief Get buffer stride, for shared physical memory.
     * @returns The buffer phyAddr.
     */
    int32_t GetStride() const
    {
        return bufferData_.handle.stride;
    }

    /**
     * @brief Set buffer stride, for shared physical memory.
     * @param [in] The buffer phyAddr
     */
    void SetStride(int32_t stride)
    {
        bufferData_.handle.stride = stride;
    }

    /**
     * @brief Get buffer reserveInts, for shared physical memory.
     * @returns The buffer phyAddr.
     */
    uint32_t GetReserveInts() const
    {
        return bufferData_.handle.reserveInts;
    }

    /**
     * @brief Set buffer reserveInts, for shared physical memory.
     * @param [in] The buffer phyAddr
     */
    void SetReserveInts(uint32_t reserveInts)
    {
        bufferData_.handle.reserveInts = reserveInts;
    }

    /**
     * @brief Get buffer reserveFds, for shared physical memory.
     * @returns The buffer phyAddr.
     */
    uint32_t GetReserveFds() const
    {
        return bufferData_.handle.reserveFds;
    }

    /**
     * @brief Set buffer reserveFds, for shared physical memory.
     * @param [in] The buffer phyAddr
     */
    void SetReserveFds(uint32_t reserveFds)
    {
        bufferData_.handle.reserveFds = reserveFds;
    }

    /**
     * @brief Set buffer virtual addr.
     * @param [in] The virtual addr.
     */
    void SetVirAddr(void* virAddr)
    {
        bufferData_.virAddr = virAddr;
    }

    /**
     * @brief Get buffer virtual addr.
     * @returns The buffer virtual addr.
     */
    void* GetVirAddr() const override
    {
        return bufferData_.virAddr;
    }

    /**
     * @brief Get buffer size.
     * @returns The buffer size.
     */
    uint32_t GetMaxSize() const
    {
        return bufferData_.size;
    }

    /**
     * @brief Set buffer size.
     * @param [in] The buffer size
     */
    void SetMaxSize(uint32_t size)
    {
        bufferData_.size = size;
    }

    /**
     * @brief Get buffer size.
     * @returns The buffer size.
     */
    uint32_t GetSize() const override
    {
        if (len_ == 0) {
            return bufferData_.size;
        }
        return len_;
    }

    /**
     * @brief Set buffer size.
     * @param [in] The buffer size
     */
    void SetSize(uint32_t size) override
    {
        if (size > bufferData_.size) {
            GRAPHIC_LOGI("Input param is invalid");
            return;
        }
        len_ = size;
    }

    /**
     * @brief Get buffer usage. Surface alloc physical or virtual memory buffer.
     *        All usage sees detail in OHOS::BUFFER_CONSUMER_USAGE.
     * @returns The buffer usage.
     */
    uint32_t GetUsage() const
    {
        return bufferData_.usage;
    }

    /**
     * @brief Set buffer usage. Surface alloc physical or virtual memory buffer.
     *        Support usage see detail in OHOS::BUFFER_CONSUMER_USAGE.
     *        Default is BUFFER_CONSUMER_USAGE_SORTWARE, which will alloc virtual memory buffer.
     * @param [in] The buffer usage.
     */
    void SetUsage(uint32_t usage)
    {
        bufferData_.usage = usage;
    }

    /**
     * @brief Get buffer delete state. If deletePending == 1, buffer will be freed when state == BUFFER_STATE_FREE.
     * @returns [in] The buffer delete state
     */
    uint8_t GetDeletePending() const
    {
        return bufferData_.deletePending;
    }

    /**
     * @brief Set buffer delete state. If deletePending == 1, buffer will be freed when state == BUFFER_STATE_FREE.
     * @param [in] The buffer delete state
     */
    void SetDeletePending(uint8_t deletePending)
    {
        bufferData_.deletePending = deletePending;
    }

    /**
     * @brief Get buffer state. Buffer state detail : BUFFER_STATE.
     * @returns [in] The buffer state
     */
    BufferState GetState() const
    {
        return bufferData_.state;
    }

    /**
     * @brief Set buffer state. Buffer state detail : BUFFER_STATE.
     * @param [in] The buffer state
     */

    void SetState(BufferState newState)
    {
        bufferData_.state = newState;
    }
    /**
     * @brief Set int32 extra data for buffer, like <key,value>.
     * @param [in] key, unique uint32_t. If exited, will overlap.
     * @param [in] value, value which storage the value;
     * @returns if succeed, return 0; else return -1.
     */
    int32_t SetInt32(uint32_t key, int32_t value) override;

    /**
     * @brief Get int32 extra data for buffer, like <key,value>.
     * @param [in] key, unique uint32_t..
     * @param [out] value, value which storage the value;
     * @returns if succeed, return 0; else return -1;
     */
    int32_t GetInt32(uint32_t key, int32_t& value) override;

    /**
     * @brief Set int64 extra data for buffer, like <key,value>.
     * @param [in] key, unique uint32_t. If exited, will overlap.
     * @param [in] value, value which storage the value;
     * @returns if succeed, return 0; else return -1.
     */
    int32_t SetInt64(uint32_t key, int64_t value) override;

    /**
     * @brief Get int64 extra data for buffer, like <key,value>.
     * @param [in] key, unique uint32_t..
     * @param [out] value, value which storage the value;
     * @returns if succeed, return 0; else return -1;
     */
    int32_t GetInt64(uint32_t key, int64_t& value) override;

    /**
     * @brief Verify the two surface buffer same or not.
     * @param [in] The other SurfaceBufferImpl object
     * @returns Is same or not.
     */
    bool equals(const SurfaceBufferImpl& buffer) const
    {
        return bufferData_ == buffer.bufferData_;
    }

    /**
     * @brief Get buffer attr from ipc object.
     * @param [in] IpcIo pointer.
     */
#ifndef SURFACE_LITE_SINGLE_PROCESS
    void ReadFromIpcIo(IpcIo& io);

    /**
     * @brief Write buffer attr to ipc object.
     * @param [in] IpcIo object.
     */
    void WriteToIpcIo(IpcIo& io);
#endif

    /**
     * @brief Copy buffer extra data from input buffer to self
     * @param [in] buffer pointer.
     */
    void CopyExtraData(SurfaceBufferImpl& buffer);

    /**
     * @brief Clear buffer extra data.
     */
    void ClearExtraData();

    /**
     * @brief Set the complete frame. Framework-managed buffers do not allow
     *        chunk writing; only the format is stored.
     * @param [in] frame, the frame description to set (see gfx_utils/graphic_frame.h).
     * @returns 0 if the format is valid; -1 otherwise.
     */
    int32_t SetFrame(const GraphicFrame& frame) override
    {
        if (frame.format > GRAPHIC_FRAME_FORMAT_H264) {
            GRAPHIC_LOGD("Invalid frame format %u.", static_cast<uint32_t>(frame.format));
            return -1;
        }
        bufferFormat_ = frame.format;
        return 0;
    }

    /**
     * @brief Get the complete frame: synthesizes a single-chunk legacy view.
     * @param [out] frame, the frame description filled (see gfx_utils/graphic_frame.h).
     * @returns 0 if succeed; -1 otherwise.
     */
    int32_t GetFrame(GraphicFrame& frame) const override
    {
        frame.format = bufferFormat_;
        frame.width = 0;
        frame.height = 0;
        frame.chunkCount = 1;
        frame.chunks[0].virAddr = bufferData_.virAddr;
        frame.chunks[0].phyAddr = bufferData_.handle.phyAddr;
        frame.chunks[0].size = GetSize();
        frame.chunks[0].stride = static_cast<uint32_t>(bufferData_.handle.stride);
        return 0;
    }

    /**
     * @brief Get the total data size. Equals GetSize() for framework-managed buffers.
     * @returns total data size in bytes.
     */
    uint32_t GetTotalDataSize() const override;

    /** @brief Whether a release function was registered at creation. */
    bool HasReleaseFunc() const
    {
        return releaseFunc_ != nullptr;
    }

    /** @brief Whether the wrapper is returned to the producer on release. */
    bool IsReusable() const
    {
        return reusable_;
    }

    /** @brief Stamp the attach mode (called by the queue at AttachBuffer). */
    void SetReusable(bool reusable)
    {
        reusable_ = reusable;
    }

    /**
     * @brief Whether a frame description was set (SetFrame) by the producer.
     *        Framework-managed buffers always have the legacy view.
     */
    virtual bool HasFrame() const
    {
        return true;
    }

protected:
    /**
     * @brief Set data length bypassing the capacity check.
     *        For externally managed buffers whose capacity is owned by the HAL/driver
     *        (bufferData_.size is always 0), used by SurfaceBufferImplEx::SetSize.
     */
    void SetSizeUnchecked(uint32_t size)
    {
        len_ = size;
    }

    /**
     * @brief Release function registered at CreateSurfaceBuffer; fired by the
     *        externally managed wrapper destructor to return the data memory.
     */
    GraphicFrameDataReleaseFunc releaseFunc_ = nullptr;
    /** Attach mode stamp: whether the wrapper is returned to the producer on release. */
    bool reusable_ = false;

private:
    /**
     * Set extra data for buffer, like <key,value>.
     * @key, unique uint32_t. If exited, will overlap;
     * @type, value type. Current support see detail enum OHOS::BUFFER_DATA_TYPE
     * @data, value pointer, which storage the value;
     * @size, value length.
     */
    int32_t SetData(uint32_t key, uint8_t type, const void* data, uint8_t size);
    int32_t GetData(uint32_t key, uint8_t* type, void** data, uint8_t* size);
    struct SurfaceBufferData bufferData_;
    std::map<uint32_t, ExtraData> extDatas_;
    uint32_t len_;
protected:
    enum GraphicFrameFormat bufferFormat_ = GRAPHIC_FRAME_FORMAT_NONE;
};
} // end namespace
#endif
