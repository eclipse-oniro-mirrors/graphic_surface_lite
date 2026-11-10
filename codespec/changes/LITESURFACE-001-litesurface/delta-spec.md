# LITESURFACE-001 delta-spec：相对现有实现的增量规格

- **基线**：foundation/graphic/surface_lite @ master（4ed2ded）
- **上游文档**：proposal.md（已评审）
- **约定**：`SP` = `SURFACE_LITE_SINGLE_PROCESS`；"追加" = 既有语句/签名/成员逐句保留，仅新增语句/成员/分支；**不变式 I0：默认配置（`surface_lite_single_process=false`）下，预处理后既有代码与基线逐句一致，仅允许出现新增内容**

---

## 1. 编译宏与错误码

### 1.1 构建开关（BUILD.gn）

```gn
declare_args() {
    surface_lite_single_process = false
}
```

- `true` → `defines += [ "SURFACE_LITE_SINGLE_PROCESS=1" ]`
- 默认 `false`：目标、源文件、依赖、ldflags 与基线**完全一致**

### 1.2 错误码（`interfaces/innerkits/buffer_common.h`，枚举尾部追加）

```cpp
enum BufferErrorCode {
    SURFACE_ERROR_INVALID_PARAM = -10,
    SURFACE_ERROR_INVALID_REQUEST,
    SURFACE_ERROR_NOT_READY,
    SURFACE_ERROR_SYSTEM_ERROR,
    SURFACE_ERROR_BUFFER_NOT_EXISTED,
    SURFACE_ERROR_BUFFER_IN_USE,      // [新增] 值 -5，占用现有空档；既有值不重排
    SURFACE_ERROR_OK = 0,
};
```

> kits 头不含错误码；调用方判断 `SURFACE_ERROR_BUFFER_IN_USE` 需包含 innerkits 的 `buffer_common.h`（实现期陷阱 #5）。

### 1.3 常量（`interfaces/kits/surface_type.h`，尾部追加）

```cpp
constexpr uint8_t SURFACE_MAX_CHUNK_NUM = 5;          // 段上限：覆盖单段/双平面/环形跨段/码流分包
constexpr uint32_t SURFACE_PIXEL_FORMAT_JPEG = 0x1000; // 避开 ImageFormat 的 0/1xx/2xx 值段
constexpr uint32_t SURFACE_PIXEL_FORMAT_H264 = 0x1001;
```

## 2. 数据结构

### 2.1 `SurfaceDataChunk`（`interfaces/kits/surface_buffer.h` 追加）

```cpp
struct SurfaceDataChunk {
    void* virAddr;      // 段虚拟地址（HAL 映射后传入）
    uint64_t phyAddr;   // 段物理地址（驱动分配，物理对齐）
    uint32_t size;      // 段字节数
    uint32_t stride;    // 段行距
    uint8_t planeId;    // 平面号：RGB=0；NV12/NV21：Y=0、UV=1；环形/码流分段沿用所属平面号
};
```

**不设 offset 字段**。逻辑位置约定：同一 `planeId` 的段按 `SetChunk` 调用顺序即数据逻辑顺序；**写入必须按 0,1,2,… 递增**（跳号失败，见 §2.2），驱动乱序由 HAL 适配层排序后依次写入。

### 2.2 段数语义

- 段数 = `SetChunk` 调用过的最大 index + 1（`chunkCount_`），**不单独声明**
- `SetChunk(index)` 合法域：`index < SURFACE_MAX_CHUNK_NUM`（即 index ∈ [0,4]），越界返回 `-1`
- **顺序写入规则（跳号拒绝）**：
  - `index == chunkCount_`：追加成功，`chunkCount_ = index + 1`
  - `index < chunkCount_`：覆写既有槽位成功，`chunkCount_` 不变（供环形复用重指向 / Attach 前修正）
  - `index > chunkCount_`：返回 `-1`（跳号失败；API 层杜绝稀疏写入与洞位）
- 段数只增不减；段数变少的帧应新建 wrapper（`CreateSurfaceBuffer` 后 Attach）

### 2.3 统一视图规则（全 Buffer 生效）

| chunkCount_ | `GetChunk(0)` | `GetVirAddr/GetPhyAddr/GetSize`（`@deprecated`） | `GetTotalDataSize()` | `GetChunkCount()` |
|---|---|---|---|---|
| 0（legacy，框架分配或未配置） | 合成：`{GetVirAddr(), GetPhyAddr(), GetSize(), bufferData_.handle.stride, 0}` | 原基类行为不变 | 等于 legacy `GetSize()` | 1 |
| 1 | chunk[0] | 与 chunk[0] 对应字段**恒等** | chunk[0].size | 1 |
| >1 | chunk[0]（主接口失读防误用） | `nullptr / 0 / 0` | 全段 size 之和 | chunkCount_ |

- 主内存接口 `GetVirAddr/GetPhyAddr/GetSize` 标注 `@deprecated`（签名与 0/1 段行为保留）
- **地址写入单一入口**：外部 Buffer 的地址只经 `SetChunk` 写入；不新增 SetVirAddr/SetPhyAddr 虚函数（innerkits 内部既有 setter 保留，供框架分配路径内部使用）

### 2.4 `SurfaceUtils`（新文件 `interfaces/innerkits/surface_utils.h` + `frameworks/surface_utils.cpp`）

innerkits 内部类，**不出现在 kits/ndk head_files**：

