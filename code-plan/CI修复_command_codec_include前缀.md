# CI 修复：command_codec.hpp 非法 include 前缀

## 修改内容

移除 `include/libxcp/command_codec.hpp` 中 `#include <span>` 前误入的 `git `，修复 Linux、macOS、Windows 及 ThreadSanitizer 任务的编译失败。

## 验证结果

- 通过：`g++ -std=c++20 -Iinclude -fsyntax-only src/command_codec.cpp`
- 通过：`g++ -std=c++20 -Iinclude -fsyntax-only src/response_parser.cpp`
- 通过：`g++ -std=c++20 -Iinclude -fsyntax-only src/command_executor.cpp`
- 未能运行 Release 配置及 GoogleTest：CMake 未找到必需的 Boost CONFIG 包；单独检查测试源时也因环境缺少 `gtest/gtest.h` 而无法编译。
