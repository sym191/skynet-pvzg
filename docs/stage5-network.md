# 阶段五：IPv4 UDP、FD、socket info 与 C gate

本阶段以 `feature/stage4-tcp` 的 `c3a5090` 为基础，提交为 `c37d90f`。
当前已合并到 `/home/anon/skynet-pvzg` 的 `feature/team-module` 分支；Lua 业务改动
在合并前单独保存为 `05977ff`，保留登录、玩家与组队服务。
按本次要求，UDP IPv6 和 IPv6 地址 blob 延期；已有 TCP IPv6 能力继续保留。

## 交付范围

| 接口/能力 | 实现与兼容语义 |
| --- | --- |
| `udp` / `udp_listen` | IPv4/DNS 绑定；udp 支持临时端口，udp_listen 拒绝端口 0；绑定失败同步返回 |
| `udp_connect` / `udp_dial` | 设置默认目标；普通 send/lsend 使用该目标，不使用 OS connect 限制来源 |
| `udp_send` / `sendto` | 指定单个报文的目标，不改变默认目标；高低优先级不重排已提交报文 |
| `udp_address` | 7 字节来源 blob：1 字节协议值 1、2 字节网络序端口、4 字节 IPv4 |
| `resolve` | 复用上游 Lua C 绑定；验证 localhost 和非法域名行为 |
| `socket.bind(fd)` / `socket.stdin()` | POSIX descriptor 的异步流；支持 socket、管道、stdin 与只写管道 |
| `socketdriver.info` / `socket.netstat` | LISTEN/TCP/UDP/BIND/CLOSING、owner、队列字节、读写累计、时间戳和状态 |
| 原生 `gate` 服务 | C 服务 ABI 的 C++ 实现，输出 `refactor/cservice/gate.so`，经已有 NativeService 加载 |

Lua 的公开名称是 `socket.netstat()`，底层名称为 `socketdriver.info()`；沿用上游返回结构，
不另造一个不兼容的 `socket.info()`。LISTEN 返回 `accept/rtime/sock`，流与 UDP 返回
`read/write/wbuffer/rtime/wtime/reading/writing/peer`，`address` 为服务 handle。
快照逐 socket 在 strand 内生成，统计与生命周期没有数据竞争；不同 socket 的快照不是
同一时刻的全局事务。`rtime/wtime` 与 Actor 的 `skynet.now()` 共用时钟，单位为 10 ms。
尚未 start 的 listener/accepted socket 与已移除 socket 不出现在快照中。

UDP 收到 0 字节仍产生 UDP 事件；IPv4 最大负载为 65507 字节，发送更大的负载同步拒绝。
接收缓冲配置必须至少容纳 65507 字节，避免默默截断报文。UDP 发送队列额度在 post 前
保留，空报文也占至少一个额度；`wbuffer` 和 warning 则按实际负载字节统计。
强制关闭时取消 I/O，completion 持有正在发送的存储直到返回；优雅关闭先发一次 CLOSE，
再排空已排队数据。UDP/FD 的 transfer 与服务退休检查均在 strand 内完成。

FD 使用 `F_DUPFD_CLOEXEC` 保留自己的副本。关闭/退休/停止只关闭副本，**不关闭也不
shutdown 调用方的原始 FD**。调用方不得并发消费同一个流；`O_NONBLOCK` 作用于共享的
open file description，因此也影响原始 FD。只写管道不启动读取；断开的管道/socket
通过 ERROR 报告 EPIPE。网络工作线程屏蔽 SIGPIPE，不修改嵌入进程的全局 signal handler。
普通文件不属于本阶段承诺的可轮询 FD 场景。

`listen/info` 等待 I/O worker，不得从 EventSink 回调调用；UDP DNS 查询沿用上游同步
返回结果的契约，但查询不持有网络生命周期锁。C 层 MEMORY 缓冲在失败时也被消费，
RAWPOINTER 在返回前复制；自定义 SOCKET_BUFFER_OBJECT 仍是阶段四已有的不支持项。
UDP 事件的外层和嵌套 payload/address 在邮箱拒绝、服务退休和未消费时均由 RAII 回收。
Lua facade 在进入上游指针 ABI 前校验地址长度；无效 FD 绑定立即报错，不进入等待状态。

## C gate 与 Lua gateserver

C gate 支持 S（2 字节）和 L（4 字节）大端帧头、半包、粘包、watchdog 的
open/data/close 通知、start/kick/close/forward/broker 命令、agent/client 来源与 session
路由，以及 PTYPE_CLIENT 的小端 socket id 尾部回写协议。跨服务写保持上游允许的行为。
空帧与上游 C gate 一致：消费但不转发。Lua gateserver/netpack 的空帧行为由阶段四用例
独立验证，不能把两者视为同一个模块。