```cpp
class SurfaceUtils {
public:
    static SurfaceUtils* GetInstance();                     // Meyers 单例（C++11 线程安全）
    uint64_t Register(Surface* surface);                    // 成功返回 id；空指针/重复注册返回 0
    Surface* GetSurface(uint64_t id);                       // 未命中返回 nullptr
    void Unregister(uint64_t id);
#ifndef SURFACE_LITE_SINGLE_PROCESS
    SvcIdentity GetSvcIdentity(uint64_t id);               // 仅本进程注册的 Surface 有效，否则返回零值
#endif
private:
    SurfaceUtils();
    ~SurfaceUtils();
    pthread_mutex_t lock_;                                  // map 并发保护
    struct SurfaceEntry {
        Surface* surface;
#ifndef SURFACE_LITE_SINGLE_PROCESS
        SvcIdentity sid;                                    // 注册时快照；Init 前为全零（见 §4.2 说明）
#endif
    };
    std::map<uint64_t, SurfaceEntry> surfaceMap_;
    uint64_t counter_ = 0;
};
```

- **ID 编码**：`(uint64_t)pid << 32 | (++counter_)`；SP 模式 `pid` 取 0（单进程无 PID 概念，且 liteos_m 无 `getpid()` 保证），跨进程模式取 `getpid()`
- 消费端 `SurfaceImpl` 构造自动 `Register`、析构自动 `Unregister`（见 §4.2）
- 读 `SurfaceImpl::sid_` 的授权：`SurfaceImpl` 追加 `friend class SurfaceUtils;`（一行，追加于 private 段）

## 3. 接口签名（逐文件）

### 3.1 `interfaces/kits/iproducer_listener.h`（新文件）

```cpp
namespace OHOS {
class SurfaceBuffer;                                    // 前置声明
class IProducerListener {
public:
    IProducerListener() {}
    virtual ~IProducerListener() {}

    /**
     * Buffer 已回到空闲（RELEASE）。复用还是归还驱动内存由生产者自决。
     * 调用上下文：队列锁外；回调内允许重入 Surface（如 DetachBuffer/AttachBuffer）。
     */
    virtual void OnBufferReleased(SurfaceBuffer* buffer) = 0;
};
} // namespace OHOS
```

**生产者回调仅此一个**。不设计第二个 Detach 回调，不设计 detachPending 延迟兑现标记。

### 3.2 `interfaces/kits/surface_buffer.h`（追加）

```cpp
#include "surface_type.h"                                // [新增 include] 取 SURFACE_MAX_CHUNK_NUM

struct SurfaceDataChunk { ... };                         // §2.1

class SurfaceBuffer {
public:
    static SurfaceBuffer* CreateSurfaceBuffer();         // [新增] 纯声明；实现在 frameworks/surface_buffer_impl_ex.cpp

    virtual void* GetVirAddr() const = 0;                // 既有；@deprecated 见 §2.3
    virtual uint64_t GetPhyAddr() const = 0;            // 既有；@deprecated
    virtual uint32_t GetSize() const = 0;                // 既有；@deprecated
    virtual void SetSize(uint32_t size) = 0;             // 既有
    virtual int32_t SetInt32(...) = 0;                   // 既有 4 个 extra data 接口
    ...                                                  // 既有不变

    // [新增] 以下虚函数均带默认实现（既有派生类无需改动即可编译）
    // 注：不新增 GetStride 虚函数——stride 经 GetChunk(i).stride / innerkits 访问（§7 陷阱 #1 规避）
    virtual int32_t SetChunk(uint8_t index, const SurfaceDataChunk& chunk) { (void)index; (void)chunk; return -1; }
    virtual int32_t GetChunk(uint8_t index, SurfaceDataChunk& chunk) const { (void)index; return -1; }
    virtual uint32_t GetChunkCount() const { return 0; }     // SurfaceBufferImpl 覆写后恒 ≥1（§2.3）
    virtual uint32_t GetTotalDataSize() const { return 0; }
protected:
    SurfaceBuffer() {}
    virtual ~SurfaceBuffer() {}                          // 既有，protected 保持（R3 风险已评审）
};
```

### 3.3 `interfaces/kits/surface.h`（追加）

```cpp
#include <string>                                        // [新增] 陷阱 #4：显式包含
#include "iproducer_listener.h"                          // [新增]

class Surface {
public:
    static Surface* CreateSurface();                      // 既有
    static Surface* CreateSurfaceAsProducer(uint64_t surfaceId);  // [新增] 见 §5.1
    virtual ~Surface();                                  // 既有
    ... 既有纯虚接口逐条不变（SetQueueSize ~ UnregisterConsumerListener）...

    // [新增] 默认实现虚函数（追加式；SurfaceImpl 覆写）
    virtual uint64_t GetSurfaceId() const { return 0; }
    virtual int32_t AttachBuffer(SurfaceBuffer* buffer) { (void)buffer; return -1; }
    virtual int32_t DetachBuffer(SurfaceBuffer* buffer) { (void)buffer; return -1; }
    virtual void RegisterProducerListener(IProducerListener& listener) { (void)listener; }
    virtual void UnregisterProducerListener() {}
protected:
    Surface() {}
};
```

### 3.4 `interfaces/innerkits/surface_buffer_impl.h`（追加）

