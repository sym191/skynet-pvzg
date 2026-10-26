# 阶段七：扩展模块、共享数据与运维

状态：待实施。基线与通用约束见 [规划索引](README.md)。

## 目标与当前基础

使本阶段模块在重构进程中完成加载、真实上层调用、错误处理和资源释放，并补齐 daemon、
信号、日志重开及调试运维。现有 NativeService 已具备 cpath 搜索、动态加载和实例释放；
LuaService 已有独立 VM、内存配额、profile 与无缓存 loader。阶段七补充完整性与边界验收，
不把这些已有基础列为从零开发。

当前 Lua VM 的内存计数不等于全局 allocator 的逐服务记账；`skynet_debug_memory` 的日志输出
也不能代替 `skynet.memory` 的真实统计。共享数据必须先确认分配、归属和跨 VM 生命周期。

## 任务与依赖

| 编号 | 工作包 | 主要落点 | 前置 |
| --- | --- | --- | --- |
| 7A | 模块/导出符号/上层 Lua 依赖清单；加载器生命周期专项 | `src/service/NativeService.cc`、`src/service/CMakeLists.txt`、`tests/` | 阶段三基础 |
| 7B | allocator、TLS 归属、memory 统计及诊断 | `src/compat/`、`src/runtime/Context.*`、`src/service/LuaService.*` | 7A |
| 7C | sharedata、stm、datasheet、sharetable | `src/service/` 或独立模块目录、构建规则、`tests/lua/` | 7A、7B |
| 7D | multicast、debugchannel 与上层服务 | 模块绑定、`src/runtime/`、`tests/` | 7A、7B |
| 7E | 算法/协议/客户端模块及可选 TLS | 模块构建、`cmake/`、`tests/` | 7A；网络模块依赖阶段四/五 |
| 7F | daemon、PID、信号、日志及 debug_console | `src/skynet_refactor/main.cc`、`src/runtime/LogService.*`、配置和进程测试 | 7B、7D 的诊断能力 |
| 7G | 生态综合回归与支持矩阵收口 | `tests/`、`docs/` | 7A–7F；混部场景依赖阶段六 |

7A 盘点时，以固定上游 Makefile、`lualib-src/`、Lua require 及服务依赖为准，
记录公开导出、模块路径、构建开关、上游实现来源和验收用例。发现清单外依赖时补入矩阵，不能用空 stub 满足 require。

## 必选与可选模块矩阵

下表是本阶段目标，当前状态均为待专项验收。基础 core/serializer/socket/netpack 继续回归，
cluster.core 的实现与协议验收归阶段六。

| 模块 | 重点依赖/风险 | 最低验收 |
| --- | --- | --- |
| `skynet.memory` | allocator 导出、服务归属、跨线程释放 | 分配/重分配/释放、总量与逐服务量、服务退出后的残留归属 |
| `skynet.sharedata.core` | sharedatad、只读对象和旧版本引用 | 创建/查询/更新/删除，多 VM 持有旧版、GC 后回收 |
| `skynet.stm` | 原子引用、快照与更新提交 | 多读者、写入、旧快照保活、生产者退出及失败提交 |
| `skynet.datasheet.core` | builder/dump、共享编码数据 | 构造/装载/查询/版本替换及最后引用释放 |
| `skynet.sharetable.core` | bundled Lua 内部接口/补丁、跨 state 对象 | 编译条件核对，更新与多 VM 查询、关闭顺序、异常路径 |
| `skynet.multicast.core` | multicastd、共享消息引用 | 零/单/多订阅者、退订、订阅者退出、拒收与最后引用释放 |
| `skynet.debugchannel` | 调试阻塞/唤醒、被调试 VM 生命周期 | 调试连接/断开、目标退出、异常命令和停机无死锁 |
| `skynet.crypt` / SHA1 | 字节算法与 Lua 值格式 | 已知测试向量、二进制/NUL、原版交叉验证和非法参数 |
| `bson` / `skynet.mongo.driver` | 编解码、socketchannel、上层 mongo 库 | 类型/边界/截断、请求响应与失败传播；真实数据库集成单列 |
| `sproto` | schema、编码/解码、RPC 辅助接口 | schema 装载、交叉编解码、非法 schema、边界值 |
| `lpeg` / `md5` | bundled Lua 构建 ABI | 上层真实调用、标准向量或匹配样例、错误与 GC |
| `client.socket` | 客户端辅助网络，不属于服务端 Actor socket | 连接、收发、关闭、失败与 Lua API；与服务端网络分开验收 |
| 上层 `snax` / `cluster.snax` 等 | Lua 服务、基础扩展及 cluster | 由 require 清单确定依赖，完成真实服务创建/调用/退出 |
| `ltls`（可选） | OpenSSL、构建开关、TLS Lua 包装 | 开关关闭/开启、握手、证书校验失败、收发/关闭/中途断开 |

