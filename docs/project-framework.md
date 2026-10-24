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
├── src/skynet_refactor/   # C++23 runtime executable
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

## C++ 命名约定

- 类、结构体、枚举类型和类型别名使用 `PascalCase`；构造函数和析构函数与类名一致。
- 常量使用 `kPascalCase`，宏使用全大写加下划线。
- 函数（包括成员函数）、变量和参数使用小写加下划线，例如 `is_remote`、`request_stop`。
- 类成员变量使用小写加下划线并以 `_` 结尾，例如 `next_session_`。
- 第三方库及其接口沿用上游命名。

## 专题文档

- [Network 设计架构](network-architecture.md)
- [阶段四 TCP 与 socketdriver 交付](stage4-tcp.md)
- [阶段五 UDP、FD 与 C gate 交付](stage5-network.md)

## Lua 业务启动

`./start-server.sh` 启动原版 Skynet，注册 `.team` 和免密码登录服务 `.login`。
登录接受玩家 ID，初始化数据并登记独立玩家服务。目录规范、RPC 接口与验证命令见
[Lua 服务说明](../lua/README.md)。

`tools/run-original.sh` 使用 CMake 生成的配置启动相同业务入口，状态文件默认位于对应
构建目录的 `original/team-state.json`；日常业务启动建议使用带状态文件锁的 `start-server.sh`。

`tools/run-refactor.sh` 使用同一 Lua 业务入口，在重构版中注册 `.team` 和 `.login`。
生成配置将状态文件放在构建目录的 `refactor/team-state.json`，与原版状态文件分开。