```cpp
#ifndef SURFACE_LITE_SINGLE_PROCESS
#include "ipc_skeleton.h"                                // [宏门控] 既有 include
#endif
#include "surface_type.h"                                // [新增] chunk 常量

class SurfaceBufferImpl : public SurfaceBuffer {
public:
    ...
    // 既有 int32_t GetStride() const 保持原样、零改动（陷阱 #1 规避：不新增 kits 虚函数则无冲突）

    // [新增 override] chunk 视图（基类语义，覆盖框架分配 Buffer）
    int32_t SetChunk(uint8_t index, const SurfaceDataChunk& chunk) override;   // 恒返回 -1（框架托管内存不开放段写入）
    int32_t GetChunk(uint8_t index, SurfaceDataChunk& chunk) const override;   // §2.3 合成规则
    uint32_t GetChunkCount() const override;                                   // 恒返回 1（legacy 单段视图）
    uint32_t GetTotalDataSize() const override;                               // 等于 GetSize()

#ifndef SURFACE_LITE_SINGLE_PROCESS
    void ReadFromIpcIo(IpcIo& io);                        // [宏门控] 既有声明
    void WriteToIpcIo(IpcIo& io);                         // [宏门控]
#endif
    ...
protected:
    // [新增] legacy → chunk 合成辅助（SurfaceBufferImplEx 复用）
    const SurfaceDataChunk& GetVirtualChunk() const;      // 写 mutable virtualChunk_ 后返回引用
    // [实现期追加] 绕过容量校验写 len_（len_ 为基类 private，Ex 的 SetSize 覆写经此桥访问）
    void SetSizeUnchecked(uint32_t size) { len_ = size; }
private:
    ...
    struct SurfaceBufferData bufferData_;                 // 既有成员不动
    std::map<uint32_t, ExtraData> extDatas_;             // 既有
    uint32_t len_;                                        // 既有
    mutable SurfaceDataChunk virtualChunk_ = { nullptr, 0, 0, 0, 0 };  // [新增·尾部追加]
};
```

### 3.5 `frameworks/surface_buffer_impl_ex.cpp`（新文件）

匿名命名空间内定义 **`SurfaceBufferImplEx : public SurfaceBufferImpl`**，不新增头文件，对外仅暴露 `SurfaceBuffer*`：

```cpp
namespace {
class SurfaceBufferImplEx : public SurfaceBufferImpl {
public:
    SurfaceBufferImplEx() = default;
    ~SurfaceBufferImplEx() override = default;

    // [必须 override｜陷阱基线] 基类 SetSize 有 `size > bufferData_.size` 容量校验，
    // 外部 Buffer 的 bufferData_.size 恒 0，不覆写会被静默拒绝。容量正确性由 HAL 自负。
    void SetSize(uint32_t size) override { len_ = size; }

    void* GetVirAddr() const override;        // §2.3 三分支：0=基类 / 1=chunk[0].virAddr / >1=nullptr
    uint64_t GetPhyAddr() const override;     // 同上：>1 返回 0
    uint32_t GetSize() const override;        // 同上：>1 返回 0

    int32_t SetChunk(uint8_t index, const SurfaceDataChunk& chunk) override;  // 真实存储
    int32_t GetChunk(uint8_t index, SurfaceDataChunk& chunk) const override;
    uint32_t GetChunkCount() const override;                                 // chunkCount_?:1
    uint32_t GetTotalDataSize() const override;                             // 求和或 legacy
private:
    SurfaceDataChunk chunks_[SURFACE_MAX_CHUNK_NUM] = {};   // 5 段 ≈ 160B，仅外部 Buffer 承担
    uint8_t chunkCount_ = 0;
};
} // namespace

SurfaceBuffer* SurfaceBuffer::CreateSurfaceBuffer()
{
    return new (std::nothrow) SurfaceBufferImplEx();
}
```

- `GetChunk(0)` 未 `SetChunk` 时经 `GetVirtualChunk()` 合成到 `virtualChunk_`（§3.4）
- `GetChunk(i)`：`i >= chunkCount_` 返回 `-1`；顺序写入规则（§2.2）保证 `i < chunkCount_` 恒为已写槽位，无需洞位探测
- 段写入仅生产者在 **Attach 前**执行；Attach 后消费者只读（线程可见性由队列锁保证）
- 段存储数组内置于 Ex：**框架分配 Buffer（SurfaceBufferImpl）零新增内存开销**（US-2 前提，RTOS 堆友好）

### 3.6 `interfaces/innerkits/buffer_queue.h`（追加）

```cpp
#include <list>
#include <map>
#include <string>                                       // [新增｜陷阱 #3] 显式包含
#include "ibuffer_consumer_listener.h"                  // [新增｜陷阱 #3] 镜像监听器类型
#include "iproducer_listener.h"                          // [新增]
#include "surface_buffer_impl.h"

class BufferQueue {
public:
    ... 既有接口不变 ...

    // [新增] 外部 Buffer 旁路生命周期
    int32_t AttachBuffer(SurfaceBufferImpl* buffer);
    int32_t DetachBuffer(SurfaceBufferImpl* buffer);
    // [新增] 监听器镜像（回调以队列为唯一触发点；命名对齐既有 Register/Unregister 惯例）
    void RegisterProducerListener(IProducerListener* listener);
    void UnregisterProducerListener();
    IProducerListener* GetProducerListener();
    void RegisterConsumerListener(IBufferConsumerListener* listener);
    void UnregisterConsumerListener();
    IBufferConsumerListener* GetConsumerListener();
private:
    ... 既有成员与私有方法不动 ...
    // [新增] 既有 GetBuffer 的 attachList_ 对应物
    SurfaceBufferImpl* GetAttachedBuffer(const SurfaceBufferImpl& buffer);
    // [新增·尾部追加成员]
    std::list<SurfaceBufferImpl*> attachList_;           // 旁路表：外部 Buffer 绝不进 allBuffers_/freeList_
    IProducerListener* producerListener_ = nullptr;       // 类内初始化，不动既有构造初始化列表
    IBufferConsumerListener* consumerListener_ = nullptr;
};
```

