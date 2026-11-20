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
 * @since 1.0
 * @version 1.0
 */

#ifndef GRAPHIC_LITE_SURFACE_BUFFER_H
#define GRAPHIC_LITE_SURFACE_BUFFER_H

#include <map>
#include "gfx_utils/graphic_frame.h"
#include "surface_type.h"

namespace OHOS {
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
     * The memory is externally owned; the framework never allocates or frees it.
     *
     * @param release Invoked when the wrapper is destroyed — by DestroySurfaceBuffer,
     *                by the queue on a one-way release, or at queue destruction —
     *                to return the described memory to its owner; receives the frame
     *                including drvHandle and context. May be <b>nullptr</b> when the
     *                caller manages the memory manually, in which case the wrapper
     *                can only be attached with isReusable = true.
     * @return Returns the pointer to the buffer wrapper; returns <b>nullptr</b> on failure.
     * @release Surface/DestroySurfaceBuffer {return}
     * @since 26.1.0
     * @version 26.1.0
     */
    static SurfaceBuffer* CreateSurfaceBuffer(GraphicFrameDataReleaseFunc release);

    /**
     * @brief Destroys a buffer wrapper created by CreateSurfaceBuffer.
     *
     * Allowed only when the wrapper is not owned by a queue (never attached, or
     * already released). Calling it on a queue-owned buffer returns
     * SURFACE_ERROR_BUFFER_IN_USE without any
     * effect. Destruction returns the described memory through the release
     * function registered at CreateSurfaceBuffer. After a successful call the
     * pointer is invalid; calling it again on the same pointer is undefined behavior.
     *
     * @param buffer Indicates the buffer wrapper to destroy.
     * @return Returns <b>0</b> if the operation is successful; returns an error
     *         code otherwise.
     * @see CreateSurfaceBuffer
     * @since 26.1.0
     * @version 26.1.0
     */
    static int32_t DestroySurfaceBuffer(SurfaceBuffer* buffer);

    /**
     * @brief Sets the complete frame description in one call.
     *
     * Sets the format, dimensions and memory segments of the frame.
     * Must be called before Surface::AttachBuffer (attaching a wrapper without
     * a frame is rejected). drvHandle and context identify the buffer to its
     * owner and are passed back to the release function when the wrapper is
     * destroyed.
     *
     * @param frame Indicates the complete frame description.
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b>
     *         if the chunk count or format is invalid.
     * @since 26.1.0
     * @version 26.1.0
     */
    virtual int32_t SetFrame(const GraphicFrame& frame)
    {
        (void)frame;
        return -1;
    }

    /**
     * @brief Gets the complete frame description in one call.
     *
     * Gets the format, dimensions and memory segments of the frame.
     *
     * @param frame Indicates the frame description to fill.
     * @return Returns <b>0</b> if the operation is successful; returns <b>-1</b> otherwise.
     * @since 26.1.0
     * @version 26.1.0
     */
    virtual int32_t GetFrame(GraphicFrame& frame) const
    {
        (void)frame;
        return -1;
    }

    /**
     * @brief Obtains the total data size of all segments of the buffer.
     *
     * For buffers whose segments have never been set, the legacy GetSize value is returned.
     *
     * @return Returns the total data size in bytes.
     * @since 26.1.0
     * @version 26.1.0
     */
    virtual uint32_t GetTotalDataSize() const
    {
        return 0;
    }

    /**
     * @brief Obtains the virtual address of shared memory for producers and consumers.
     *
     * @return Returns the virtual address of shared memory.
     * @since 1.0
     * @version 1.0
     */
    virtual void* GetVirAddr() const = 0;

    /**
     * @brief Obtains the physical address of shared memory.
     *
     * @return Returns the physical address of shared memory.
     * @since 1.0
     * @version 1.0
     */
    virtual uint64_t GetPhyAddr() const = 0;

    /**
     * @brief Obtains the size of shared memory.
     *
     * @return Returns the size of shared memory.
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

protected:
    SurfaceBuffer() {}
    virtual ~SurfaceBuffer() {}
};
} // end namespace
#endif
