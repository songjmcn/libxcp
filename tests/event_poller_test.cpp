/**
 * @file event_poller_test.cpp
 * @brief `detail::EventPoller` 的单元测试：多句柄等待、超时、注入接缝。
 *
 * 依据 `code-plan/Platform接口分层_实施计划_EventPoller与SocketHandle.md`
 * §6 的测试方案：改造前 `select` 的"一次等多个句柄"能力没有直接用例，
 * 只有间接覆盖（UDP 接收循环 / TCP connect 等待）。本文件把该能力显式
 * 固定下来，并覆盖三态返回值中的"超时"与"就绪"两支。
 *
 * @par 为什么用真实 UDP Socket
 *   `EventPoller` 的全部价值就是"与真实内核事件源打交道"，用假实现测它
 *   只能验证转发，无法验证 `poll`/`WSAPoll` 的语义。用例全部绑定回环地址，
 *   不依赖外部网络。
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "src/event_poller.hpp"
#include "src/detail/socket/socket_ops.hpp"

namespace {

using calmcar::xcp::detail::CurrentEventPoller;
using calmcar::xcp::detail::EventPoller;
using calmcar::xcp::detail::Ipv4Endpoint;
using calmcar::xcp::detail::kInvalidSocketHandle;
using calmcar::xcp::detail::RealEventPoller;
using calmcar::xcp::detail::RealSocketOps;
using calmcar::xcp::detail::SetEventPollerForTesting;
using calmcar::xcp::detail::SocketHandle;
using calmcar::xcp::detail::SocketKind;
using calmcar::xcp::detail::WaitAnySocket;
using calmcar::xcp::detail::WaitItem;
using calmcar::xcp::detail::WaitStatus;

/**
 * @brief 句柄 RAII 守卫：用例结束时关闭 Socket
 *
 * @details 用例中途 `ASSERT_*` 失败会提前 return，裸句柄会泄漏；
 *          用析构保证真实 Socket 用例的所有分支都关闭。
 */
class SocketGuard {
public:
    /**
     * @brief 接管一个句柄
     * @param handle 已创建的句柄（可为无效值）
     */
    explicit SocketGuard(SocketHandle handle) noexcept : handle_(handle) {}

    /// @brief 关闭仍持有的句柄
    ~SocketGuard() {
        if (handle_ != kInvalidSocketHandle) {
            RealSocketOps().close(handle_);
        }
    }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    /// @brief 取句柄
    [[nodiscard]] SocketHandle get() const noexcept { return handle_; }

private:
    SocketHandle handle_;  ///< 被接管的句柄
};

/**
 * @brief 创建一个绑定到 `127.0.0.1:0` 的 UDP Socket
 *
 * @param[out] address 成功时回填实际绑定的地址（含 OS 分配的端口）
 * @return 句柄；失败返回 `kInvalidSocketHandle`
 */
SocketHandle MakeLoopbackUdp(Ipv4Endpoint* address) {
    calmcar::xcp::detail::SocketOps& ops = RealSocketOps();
    const SocketHandle handle = ops.create(SocketKind::Udp);
    if (handle == kInvalidSocketHandle) {
        return kInvalidSocketHandle;
    }
    // IPv4 字节按网络顺序存放，端口 0 交由系统动态分配。
    const Ipv4Endpoint local{{127, 0, 0, 1}, 0};
    if (ops.bind(handle, local) != 0) {
        ops.close(handle);
        return kInvalidSocketHandle;
    }
    Ipv4Endpoint actual{};
    if (ops.getsockname(handle, &actual) != 0) {
        ops.close(handle);
        return kInvalidSocketHandle;
    }
    if (address != nullptr) {
        *address = actual;
    }
    return handle;
}

/**
 * @brief 统计事件记录表里"就绪/失败"的项数
 * @param status 等待结果
 * @return 任一关注事件就绪或出现错误位的项数
 */
std::size_t ReadyCount(const std::vector<WaitStatus>& status) {
    std::size_t count = 0;
    for (const WaitStatus& item : status) {
        if (item.Ready()) {
            ++count;
        }
    }
    return count;
}

/**
 * @brief 脚本化 EventPoller：记录入参并返回预设结果
 *
 * @details 用于验证 `RealSocketOpsImpl::wait_readable/wait_writable`
 *          确实把等待委托给了 `CurrentEventPoller()`——即注入接缝有效，
 *          这正是"平台等待原语单点化"的验收点。
 */
class RecordingPoller final : public EventPoller {
public:
    /**
     * @brief 记录等待入参并返回预设值
     * @param items 等待项
     * @param timeout_ms 超时
     * @param status_out 结果表（仅成功时按全就绪填充）
     * @param error_out 预设失败时写入错误文本（可为空）
     * @return 预设返回值
     */
    int Wait(std::span<const WaitItem> items, std::uint32_t timeout_ms,
             std::vector<WaitStatus>& status_out,
             std::string* error_out) override {
        if (result < 0 && error_out != nullptr) {
            *error_out = error_text;
        }
        ++call_count;
        last_timeout_ms = timeout_ms;
        last_items.assign(items.begin(), items.end());
        status_out.assign(items.size(), WaitStatus{});
        if (result > 0) {
            for (WaitStatus& item : status_out) {
                item.readable = true;
                item.writable = true;
            }
        }
        return result;
    }

