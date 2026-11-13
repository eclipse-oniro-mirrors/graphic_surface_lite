#include "surface_utils.h"

#include <unistd.h>
#include "buffer_common.h"
#include "surface_impl.h"

namespace OHOS {
SurfaceUtils::SurfaceUtils()
{
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
        return 0;
    }
    uint64_t id = 0;
    pthread_mutex_lock(&lock_);
    std::map<uint64_t, SurfaceEntry>::iterator iter;
    for (iter = surfaceMap_.begin(); iter != surfaceMap_.end(); ++iter) {
        if (iter->second.surface == surface) {
            pthread_mutex_unlock(&lock_);
            return 0;
        }
    }
#ifdef SURFACE_LITE_SINGLE_PROCESS
    const uint32_t pid = 0;
#else
    const uint32_t pid = static_cast<uint32_t>(getpid());
#endif
    id = (static_cast<uint64_t>(pid) << 32) | (++counter_);
    SurfaceEntry entry;
    entry.surface = surface;
    entry.proxyCount = 0;
#ifndef SURFACE_LITE_SINGLE_PROCESS
    entry.sid.handle = IPC_INVALID_HANDLE;
    entry.sid.token = 0;
    entry.sid.cookie = 0;
#endif
    surfaceMap_[id] = entry;
    pthread_mutex_unlock(&lock_);
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
            GRAPHIC_LOGE("Surface %llu destroyed with %u live producer proxies.",
                         static_cast<unsigned long long>(id), iter->second.proxyCount);
        }
        surfaceMap_.erase(iter);
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
        SurfaceImpl* impl = static_cast<SurfaceImpl*>(iter->second.surface);
        sid = impl->sid_;
        iter->second.sid = sid;
    }
    pthread_mutex_unlock(&lock_);
    return sid;
}
#endif
} // namespace OHOS
