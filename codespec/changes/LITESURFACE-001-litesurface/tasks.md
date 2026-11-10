# LITESURFACE-001 任务拆分与执行记录

## 任务清单（对应 delta-spec §8）

### M2 数据模型与 Buffer 扩展
| # | 任务 | 文件 | 自验 |
|---|------|------|------|
| T1 | 常量与格式 | surface_type.h | 编译 |
| T2 | SurfaceDataChunk + chunk 虚函数（默认实现）+ 工厂声明 | surface_buffer.h | 编译 |
| T3 | 错误码 | buffer_common.h | 编译 |
| T4 | 基类 chunk 实现 + IPC 门控 + virtualChunk_ | surface_buffer_impl.h/.cpp | 双配置编译 |
| T5 | SurfaceBufferImplEx + 工厂 | surface_buffer_impl_ex.cpp | 双配置编译 |

### M3 队列生命周期 + ID 注册 + 单进程隔离
| # | 任务 | 文件 | 自验 |
|---|------|------|------|
| T6 | 生产者回调接口 | iproducer_listener.h（新） | 编译 |
| T7 | Surface 五个默认虚函数 + CreateSurfaceAsProducer 声明 | surface.h | 编译 |
| T8 | 队列 Attach/Detach/镜像监听声明 + attachList_ 成员 | buffer_queue.h | 编译 |
| T9 | BufferProducer 默认实现虚函数 | buffer_producer.h | 编译 |
| T10 | 队列实现（旁路 Release 分支/析构清理/Gralloc 门控） | buffer_queue.cpp | 双配置编译 |
| T11 | 委托 override + 镜像注册追加 + IPC 段门控 | buffer_queue_producer.h/.cpp | 双配置编译 |
| T12 | SurfaceUtils | surface_utils.h/.cpp（新） | 编译 |
| T13 | SurfaceImpl 增补（注册/代理/新方法/IPC 门控） | surface_impl.h/.cpp | 双配置编译 |
| T14 | kits CreateSurfaceAsProducer 实现 | surface.cpp | 编译 |
| T15 | BUILD.gn（declare_args/条件源/条件依赖/示例目标） | BUILD.gn | mini 产品级编译 |

### M4 示例与验证
| # | 任务 | 自验 |
|---|------|------|
| T16 | 单进程全流程示例（HAL 生产线程 + 消费线程） | 运行 + 断言全过 |
| T17 | 隔离环境（无 IPC/HDI 头）编译链接运行 | 编译链接 + ASan/UBSan |
| T18 | 默认配置对象级编译 | -c 全源通过 |
| T19 | mini 产品级编译（wifiiot_hispark_pegasus） | hb build 通过 |
| T20 | 验证记录 + 文件变更清单交付 | verification.md |

## 执行日志

| # | 任务 | 结果 | 备注 |
|---|------|------|------|
| T1-T3 | 常量/结构体/错误码头文件 | 完成 | SURFACE_MAX_CHUNK_NUM=5（评审修订） |
| T4-T5 | 基类 chunk 实现 + Ex 工厂 | 完成 | 实现期追加 protected `SetSizeUnchecked`（len_ 为 private，Ex 绕过容量校验的桥） |
| T6-T9 | 回调接口/Surface/队列/Producer 声明 | 完成 | 监听器命名评审修订为 Register/Unregister |
| T10-T11 | 队列实现 + 委托 + 门控 | 完成 | GetAttachedBuffer 用指针同一性（Ex handle 恒 0 会致 equals 误匹配）；OnIpcMsg 定义补门控（首验遗漏） |
| T12-T14 | SurfaceUtils/Impl/CreateSurfaceAsProducer | 完成 | surface.cpp include 门控方向修正 |
| T15 | BUILD.gn | 完成 | 评审演进：sources 纯增量结构（-= 误用修复）；SP 裁掉 utils_lite public_dep（头文件依赖 + 规避 riscv32 上游编译问题）；补 surface_single_process_config（graphic_config.h 路径） |
| T16 | 全流程示例 | 完成 | 修复 3 处自研 bug（QUIT 判断/帧号错位/槽索引越界） |
| T17 | 隔离环境 ASan | **152 passed / 0 failed，零报告** | |
| T18 | 默认配置对象级 | 10/10 | |
| T19 | mini 产品级 | GN 通过 + riscv32 8/8 + libsurface_sp.a；in-tree ninja 被 Hi3861 SDK C++ 头环境阻断（R5） | 过程另发现上游 NOTICE 图冲突（developtools 规避） |
| T20 | 验证记录 + 变更清单 | 完成 | verification.md；另完成 I0 预处理级纯追加验证（5 文件 0 缺失行，含 1 处空格偏差修复） |

| T21 | 帧级格式标签（评审演进：uint32 开放集合 → 封闭四格式枚举+校验） | 完成 | SetBufferFormat/GetBufferFormat + SurfaceBufferFormat 枚举（surface_type.h）；S0-S3 断言升级；ASan 167/0；I0 复验 0 缺失 |
| T22 | 安全检视：F1-F4 修复（attach 来源校验/示例空指针/累加饱和/代理门控）+ F5-F10 文档化 + 注释清理 | 完成 | 回归全绿 |
| T23 | 圈复杂度检查与优化：示例 Run/ProducerThread 拆分（113→12、95/CCN15→44/CCN10），库代码合规或 I0 豁免 | 完成 | lizard 复测全合规，ASan 167/0 |
| T24 | bundle.json 声明 mini 形态支持（adapted_system_type 增补，对齐 graphic_utils_lite 惯例） | 完成 | JSON 校验通过；hb LOAD 阶段零错误 |