### 3.7 `interfaces/innerkits/buffer_producer.h`（追加）

```cpp
class BufferProducer {
public:
    ... 既有纯虚接口不变 ...
    // [新增] 默认实现虚函数：BufferClientProducer（跨进程客户端）默认不支持 → 返回 -1/no-op
    virtual int32_t AttachBuffer(SurfaceBufferImpl* buffer) { (void)buffer; return -1; }
    virtual int32_t DetachBuffer(SurfaceBufferImpl* buffer) { (void)buffer; return -1; }
    virtual void RegisterProducerListener(IProducerListener& listener) { (void)listener; }
    virtual void UnregisterProducerListener() {}
};
```

### 3.8 `interfaces/innerkits/surface_impl.h`（追加 + 宏门控）

```cpp
#ifndef SURFACE_LITE_SINGLE_PROCESS
#include "ipc_skeleton.h"      // [宏门控] 经由既有 include 链引入 SvcIdentity/IpcIo/MessageOption
#endif

class SurfaceImpl : public Surface {
public:
    SurfaceImpl();                                     // 既有消费端构造：尾部追加注册（§4.2）
#ifndef SURFACE_LITE_SINGLE_PROCESS
    static Surface* GenericSurfaceByIpcIo(IpcIo& io);  // [宏门控] 既有；跨进程生产者创建**仅此既有通路**（评审定论：跨进程接口面零新增，不设 (id, sid) 重载）
#endif
    // [新增] 单进程生产者代理工厂（无 IPC 类型，两态皆编译）
    static Surface* CreateProducerProxy(Surface* consumerSurface);

    ... 既有 override 不变 ...
    // [新增 override]
    uint64_t GetSurfaceId() const override;
    int32_t AttachBuffer(SurfaceBuffer* buffer) override;
    int32_t DetachBuffer(SurfaceBuffer* buffer) override;
    void RegisterProducerListener(IProducerListener& listener) override;
    void UnregisterProducerListener() override;

#ifndef SURFACE_LITE_SINGLE_PROCESS
    void WriteIoIpcIo(IpcIo& io);                      // [宏门控] 既有
    int32_t DoIpcMsg(...);                             // [宏门控] 既有
#endif
    bool Init();
private:
#ifndef SURFACE_LITE_SINGLE_PROCESS
    SurfaceImpl(const SvcIdentity& sid);                // [宏门控] 既有私有构造
    static int32_t IpcRequestHandler(...);              // [宏门控] 既有
    SvcIdentity sid_;                                   // [宏门控] 既有成员
    IpcObjectStub objectStub_;                          // [宏门控] 既有成员
#endif
    explicit SurfaceImpl(BufferProducer* sharedProducer);  // [新增] 单进程代理构造：不注册、不拥有
    BufferQueueConsumer* consumer_;                     // 既有
    BufferProducer* producer_;                           // 既有
    bool IsConsumer_;                                    // 既有
    friend class SurfaceUtils;                          // [新增] 供 GetSvcIdentity 读 sid_
    uint64_t surfaceId_ = 0;                             // [新增·尾部追加]
    bool ownProducer_ = true;                            // [新增·尾部追加] 代理实例为 false
};
```

### 3.9 `frameworks/buffer_queue_producer.h`（追加 + 宏门控）

```cpp
class BufferQueueProducer : public BufferProducer {
public:
    ... 既有接口不变 ...
    // [新增 override] 委托到队列
    int32_t AttachBuffer(SurfaceBufferImpl* buffer) override;
    int32_t DetachBuffer(SurfaceBufferImpl* buffer) override;
    void RegisterProducerListener(IProducerListener& listener) override;
    void UnregisterProducerListener() override;
#ifndef SURFACE_LITE_SINGLE_PROCESS
    int32_t OnIpcMsg(uint32_t code, IpcIo *data, IpcIo *reply, MessageOption option);  // [宏门控] 既有
#endif
private:
    BufferQueue* bufferQueue_;      // 既有
    IBufferConsumerListener* consumerListener_;  // 既有（既有 Flush 路径触发点保持不变）
};
```

## 4. 状态机与生命周期不变量

### 4.1 外部 Buffer 状态机（BufferState 既有枚举复用，无新增状态）

```
            AttachBuffer(首次)                AcquireBuffer
 (新建) ───────────────────► FLUSH ────────────────────────► ACQUIRE
                              ▲  │                            │
                              │  │ DetachBuffer(FLUSH 态)     │ ReleaseBuffer
                              │  ▼  ── 摘链+delete wrapper     ▼
              AttachBuffer    │ (同步兑现)                  RELEASE
           (RELEASE 态重入,   │                              ┌──────┴───────┐
            仅重入 dirtyList_) │                     DetachBuffer(RELEASE 态)
                              └──────────────────────────────┤  摘链+delete
                                                             │  (同步兑现)
                                                   DetachBuffer(ACQUIRE 态)
                                                   → SURFACE_ERROR_BUFFER_IN_USE
```

