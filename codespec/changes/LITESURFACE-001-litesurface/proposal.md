# LITESURFACE-001：surface_lite 单进程多线程 + 外部 Buffer + 多段数据扩展

- **需求号**：LITESURFACE-001
- **仓**：foundation/graphic/surface_lite
- **基线分支**：master（4ed2ded）
- **状态**：proposal 评审中

---

## 1. 背景与动机

目标设备为鸿蒙 **Mini liteOS-m RTOS 穿戴设备**：内存与算力受限、无多进程 IPC 基础设施，相机 HAL 与图形框架运行在**同一进程的不同线程**。

现有 surface_lite 为 small/标准系统设计，在上述设备上存在三类不匹配：

| # | 现状 | 目标设备约束 |
|---|------|-------------|
| 1 | 生产者经 `BufferClientProducer` 走 IPC（`SvcIdentity` + `IpcIo` 序列化），依赖 `ipc_single`；`BufferManager` 依赖 `hdi_display`/Gralloc | liteos_m 上两依赖**无编译目标**，组件无法接入 mini 产品 |
| 2 | Buffer 内存由框架经 `RequestBuffer` → `BufferManager::AllocBuffer`（Gralloc）分配 | 相机内存由**芯片驱动**分配（物理地址对齐、环形布局），框架不负责分配，裸指针/SvcIdentity 传递也不符合模块解耦要求 |
| 3 | 数据模型为单段内存（`virAddr/phyAddr/size` 三元组） | 一帧 = 1~N 段：RGB 单段、NV12/NV21 双平面、**环形 Buffer 跨两段**、码流多包（JPEG/H264） |

本需求以**追加式增补**方式扩展 surface_lite：不改动原有 lite 接口签名与执行路径，通过编译宏 `SURFACE_LITE_SINGLE_PROCESS` 隔离依赖，使同一份源码同时服务跨进程（默认，行为零差异）与单进程多线程两种形态。

## 2. 目标用户

| 用户 | 角色 | 使用方式 |
|------|------|---------|
| 相机 HAL 开发者 | 生产者 | 自构 Buffer（驱动内存）→ `AttachBuffer` 入队 → `DetachBuffer`/`OnBufferReleased` 管理驱动内存 |
| 图形/显示框架开发者 | 消费者 | `CreateSurface()` 建 Surface（自动注册全局 ID）→ `AcquireBuffer` → 按 chunk 视图消费多平面数据 |
| 产品构建/集成工程师 | 构建 | `BUILD.gn` 开关 `surface_lite_single_process` 启用单进程形态，无 IPC/HDI 头文件依赖 |
| 既有跨进程用户 | 回归 | 默认配置（宏未定义）下接口、行为与现版本零差异 |

## 3. 功能清单（F-xx）

| 编号 | 功能 | 概述 | 覆盖需求 |
|------|------|------|---------|
| F-01 | 全局 Surface ID 注册中心 | innerkits 内部类 `SurfaceUtils`：消费端 SurfaceImpl 构造自动注册、析构自动注销；ID 编码 `PID<<32 \| counter` | 需求 2 |
| F-02 | 生产者按 ID 创建 Surface | kits 层 `Surface::CreateSurfaceAsProducer(uint64_t id)`；单进程本地查表建共享 BufferQueue 的代理；跨进程返回 `nullptr` 并提示用 innerkits `(id, sid)` 重载 | 需求 2 |
| F-03 | HAL 自构 Buffer 工厂 | `SurfaceBuffer::CreateSurfaceBuffer()` 纯声明，实现在 `frameworks/surface_buffer_impl_ex.cpp` 匿名命名空间 `SurfaceBufferImplEx`（override `SetSize` 解除容量校验） | 需求 3 |
| F-04 | 多段数据模型 | `SurfaceDataChunk{virAddr, phyAddr, size, stride, planeId}`，`SetChunk/GetChunk/GetChunkCount/GetTotalDataSize`；单段与主内存接口恒等、多段主接口返回失效值（`@deprecated`）；地址写入单一入口 `SetChunk` | 需求 7 |
| F-05 | Attach / Detach 旁路生命周期 | 外部 Buffer 走 `attachList_` 旁路表（绝不进 `allBuffers_/freeList_`）；重复 Attach 支持环形复用；ACQUIRED 态 Detach 返回新错误码 `SURFACE_ERROR_BUFFER_IN_USE` | 需求 3 |
| F-06 | 生产者释放回调 | 唯一回调 `IProducerListener::OnBufferReleased(SurfaceBuffer*)`；生产者/消费者监听器镜像存储于队列；回调一律锁外执行 | 需求 4 |
| F-07 | 单进程宏与依赖隔离 | `declare_args(){ surface_lite_single_process }`；true 时剔除 IPC/Gralloc 源文件、依赖与链接项，源码内全部 IPC/Gralloc 触点 `#ifndef SURFACE_LITE_SINGLE_PROCESS` 隔离 | 需求 6 |
| F-08 | 格式扩展 | `surface_type.h` 新增 `SURFACE_PIXEL_FORMAT_JPEG/H264` 常量（取值避开 `ImageFormat` 0/1xx/2xx 段）；NV21/NV12 双平面走通用例验证 | 需求 8 |

