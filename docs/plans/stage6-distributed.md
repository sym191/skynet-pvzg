# 阶段六：harbor 多节点与 cluster

状态：待实施。基线与通用约束见 [规划索引](README.md)。

## 目标与前置

分别交付 harbor 和 cluster 两套通信机制，使原版上层 Lua 能在原版、新版和混合部署中
完成跨节点请求、响应、单向消息、名称查询及可观察的失败处理。
harbor 基于 handle 高位节点身份和全局名称路由；cluster 基于节点配置、socketchannel
和 `skynet.cluster.core`。两者独立验收，cluster 测试可使用 `harbor=0`。

进入开发前复验阶段三至五和登录/组队回归，固定原版 SHA、Lua 版本、独立端口及状态目录。
当前 `Runtime::send` 只投递本地 handle，`send_global` 在本地维护名称与待投递消息；
`HarborService.hpp` 只支持单节点。阶段六需扩展这些路径，不能只移除入口的配置拒绝。

## 任务拆分

| 编号 | 任务与交付物 | 预计落点 | 依赖 |
| --- | --- | --- | --- |
| 6A | 原版协议/失败语义表、报文字段与所有权表、多进程测试骨架 | `docs/`、`tests/`、根 `CMakeLists.txt` | 基线复验 |
| 6B | harbor 远端路由、节点握手、全局名称、连接生命周期 | `src/service/HarborService.*`、`src/runtime/Runtime.*`、`src/compat/skynet_compat.cc` | 6A |
| 6C | 原版 cmaster/cslave 启动、异常启动清理、配置入口 | `src/skynet_refactor/main.cc`、`src/config/`、`configs/` | 6B |
| 6D | cluster.core 编解码与模块装载、上层 cluster 服务闭环 | `src/service/CMakeLists.txt`、Lua 模块注册处、`tests/lua/` | 6A、已有 TCP |
| 6E | 双向混部、断线/重启/大包/停机回归与交付记录 | `tests/`、`docs/` | 6C、6D |

上述新文件名为建议，现有入口和测试注册统一沿用仓库结构；当前没有 `tests/CMakeLists.txt`。

## 6A：先明确协议与错误契约

- 参照上游 `skynet_harbor.c`、`service_harbor.c`、`cmaster.lua`、`cslave.lua`、
  `lua-cluster.c` 及 cluster 系列服务，记录字段宽度、字节序、长度上限、节点身份和握手顺序。
- 按操作列出同步拒绝、异步错误、排队等待和断线通知行为；特别区分未建立连接、已断开的连接、
  名称未注册、目标服务已退出与远端业务不响应。原版没有通用 RPC 超时保证的场景不擅自新增语义。
- 记录普通消息、harbor 嵌套消息、分片缓存的创建者、持有者、转移点和释放点；失败与停机路径必须覆盖。
- 测试 runner 启动两个或多个子进程，以就绪握手开始测试；设置单用例及整个进程组超时，失败也回收子进程、端口与临时文件。
- 对正常协议做行为差分；畸形输入若上游行为不安全，重构版按明确的拒绝契约单独验证并记录差异。

## 6B–6C：harbor 路由与启动

1. 保持 handle 高 8 位节点身份、低 24 位服务 ID 的规则，区分本地和远端地址。
   校验配置节点 ID、保留值及重复节点的行为；节点身份在服务地址发布前固定。
2. 远端数字地址和全局名称进入 harbor 路径，本地数字地址与 `.local` 名称继续走既有本地投递。
   保留显式 source、session、协议号和 DONTCOPY 的接管语义，拒绝后不能泄漏 payload。
3. 实现连接建立、握手、就绪、断开、清理的状态转换，兼容 cmaster/cslave 的连接管理。
   全局名称长度以 `GLOBALNAME_LENGTH` 及原版边界为准，补名称先查询/后注册和重复注册用例。
4. 对未解析名称与尚未连通节点的 pending 队列逐项定义排空和失败处理。
   记录消息数、字节数及积压观测；如增加容量限制，必须说明默认值、超限返回和兼容差异，不能静默丢弃。
5. 保持 reserved harbor Context 的生命周期，避免它阻止正常退出或在 I/O 完成前释放。
   服务退休、节点断开与 Runtime stop 竞争时，每份缓冲仅释放一次。