### 4.2 不变量（评审与实现的验收判据）

| # | 不变量 |
|---|--------|
| V1 | 外部 Buffer 只存在于 `attachList_`，**任何路径**不得进入 `allBuffers_`/`freeList_`（否则既有析构/Reset/SetQueueSize 缩容路径的 `BufferManager::FreeBuffer` 将误释放驱动内存） |
| V2 | 既有路径安全域推导：`GetBuffer()` 只查 `allBuffers_`；`RequestBuffer` 只出 `freeList_`；`Reset`/`SetQueueSize`/析构 只遍历 `freeList_`/`allBuffers_` —— 对外部 Buffer 天然不触及，**既有函数体无需修改** |
| V3 | 状态转换单一事实来源 = 既有 `BufferState`；不引入引用计数、不引入 detachPending 标记 |
| V4 | 重复 Attach：wrapper 已在 `attachList_` 且状态 **RELEASE** → 仅重入 `dirtyList_`（防析构重复 delete → UAF）；**FLUSH/ACQUIRE** → 拒绝（`SURFACE_ERROR_INVALID_REQUEST`，防双入队）；`existed != buffer`（同句柄异 wrapper）→ `SURFACE_ERROR_INVALID_PARAM` |
| V5 | 外部 Buffer 的 Release 分支：置 RELEASE、留在 `attachList_`、**不 ClearExtraData()**（`DRIVER_BUF_ID_KEY` 等驱动句柄生命周期随 wrapper）；不 push `freeList_` |
| V6 | 生产者回调 = `IProducerListener::OnBufferReleased` 唯一；消费侧 = 既有 `IBufferConsumerListener::OnBufferAvailable`（镜像至队列后由 Attach 路径触发）。**一律锁外执行** |
| V7 | 回调语义 = "Buffer 已空闲"：HAL 查自有待回收表，命中 → 回调内调 `DetachBuffer`（RELEASE 态同步兑现）归还驱动内存；未命中 → re-Attach 复用 |
| V8 | 消费端 SurfaceImpl 构造自动注册（id 存 `surfaceId_`）、析构自动注销；生产者代理**不注册**（GetSurface(id) 恒返回消费端实例） |
| V9 | 单进程消费端/生产端为两个 SurfaceImpl 实例**共享同一 BufferQueueProducer**（进而共享 BufferQueue）；生产者代理 `ownProducer_=false`，析构不 delete producer_ |
| V10 | `RequestBuffer`（SP 模式）：`freeList_` 恒空且无法分配 → 返回 nullptr 并 GRAPHIC_LOGI 说明"单进程模式不支持内部分配，请使用 AttachBuffer" |

### 4.3 注册/注销时点

- `SurfaceImpl()`（消费端构造）：函数体**末尾追加** `surfaceId_ = SurfaceUtils::GetInstance()->Register(this);`
- `~SurfaceImpl()`：**开头追加**注销 + `ownProducer_` 保护（`if (!ownProducer_) producer_ = nullptr;` 置空后走既有 delete 判空分支），随后既有语句原样执行
- `GetSvcIdentity` 返回注册时刻的 sid 快照；`Init()` 完成前为全零（文档明示：生产者应在 `CreateSurface()` 返回后取 sid）

## 5. 时序图

### 5.1 单进程：创建 → 生产 → 消费 → 释放回调 → 环形复用

```
 消费线程            SurfaceUtils        生产线程(HAL)          BufferQueue
    │ CreateSurface()     │                   │                    │
    ├─new SurfaceImpl────►│Register──────────►│                    │
    │ surfaceId◄──────────┼───id               │                    │
    │  │                  │                   │CreateSurfaceAsProducer(id)
    │  │                  │◄──────────────────┤GetSurface(id)────►│
    │  │                  │  consumerSurface►├────────────────────┤
    │  │                  │                   │ new SurfaceImpl(consumer->producer_)
    │  │                  │                   │   (proxy, 不注册)   │
    │  │                  │                   │                    │
    │  │                  │  [帧循环] CreateSurfaceBuffer()+SetChunk+SetSize
    │  │                  │                   ├────AttachBuffer────►│ attachList_+dirtyList_
    │  │                  │                   │                    │ state=FLUSH (锁外)
    │◄─────────────────────────────────────────────────────────────┤ OnBufferAvailable()
    ├────AcquireBuffer─────────────────────────────────────────────►│ dirtyList_→ACQUIRE
    │  读 GetChunkCount/GetChunk/GetTotalDataSize                  │
    ├────ReleaseBuffer─────────────────────────────────────────────►│ 置RELEASE,留attachList_
    │  │                  │                   │◄───────────────────┤ OnBufferReleased(buffer)  [锁外]
    │  │                  │                   │ 查待回收表: 未命中  │
    │  │                  │                   ├────AttachBuffer────►│ 仅重入dirtyList_ (环形复用)
    │  ...                │                   │ ... 多帧后停止 ...   │
    │  │                  │                   ├────DetachBuffer────►│ RELEASE态:摘链+delete wrapper
    │ delete consumer  ──►│Unregister        │ delete proxy        │ 析构: attachList_ 清理
```

