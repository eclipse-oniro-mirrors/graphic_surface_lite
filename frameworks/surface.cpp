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

#include "surface.h"
#include "surface_impl.h"
#include "surface_utils.h"

namespace OHOS {
Surface* Surface::CreateSurface()
{
    SurfaceImpl* surface = new SurfaceImpl();
    if (surface != nullptr) {
        if (surface->Init()) {
            return reinterpret_cast<Surface *>(surface);
        } else {
            GRAPHIC_LOGE("surface init failed");
            delete surface;
        }
    }
    GRAPHIC_LOGE("Create surface failed");
    return nullptr;
}

Surface* Surface::CreateSurfaceAsProducer(uint64_t surfaceId)
{
#ifdef SURFACE_LITE_SINGLE_PROCESS
    // H routing: the proxy holds only the id; all calls go through the registry.
    return SurfaceUtils::CreateSurfaceProxy(surfaceId);
#else
    // Cross process: use the existing path instead — GetSurface(id) plus
    // WriteIoIpcIo on the consumer side, GenericSurfaceByIpcIo on the producer side.
    (void)surfaceId;
    GRAPHIC_LOGE("CreateSurfaceAsProducer is unsupported in cross process mode, "
                 "use GetSurface(id) and the existing WriteIoIpcIo/GenericSurfaceByIpcIo path instead.");
    return nullptr;
#endif
}

Surface::~Surface()
{
}
} // namespace OHOS
