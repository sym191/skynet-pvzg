# Lua Service 兼容设计

`skynet::service::LuaService` 是重构 runtime 中与 Skynet SNLua 对等的服务。
兼容基准固定为仓库 `third_party/skynet` 当前提交，而不是另行维护一份近似实现。

## 同源兼容层

以下代码直接参与 `skynet_lua_service` 的构建：

- `service-src/service_snlua.c`：Lua VM、profile 协程、signal 和内存限制；
- `lualib-src/lua-skynet.c`：完整 `skynet.core` Lua API；
- `lualib-src/lua-seri.c`：与 Skynet 一致的 `pack`/`unpack` 二进制格式；
- `lualib/loader.lua`：服务查找、参数拆分、预加载和 Lua 路径切换。

桥接代码只完成两件事：把静态链接的 `skynet.core` 放入
`package.preload`，以及把 Skynet C runtime API 转发到 C++ `Context`。
因此 Lua 侧函数名、参数、返回值、消息指针生命周期和序列化格式均由上游实现决定。

## Runtime 边界

`Context` 提供 SNLua 所需的稳定边界：

- 按 handle 或名称发送消息，并保留 source、type、session 和 payload；
- session 分配、服务地址解析和 harbor 判断；
- `REG`、`QUERY`、`NAME`、`GETENV`、`SETENV`、`TIMEOUT`、
  `STARTTIME`、`STAT`、`EXIT` 等基础命令；
- 日志、定时器、命名、路由及扩展命令回调。

单 Context 默认实现支持自投递和零延迟启动，方便独立测试。完整 runtime 应安装
send、resolve、name、timeout 和 command handler，将 `LAUNCH`、跨服务投递、真实定时器、
服务管理与集群路由接到相应子系统。LuaService 不为缺失的 runtime 能力伪造成功结果。

## 生命周期

1. `LuaService::init` 创建兼容 context，初始化 SNLua VM，并同步运行 loader；
2. loader 加载目标服务，标准 `skynet.start` 注册原生回调并申请启动 timeout；
3. runtime 将消息交给 `dispatch`，原始 `skynet.core` 回调收到五元组
   `(type, msg, size, session, source)`；
4. `signal(0)` 使用上游 hook 中断运行中的 Lua，`signal(1)` 报告实际内存；
5. 析构时先关闭 Lua VM，再释放 runtime 兼容 context。

`register_lua_service` 将工厂按上游模块名 `snlua` 注册到 `ServiceRegistry`，
服务启动器无需了解具体 C++ 类型。

## 验证

`lua.service` 测试覆盖：

- 全部 `skynet.core` 导出存在且可调用；
- pack/unpack 与 packstring；
- handle、harbor、环境、时间和 session 行为；
- callback 消息元数据及发送路径；
- SNLua 内存限制和 signal；
- 未修改的上游 `skynet.lua` 的 start/timeout/exit 生命周期。
