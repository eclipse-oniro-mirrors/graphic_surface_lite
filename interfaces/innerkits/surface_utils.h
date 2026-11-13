#ifndef GRAPHIC_LITE_SURFACE_UTILS_H
#define GRAPHIC_LITE_GRAPHIC_UTILS_H

#include <map>
#include <pthread.h>
#ifndef SURFACE_LITE_SINGLE_PROCESS
#include "ipc_skeleton.h"
#endif
#include "surface.h"
#include "surface_impl.h"

namespace OHOS {
class SurfaceUtils {
public:
    static SurfaceUtils* GetInstance();
    uint64_t Register(Surface* surface);
    Surface* GetSurface(uint64_t id);
    void Unregister(uint64_t id);
#ifndef SURFACE_LITE_SINGLE_PROCESS
    SvcIdentity GetSvcIdentity(uint64_t id);
#endif
    bool AddProxy(uint64_t id);
    void RemoveProxy(uint64_t id);

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

    static Surface* CreateSurfaceProxy(uint64_t surfaceId);

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