> 需求 1（追加式）与需求 5（裸指针风格）为**全局设计约束**，不单列功能，见 §6 影响分析与 delta-spec 约束节。

## 4. 用户故事与验收标准（US-xx）

### US-1 相机 HAL 生产者：外部内存入队与生命周期管理
> 作为相机 HAL 开发者，我要把芯片驱动分配的 Buffer 挂到 Surface 上供框架消费，并在消费完成后感知，以便复用或归还驱动内存。

- [ ] `SurfaceBuffer::CreateSurfaceBuffer()` 创建 wrapper 后，`SetChunk` 写入驱动段地址、`SetSize` 不被基类容量校验拒绝（`bufferData_.size` 恒 0 场景）
- [ ] `AttachBuffer` 后消费端 `AcquireBuffer` 可获得同一 Buffer，状态经 FLUSH→ACQUIRE
- [ ] ACQUIRED 态 `DetachBuffer` 返回 `SURFACE_ERROR_BUFFER_IN_USE`，Buffer 不被摘链
- [ ] 消费端 `ReleaseBuffer` 后，生产者 `OnBufferReleased` 收到该 Buffer 指针（锁外回调）
- [ ] `OnBufferReleased` 到达且 Buffer 记在 HAL 待回收表 → HAL 归还驱动内存；未命中 → re-Attach 复用
- [ ] 空闲（RELEASE）态 `DetachBuffer` 同步成功：从 `dirtyList_/freeList_/attachList_` 摘链并 delete wrapper；驱动内存在 HAL 调用栈上归还
- [ ] 同一 wrapper RELEASE 态重复 `AttachBuffer` 成功且只重入 `dirtyList_`（队列析构无双重 delete / ASan 无 UAF）
- [ ] FLUSH/ACQUIRE 态重复 `AttachBuffer` 被拒绝（防双入队）

### US-2 多平面消费者：分段数据视图
> 作为图形框架开发者，我要按统一视图消费 RGB 单段、NV12/NV21 双平面、环形跨段、码流多包数据。

- [ ] 单段（`GetChunkCount()==1`）：`GetChunk(0)` 与 `GetVirAddr/GetPhyAddr/GetSize` 恒等
- [ ] 多段（`GetChunkCount()>1`）：主内存接口返回 `nullptr/0/0`；`GetTotalDataSize()` 为全段 size 之和
- [ ] 同 `planeId` 段按设置顺序即数据逻辑顺序；驱动乱序由 HAL 适配层排序（示例含乱序 SetChunk 后按序消费的演示）
- [ ] `GetChunk(0)` 在未 SetChunk 的存量 Buffer（框架 Gralloc 分配）上合成 legacy 视图（`mutable virtualChunk_`）
- [ ] NV12 帧：plane0（Y）+ plane1（UV）两 chunk；环形帧：跨两段的 chunk 序列
- [ ] `SURFACE_MAX_CHUNK_NUM=5` 超限写入被拒绝

### US-3 系统集成者：按 ID 寻址
> 作为系统集成者，我要通过全局唯一 ID 获取 Surface，避免在模块间传递裸指针或 SvcIdentity。

- [ ] 消费端 `CreateSurface()` 返回的 Surface 拥有全局唯一 ID（`PID<<32 | counter`，进程内可重复创建不冲突）
- [ ] 单进程内 `Surface::CreateSurfaceAsProducer(id)` 返回共享同一 BufferQueue 的生产者 Surface
- [ ] id 不存在时返回 `nullptr`
- [ ] 消费端 Surface 析构后，同 id 再查询返回 `nullptr`（自动注销）

