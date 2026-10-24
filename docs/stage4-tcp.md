# 阶段四：Asio TCP 与 socketdriver

> 本文保留阶段四交付时的范围与结果。后续 IPv4 UDP、FD、socket info 和 C gate
> 已由 [阶段五](stage5-network.md) 补齐；阶段四的占位接口错误不再代表当前行为。

重构进程由 Actor Runtime 拥有 Asio 网络组件。网络线程只把事件转换为
`PTYPE_SOCKET=6` 消息；Lua/native callback 在 Actor worker 上执行。
启动失败回收网络、timer、scheduler；停机先取消 I/O，并在销毁服务前 join 网络线程。

## 支持范围

- TCP listen/connect、DNS、IPv4/IPv6、accepted start、pause/resume、owner transfer。
- send/lsend、TCP_NODELAY、graceful close、shutdown、读半关闭。
- 原版 socketdriver 的 buffer/push/pop/drop/readline/readall/clear/unpack/header/str2p/resolve。
- 未修改的上游 socket.lua、socketchannel、snax.gateserver 与 netpack TCP framing。
- send/close/pause/nodelay 允许跨服务操作；listen/connect/start 检查新 owner 的存活状态。

Lua socketdriver/netpack 的 C 绑定复用仓库内上游源码，链接 C++ socket facade。
没有链接原版 socket_server、scheduler，未改动第三方 Lua/C 文件。
UDP、FD bind、socket info、C gate 和多节点通信留给后续阶段；Lua 的未实现接口
明确报错，C facade 返回失败。自定义 SOCKET_BUFFER_OBJECT 需要后续对象接口支持；
当前支持 malloc 所有权缓冲与借用指针/字符串/完整 Lua userdata。

## 事件与所有权

| Asio 事件 | Skynet 编号 | id / ud / 数据 |
| --- | --- | --- |
| data | 1 | socket / 字节数 / malloc 嵌套缓冲 |
| open | 2 | socket / listen-ready 端口或 0 / 内联地址、start、transfer |
| close | 3 | socket / 0 / 空 |
| accept | 4 | listener / accepted socket / 内联对端地址 |
| error | 5 | socket / 0 / 内联错误文本 |
| warning | 7 | socket / KiB 或归零通知 / 空 |

listen 等待 OS 监听结果后返回，端口占用/DNS 失败当场返回错误，同时保留 listen-ready 消息。
等待期间不持有 network lifecycle mutex，stop/retire 可取消建立中的 listener；
该 API 不可从 I/O worker 或 EventSink 回调调用。
listener 的 listen-ready 与 start-ready 分别确认；accepted socket 首次 start 和
再次 transfer 均确认。投递拒绝时 listener 回收 accepted socket；服务退出关闭仍归属
该服务的 socket。归属检查在线程 strand 内执行，避免 transfer 尚未执行时漏关。

消息外层为 move-only OwnedBuffer。未消费或邮箱丢弃时同时释放外层与嵌套缓冲；
进入 native callback 时，嵌套数据交给消费者，外层按 callback 返回值释放或保留。
SOCKET_BUFFER_MEMORY 即使发送失败也回收，RAWPOINTER 在 C 调用返回前复制。

graceful close 先发布一次 CLOSE，唤醒 Lua 的 close/read，再排空现有写队列；
对端 EOF 只关闭读端，仍可写响应。shutdown 取消读写，但持有 in-flight buffer
直到 completion 返回。发送额度在 post 前原子保留，覆盖尚未执行的闭包；
high priority 不重排正在写入的部分消息。超限发送返回失败并关闭连接。

## 验证入口

```bash
cmake --preset linux-debug
cmake --build --preset all-debug -j 4
ctest --preset linux-debug --output-on-failure
cmake --preset linux-release
cmake --build --preset all-release -j 4
ctest --preset linux-release --output-on-failure
```

`lua.stage4.original/refactor` 运行同一组真实 Lua 用例：缓冲 API、IPv4/IPv6、
跨服务写、暂停恢复、含 NUL 的 1.2 MB 大包、连续消息、两次 owner start、
socketchannel 断线重连、gateserver 分割帧头/粘包/空包/60 KB 帧、
4 MB 写后关闭与慢读、服务退出、拒绝连接、DNS 失败、监听端口占用/非法域名
及发送拒绝的所有权。

`network.runtime` 使用暂停 EventSink 的确定性场景验证 post 闭包额度、
高低优先级、transfer/close_all 竞态、退休 owner 拒绝、半关闭后响应、
CLOSE 先于尾包排空、warning 清零、取消写与 Runtime 重启。
`runtime.socket` 验证 ABI 字段、worker 派发、转移后旧 owner 退出、
callback 在途时退休、邮箱内嵌套数据丢弃与拒绝投递。
消息投递在 HandleManager 读锁内完成入队/调度，I/O 线程不取得 Context 的最后一个
共享引用，避免与退休并发时在网络线程执行 Lua VM 析构/终结器。

内存安全验证使用 C/C++ 均启用 ASan+UBSan 的构建运行上述测试；
线程安全验证使用独立 TSan 构建运行 runtime/network 测试。

## 本次验收结果（2026-10-06）

| 构建 | 结果 |
| --- | --- |
| Debug | 17/17 CTest 通过，含 original/refactor 共用用例 |
| Release | 17/17 CTest 通过 |
| ASan+UBSan | 13/13 重构侧 CTest 通过；启用 leak 检查，无内存错误 |
| TSan | 13/13 重构侧 CTest 通过，无数据竞争报告 |
| 扩展告警 | 构建通过；新加 socket facade 的窄化转换已显式校验/转换，保留上游及原有告警 |

sanitizer 覆盖重构侧的 C++、Lua C 库、绑定和 native 测试模块；原版由独立
Makefile 构建，因此差分用例在普通 Debug/Release 构建验收。

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan -E original --output-on-failure
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/tsan -E original --output-on-failure
```

本机直接启动 TSan 会报 `unexpected memory mapping`；上述 setarch 只改变测试
子进程的 ASLR 设置，无需更改系统配置。慢读用例显式 pause 后恢复读取，
避免上游 socket.lua 的自动暂停与 readall 开始时机使测试依赖机器运行速度。