### 5.2 Detach 争用：ACQUIRED → IN_USE → 待回收表 → 归还驱动

```
 生产线程(HAL)                     BufferQueue                      消费线程
    │                               │                                │
    ├────DetachBuffer(buffer)──────►│ state==ACQUIRE                 │
    │◄── SURFACE_ERROR_BUFFER_IN_USE│ (不摘链)                        │
    │ [记入 HAL 自有"待回收表"]        │                                │
    │                               │◄─────────ReleaseBuffer────────┤
    │                               │ 置RELEASE, 留attachList_       │
    │◄──────OnBufferReleased(buffer)│ [锁外]                          │
    │ 查待回收表: 命中               │                                │
    ├────DetachBuffer(buffer)───────►│ RELEASE态:                     │
    │  (回调内重入, 锁外安全)          │  dirty/attachList_摘链          │
    │  归还驱动内存(调用栈上)          │  + delete wrapper               │
```

### 5.3 跨进程（默认配置）行为对照：零差异

```
 消费进程                                       生产进程
    │ CreateSurface() → id 自动注册              │
    │ (sid 经既有 WriteIoIpcIo 通路传出)           │
    │                                           │ GenericSurfaceByIpcIo(io)  [既有通路，评审定论：跨进程接口面零新增]
    │                                           │  → new SurfaceImpl(sid) + Init()
    │                                           │  → BufferClientProducer(sid_)      [既有路径]
    │  RequestBuffer/FlushBuffer/Cancel ──既有 IpcIo 流量──► OnIpcMsg 处理
    │  CreateSurfaceAsProducer(id) → nullptr + 日志提示使用 (id,sid) 重载
    │  AttachBuffer/DetachBuffer/RegisterProducerListener on BufferClientProducer → 默认 -1/no-op
```

## 6. 构建与编译隔离

### 6.1 `BUILD.gn`（默认配置输出与基线逐项一致）

```gn
import("//build/lite/config/component/lite_component.gni")
import("//build/lite/ndk/ndk.gni")
import("//foundation/graphic/surface_lite/surface_lite_config.gni")  # [R-B2] 共享 declare_args

lite_component("surface_lite") { ... }                     # 既有不动
ndk_lib("surface_lite_ndk")    { ... }                     # 既有不动

shared_library("surface") {
    sources = [
        "frameworks/buffer_queue.cpp",
        "frameworks/buffer_queue_consumer.cpp",
        "frameworks/buffer_queue_producer.cpp",
        "frameworks/surface.cpp",
        "frameworks/surface_buffer_impl.cpp",
        "frameworks/surface_buffer_impl_ex.cpp",           # [新增] 常态参与
        "frameworks/surface_impl.cpp",
        "frameworks/surface_utils.cpp",                    # [新增] 常态参与
    ]
    if (surface_lite_single_process) {
        # 纯 IPC / 纯 Gralloc 翻译单元按设计剔除（基线路径在 SP 下不可达）
        defines = [ "SURFACE_LITE_SINGLE_PROCESS=1" ]
    } else {
        sources += [
            "frameworks/buffer_client_producer.cpp",
            "frameworks/buffer_manager.cpp",
        ]
    }
    include_dirs = [ ...既有两项... ]                       # 不变
    public_configs = [ ":surface_public_config" ]
    if (surface_lite_single_process) {
        # [R-B1] SP 模式对 graphic_utils_lite 仅为头文件依赖（日志宏 printf 回退 +
        # 枚举常量，无链接符号）：不链接 utils_lite，补其 innerkits 头路径
        # （默认 graphic_config.h 所在目录，原经 public_deps 传播）。
        public_configs += [ ":surface_single_process_config" ]
        public_deps = []
    } else {
        public_deps  = [ "//foundation/graphic/graphic_utils_lite:utils_lite" ]  # 不变
    }
    deps = [ "//third_party/bounds_checking_function:libsec_shared" ]         # libsec 两态保留
    if (!surface_lite_single_process) {
        deps += [
            "//drivers/peripheral/display/hal:hdi_display",
            "//foundation/communication/ipc/interfaces/innerkits/c/ipc:ipc_single",
        ]
        ldflags = [ "-ldisplay_gfx", "-ldisplay_gralloc", "-ldisplay_layer" ]
    }
    cflags = [ "-fPIC" ]                                    # 不变
    cflags += [ "-Wall" ]                                    # 不变
    cflags_cc = cflags                                      # 不变
}

if (surface_lite_single_process) {                         # [新增] 全流程示例（M4 验证载体）
    executable("surface_lite_example") {
        sources = [ "test/example/surface_single_process_example.cpp" ]
        ...
    }
}

config("surface_public_config") { ... }                     # 既有不动

config("surface_single_process_config") {                   # [R-B1 新增]
    include_dirs = [ "//foundation/graphic/graphic_utils_lite/interfaces/innerkits" ]
}
```

#### 6.1.1 实现期构建修订（编译驱动，已评审回填）