### US-4 产品构建者：单进程形态接入
> 作为产品构建工程师，我要在不引入 IPC/HDI 依赖的前提下把 surface_lite 编进 liteos_m 产品。

- [ ] `surface_lite_single_process=true` 时，`buffer_client_producer.cpp`、`buffer_manager.cpp` 不参与编译；无 `ipc_single/hdi_display` 依赖与 `-ldisplay_*` 链接（保留 libsec）
- [ ] 单进程配置在**无 `ipc_skeleton.h/serializer.h/display_gralloc.h` 头文件环境**下可编译、链接、运行（示例证明）
- [ ] 单进程下 `RequestBuffer` 打日志说明不可用，生产者走 Attach 路径
- [ ] mini 产品（wifiiot_hispark_pegasus, riscv32）上产品级编译通过

### US-5 既有跨进程用户：零影响回归
> 作为既有跨进程用户，我升级本版本后行为不变。

- [ ] 默认配置（宏未定义）下 `hb build`/对象级编译通过，产物 `libsurface.so` 目标不变
- [ ] 既有接口签名、`SurfaceBufferImpl` 存量成员布局、既有执行路径（Request/Flush/Acquire/Release/Cancel、IPC 处理）不变
- [ ] 存量单测用例（`test/unittest/graphic_surface_test.cpp`）源码不需修改即可编译

## 5. 不在范围内

1. **跨进程模式下的 Attach/Detach 与 `OnBufferReleased`**：驱动内存为进程私有，跨进程回调需 IPC 通道设计，另行立项
2. **智能指针 / 引用计数**：明确排除（需求 5），生命周期以 BufferState 状态机为单一事实来源
3. **其他仓修改**：graphic_utils_lite、drivers/peripheral、communication/ipc、内核与驱动 HAL 均不在本需求变更范围（验证所需的构建环境调整除外，见 §8 风险）
4. **真机/上板运行验证**：riscv32 目标仅做产品级编译验证；运行时验证在宿主机示例 + ASan/UBSan 完成
5. **`SurfaceDataChunk` 的 offset 字段、独立销毁接口**（如 `DeleteSurfaceBuffer`）：工厂创建但从未 Attach 的 wrapper 由调用方保证最终 Attach，独立销毁路径另行评估
6. **第二个 Detach 回调 / detachPending 延迟兑现**：按已定稿生命周期模型（方案 B）明确排除

## 6. 影响分析

### 6.1 设计章节对照表（需求 → delta-spec 章节）

| 设计基线条目 | delta-spec 章节 |
|-------------|---------------|
| SurfaceDataChunk / MAX_CHUNK_NUM / 统一视图 / 单一入口 | §2 数据结构、§3 接口签名 |
| SurfaceUtils（注册/寻址/ID 编码） | §4 模块设计 |
| CreateSurfaceAsProducer / SurfaceBufferImplEx 工厂 | §3 接口签名 |
| attachList_ 旁路表 / 状态机扩展 / IN_USE 错误码 | §5 状态机与生命周期 |
| IProducerListener / 镜像监听 / 锁外回调 | §5、§6 时序 |
| 单进程宏 / BUILD.gn / 依赖隔离清单 | §7 构建与编译隔离 |
| GetStride 冲突规避等 5 项已知陷阱 | §8 实现陷阱与对策 |

### 6.2 破坏性变更

**无破坏性变更、无签名修改（100% 追加式）**。需评审知悉的三点：

1. ~~`SurfaceBufferImpl::GetStride()` 返回类型统一~~（评审定论撤销）：不在 kits `SurfaceBuffer` 新增 `GetStride` 虚函数——无消费方（stride 经 `GetChunk(i).stride` 与 innerkits 既有接口访问），且规避了与 `SurfaceBufferImpl` 既有非虚 `int32_t GetStride() const` 的同名同参返回类型冲突（C++11 编译错误）。`SurfaceBufferImpl::GetStride()` 保持原样零改动
2. `Surface`/`SurfaceBuffer` 新增虚函数（带默认实现）会改变 vtable 布局——surface_lite 与使用方同源同版本编译，不构成跨版本二进制兼容问题
3. `buffer_common.h` 的 `BufferErrorCode` 追加 `SURFACE_ERROR_BUFFER_IN_USE` 于枚举尾部（取现有 -10..-6 之后，不重排既有值）

### 6.3 依赖关系

