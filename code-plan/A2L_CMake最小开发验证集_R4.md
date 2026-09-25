# A2L CMake 最小开发验证集（R4）

> 状态：用户已确认“现在只考虑实现代码，只保证最小开发验证集，其他先忽略”。  
> 平台：仅 Windows x64 / MSVC vc143。默认验证 Release；Debug 仅供开发者自行调试。  
> 本文覆盖构建组织，不代表整体设计已批准。

## 1. 当前必须具备

1. 独立构建上游 `a2l` STATIC 与薄壳 `liba2l.dll`。
2. 主树在 `LIBXCP_BUILD_A2L=ON` 时构建 `libxcp_a2lbridge`。
3. 构建并运行一个 `A2lSmoke` 冒烟测试。
4. Boost 只在 SDK 构建树通过 `find_package(Boost CONFIG REQUIRED COMPONENTS locale filesystem process)` 查找。
5. `LIBXCP_LIBA2L_ROOT` 必须显式指定；不设计环境变量、注册表、`.deps-cache` 等回退搜索顺序。
6. 测试目标通过 `POST_BUILD copy_if_different` 把 `liba2l.dll` 复制到 `$<TARGET_FILE_DIR:A2lSmoke>`。
7. A2L 测试生成器由 `find_package(Python3 REQUIRED COMPONENTS Interpreter)` 定位，并作为 CTest fixture 在冒烟测试前运行。
8. `LIBXCP_BUILD_A2L=OFF` 时，原 `libxcp` 构建图、依赖和测试行为保持不变。

## 2. 当前明确不做

- `cmake --install` 安装规则；
- `liba2lConfig.cmake` / export package / 下游 SDK 发布；
- CI 矩阵和制品缓存；
- 构建耗时 SLA；
- Linux/macOS 构建验证；
- Debug/Release 自动 CRT 匹配校验（A-10 已接受风险）；
- 完整测试标签体系和测试套件分组；
- 将预编译 DLL、LIB、PDB 或生成的 A2L 文件提交仓库。

## 3. 最小目录和目标

```text
thirdparty/a2llib/                   # submodule，main HEAD 的实际 SHA 由 gitlink 固定
thirdparty/uchardet-shim/            # P1 验证通过后使用
thirdparty/a2l-sdk/                  # 独立 CMake project
  CMakeLists.txt
  include/liba2l/liba2l_api.hpp
  include/liba2l/liba2l_export.h
  src/liba2l_export.cpp
  build-sdk.ps1
a2lbridge/
  CMakeLists.txt
  include/libxcp/a2l/*.hpp
  src/*.cpp
tests/
  a2l_smoke_test.cpp
  a2l_gen/gen_a2l.py
```

目标关系：

```text
a2l (STATIC, C++23, upstream + static Boost)
  └── liba2l (SHARED, C++23, A2L_INTERFACE)
        └── libxcp_a2lbridge (STATIC, C++20)
              └── A2lSmoke (C++20)
```

## 4. 主树最小消费方式

本阶段不为本工程自己生成可发布的 `liba2lConfig.cmake`。主树使用显式根目录查找三个文件，并创建 IMPORTED target：

```cmake
option(LIBXCP_BUILD_A2L "Build the A2L bridge" OFF)
set(LIBXCP_LIBA2L_ROOT "" CACHE PATH "Prepared liba2l root")

if(LIBXCP_BUILD_A2L)
  if(NOT LIBXCP_LIBA2L_ROOT)
    message(FATAL_ERROR "LIBXCP_LIBA2L_ROOT is required")
  endif()

  find_path(LIBXCP_LIBA2L_INCLUDE_DIR
    NAMES liba2l/liba2l_api.hpp
    PATHS "${LIBXCP_LIBA2L_ROOT}/include"
    NO_DEFAULT_PATH REQUIRED)
  find_library(LIBXCP_LIBA2L_IMPORT_LIBRARY
    NAMES liba2l
    PATHS "${LIBXCP_LIBA2L_ROOT}/lib"
    NO_DEFAULT_PATH REQUIRED)
  find_file(LIBXCP_LIBA2L_RUNTIME
    NAMES liba2l.dll
    PATHS "${LIBXCP_LIBA2L_ROOT}/bin"
    NO_DEFAULT_PATH REQUIRED)

  add_library(liba2l::liba2l SHARED IMPORTED GLOBAL)
  set_target_properties(liba2l::liba2l PROPERTIES
    IMPORTED_IMPLIB "${LIBXCP_LIBA2L_IMPORT_LIBRARY}"
    IMPORTED_LOCATION "${LIBXCP_LIBA2L_RUNTIME}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBXCP_LIBA2L_INCLUDE_DIR}")

  add_subdirectory(a2lbridge)
endif()
```

## 5. 最小测试门禁

`A2lSmoke` 至少验证：

1. DLL 可加载，ABI 版本检查通过；
2. 生成的最小 A2L 可解析；
3. 一个符号可按 `module::symbol` 查询；
4. B-1 地址和 extension 保持原值，AG 元素计数正确；
5. 一个 LINEAR 换算正确；
6. 一个 STATIC DTO 可按冻结布局解出 raw；
7. `LIBXCP_BUILD_A2L=OFF` 时原工程构建不变。

完整 B 类黄金测试仍保留在 `A2L_接口语义_B类决策_R4.md`，但不要求在最小 CMake PoC 一次全部完成。