| # | 修订 | 动因 |
|---|------|------|
| R-B1 | SP 模式裁掉 `utils_lite` **链接**依赖（public_deps 置空），新增 `surface_single_process_config` 补 innerkits 头路径 | ① SP 对 utils 仅头文件依赖（`GRAPHIC_LOG` 宏在 `ENABLE_GRAPHIC_LOG=0` 时回退 printf，`IMAGE_PIXEL_FORMAT_*` 为枚举常量，均无链接符号）；② riscv32 目标上 utils_lite 的 diagram/SVG 源因上游板级头环境无法编译（R5），不拉入可让 SP 组件图自洽；③ 契合 mini 穿戴裁剪哲学。**默认配置 public_deps 原样保留，行为不变** |
| R-B2 | `declare_args` 从 BUILD.gn 移至共享 `surface_lite_config.gni` | `test/BUILD.gn` 需引用同一变量；GN 对不同 BUILD.gn 的加载顺序不保证主 BUILD.gn 先行，直接引用报 "Undefined identifier"。共享 .gni 为社区标准做法（参照 graphic_utils_lite/utils.gni） |
| R-B3 | `test/BUILD.gn`：单测目标 `ipc_single` 依赖改为 `if (!surface_lite_single_process)` 条件加入 | liteos_m 下加载 ipc 的 BUILD.gn 会连带评估 `rpc_manager`（external_deps dsoftbus）导致 GN 失败；SP 单测本不应依赖 IPC（评审指示："单进程的不要依赖 ipc"） |
| R-B4 | `bundle.json` 追加 `features: ["surface_lite_single_process"]` | hb loader 校验产品 feature 必须在组件 bundle.json 声明，否则 LOAD 期拒绝 |

**实现期决策点 D1**（liteos_m 目标类型）状态：`shared_library("surface")` 在 liteos_m/riscv32 的 **GN 图层面通过**（gn gen 成功，工具链含 solink，产物名 libsurface.so）；源文件编译以一致头环境验证 8/8 并手动打包 `libsurface_sp.a`。完整 .so 链接受 R5（板级 C++ 头环境）阻断未在板内验证；如目标板要求静态库形态，仍可切换 `lite_library` 方案（不影响 liteos_a/linux）。

### 6.2 宏门控触点矩阵（`#ifndef SURFACE_LITE_SINGLE_PROCESS` 全清单）

| 文件 | 门控内容 |
|------|---------|
| `interfaces/innerkits/surface_buffer_impl.h` | `#include "ipc_skeleton.h"`；`ReadFromIpcIo/WriteToIpcIo` 声明 |
| `frameworks/surface_buffer_impl.cpp` | `ReadFromIpcIo/WriteToIpcIo` 函数体（IpcIo/ReadInt32/WriteInt32 等） |
| `interfaces/innerkits/surface_impl.h` | `GenericSurfaceByIpcIo` 声明、`WriteIoIpcIo/DoIpcMsg/IpcRequestHandler` 声明、`SurfaceImpl(SvcIdentity&)` 私有构造、`sid_`/`objectStub_` 成员 |
| `frameworks/surface_impl.cpp` | `#include "buffer_client_producer.h"`、`#include "buffer_manager.h"`；sid 初始化、`objectStub_` 挂接、`ReleaseSvc`、`GenericSurfaceByIpcIo/DoIpcMsg/IpcRequestHandler` 函数体、`Init()` 内 `BufferManager::GetInstance()->Init()` 与 `new BufferClientProducer(sid_)` 分支 |
| `frameworks/buffer_queue.cpp` | `#include "buffer_manager.h"`；析构/`NeedAttach`/`Detach`/`Reset`/`SetQueueSize` 内 `BufferManager::GetInstance()/FreeBuffer` 语句块 |
| `frameworks/buffer_queue_producer.{h,cpp}` | `OnIpcMsg` 声明与实现、ipc 静态 handler 段与 `g_ipcMsgHandleList`、`FlushBuffer` 内 `BufferManager::FlushCache` |
| `frameworks/surface_utils.cpp` | `GetSvcIdentity` 实现及 `SurfaceEntry::sid` 相关语句、`getpid()` 调用（SP 用常量 0） |
| `frameworks/buffer_client_producer.{h,cpp}` / `frameworks/buffer_manager.{h,cpp}` | 整文件不门控——由 BUILD.gn 在 SP 模式剔除 |
| `interfaces/kits/*` | **零门控**：kits 头文件两态暴露同一接口（依赖隔离由 innerkits/impl 层完成） |

**陷阱 #2 落实**：`buffer_queue_producer.cpp` 与 `surface_impl.cpp` 的 IPC 静态辅助段位于 `namespace OHOS {` **之内**，门控 `#ifndef/#endif` 必须包裹**段内容**而不得包裹 `namespace` 关键字本身，确保默认配置不出现双 namespace 未闭合。

## 7. 实现期陷阱与对策（执行清单）

| # | 陷阱 | 对策（本 spec 已内建） |
|---|------|----------------------|
| 1 | 若在 kits `SurfaceBuffer` 新增 `uint32_t GetStride() const` 虚函数，将与 `SurfaceBufferImpl` 既有非虚 `int32_t GetStride() const` 同名同参仅返回类型不同 → C++11 编译错误 | **规避**：不新增该虚函数（评审定论：无消费方）；stride 经 `GetChunk(i).stride`（chunk 权威来源）或 innerkits `SurfaceBufferImpl::GetStride()`（原样保留）访问；由此本设计**零签名修改**，100% 追加式 |
| 2 | `#ifndef` 包裹 IPC 辅助段时 namespace 位置错误 → 默认配置双 namespace 未闭合 | §6.2 末段：门控内容不门控 namespace 关键字；M2/M4 双配置编译即回归 |
| 3 | `buffer_queue.h` 靠传递包含拿 `std::string`/`IBufferConsumerListener` | §3.6 显式 `#include <string>` 与 `#include "ibuffer_consumer_listener.h"` |
| 4 | `surface.h` 用 `std::string` 未显式包含 | §3.3 显式 `#include <string>` |
| 5 | 示例判断 `SURFACE_ERROR_BUFFER_IN_USE` 时 kits 头无错误码 | 示例显式 `#include "buffer_common.h"`（innerkits） |