| 依赖 | 默认配置 | 单进程配置 |
|------|---------|-----------|
| graphic_utils_lite（头文件/日志宏） | 需要 | 需要 |
| bounds_checking_function（libsec） | 需要 | 需要 |
| communication/ipc（ipc_single） | 需要 | **不需要** |
| drivers/peripheral（hdi_display + `-ldisplay_*`） | 需要 | **不需要** |

## 7. DFX 约束

- **性能**：单进程模式数据路径零序列化、零拷贝（指针直传共享 BufferQueue）；Attach/Detach 为 O(attachList_) 线性查找，n 受队列规模（≤10）约束；ID 分配为原子递增，无锁竞争热点
- **可靠性**：双入队拒绝（FLUSH/ACQUIRE 拒绝 Attach）；重复 Attach 只重入 dirtyList_（杜绝队列析构双重 delete）；回调一律锁外（回调内可重入 Surface，防死锁）；外部 Buffer Release 不 ClearExtraData（驱动句柄随 wrapper 生命周期）；ASan/UBSan 全流程示例零泄漏、零 UAF
- **可观测性**：沿用 `GRAPHIC_LOG*` 打点（关键路径：Attach/Detach 拒绝、RequestBuffer 单进程不可用、ID 注册/注销）
- **静态约束**：`-Wall` 零新增告警；双配置编译均为门禁项

## 8. 里程碑

| 里程碑 | 内容 | 出口条件 |
|--------|------|---------|
| M1 | proposal + delta-spec 评审 | 用户确认两份文档 |
| M2 | 数据模型与 Buffer 扩展：`SurfaceDataChunk`/chunk 接口/统一视图、`SurfaceBufferImplEx` 工厂、错误码追加 | 对象级编译通过（双配置） |
| M3 | 队列生命周期：`attachList_` 旁路、Attach/Detach、`IProducerListener` 镜像监听、`SurfaceUtils`、`CreateSurfaceAsProducer`、单进程宏隔离与 BUILD.gn | mini 产品级编译通过（单进程配置） |
| M4 | 示例 + 验证 + 交付：单进程全流程示例（环形复用、待回收表、乱序 chunk 排序演示）、ASan/UBSan 记录、双配置编译记录、文件变更清单 | 全部 US 验收标准可勾选 |

### 验证策略（按用户已确认口径）

| 配置 | 验证方式 | 强度 |
|------|---------|------|
| 单进程（`SURFACE_LITE_SINGLE_PROCESS=1`） | ① mini 产品（wifiiot_hispark_pegasus, riscv32）产品级编译；② 宿主机隔离头文件环境编译 + 链接 + 运行示例（含 ASan/UBSan） | 产品级 + 运行时 |
| 默认（跨进程） | 宿主机对象级编译（`-c`，含 IPC/驱动头文件路径），证明源码双配置可编译 | 对象级 |

## 9. 开放问题与风险

| # | 问题/风险 | 处置 |
|---|----------|------|
| R1 | mini 产品默认不含 graphic 子系统，单进程配置产品级编译需向产品配置追加组件（vendor 仓构建环境调整，非交付物 diff） | M3 实施时处理，验证记录中注明 |
| R2 | 单进程编译开关需经 gn args 传入（`surface_lite_single_process=true`），lite 构建的产品 features 通路待实测 | M3 实测通过：产品 features → bundle.json 声明 → gn args 注入链路 work（见 delta-spec §6.1.1 R-B4） |
| R3 | `SurfaceBuffer` 析构函数为 protected，HAL 无法 `delete SurfaceBuffer*`——工厂创建但从未 Attach 的 wrapper 无法独立销毁 | 本需求约定：工厂产物必须最终 Attach（示例与文档明示）；独立销毁接口列入"不在范围内" |
| R4 | 宿主机单进程运行示例与 riscv32 目板的线程/内存行为差异 | 以 ASan/UBSan + 静态断言（`static_assert` 校验结构体大小/字段偏移不变）补偿，真机验证另行安排 |
| R5 | （验证期发现）Hi3861 板 SDK 的 musl 头路径与 riscv32 工具链 newlib 头混用，libstdc++ 组件无法在板内 in-tree 编译（graphic_utils_lite 等上游 C++ 组件同被阻断）；另发现 host/target 工具链 cjson_static NOTICE 同路径冲突（图形组件入图后暴露） | 均为上游平台问题，非本需求代码问题；验证以"GN 图 + riscv32 一致头环境交叉编译 8/8 + 宿主机全量运行"替代板内 ninja（verification.md §5）；建议上游单独立项 |
