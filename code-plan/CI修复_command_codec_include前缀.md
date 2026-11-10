# CI 修复：command_codec.hpp 非法 include 前缀

## 修改内容

移除 `include/libxcp/command_codec.hpp` 中 `#include <span>` 前误入的 `git `，修复 Linux、macOS、Windows 及 ThreadSanitizer 任务的编译失败。

## 验证结果

- 通过：`g++ -std=c++20 -Iinclude -fsyntax-only src/command_codec.cpp`
- 通过：`g++ -std=c++20 -Iinclude -fsyntax-only src/response_parser.cpp`
- 通过：`g++ -std=c++20 -Iinclude -fsyntax-only src/command_executor.cpp`
- 未能运行 Release 配置及 GoogleTest：CMake 未找到必需的 Boost CONFIG 包；单独检查测试源时也因环境缺少 `gtest/gtest.h` 而无法编译。

## 后续 CI 修复：Packed-mode 测试回调悬空引用

macOS CI 复测时构建成功，但 `DaqPackedModeSession.RejectsWhenEventIsNotPeriodic` 和 `DaqPackedModeSession.RejectsWhenEventLacksPackedProperty` 均在测试结束时发生段错误。两个测试把按引用捕获局部 Slave 的回调保存在 `MockTransport` 中；局部 Slave 先于 rig 销毁，清理阶段仍可能调用回调。改为按值捕获，使回调拥有自身的 Slave 副本。

验证限制：本地 CMake Release 配置因缺少 Boost CONFIG 包失败；直接语法检查受影响测试源也因缺少 GoogleTest 头文件无法执行。已检查测试、MockTransport 的同步发送/保存回调行为及 rig 生命周期，并经代码审查确认修复没有显著问题。