## 8. 文件变更清单

### 新增（7 个代码/构建文件 + SDD 文档）

| 文件 | 内容 |
|------|------|
| `interfaces/kits/iproducer_listener.h` | §3.1 生产者回调接口 |
| `interfaces/innerkits/surface_utils.h` | §2.4 ID 注册中心 |
| `frameworks/surface_utils.cpp` | SurfaceUtils 实现 |
| `frameworks/surface_buffer_impl_ex.cpp` | §3.5 SurfaceBufferImplEx + 工厂 |
| `test/example/surface_single_process_example.cpp` | §5.1/5.2 全流程示例（HAL 生产线程 + 消费线程，环形复用/待回收表/乱序段排序演示） |
| `surface_lite_config.gni` | [R-B2] 共享 declare_args（BUILD.gn 与 test/BUILD.gn 引用同一开关，规避 GN 加载顺序） |
| `BUILD.gn` 内示例目标与 `surface_single_process_config` | §6.1（属 BUILD.gn 变更） |

### 修改（均为追加式；默认配置预处理输出满足不变式 I0）

| 文件 | 追加内容 |
|------|---------|
| `interfaces/kits/surface_type.h` | §1.3 常量 |
| `interfaces/kits/surface_buffer.h` | §2.1 结构体、§3.2 虚函数与工厂声明、`@deprecated` 标注 |
| `interfaces/kits/surface.h` | §3.3 静态方法与 5 个默认实现虚函数、include 补齐 |
| `interfaces/innerkits/buffer_common.h` | §1.2 错误码 |
| `interfaces/innerkits/surface_buffer_impl.h` | §3.4 chunk override、virtualChunk_、SetSizeUnchecked（实现期追加）、宏门控（GetStride 零改动） |
| `interfaces/innerkits/buffer_queue.h` | §3.6 Attach/Detach/镜像监听、attachList_ 等 3 成员、include 补齐 |
| `interfaces/innerkits/buffer_producer.h` | §3.7 默认实现虚函数 |
| `interfaces/innerkits/surface_impl.h` | §3.8 新方法/代理构造/成员/宏门控/friend |
| `frameworks/surface_buffer_impl.cpp` | chunk 基类实现、IPC 函数门控 |
| `frameworks/buffer_queue.cpp` | Attach/Detach/旁路 Release 分支/镜像监听/析构 attachList_ 清理/Gralloc 门控 |
| `frameworks/buffer_queue_producer.h/.cpp` | 委托 override、Register/UnregisterConsumerListener 镜像追加一行、IPC 段门控 |
| `frameworks/surface_impl.cpp` | 注册/注销追加、代理构造、Init 分支、新方法实现、IPC 门控 |
| `frameworks/surface.cpp` | `CreateSurfaceAsProducer` |
| `BUILD.gn` | §6.1 共享 gni 引用/条件源/条件依赖（R-B1 utils_lite 链接裁剪 + surface_single_process_config）/ldflags/示例目标 |
| `test/BUILD.gn` | [R-B3] SP 模式剔除 `ipc_single` 依赖 + 引入共享 gni |
| `bundle.json` | [R-B4] features 声明 `surface_lite_single_process` |

**零改动**：`frameworks/buffer_queue_consumer.{h,cpp}`、`frameworks/buffer_manager.{h,cpp}`（仅 BUILD.gn 剔除参与）、`frameworks/buffer_client_producer.{h,cpp}`、`test/unittest/graphic_surface_test.cpp`（存量单测**源码**零改动，git 可证；US-5 验收）、ndk 配置（head_files 不新增 kits 文件——`iproducer_listener.h` 经 `surface.h` include 暴露，随目录 `interfaces/kits` 整体发布）。

## 9. US 验收 → spec 条目映射

| US 验收点 | spec 条目 |
|-----------|----------|
| 工厂/SetSize 不被拒 | §3.5 SetSize override |
| Attach→Acquire 状态流 | §4.1、§3.6 |
| ACQUIRED Detach → IN_USE | §1.2、§4.1 |
| OnBufferReleased 锁外/待回收表 | V6/V7、§5.2 |
| RELEASE Detach 同步兑现 | §4.1 |
| 重复 Attach 仅重入 dirtyList_ | V4 |
| FLUSH/ACQUIRE 拒绝 | V4 |
| 统一视图/恒等/失效 | §2.3 |
| 乱序段排序兜底 | §2.1 |
| legacy 合成 virtualChunk_ | §3.4 |
| 超限拒绝 | §2.2 |
| ID 唯一/注销失效 | §2.4、V8 |
| 代理共享队列 | V9、§3.8 |
| SP 剔除源文件/依赖/ldflags | §6.1 |
| 无 IPC 头环境编译链接运行 | §6.2 + M4 宿主机验证 |
| RequestBuffer 打日志 | V10 |
| 默认配置零差异 | 不变式 I0、§5.3 |
