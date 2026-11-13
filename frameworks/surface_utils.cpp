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

#include "surface_utils.h"

#include <unistd.h>
#include "buffer_common.h"
#include "surface_impl.h"

namespace OHOS {
SurfaceUtils::SurfaceUtils()
{
    // Recursive: listener callbacks fired inside a WithProducer call scope may
    // legally re-enter surfaces (see the class comment).
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock_, &attr);
    pthread_mutexattr_destroy(&attr);
}

SurfaceUtils::~SurfaceUtils()
{
    pthread_mutex_destroy(&lock_);
}

SurfaceUtils* SurfaceUtils::GetInstance()
{
    static SurfaceUtils instance;
    return &instance;
}

uint64_t SurfaceUtils::Register(Surface* surface)
{
    if (surface == nullptr) {
        GRAPHIC_LOGE("Register surface failed: surface is null.");
        return 0;
    }
    uint64_t id = 0;
    pthread_mutex_lock(&lock_);
    std::map<uint64_t, SurfaceEntry>::iterator iter;
    for (iter = surfaceMap_.begin(); iter != surfaceMap_.end(); ++iter) {
        if (iter->second.surface == surface) {
            GRAPHIC_LOGE("Register surface failed: surface is already registered.");
            pthread_mutex_unlock(&lock_);
            return 0;
        }
    }
#ifdef SURFACE_LITE_SINGLE_PROCESS
    const uint32_t pid = 0; // single process mode has no PID concept
#else
    const uint32_t pid = static_cast<uint32_t>(getpid());
#endif
    id = (static_cast<uint64_t>(pid) << 32) | (++counter_);
    SurfaceEntry entry;
    entry.surface = surface;
#ifndef SURFACE_LITE_SINGLE_PROCESS
    entry.sid.handle = IPC_INVALID_HANDLE;
    entry.sid.token = 0;
    entry.sid.cookie = 0;
#endif
    surfaceMap_[id] = entry;
    pthread_mutex_unlock(&lock_);
    GRAPHIC_LOGD("Register surface, id = %llu.", static_cast<unsigned long long>(id));
    return id;
}

Surface* SurfaceUtils::GetSurface(uint64_t id)
{
    pthread_mutex_lock(&lock_);
    std::map<uint64_t, SurfaceEntry>::iterator iter = surfaceMap_.find(id);
    if (iter == surfaceMap_.end()) {
        pthread_mutex_unlock(&lock_);
        return nullptr;
    }
    Surface* surface = iter->second.surface;
    pthread_mutex_unlock(&lock_);
    return surface;
}

void SurfaceUtils::Unregister(uint64_t id)
{
    pthread_mutex_lock(&lock_);
    std::map<uint64_t, SurfaceEntry>::iterator iter = surfaceMap_.find(id);
    if (iter != surfaceMap_.end()) {
        if (iter->second.proxyCount > 0) {
            GRAPHIC_LOGE("Surface %llu destroyed with %u live producer proxies: "
                         "their calls will fail from now on.",
                         static_cast<unsigned long long>(id), iter->second.proxyCount);
        }
        surfaceMap_.erase(iter);
        GRAPHIC_LOGD("Unregister surface, id = %llu.", static_cast<unsigned long long>(id));
    }
    pthread_mutex_unlock(&lock_);
}

bool SurfaceUtils::AddProxy(uint64_t id)
{
    pthread_mutex_lock(&lock_);
    std::map<uint64_t, SurfaceEntry>::iterator iter = surfaceMap_.find(id);
    bool found = (iter != surfaceMap_.end());
    if (found) {
        iter->second.proxyCount++;
    }
    pthread_mutex_unlock(&lock_);
    return found;
}

void SurfaceUtils::RemoveProxy(uint64_t id)
{
    pthread_mutex_lock(&lock_);
    std::map<uint64_t, SurfaceEntry>::iterator iter = surfaceMap_.find(id);
    if ((iter != surfaceMap_.end()) && (iter->second.proxyCount > 0)) {
        iter->second.proxyCount--;
    }
    pthread_mutex_unlock(&lock_);
}

#ifndef SURFACE_LITE_SINGLE_PROCESS
SvcIdentity SurfaceUtils::GetSvcIdentity(uint64_t id)
{
    SvcIdentity sid;
    sid.handle = IPC_INVALID_HANDLE;
    sid.token = 0;
    sid.cookie = 0;
    pthread_mutex_lock(&lock_);
    std::map<uint64_t, SurfaceEntry>::iterator iter = surfaceMap_.find(id);
    if (iter != surfaceMap_.end() && iter->second.surface != nullptr) {
        // Read the live value from the surface implementation (friend access);
        // the constructor-time snapshot would be zero before Init.
        SurfaceImpl* impl = static_cast<SurfaceImpl*>(iter->second.surface);
        sid = impl->sid_;
        iter->second.sid = sid;
    }
    pthread_mutex_unlock(&lock_);
    return sid;
}
#endif
} // namespace OHOS
