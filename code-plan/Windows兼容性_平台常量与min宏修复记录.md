# Windows 兼容性修复：平台 Socket 常量混用与 min 宏展开

- 报障来源：GitHub Actions `MSBuild` job（Windows）
- 失败运行：run `40063054973`（PR #8，分支 `feat_update_tools`，head `b53f2d3aa917d6b7c2e7ab8917f284f5e00d887f`，job `build-windows` 步骤 `Build`）
- 修复范围：`src/socket_util.{hpp,cpp}`、`src/tcp_transport.cpp`、`tests/tcp_test_slave.cpp`、根 `CMakeLists.txt`、`tests/CMakeLists.txt`、`tests/tcp_transport_test.cpp`

## 0. 结论速览

| # | 位置 | CI 症状 | 根因 | 处置 |
|---|---|---|---|---|
| 1 | `src/socket_util.cpp:278`（改前） | `SHUT_RDWR` 未定义 | `shutdown_socket()` 没有平台分支，直接使用 POSIX 常量 | 新增 `detail::kShutdownBoth`（POSIX `SHUT_RDWR` / Winsock `SD_BOTH`） |
| 2 | `src/socket_util.cpp:325`（改前） | `WSAEPIPE` 未定义 | Winsock **不存在**该错误码；`EPIPE` 只是 MSVC CRT 的 `errno` | 删除该项，判定集合外提为 `detail::IsConnectionLostCode()` |
| 3 | `src/tcp_transport.cpp:213`（改前） | `C2589` / `C2059` + `decoder_limit` 连带未初始化 | `std::min(...)` 中的 `min` 被 Windows 的 `min` 宏展开 | 改 `(std::min)(...)`，并在 Windows 目标上定义 `NOMINMAX` |
| 4 | `tests/tcp_test_slave.cpp:861` | CI 未报（`LIBXCP_BUILD_TESTS=OFF`） | 同上，裸 `std::min(` | 主动一并改为 `(std::min)(...)` |

## 1. 缺陷 1：`SHUT_RDWR` 未定义

改前（无平台分支）：

```cpp
int shutdown_socket(SocketHandle handle) override {
    // 双向关闭：立即让阻塞中的 send/recv 返回，但**不**释放句柄号
    return ::shutdown(handle, SHUT_RDWR);
}
```

- `SHUT_RDWR` 由 POSIX 的 `<sys/socket.h>` 提供；Winsock 只提供 `SD_RECEIVE` / `SD_SEND` / `SD_BOTH`。
- 同文件中 `is_interrupted()`（`:296`）、`is_would_block()`（`:305`）、`is_in_progress()`（`:314`）都做了 `#if defined(_WIN32)` 分叉，**只有 `shutdown` 与 `is_connection_lost` 漏了**，属同批代码的疏漏而非设计取舍。
- 仓库里已有正确写法可对齐：`tests/tcp_test_slave.cpp:77-79` 的 `#ifdef _WIN32 ::shutdown(handle, SD_BOTH); #else SHUT_RDWR`。

改后（`src/socket_util.hpp` 定义常量，`src/socket_util.cpp` 直接引用）：

```cpp
#if defined(_WIN32)
inline constexpr int kShutdownBoth = SD_BOTH;
#else
inline constexpr int kShutdownBoth = SHUT_RDWR;
#endif
```

```cpp
int shutdown_socket(SocketHandle handle) override {
    return ::shutdown(handle, kShutdownBoth);
}
```

## 2. 缺陷 2：`WSAEPIPE` 未定义

改前：

```cpp
    [[nodiscard]] bool is_connection_lost() const override {
        const int code = lastSocketErrorCode();
#if defined(_WIN32)
        return code == WSAECONNRESET || code == WSAECONNABORTED ||
               code == WSAENOTCONN || code == WSAESHUTDOWN ||
               code == WSAEPIPE;
#else
        ...
```

- Winsock 错误码表里没有 `WSAEPIPE`；`EPIPE` 是 CRT 的 `errno`，与 `WSAGetLastError()` 不是同一套编码。
- Windows 侧“对端已关闭”的等价表现是 `WSAECONNRESET(10054)` / `WSAECONNABORTED(10053)`，本地已 shutdown 后为 `WSAESHUTDOWN(10058)`；三项原本就在集合内，**删除 `WSAEPIPE` 不损失任何判据**。
- 同时把判定集合从类内联实现外提为独立函数，使其可被单测直接覆盖（见第 4 节）。

## 3. 缺陷 3：`min` 宏展开

改前：

```cpp
    const std::size_t decoder_limit =
        std::min(m_config_.max_packet_size, m_config_.max_frame_packet_size);
```

