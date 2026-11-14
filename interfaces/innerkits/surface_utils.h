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

#ifndef GRAPHIC_LITE_SURFACE_UTILS_H
#define GRAPHIC_LITE_SURFACE_UTILS_H

#include <map>
#include <pthread.h>
#include "buffer_common.h"
#ifndef SURFACE_LITE_SINGLE_PROCESS
#include "ipc_skeleton.h"
#endif
#include "surface.h"
#include "surface_impl.h"

namespace OHOS {
/**
 * @brief Global surface id registry (innerkits internal, not exported to kits).
 *
 * Consumer surfaces register automatically at construction and unregister at
 * destruction. The id is encoded as (PID << 32 | incremental counter); in single
 * process mode the PID part is 0 (there is exactly one process by definition).
 * Other modules address a surface by its id instead of passing raw pointers
 * or SvcIdentity around.
 *
 * The registry also hosts the producer-proxy call routing: proxy calls run
 * under the registry lock, serialized against consumer unregistration and
 * queue destruction, so destruction order does not decide safety. The mutex
 * is recursive on purpose: listener callbacks fired inside a routed call may
 * legally re-enter surfaces.
 */
class SurfaceUtils {
public:
    /**
     * @brief Get the singleton instance.
     */
    static SurfaceUtils* GetInstance();

    /**
     * @brief Register a surface and allocate a global unique id for it.
     * @param [in] surface, the consumer surface to register.
     * @returns the allocated id; 0 on failure (null pointer or duplicate surface).
     */
    uint64_t Register(Surface* surface);

    /**
     * @brief Get a registered surface by its global id.
     * @param [in] id, the global surface id.
     * @returns the surface pointer; nullptr if the id is unknown (or already
     *          unregistered).
     */
    Surface* GetSurface(uint64_t id);

    /**
     * @brief Unregister a surface by its global id.
     *        Warns if live producer proxies remain (their calls will fail).
     * @param [in] id, the global surface id.
     */
    void Unregister(uint64_t id);

#ifndef SURFACE_LITE_SINGLE_PROCESS
    /**
     * @brief Get the SvcIdentity of a surface registered in the current process.
     *        The live value is read from the surface implementation, so it is
     *        valid after the surface has been initialized.
     * @param [in] id, the global surface id.
     * @returns the SvcIdentity; zero values if the id is unknown.
     */
    SvcIdentity GetSvcIdentity(uint64_t id);
#endif

    /**
     * @brief Count a live producer proxy on a registered surface
     *        (used by the destruction warning).
     * @returns true if the surface is registered (proxy counted); false if unknown.
     */
    bool AddProxy(uint64_t id);

    /**
     * @brief Drop a producer proxy count (proxy destruction); no-op if the
     *        surface is already gone.
     */
    void RemoveProxy(uint64_t id);

    /**
     * @brief Create a producer proxy for a registered surface. The proxy holds
     *        only the surface id; all its calls are routed through this registry
     *        (see WithProducer). Implementation in frameworks/surface_proxy_impl.cpp.
     * @param [in] surfaceId, the consumer surface id.
     * @returns Surface pointer; nullptr if the id is unknown.
     */
    static Surface* CreateSurfaceProxy(uint64_t surfaceId);

    /**
     * @brief Call fn(producer) holding the registry lock (H routing for producer
     *        proxies). In-flight calls complete before the consumer can
     *        unregister or destroy the queue; calls issued after unregistration
     *        fail cleanly with SURFACE_ERROR_NOT_READY instead of dangling.
     * @param [in] id, the consumer surface id.
     * @param [in] fn, callable invoked as fn(BufferProducer*).
     * @returns SURFACE_ERROR_OK if invoked, SURFACE_ERROR_NOT_READY if the
     *          consumer surface is gone.
     */
    template <typename F>
    int32_t WithProducer(uint64_t id, F fn)
    {
        SurfaceUtils* utils = GetInstance();
        pthread_mutex_lock(&utils->lock_);
        std::map<uint64_t, SurfaceEntry>::iterator iter = utils->surfaceMap_.find(id);
        int32_t ret = SURFACE_ERROR_NOT_READY;
        if (iter != utils->surfaceMap_.end()) {
            SurfaceImpl* impl = static_cast<SurfaceImpl*>(iter->second.surface);
            if ((impl != nullptr) && (impl->producer_ != nullptr)) {
                fn(impl->producer_);
                ret = SURFACE_ERROR_OK;
            }
        }
        pthread_mutex_unlock(&utils->lock_);
        return ret;
    }

private:
    SurfaceUtils();
    ~SurfaceUtils();
    struct SurfaceEntry {
        Surface* surface;
#ifndef SURFACE_LITE_SINGLE_PROCESS
        SvcIdentity sid;
#endif
        uint32_t proxyCount = 0;
    };
    pthread_mutex_t lock_;
    std::map<uint64_t, SurfaceEntry> surfaceMap_;
    uint64_t counter_ = 0;
};

} // end namespace OHOS
#endif
