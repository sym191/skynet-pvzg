## 目录结构

```
.
├── cmake/                 # CMake helper modules and dependency wiring
├── configs/               # Runtime config templates generated into build/
├── lua/                   # Shared Lua code used by both runtimes
│   ├── examples/
│   ├── lualib/
│   └── service/
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