6. 实现完成后再开放 `harbor!=0` 启动；`harbor=0`、bootstrap、launcher、datacenter 和 uniqueservice 必须继续通过。
   配置错误、监听失败、master 不可达分别给出诊断和有界测试结果。

传输按字段编码，禁止把 C++ 结构体布局或进程内指针写到网络。
socket 事件仍只向 Actor 投递，I/O 回调不能直接执行 Lua。
对重启后的连接和迟到响应使用明确的生命周期判断，防止旧连接清理新连接、旧响应匹配新请求。

## 6D：cluster 独立闭环

- 先在当前 Lua ABI 下接入上游 `lua-cluster.c` 薄绑定，审计其 allocator、payload 消费及错误路径；
  如需重写，保持原版字节编码和 Lua 返回格式，并保留交叉编解码用例。
- 验收 `cluster.open/call/send/proxy/register/unregister/query/reload`；
  `cluster.snax` 若受上层服务依赖限制，必须在模块清单中单独标注并交由阶段七闭环。
- 原版 `clusterd/clustersender/clusteragent/clusterproxy` 使用当前 socketchannel、netpack 和模块路径启动；
  重构进程不得误加载 original 输出目录的模块来掩盖缺失。
- 覆盖数值地址/名称地址、请求/响应/push/trace、session 正数回绕与请求关联。
  分片阈值以当前 `lua-cluster.c` 的 `0x8000` 为依据，验证阈值前一字节、阈值、阈值后一字节及多片大消息。
- 覆盖分片缺失、截断、非法长度、重组中断及重组缓存清理；实际消息上限与资源预算在 6A 固定。
- 对断线时未完成请求按上游契约返回错误；对远端不响应的情况由测试 watchdog 有界结束并记录行为。
  不自动重放具有业务副作用的请求，不承诺网络故障下的恰好一次执行。
- 验证节点配置 reload、不可达地址、重连及恢复后新请求；名称空间与 harbor 全局名称分开管理。

## 验收矩阵

以下拓扑对 harbor 和 cluster 分别执行。O 表示原版，R 表示重构版；混部同时覆盖请求发起方向与节点启动顺序。

| 用例组 | O→O 基准 | R→R | O→R | R→O | 核心断言 |
| --- | --- | --- | --- | --- | --- |
| 请求/响应与单向消息 | 必跑 | 必跑 | 必跑 | 必跑 | 值、协议、source/session 关系一致 |
| 注册、查询、注册前发送 | 必跑 | 必跑 | 必跑 | 必跑 | 排队与唤醒符合对应机制；无漏投/重复投递 |
| 空包、NUL、分片边界、大包 | 必跑 | 必跑 | 必跑 | 必跑 | 字节一致；无越界、截断或跨请求串包 |
| 目标退出、断线、重启 | 必跑 | 必跑 | 必跑 | 必跑 | 错误可观察；新连接可继续工作 |
| 服务退休、全进程停机 | 必跑 | 必跑 | 必跑 | 必跑 | 请求与缓冲得到处理，进程组有界退出 |
| 畸形帧与资源预算 | 源码对照 | 必跑 | 受控注入 | 受控注入 | 明确拒绝，无无限分配和泄漏 |

多节点服务发现另加至少三节点场景，避免只验证点对点而漏掉名称传播。
正常已确认投递的消息按唯一 ID 集合和契约内顺序检查；故障窗口内无法确认的执行结果单独记录，不能当成可靠投递保证。

## 建议提交顺序与退出条件

1. `test(distributed): add protocol fixtures and multi-process runner`（6A）。
2. `feat(harbor): route remote messages and manage node connections`（6B）。
3. `feat(harbor): support cmaster and cslave bootstrap`（6C）。
4. `feat(cluster): integrate codecs and upstream Lua services`（6D）。
5. `test(distributed): cover mixed runtimes and failure recovery`（6E）。
6. `docs(distributed): record supported topology and validation`。

- [ ] 两套通信机制均完成矩阵，单节点及业务回归通过。
- [ ] Debug/Release 共用用例通过；ASan+UBSan 与独立 TSan 覆盖重构侧路由、重组、断开和停机。
- [ ] 无悬挂子进程、未解释的 FD/缓冲增长或遗留等待者；失败日志包含节点、连接与请求关联信息。
- [ ] 交付文档注明配置示例、消息上限、重连/错误语义及未支持项，提交实际命令与结果。
