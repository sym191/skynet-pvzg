# Lua Service 兼容设计

`skynet::service::LuaService` 在 C++ runtime 中提供与 SNLua 对等的服务边界，
目标是直接运行原版 Lua 层，同时不把原版 Skynet 的 native runtime 源码编入
重构目标。

## 源码边界

`skynet_lua_service` 只编译 `src/service` 下的 C++ 源文件：

- `LuaRuntime.cc`：Lua VM 生命周期、自定义 allocator、profile、codecache API、
  loader、callback dispatch 和 signal；
- `LuaCore.cc`：`skynet.core` 的命令、发送、日志、时间、回调和消息所有权接口；
- `LuaSerialization.cc`：兼容 Skynet wire format 的 `pack`/`unpack`；
- `LuaService.cc`：把上述能力接入通用 `runtime::Service`。

服务目标不包含、不链接以下原版 Skynet native 实现：

- `service-src/service_snlua.c`；
- `lualib-src/lua-skynet.c`；
- `lualib-src/lua-seri.c`；
- `skynet-src` 中的 context、message queue 或 scheduler 实现。

Lua VM 仍由项目的 `skynet::lua_c` 目标提供；原版 `lualib/*.lua` 是被兼容和
验证的 Lua 层输入，不作为 C++ runtime 实现的一部分。

## Lua 加载隔离

仓库中的 Lua VM 带共享 prototype cache。直接调用默认 `loadfile` 时，连续创建
多个 VM 可能复用绑定到旧 `_ENV` 的 chunk。LuaRuntime 用 C++ 安装独立
`loadfile` 和 Lua module searcher，并从未缓存的 Lua 文件加载入口读取 chunk，
避免 service 之间共享 Lua closure 或环境。

`skynet.codecache` 保留 `clear`、`mode` 和 `loadfile` 接口。目前 C++ runtime
选择确定性的隔离加载，`clear`/`mode` 不改变加载策略。

## 消息与所有权

- 普通 callback 获得仅在本次 dispatch 内有效的消息视图；
- forward callback 获得独立分配的消息，并通过 `skynet.core.send` 或
  `skynet.core.trash` 转移/释放所有权；
- Lua string 消息由 `Context::send` 复制；
- `pack` 返回的 lightuserdata 由 `send` 或 `trash` 消费；
- `packstring` 返回 Lua 自有 string，不需要显式释放。

这些规则保持原版 Lua 层对指针、长度、session、source 和 protocol type 的调用
约定，同时把实际路由交给 C++ `Context`。

## Runtime 边界

`Context` 提供 Lua 层所需的稳定接口：

- 按 handle 或名称发送消息，并保留 source、type、session 和 payload；
- session 分配、服务地址解析和 harbor 判断；
- `REG`、`QUERY`、`NAME`、`GETENV`、`SETENV`、`TIMEOUT`、
  `STARTTIME`、`STAT`、`EXIT` 等基础命令；
- 日志、定时器、命名、路由及扩展命令回调。

单 Context 默认实现支持自投递和零延迟 timeout，便于独立测试。完整 runtime
仍需安装 send、resolve、name、timeout 和 command handler，把 `LAUNCH`、跨服务
投递、真实定时器、服务管理和集群路由接入相应 C++ 子系统。

## 验证

`lua.service` 覆盖：

- `skynet.core` 导出及数值命令类型；
- 与 Skynet 一致的序列化字节格式、table 和 `__pairs`；
- callback metadata、forward 所有权和发送路径；
- memory limit、signal 和失败关闭；
- 连续创建多个 Lua VM；
- 未修改 `skynet.lua` 的 start/timeout/exit 生命周期。

`lua.service_native_sources` 读取编译数据库，阻止上述三份原版 Skynet native
源码重新进入重构目标。
