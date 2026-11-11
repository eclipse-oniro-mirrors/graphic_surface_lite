/*
 * Copyright (c) 2020-2021 Huawei Device Co., Ltd.
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

/**
 * @addtogroup Surface
 * @{
 *
 * @brief Provides shared memory for multimedia and graphics.
 *
 * This module is used to apply for and release shared memory and is used by the multimedia and graphics
 * modules across processes.
 *
 * @since 1.0
 * @version 1.0
 */

/**
 * @file surface_buffer.h
 *
 * @brief Provides functions such as setting the virtual address, size, and additional attributes of shared memory.
 *
 *
 *
 * @since 1.0
 * @version 1.0
 */

#ifndef GRAPHIC_LITE_SURFACE_BUFFER_H
#define GRAPHIC_LITE_SURFACE_BUFFER_H

#include <map>
#include "surface_type.h"

namespace OHOS {
/**
 * @brief Defines one data segment of a buffer.
 *
 * One buffer = one frame = 1 to SURFACE_MAX_CHUNK_NUM segments. Segments of
 * the same planeId are set in data logical order via SetChunk; a driver that
 * produces out-of-order segments must sort them in the HAL adapter layer
 * before calling SetChunk with indexes 0,1,2,... in ascending order.
 *
 * @since 4.0
 * @version 1.0
 */
struct SurfaceDataChunk {
    /** Virtual address of the segment. */
    void* virAddr;
    /** Physical address of the segment (driver allocated, aligned). */
    uint64_t phyAddr;
    /** Size of the segment in bytes. */
    uint32_t size;
    /** Stride of the segment in bytes. */
    uint32_t stride;
    /** Plane id: RGB = 0; NV12/NV21: Y = 0, UV = 1; ring/stream segments use
     *  the plane id they belong to. */
    uint8_t planeId;
};

/**
 *
 * @brief Provides functions such as setting the virtual address, size, and additional attributes of shared memory.
 *
 *
 * @since 1.0
 * @version 1.0
 */
class SurfaceBuffer {
public:
    /**
     * @brief Creates a buffer wrapper for externally allocated (driver-owned) memory.
     *
     * The memory is owned by the chip driver; the framework never allocates or frees it.
     * The returned wrapper is attached to a surface via Surface::AttachBuffer. Its
     * lifecycle pairs with DestroySurfaceBuffer (the canonical teardown sequence is
     * DetachBuffer followed by DestroySurfaceBuffer).
     *
     * @return Returns the pointer to the buffer wrapper; returns <b>nullptr</b> on failure.
     * @since 4.0
     * @version 1.0
     */
    static SurfaceBuffer* CreateSurfaceBuffer();

    /**
     * @brief Destroys a buffer wrapper created by CreateSurfaceBuffer.
     *
     * Allowed only when the wrapper is not owned by a queue (never attached, or
     * already detached). Calling it on a queue-owned buffer returns
     * SURFACE_ERROR_BUFFER_IN_USE (see innerkits buffer_common.h) without any
     * effect, so the buffer must be detached first. After a successful call the
     * pointer is invalid; calling it again on the same pointer is undefined behavior.
     *
     * @param buffer Indicates the buffer wrapper to destroy.
     * @return Returns <b>0</b> if the operation is successful; returns an error
     *         code otherwise.
     * @since 4.0
     * @version 1.0
     */
    static int32_t DestroySurfaceBuffer(SurfaceBuffer* buffer);

    /**
     * @brief Obtains the virtual address of shared memory for producers and consumers.
     *
     * @return Returns the virtual address of shared memory.
     * @deprecated Since 4.0. Single-segment buffers behave as before; multi-segment
     *             buffers (GetChunkCount() > 1) return <b>nullptr</b>. Use GetChunk instead.
     * @since 1.0
     * @version 1.0
     */
    virtual void* GetVirAddr() const = 0;

    /**
     * @brief Obtains the physical address of shared memory.
     *
     * @return Returns the physical address of shared memory.
     * @deprecated Since 4.0. Single-segment buffers behave as before; multi-segment
     *             buffers (GetChunkCount() > 1) return <b>0</b>. Use GetChunk instead.
     * @since 1.0
     * @version 1.0
     */
    virtual uint64_t GetPhyAddr() const = 0;

    /**
     * @brief Obtains the size of shared memory.
     *
     * @return Returns the size of shared memory.
     * @deprecated Since 4.0. Single-segment buffers behave as before; multi-segment
     *             buffers (GetChunkCount() > 1) return <b>0</b>. Use GetTotalDataSize instead.
     * @since 1.0
     * @version 1.0
     */
    virtual uint32_t GetSize() const = 0;

    /**
     * @brief Sets the size of shared memory.
     *
     * @param size Indicates the size of shared memory to set.
     * @since 1.0
     * @version 1.0
     */
    virtual void SetSize(uint32_t size) = 0;

