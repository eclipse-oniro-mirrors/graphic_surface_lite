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
 * Callbacks may re-enter the surface (for example calling AttachBuffer).
 *
 * @since 26.1.0
 * @version 26.1.0
 */
class IProducerListener {
public:
    IProducerListener() {}
    virtual ~IProducerListener() {}

    /**
     * @brief Called when a buffer is released by the consumer.
     *
     * Fires for every released wrapper while a listener is registered:
     * - isReusable = true: the wrapper is idle and returns to the producer —
     *   re-attach it or destroy it by DestroySurfaceBuffer.
     * - isReusable = false: observational only — the wrapper is valid inside this
     *   callback and destroyed right after it returns. Read what you need, do not
     *   retain, re-attach or destroy the pointer.
     *
     * @param buffer Indicates the pointer to the released buffer.
     * @since 26.1.0
     * @version 26.1.0
     */
    virtual void OnBufferReleased(SurfaceBuffer* buffer) = 0;

    /**
     * @brief Called once when the consumer surface (and its queue) is being destroyed
     *        while producer-side wrappers are still in flight.
     *
     * After this callback, every outstanding wrapper pointer is invalid (the
     * queue deletes them right after this call), surface calls return errors,
     * and driver memory must be returned from the producer's own records. The
     * callback runs before wrapper deletion, so wrapper attributes may be read
     * inside it. The queue never calls the listener again once this callback
     * returns, so unregistering and destroying the listener afterwards is
     * safe (but not inside the callback body itself).
     *
     * @since 26.1.0
     * @version 26.1.0
     */
    virtual void OnConsumerSurfaceDestroyed() {}
};
} // end namespace OHOS
#endif