- 未定义 `NOMINMAX` 时，Windows 头会注入函数式宏 `min(a, b)`；`std::min(...)` 被预处理为 `std::((a)<(b)?(a):(b))`，MSVC 报 `C2589 '(': illegal token on right side of '::'` 与 `C2059`。
- `decoder_limit` 的“未初始化”是**连带错误**：初始化表达式解析失败后，到下一处使用时再报一次，并非独立缺陷。
- 取证：全仓 `grep NOMINMAX` 零命中（`include/ src/ CMakeLists.txt tests/CMakeLists.txt`）；glog 只在 `cmake-build-release/_deps/glog-src/CMakeLists.txt:426` 用 `target_compile_definitions(glog PRIVATE NOMINMAX)` 保护自身，**不传递给使用方**。
- 仓库内已有同类写法先例：`src/command_executor.cpp:914`、`tools/xcp_master/main.cpp:1243/1246/1251` 都用 `(std::min)(...)`；`src/socket_util.cpp:131` 因写作 `std::min<std::uint32_t>(...)`（`min` 后紧跟 `<`）而不触发宏展开。
- 说明：具体哪个 SDK 头把 `windows.h` 传递进来，本机（macOS）无法验证（CI 日志下载需 token，`403`）。**修复不依赖该结论**：`NOMINMAX` 让宏不产生，`(std::min)` 让宏不展开，两者各自独立生效。

## 4. 防回归措施

1. **`NOMINMAX` 按目标开启**（不改动第三方库，避免影响其构建）：
   - 根 `CMakeLists.txt` 的 `if(WIN32)` 块中 `target_compile_definitions(libxcp PRIVATE NOMINMAX)`；
   - `tests/CMakeLists.txt` 顶部 `if(WIN32) add_compile_definitions(NOMINMAX)`（覆盖该目录及其子目录下的所有测试目标）。
2. **新增 3 个单测** `tests/tcp_transport_test.cpp` → `SocketPlatformTest.*`：
   - `ClassifiesConnectionLostCodes`：本平台“连接丢失”错误码全部返回 true；
   - `KeepsOtherCodesOutOfConnectionLostSet`：would-block / 被打断 / 0 必须返回 false；
   - `ShutdownBothSelectsBothDirections`：常量不是单向别名。
3. **编译期断言**：`static_assert(detail::kShutdownBoth == SD_BOTH)`（Windows）/ `== SHUT_RDWR`（POSIX），误写另一平台常量时连编译都过不去。
4. `tests/CMakeLists.txt` 为 `libxcp_tests` 增加 `${PROJECT_SOURCE_DIR}` 头文件目录，使测试可用 `#include "src/socket_util.hpp"` 覆盖内部平台适配逻辑。

## 5. 验证结果

| 验证项 | 命令 | 结果 |
|---|---|---|
| 宏展开最小复现（证明诊断） | `clang++ -std=c++20 -c minmacro_bad.cpp` | 复现 `error: expected unqualified-id ... expanded from macro 'min'`（等价 MSVC C2589） |
| 修复写法可用 | `clang++ -std=c++20 -c minmacro_good.cpp` | 编译通过 |
| 新增用例 | `./tests/libxcp_tests --gtest_filter='SocketPlatformTest.*'` | **3 passed** |
| 全量测试（macOS Release） | `ctest -E "^Example" -j 4` | **992 项全通过**（1 项既有 Skip） |
| Examples（串行） | `ctest -R "^Example" -j 1` | 13/14，唯一失败 `ExampleXcpIntegrationDemoCppDemo`（既有问题，与本次改动无关；并行跑会因抢占默认端口 5555 额外失败） |

编译告警：仅 `ld: warning: ignoring duplicate libraries`（既有）。

## 6. 未验证项（如实声明）

- **本机无 MSVC 环境**，Windows 侧只能靠代码审查 + 编译期断言保证；真正结论需等该 PR 的 `build-windows` job 复跑。
- Windows CI 当前 `LIBXCP_BUILD_TESTS=OFF`，因此 `tests/` 下的同类问题（第 4 项）在本轮 CI 中不会暴露，属于**主动修复**而非 CI 驱动。

## 7. 回滚

```bash
git checkout -- CMakeLists.txt src/socket_util.cpp src/socket_util.hpp \
    src/tcp_transport.cpp tests/CMakeLists.txt tests/tcp_test_slave.cpp \
    tests/tcp_transport_test.cpp
```

## 8. 后续建议（待裁决，本次未做）

1. Windows job 打开 `LIBXCP_BUILD_TESTS`（至少编译不运行），让“只有 MSVC 才暴露”的测试代码问题进入 CI 视野。
2. 把 `tests/tcp_transport_test.cpp:10` 提到的 `tests/tcp_fake_socket.hpp` 补齐：该文件当前**不存在**，注释描述的“注入假 SocketOps 精确驱动错误分支”实际没有实现，`SocketOps` 也没有测试注入入口（`src/socket_util.cpp` 只有 `RealSocketOps()` 单例）。这属于既有文档与实现不一致，与本次改动无关。