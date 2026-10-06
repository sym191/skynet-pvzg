## 目录结构

```
.
├── cmake/                 # CMake helper modules and dependency wiring
├── configs/               # Runtime config templates generated into build/
├── lua/                   # Shared Lua code used by both runtimes
│   ├── examples/
│   ├── lualib/
│   └── service/
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