    int call_count{0};                 ///< 被调用次数
    std::uint32_t last_timeout_ms{0};  ///< 最近一次超时入参
    std::vector<WaitItem> last_items;  ///< 最近一次等待项
    int result{1};                     ///< 预设返回值
    std::string error_text;            ///< 返回失败时写入的错误文本
};

/**
 * @brief 恢复真实 EventPoller，避免用例之间互相污染
 */
class EventPollerInjectionGuard {
public:
    /// @brief 用例开始时确保处于"未注入"状态
    EventPollerInjectionGuard() { SetEventPollerForTesting(nullptr); }

    /// @brief 用例结束时（含断言失败）恢复"未注入"状态
    ~EventPollerInjectionGuard() { SetEventPollerForTesting(nullptr); }

    EventPollerInjectionGuard(const EventPollerInjectionGuard&) = delete;
    EventPollerInjectionGuard& operator=(const EventPollerInjectionGuard&) =
        delete;
};

// ---------------------------------------------------------------------------
// 基础契约
// ---------------------------------------------------------------------------

/**
 * @brief 空等待项集合立即返回超时，且结果表保持为空
 *
 * @details 调用方（如接收循环在句柄全部失效时）不应看到"就绪"，
 *          返回 0 让它走"回到 while 重新检查关闭标记"的分支。
 */
TEST(EventPollerTest, EmptyItemSetReturnsTimeout) {
    std::vector<WaitStatus> status(1);  // 预置非空，验证会被清空
    const int ready = WaitAnySocket({}, 50, status);
    EXPECT_EQ(ready, 0);
    EXPECT_TRUE(status.empty());
}

/**
 * @brief 错误事件也算就绪，以便调用方继续通过 I/O 判定故障。
 */
TEST(EventPollerTest, FailedStatusCountsAsReady) {
    WaitStatus status{};
    EXPECT_FALSE(status.Ready());
    status.failed = true;
    EXPECT_TRUE(status.Ready());
}

/**
 * @brief 直接调用真实平台实现时空集合立即超时且不改写错误文本。
 * @details 不依赖 SocketOps 或测试注入，可单独验证平台源文件的链接与契约。
 */
TEST(EventPollerTest, RealPollerKeepsEmptyWaitContract) {
    std::vector<WaitStatus> status(1);
    std::string error = "unchanged";
    EXPECT_EQ(RealEventPoller().Wait({}, 0, status, &error), 0);
    EXPECT_TRUE(status.empty());
    EXPECT_EQ(error, "unchanged");
}

/**
 * @brief 当前实现跟随注入指针，撤销后重新返回真实平台实现。
 */
TEST(EventPollerTest, InjectionRestoresPlatformImplementation) {
    const EventPollerInjectionGuard injection_guard;
    EventPoller* const real = &RealEventPoller();
    EXPECT_EQ(&CurrentEventPoller(), real);
    RecordingPoller recorder;
    SetEventPollerForTesting(&recorder);
    EXPECT_EQ(&CurrentEventPoller(), &recorder);
    SetEventPollerForTesting(nullptr);
    EXPECT_EQ(&CurrentEventPoller(), real);
}

/**
 * @brief Wait 保留错误文本注入，同时 WaitAnySocket 保留只转发三态的语义。
 */
TEST(EventPollerTest, InjectedErrorTextAndWaitAnyFailure) {
    const EventPollerInjectionGuard injection_guard;
    RecordingPoller recorder;
    recorder.result = -1;
    recorder.error_text = "scripted poll failure";
    SetEventPollerForTesting(&recorder);
    const std::array<WaitItem, 1> items{
        WaitItem{kInvalidSocketHandle, true, false}};
    std::vector<WaitStatus> status;
    std::string error;
    EXPECT_EQ(CurrentEventPoller().Wait(items, 11, status, &error), -1);
    EXPECT_EQ(error, "scripted poll failure");
    ASSERT_EQ(status.size(), items.size());
    EXPECT_FALSE(status[0].Ready());
    EXPECT_EQ(recorder.last_timeout_ms, 11U);
    EXPECT_EQ(WaitAnySocket(items, 13, status), -1);
    EXPECT_EQ(recorder.call_count, 2);
    EXPECT_EQ(recorder.last_timeout_ms, 13U);
}

/**
 * @brief 静默 Socket 上等待可读会超时并返回 0
 */
TEST(EventPollerTest, QuietSocketTimesOut) {
    const SocketHandle handle = MakeLoopbackUdp(nullptr);
    ASSERT_NE(handle, kInvalidSocketHandle) << "无法创建回环 UDP Socket";
    const SocketGuard guard(handle);

    std::vector<WaitStatus> status;
    const int ready = WaitAnySocket(
        std::array<WaitItem, 1>{WaitItem{handle, true, false}}, 50, status);
    EXPECT_EQ(ready, 0);
    ASSERT_EQ(status.size(), 1U);
    EXPECT_FALSE(status[0].Ready());
}

