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
#ifndef SURFACE_LITE_SINGLE_PROCESS
#include "ipc_skeleton.h"
#endif
#include "surface.h"

namespace OHOS {
/**
 * @brief Global surface id registry (innerkits internal, not exported to kits).
 *
 * Consumer surfaces register automatically at construction and unregister at
 * destruction. The id is encoded as (PID << 32 | incremental counter); in single
 * process mode the PID part is 0 (there is exactly one process by definition).
 * Other modules address a surface by its id instead of passing raw pointers
 * or SvcIdentity around.
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

private:
    SurfaceUtils();
    ~SurfaceUtils();
    struct SurfaceEntry {
        Surface* surface;
#ifndef SURFACE_LITE_SINGLE_PROCESS
        SvcIdentity sid;
#endif
    };
    pthread_mutex_t lock_;
    std::map<uint64_t, SurfaceEntry> surfaceMap_;
    uint64_t counter_ = 0;
};
} // end namespace OHOS
#endif