Mongo 验收先用受控协议对端覆盖确定性错误路径，再用固定版本的临时 MongoDB 验证承诺支持的
上层操作。缺少数据库环境时标为阻塞/未验收，不能以 codec 通过声称数据库集成已完成。
可选 TLS 未启用不阻止基础版本验收；启用却未通过测试时不能发布为受支持能力。

## 7A–7B：模块与分配器边界

1. 覆盖 cpath 多候选、缺失必需符号、create/init 失败、signal、重复创建和并发卸载。
   保证所有实例及回调释放完成后才 `dlclose`，多服务共享模块时不提前卸载代码。
2. 建立 C facade 导出清单；C++ 异常不跨 C ABI，Lua longjmp 不跨活动 RAII 对象。
   审核 Lua 参数检查和错误出口，资源由实现层管理，边界层只负责转换结果。
3. 对齐 `skynet_malloc/free/realloc/strdup/lalloc` 等实际所需入口；记录对齐、零长度、
   分配失败、realloc 失败时旧指针保留、跨模块释放的契约。
4. 逐服务归属按分配时身份记录；跨服务转移和其他线程释放不能错误扣减当前服务。
   在 init、dispatch、signal 等上下文切换及退出后验证 TLS 恢复；无服务上下文的分配单列。
5. 区分 Lua VM 配额、消息内存、共享数据、原生模块内存和进程 RSS，避免重复计账。
   先固定统计口径和误差来源，再验证 debug/memory 返回值，不能将 libc 可见字节数等同 RSS。
6. 重构模块输出与 original 输出保持隔离，记录实际加载路径和 ABI；动态模块也纳入 sanitizer 构建范围。

## 7C–7E：共享资源与扩展策略

共享模块逐个实施，不把“能 require”当成完成。每个模块先给出创建、发布、读引用、版本替换、
删除及 VM 关闭的所有权表，再实现最小真实调用，最后覆盖故障与并发。
只读共享数据也需验证其引用获取/释放与版本发布之间的同步。

- 覆盖生产者先退出、读者先退出、旧版本长期持有、GC 与替换竞争、发布失败及 Runtime 停机。
- Lua 内部补丁先审计仓库现有版本，不假定 sharetable 能作为普通独立 C 模块直接编译；
  如需要新增 Lua 修改，先形成兼容性设计与回归范围，不能静默切换系统 Lua。
- multicast 拒绝投递、订阅者退休和零订阅者时也必须释放共享 payload。
- 算法库优先复用成熟实现，通过向量和交叉编码验证。绑定层的语言替换按收益决定，不要求为 C++23 重写算法。
- TLS 开关默认关闭，开启时显式检查依赖并报错；不接受缺失 OpenSSL 后静默构建成无 TLS 的成功结果。

## 7F：运维闭环

| 能力 | 计划行为 | 失败与生命周期验收 |
| --- | --- | --- |
| daemon/PID | 实现配置约定、后台启动结果、PID 文件创建/持有/删除 | 重复启动、目录不可写、陈旧 PID、启动失败、正常/异常退出；不误删其他进程 PID 文件 |
| 信号 | 对照原版固定 SIGPIPE/SIGHUP 及退出信号语义 | 安全线程接收并转发动作，signal handler 不直接访问 Lua、锁或分配器 |
| 日志重开 | 默认 logger 和可替换 logger 支持对应重开协议 | rename 后重开、追加、路径失败、连续重开、退出前排空 |
| debug_console | list/stat/info/task/signal/logon/logoff 的真实结果 | 非法地址、目标退休、长回调、连接断开、不支持项明确报错 |
| 停机 | 停止新工作，结束服务/共享引用及 I/O，排空日志，释放模块/PID | 启动各阶段失败、活跃连接和调试等待期间停机，进程有界退出 |

daemon 化必须在创建 worker/I/O 等线程前完成，避免多线程进程 fork 后继承锁状态。
已有网络线程局部屏蔽 SIGPIPE 的行为需与进程入口策略协调，并保持嵌入式 Runtime 的边界。
调试控制台验收使用回环地址，不引入新的业务鉴权或外网运维入口。

## 建议提交与完成门槛

建议按 7A → 7B → 7C（每个共享模块单独提交）→ 7D → 7E（按模块拆分）→ 7F → 7G 推进。
独立的工具模块或日志任务可提前，但必须显式记录前置和专项验收结果。

- [ ] 必选模块逐项具有加载路径、真实 Lua 用例、错误/释放用例及原版对照；可选项有明确状态。
- [ ] Debug/Release 及原有业务回归通过；共享资源、分配器、动态模块通过 ASan+UBSan 和独立 TSan。
- [ ] daemon、信号、PID、日志重开、debug_console 与异常停机测试可独立运行且不残留进程/文件锁。
- [ ] 支持矩阵列出 Lua 补丁、外部依赖版本、统计口径及未完成项；阻塞的必选模块不得标为阶段完成。