    /**
     * @brief Sets an extra attribute value of the int32 type.
     *
     * Sets an extra attribute value of the int32 type, The extra attribute is stored in the format of <key, value>.
     * Each key corresponds to a value. If the same keys are used in two calls, \n
     * the value in the second call overwrites that in the first call. \n
     *
     * @param key Indicates the key of a key-value pair to set.
     * @param value Indicates the value of the key-value pair to set.
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b> otherwise.
     * @since 1.0
     * @version 1.0
     */
    virtual int32_t SetInt32(uint32_t key, int32_t value) = 0;

    /**
     * @brief Obtains an extra attribute value of the int32 type.
     *
     * Obtains an extra attribute value of the int32 type, The extra attribute is stored in the format of <key, value>.
     * Each key corresponds to a value. If the key does not exist or the value is not int32, <b>-1</b> is returned.
     *
     * @param key Indicates the key of a key-value pair for which the value is to be obtained.
     * @param value Indicates the value of the key-value pair obtained.
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b> otherwise.
     * @since 1.0
     * @version 1.0
     */
    virtual int32_t GetInt32(uint32_t key, int32_t& value) = 0;

    /**
     * @brief Sets an extra attribute value of the int64 type.
     *
     * Sets an extra attribute value of the int64 type. The storage mode of the extra attribute is <key, value>.
     * the value in the second call overwrites that in the first call.
     *
     * @param key Indicates the key of a key-value pair to set.
     * @param value Indicates the value of the key-value pair to set.
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b> otherwise.
     * @since 1.0
     * @version 1.0
     */
    virtual int32_t SetInt64(uint32_t key, int64_t value) = 0;

    /**
     * @brief Obtains an extra attribute value of the int64 type.
     *
     * Obtains an extra attribute value of the int64 type, The extra attribute is stored in the format of <key, value>.
     * Each key corresponds to a value. If the key does not exist or the value is not int64, <b>-1</b> is returned.
     *
     * @param key Indicates the key of a key-value pair for which the value is to be obtained.
     * @param value Indicates the value of the key-value pair obtained.
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b> otherwise.
     * @since 1.0
     * @version 1.0
     */
    virtual int32_t GetInt64(uint32_t key, int64_t& value) = 0;

    /**
     * @brief Writes one data segment of the buffer (the single entry for address writing).
     *
     * Segments must be written in ascending order 0,1,2,.... Writing an index greater
     * than the current segment count (skip) fails. Rewriting an already-written index
     * is allowed (for ring reuse re-pointing before attach).
     *
     * @param index Indicates the segment index, in [0, SURFACE_MAX_CHUNK_NUM).
     * @param chunk Indicates the segment to set.
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b> otherwise.
     * @since 4.0
     * @version 1.0
     */
    virtual int32_t SetChunk(uint8_t index, const SurfaceDataChunk& chunk)
    {
        (void)index;
        (void)chunk;
        return -1;
    }

    /**
     * @brief Obtains one data segment of the buffer.
     *
     * For buffers whose segments have never been set, index 0 is synthesized from the
     * legacy single-memory view (virtual address, physical address, size, stride).
     *
     * @param index Indicates the segment index.
     * @param chunk Indicates the segment obtained.
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b> otherwise.
     * @since 4.0
     * @version 1.0
     */
    virtual int32_t GetChunk(uint8_t index, SurfaceDataChunk& chunk) const
    {
        (void)index;
        (void)chunk;
        return -1;
    }

    /**
     * @brief Obtains the number of data segments of the buffer.
     *
     * Buffers whose segments have never been set are always reported as single-segment
     * (legacy view). The return value is never less than 1 for valid buffer implementations.
     *
     * @return Returns the number of segments.
     * @since 4.0
     * @version 1.0
     */
    virtual uint32_t GetChunkCount() const
    {
        return 0;
    }

    /**
     * @brief Obtains the total data size of all segments of the buffer.
     *
     * For buffers whose segments have never been set, the legacy GetSize value is returned.
     *
     * @return Returns the total data size in bytes.
     * @since 4.0
     * @version 1.0
     */
    virtual uint32_t GetTotalDataSize() const
    {
        return 0;
    }

    /**
     * @brief Sets the format of this buffer (frame-level tag).
     *
     * A pure per-frame tag for consumers to interpret the chunk data (NV12/NV21
     * plane ordering, JPEG/H264 stream packets). It does not affect allocation
     * and is not serialized over IPC (process-local, same principle as chunks).
     * Valid values: the closed SurfaceBufferFormat enum; other values (including
     * ImageFormat constants) are rejected.
     *
     * @param format Indicates the frame format to set (SurfaceBufferFormat).
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b>
     *         if the format is out of the domain.
     * @since 4.0
     * @version 1.0
     */
    virtual int32_t SetBufferFormat(uint32_t format)
    {
        (void)format;
        return -1;
    }

    /**
     * @brief Obtains the format of this buffer.
     *
     * @return Returns the frame format (SurfaceBufferFormat); returns
     *         SURFACE_BUFFER_FORMAT_NONE (0) if never set, in which case the
     *         surface-level format applies.
     * @since 4.0
     * @version 1.0
     */
    virtual uint32_t GetBufferFormat() const
    {
        return 0;
    }

protected:
    SurfaceBuffer() {}
    virtual ~SurfaceBuffer() {}
};
} // end namespace
#endif
