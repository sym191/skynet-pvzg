## 目录结构

```
.
├── cmake/                 # CMake helper modules and dependency wiring
├── configs/               # Runtime config templates generated into build/
├── lua/                   # Shared Lua code used by both runtimes
│   ├── config/            # Standalone original Skynet config
│   ├── examples/
│   ├── lualib/            # common / player / team business modules
│   ├── service/           # main + login / player / team service entries
│   └── tests/             # Lua model tests and original Skynet RPC tests
├── src/skynet_refactor/   # New C++23 runtime scaffold
├── tests/                 # CTest smoke tests
├── third_party/skynet/    # Original skynet git submodule
└── tools/                 # Bootstrap, build, and run scripts
```

## 构建入口

```bash
tools/bootstrap.sh
tools/build.sh refactor debug
tools/build.sh original debug
tools/build.sh all debug
ctest --preset linux-debug
```

## 专题文档

- [Network 设计架构](network-architecture.md)

## Lua 业务启动

`./start-server.sh` 启动原版 Skynet，注册 `.team` 和免密码登录服务 `.login`。
登录接受玩家 ID，初始化数据并登记独立玩家服务。目录规范、RPC 接口与验证命令见
[Lua 服务说明](../lua/README.md)。

`tools/run-original.sh` 使用 CMake 生成的配置启动相同业务入口，状态文件默认位于对应
构建目录的 `original/team-state.json`；日常业务启动建议使用带状态文件锁的 `start-server.sh`。
