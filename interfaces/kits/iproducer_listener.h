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

/**
 * @addtogroup Surface
 * @{
 *
 * @brief Provides the producer listener used to notify producers when a buffer becomes idle.
 *
 * @since 26.1.0
 * @version 26.1.0
 */

/**
 * @file iproducer_listener.h
 *
 * @brief Declares the producer listener of the surface.
 *
 * @since 26.1.0
 * @version 26.1.0
 */

#ifndef GRAPHIC_LITE_IPRODUCER_LISTENER_H
#define GRAPHIC_LITE_IPRODUCER_LISTENER_H

namespace OHOS {
class SurfaceBuffer;

/**
 * @brief Defines the producer listener used to notify producers when a buffer becomes idle.
 *
 * The callback is always invoked outside the queue lock, so it may re-enter the surface
 * (for example calling AttachBuffer or DetachBuffer).
 *
 * @since 26.1.0
 * @version 26.1.0
 */
class IProducerListener {
public:
    IProducerListener() {}
    virtual ~IProducerListener() {}

    /**
     * @brief Called when a buffer becomes idle (released by the consumer).
     *
     * The semantics is "the buffer is idle now"; whether to reuse it (re-attach)
     * or destroy it (DestroySurfaceBuffer, the state is NONE after release) is
     * decided by the producer itself.
     *
     * @param buffer Indicates the pointer to the idle buffer.
     * @since 26.1.0
     * @version 26.1.0
     */
    virtual void OnBufferReleased(SurfaceBuffer* buffer) = 0;

    /**
     * @brief Called once when the consumer surface (and its queue) is being destroyed
     *        while producer-side wrappers are still in flight.
     *
     * Industry-identical to binder death notification / mojo disconnect: after this
     * callback, every outstanding wrapper pointer is invalid (the queue deletes them
     * right after this call), surface calls return errors, and driver memory must be
     * returned from the producer's own records. The callback runs before wrapper
     * deletion, so wrapper attributes may be read inside it.
     *
     * @since 26.1.0
     * @version 26.1.0
     */
    virtual void OnConsumerSurfaceDestroyed() {}
};
} // end namespace OHOS
#endif