// ---------------------------------------------------------------------------
// 多句柄等待（改造前 select 的能力，之前只有间接覆盖）
// ---------------------------------------------------------------------------

/**
 * @brief 两个句柄同时等待时，只报告真正就绪的那一个
 *
 * @details 这是 `select` → `poll` 迁移后最需要固定下来的语义：
 *          结果表的下标必须与入参一一对应，否则接收循环会把唤醒 Socket
 *          的字节当成数据包，或反之。
 */
TEST(EventPollerTest, ReportsOnlyTheSignalledSocket) {
    Ipv4Endpoint first_addr{};
    Ipv4Endpoint second_addr{};
    const SocketHandle first = MakeLoopbackUdp(&first_addr);
    ASSERT_NE(first, kInvalidSocketHandle) << "无法创建第一个回环 UDP Socket";
    const SocketGuard first_guard(first);
    const SocketHandle second = MakeLoopbackUdp(&second_addr);
    ASSERT_NE(second, kInvalidSocketHandle) << "无法创建第二个回环 UDP Socket";
    const SocketGuard second_guard(second);

    // 用第三个 Socket 只向 second 写 1 个字节，制造"仅第二项可读"
    const SocketHandle writer = MakeLoopbackUdp(nullptr);
    ASSERT_NE(writer, kInvalidSocketHandle) << "无法创建回环 UDP 写端";
    const SocketGuard writer_guard(writer);
    calmcar::xcp::detail::SocketOps& ops = RealSocketOps();
    ASSERT_EQ(ops.connect(writer, second_addr), 0)
        << "UDP connect 失败: " << ops.last_error();
    const std::uint8_t token = 0x5A;
    ASSERT_EQ(ops.send(writer, &token, 1, true), 1)
        << "UDP 唤醒字节发送失败: " << ops.last_error();

    std::array<WaitItem, 2> items{
        WaitItem{first, true, false},
        WaitItem{second, true, false},
    };
    std::vector<WaitStatus> status;
    const int ready = WaitAnySocket(items, 1000, status);
    ASSERT_EQ(ready, 1);
    ASSERT_EQ(status.size(), 2U);
    EXPECT_FALSE(status[0].Ready()) << "第一个 Socket 不应就绪";
    EXPECT_TRUE(status[1].readable) << "第二个 Socket 应报告可读";
    EXPECT_EQ(ReadyCount(status), 1U);
}

/**
 * @brief 可写等待在空闲 UDP Socket 上立即就绪
 *
 * @details 覆盖 `WaitItem::writable` 分支——TCP 发送路径依赖它，
 *          而改造前该分支走的是另一个 `select` 写集。
 */
TEST(EventPollerTest, ReportsWritableForIdleUdpSocket) {
    const SocketHandle handle = MakeLoopbackUdp(nullptr);
    ASSERT_NE(handle, kInvalidSocketHandle) << "无法创建回环 UDP Socket";
    const SocketGuard guard(handle);

    std::vector<WaitStatus> status;
    const int ready = WaitAnySocket(
        std::array<WaitItem, 1>{WaitItem{handle, false, true}}, 1000, status);
    ASSERT_EQ(ready, 1);
    ASSERT_EQ(status.size(), 1U);
    EXPECT_TRUE(status[0].writable);
}

// ---------------------------------------------------------------------------
// 注入接缝
// ---------------------------------------------------------------------------

/**
 * @brief `SocketOps::wait_readable/wait_writable` 把等待委托给注入的
 * EventPoller
 *
 * @details 这是平台等待原语"只剩一处实现"的验收：只要往
 *          `SetEventPollerForTesting` 里塞入实现，SocketOps 的两条等待
 *          路径就必须改走它。
 */
TEST(EventPollerTest, SocketOpsWaitsDelegateToInjectedPoller) {
    const EventPollerInjectionGuard injection_guard;
    RecordingPoller recorder;
    SetEventPollerForTesting(&recorder);

    calmcar::xcp::detail::SocketOps& ops = RealSocketOps();
    const SocketHandle handle = MakeLoopbackUdp(nullptr);
    ASSERT_NE(handle, kInvalidSocketHandle) << "无法创建回环 UDP Socket";
    const SocketGuard guard(handle);

    EXPECT_EQ(ops.wait_readable(handle, 123U), 1);
    ASSERT_EQ(recorder.call_count, 1);
    EXPECT_EQ(recorder.last_timeout_ms, 123U);
    ASSERT_EQ(recorder.last_items.size(), 1U);
    EXPECT_EQ(recorder.last_items[0].handle, handle);
    EXPECT_TRUE(recorder.last_items[0].readable);
    EXPECT_FALSE(recorder.last_items[0].writable);

    EXPECT_EQ(ops.wait_writable(handle, 77U), 1);
    ASSERT_EQ(recorder.call_count, 2);
    EXPECT_EQ(recorder.last_timeout_ms, 77U);
    ASSERT_EQ(recorder.last_items.size(), 1U);
    EXPECT_FALSE(recorder.last_items[0].readable);
    EXPECT_TRUE(recorder.last_items[0].writable);
}

}  // namespace
