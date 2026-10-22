/**
 * @file logging_probe.cpp
 * @brief 用于验证 glog 文件输出和普通终端输出隔离的独立测试程序。
 */

#include <cstdio>
#include <stdexcept>
#include <string>

#include "libxcp/command_executor.hpp"
#include "libxcp/ixcp_transport.hpp"
#include "libxcp/log.hpp"
#include "libxcp/memory_access.hpp"
#include "libxcp/session.hpp"
#include "program_logging.hpp"

/**
 * @brief 同步返回测试响应的内存传输层。
 */
class ProbeTransport final : public calmcar::xcp::IXcpTransport {
public:
    /// @brief 绑定包监听器并标记传输层已打开。
    void Open(calmcar::xcp::IPacketListener& listener) override {
        listener_ = &listener;
        open_ = true;
    }

    /// @brief 关闭内存传输层并解除监听器引用。
    void Close() override {
        open_ = false;
        listener_ = nullptr;
    }

    /// @brief 根据命令种类回送测试用 RES 包。
    /// @param packet 完整的待发送 XCP 命令包。
    void Send(calmcar::xcp::BytesView packet) override {
        if (!open_ || listener_ == nullptr || packet.empty()) {
            throw std::runtime_error("测试传输层未打开或命令为空");
        }
        using calmcar::xcp::CommandCode;
        using calmcar::xcp::PacketType;
        calmcar::xcp::Bytes response{
            static_cast<std::uint8_t>(PacketType::Res)};
        if (packet.front() == static_cast<std::uint8_t>(CommandCode::GetSeed)) {
            // 异步事件/服务先于最终响应，不得占用单命令响应槽位。
            const calmcar::xcp::Bytes event{
                static_cast<std::uint8_t>(PacketType::Ev),
                static_cast<std::uint8_t>(calmcar::xcp::EventCode::CmdPending)};
            listener_->OnPacketReceived(event);
            const calmcar::xcp::Bytes service{
                static_cast<std::uint8_t>(PacketType::Serv), 0x01U};
            listener_->OnPacketReceived(service);
            response.insert(response.end(), {2U, 0xA5U, 0x5AU});
        } else if (packet.front() ==
                   static_cast<std::uint8_t>(CommandCode::Unlock)) {
            response.push_back(0U);
        }
        listener_->OnPacketReceived(response);
    }

    /// @brief 返回内存传输层当前打开状态。
    /// @return 传输层是否已打开。
    [[nodiscard]] bool IsOpen() const noexcept override { return open_; }

private:
    calmcar::xcp::IPacketListener* listener_{nullptr};  ///< 当前包监听器。
    bool open_{false};                                  ///< 传输层打开状态。
};

/**
 * @brief 初始化文件日志并发出三种日志级别，供 CTest 检查文件和标准流。
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组。
 * @return 日志初始化失败时返回 2，成功时返回 0。
 */
int main(int argc, char** argv) {
    (void)argc;
    calmcar::xcp::internal::ProgramLogSession logging;
    std::string error_message;
    if (!logging.Initialize(argv[0], "tests", error_message)) {
        std::fprintf(stderr, "日志初始化失败: %s\n", error_message.c_str());
        return 2;
    }

    LIBXCP_LOG_INFO("probe-info-{}", 17);
    LIBXCP_LOG_WARNING("probe-warning-{}", 23);
    LIBXCP_LOG_ERROR("probe-error-{}", 42);

    try {
        calmcar::xcp::Session session;
        session.BeginConnecting();
        calmcar::xcp::ConnectResponse connect_response{};
        connect_response.resource_mask =
            static_cast<std::uint8_t>(calmcar::xcp::Resource::CalPag);
        connect_response.max_cto = 8U;
        connect_response.max_dto = 8U;
        session.EstablishConnection(connect_response);

        ProbeTransport transport;
        calmcar::xcp::CommandExecutor executor(transport, session);
        transport.Open(executor.AsListener());
        const auto seed = executor.ExecuteGetSeed(
            calmcar::xcp::Resource::CalPag, calmcar::xcp::SeedMode::First);
        const calmcar::xcp::Bytes expected_seed{0xA5U, 0x5AU};
        if (seed.seed != expected_seed) {
            return 3;
        }
        const calmcar::xcp::Bytes key{0xC1U, 0x5EU};
        (void)executor.ExecuteUnlock(2U, key, true);

        // 通过异步 DTO 检查 64 字节摘要上限和“不伪造命令上下文”。
        calmcar::xcp::Bytes dto_packet(70U, 0xAAU);
        dto_packet.front() = 0x00U;
        executor.OnPacketReceived(dto_packet);

        // 迟到的 Seed RES 到达时，当前 pending 已是无关的 GET_STATUS；
        // 由于协议包没有事务 ID，响应正文仍必须脱敏。
        session.MarkCommandSent(calmcar::xcp::CommandCode::GetStatus);
        const calmcar::xcp::Bytes delayed_seed_response{
            static_cast<std::uint8_t>(calmcar::xcp::PacketType::Res), 0xA5U,
            0x5AU};
        executor.OnPacketReceived(delayed_seed_response);
        session.ClearPendingCommand();
        transport.Close();

        calmcar::xcp::Session disconnected_session;
        calmcar::xcp::MemoryAccess memory(executor, disconnected_session);
        try {
            (void)memory.ReadElements(0U, 0U, 1U);
            return 4;
        } catch (const calmcar::xcp::XcpException&) {
        }
    } catch (...) {
        std::fprintf(stderr, "PROBE_PROTOCOL_FAILED\n");
        return 3;
    }

    std::printf("PROBE_STDOUT\n");
    std::fprintf(stderr, "PROBE_STDERR\n");
    logging.Shutdown();
    return 0;
}