帧头长度达到 16 MiB 即拒绝，包含 `0x01000000`、`0x80000000`、`0xffffffff`。
这是对上游的明确加固：上游通常等到完整 payload 才检查长度，且有符号长度会影响判断；
重构版在帧头到达时就关闭连接，不等待攻击者补齐大包。共用用例验证正常协议行为，
这三个恶意长度用例只对重构版执行。

## 验证入口

```bash
cmake --preset linux-debug
cmake --build --preset all-debug -j 4
ctest --preset linux-debug --output-on-failure
cmake --preset linux-release
cmake --build --preset all-release -j 4
ctest --preset linux-release --output-on-failure
```

新增 `lua.stage5.original/refactor`：同一份上游 Lua socket 库与验收脚本，覆盖所有 UDP
入口、空包/含 NUL/65507 字节、DNS 失败、地址往返、stdin 真管道、socket 统计以及
原生 gate 的两种帧头、半包/粘包、watchdog/agent/broker 路由、client 回写与关闭。
Python runner 用 stdin pipe 启动进程并设置超时，不能用普通文件重定向替代管道验收。

新增 `network.stage5`：外部 UDP socket 互通、非法地址/长度、端口 0、默认目标更新、
跨服务发送、暂停/转移、旧 owner 退休、事件投递拒绝、空包队列预算、warning 清零、
FD/socketpair/管道/只写管道、原始 FD 保留、在途写取消、EPIPE、info 与 stop 并发和重启。
`runtime.socket` 补充 UDP 空包/最大包 ABI 与退休邮箱拒绝；`runtime.contract` 补充
C facade 的 MEMORY 失败释放；原阶段四用例继续覆盖 Lua gateserver/netpack。

UDP 大包使用 `127.0.0.2`：本机 WSL 的 `127.0.0.1` 对 8192 字节以上的 UDP 会丢包，
直接 Python socket 和原版 Skynet 都可复现；同属回环网段的 `127.0.0.2` 可完成 65507
字节往返。未跳过最大包验收，也未修改系统网络配置。

本次构建复用原工作区缓存的 Asio/spdlog 源码，构建目录为 `build/debug`、`build/release`、
`build/asan`、`build/tsan`。Sanitizer 对 C/C++ 均启用，并覆盖 gate 模块及 Lua 绑定；
原版由独立 Makefile 构建，原版差分验收在 Debug/Release 运行。

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan -E original --output-on-failure
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tsan -E original --output-on-failure
```

## 本次验收结果（2026-10-24）

| 构建 | 结果 |
| --- | --- |
| Debug | 21/21 CTest 通过，含 original/refactor 共用验收 |
| Release | 21/21 CTest 通过 |
| ASan + UBSan | 16/16 重构侧 CTest 通过，开启 leak 检查，无内存错误 |
| TSan | 16/16 重构侧 CTest 通过，无数据竞争报告 |
| Git diff | `git diff --check` 通过；未修改 third_party 子模块源码 |

全量 TSan 首次发现基线 worker monitor 的 source/destination 普通字段存在并发读写。
本次一并改为原子发布地址对、版本号标记更新并二次检查，避免数据竞争与跨消息字段混读；
保留既有 monitor 行为测试并通过全部四种构建。该修复不是 UDP 协议逻辑的一部分，
纳入本次交付以消除完整线程验收的已知失败。

实现和交付文档已提交并合入原工作区，日常开发继续在 `/home/anon/skynet-pvzg` 进行。
`docs/local-plans` 已更新实施记录，继续保持 Git 忽略。

## 合入当前工作区后的复验

第五阶段及阶段三、四前置实现已与 `feature/team-module` 合并。文档冲突保留两边内容，
现有 Lua 登录、玩家、组队代码保持不变。重构版生成配置补齐 `team_state_path`、
`team_invite_ttl`、`team_policy_module`，状态文件位于重构版输出目录，独立于原版。

- 当前目录 `build/linux-debug` 的 21 项 CTest 全部通过。
- Lua 组队模型/JSON 的 85 项检查通过。
- 原版组队、登录 RPC 的 seed/recover/rollback 与启动器状态锁检查全部通过。
- 重构版使用共享业务入口、临时独立状态文件，成功注册 `.team` 与 `.login`。

本次合并仅涉及文档冲突处理及启动配置补齐；上表 sanitizer 结果对应第五阶段实现提交。
