/**
 * @file xcplite_slave_fixture.cpp
 * @brief XCPlite Slave 子进程管理与 A2L 轻量扫描的实现。
 */

#include "xcplite_slave_fixture.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "libxcp/udp_transport.hpp"

#include "xcp_test_slave/xcplite_test_types.hpp"

/// @brief Slave 可执行文件路径（由 CMake 注入，非硬编码）
#ifndef XCPLITE_SLAVE_EXECUTABLE
#error "XCPLITE_SLAVE_EXECUTABLE 缺失（tests/CMakeLists.txt 注入）"
#endif

namespace calmcar::xcp::test {
namespace {

/// @brief 单个端口的就绪探测总超时（毫秒）
constexpr std::uint32_t kProbeTimeoutMs = 4000;

/// @brief 两轮探测之间的间隔（毫秒）
constexpr std::uint32_t kProbeIntervalMs = 150;

/// @brief 端口被占用时的最大顺延重试次数
constexpr int kMaxPortAttempts = 8;

/// @brief 进程级端口分配游标（同一测试二进制内递增，避免并发用例互踩）
std::atomic<std::uint32_t> g_port_cursor{0};

/**
 * @brief 切分一行文本为空白分隔的 token（A2L 轻量扫描用）
 * @param line 输入行
 * @return token 列表
 */
std::vector<std::string> Tokenize(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> tokens;
    std::string tok;
    while (in >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

/// @brief 当前进程的 pid（用于端口基数，避免并发 ctest 进程撞端口）
std::uint32_t CurrentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// XcpliteSlaveProcess::Impl —— 平台相关部分
// ---------------------------------------------------------------------------

/**
 * @brief Slave 进程控制器的实现
 */
struct XcpliteSlaveProcess::Impl {
    std::uint16_t port{0};             ///< 实际使用的端口
    std::filesystem::path work_dir;    ///< Slave 工作目录
    std::optional<std::uint16_t> first_candidate_port;  ///< 测试指定首选端口
    std::uint32_t first_attempt_startup_delay_ms{0};    ///< 首次启动测试延迟
    std::atomic<bool> started{false};  ///< 是否已启动（Stop 幂等判定）

#ifdef _WIN32
    HANDLE process_handle{nullptr};  ///< 子进程句柄
    HANDLE thread_handle{nullptr};   ///< 子进程主线程句柄

    /// @brief 进程是否仍在运行（GetExitCodeProcess 判定）
    [[nodiscard]] bool Alive() const {
        if (process_handle == nullptr) {
            return false;
        }
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_handle, &exit_code)) {
            return false;
        }
        return exit_code == STILL_ACTIVE;
    }

    /// @brief 强制终止进程并关闭句柄
    void Kill() {
        if (process_handle != nullptr) {
            TerminateProcess(process_handle, 0);
            WaitForSingleObject(process_handle, 2000);
            CloseHandle(process_handle);
            process_handle = nullptr;
        }
        if (thread_handle != nullptr) {
            CloseHandle(thread_handle);
            thread_handle = nullptr;
        }
    }
#else
    pid_t pid{-1};       ///< 子进程 pid；-1 = 未启动或已回收
    bool exited{false};  ///< 已回收（waitpid 成功）标记

    /// @brief 进程是否仍在运行（WNOHANG 收割，退出状态记录到 exited）
    bool Alive() {
        if (pid <= 0 || exited) {
            return false;
        }
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            exited = true;
            return false;
        }
        return true;
    }

    /// @brief SIGTERM 优雅退出，超时后 SIGKILL，最后回收
    void Kill() {
        if (pid > 0 && !exited) {
            kill(pid, SIGTERM);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            int status = 0;
            while (std::chrono::steady_clock::now() < deadline) {
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    exited = true;
                    pid = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        pid = -1;
        exited = true;
    }
#endif

    /**
     * @brief 在指定端口启动 Slave 子进程（工作目录 = dir）
     * @return 成功 true；进程无法创建 false
     */
    bool Spawn(std::uint16_t use_port, const std::filesystem::path& dir,
               const std::string& ready_token,
               std::uint32_t startup_delay_ms) {
        port = use_port;
        work_dir = dir;
#ifdef _WIN32
        // 命令行：<exe> <port> <delay-ms> <token>（路径含空格时加引号）
        std::string cmdline = std::string("\"") + XCPLITE_SLAVE_EXECUTABLE +
                              "\" " + std::to_string(use_port) + " " +
                              std::to_string(startup_delay_ms) + " " +
                              ready_token;
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // bInheritHandles=TRUE：Slave 的输出直接进入测试控制台的继承句柄
        const BOOL ok =
            CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0,
                           nullptr, dir.string().c_str(), &si, &pi);
        if (!ok) {
            return false;
        }
        process_handle = pi.hProcess;
        thread_handle = pi.hThread;
        return true;
#else
        pid = fork();
        if (pid < 0) {
            return false;
        }
        if (pid == 0) {
            // 子进程：切到工作目录后 exec Slave（A2L/AML 落盘位置 = 工作目录）
            if (chdir(dir.string().c_str()) != 0) {
                _exit(127);
            }
            const std::string port_arg = std::to_string(use_port);
            const std::string delay_arg = std::to_string(startup_delay_ms);
            execl(XCPLITE_SLAVE_EXECUTABLE, "xcp_test_slave", port_arg.c_str(),
                  delay_arg.c_str(), ready_token.c_str(),
                  static_cast<char*>(nullptr));
            _exit(126);  // exec 失败
        }
        return true;
#endif
    }
};

// ---------------------------------------------------------------------------
// XcpliteSlaveProcess
// ---------------------------------------------------------------------------

XcpliteSlaveProcess::XcpliteSlaveProcess(
    std::optional<std::uint16_t> first_candidate_port,
    std::uint32_t first_attempt_startup_delay_ms)
    : impl_(std::make_unique<Impl>()) {
    impl_->first_candidate_port = first_candidate_port;
    impl_->first_attempt_startup_delay_ms = first_attempt_startup_delay_ms;
}

XcpliteSlaveProcess::~XcpliteSlaveProcess() { Stop(); }

namespace {

/**
 * @brief 对 Slave 端口做一次 CONNECT+DISCONNECT 探测
 * @return 会话可建立 true；任何失败 false（异常吞掉即为未就绪）
 */
bool ProbeOnce(std::uint16_t port) {
    UdpTransportConfig cfg;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = port;
    cfg.local_host = "127.0.0.1";
    cfg.local_port = 0;
    cfg.receive_poll_interval_ms = 20;
    try {
        auto master = std::make_unique<XcpMaster>(
            std::make_unique<UdpTransport>(cfg),
            CommandTimeouts{std::chrono::milliseconds(300),
                            std::chrono::milliseconds(300), 0});
        master->Connect();
        master->Disconnect();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// @brief 确认就绪标记由本次启动的子进程生成
bool HasReadyMarker(const std::filesystem::path& dir,
                    const std::string& expected_token) {
    std::ifstream marker(dir / kXcpliteSlaveReadyMarkerFile, std::ios::binary);
    std::string actual_token;
    return marker && std::getline(marker, actual_token) &&
           actual_token == expected_token;
}

}  // namespace

/// 21-3 陈旧运行目录修剪：删除 root 下超过一小时未动的 run_* 目录。
/// 正常路径由 Stop() 即时回收，本函数兜底崩溃/强杀残留；按 mtime 判定，
/// 并发 ctest（-j）下在跑目录必然新于阈值，不会被误删。
void PruneStaleRunDirs(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) {
        return;
    }
    const auto cutoff =
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("run_", 0U) != 0U) {
            continue;
        }
        std::error_code tec;
        const auto stamp = it->last_write_time(tec);
        if (!tec && stamp < cutoff) {
            std::filesystem::remove_all(it->path(), ec);  // 尽力而为
            ec.clear();
        }
    }
}

void XcpliteSlaveProcess::Start() {
    if (impl_->started.load()) {
        return;  // 幂等
    }
    // 普通路径混入 pid 与游标分配端口；测试可注入首选端口以确定性制造冲突。
    const std::uint32_t allocation_id = g_port_cursor.fetch_add(1);
    const std::uint32_t base = impl_->first_candidate_port.has_value()
                                   ? *impl_->first_candidate_port
                                   : 45000U + (CurrentProcessId() % 3000U) +
                                         allocation_id * 8U;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string ready_token = std::to_string(CurrentProcessId()) + "-" +
                                    std::to_string(allocation_id) + "-" +
                                    std::to_string(nonce);

    constexpr std::uint32_t kLastAttemptOffset =
        static_cast<std::uint32_t>(kMaxPortAttempts - 1) * 4U;
    if (base + kLastAttemptOffset > 65535U) {
        throw std::runtime_error(
            "XcpliteSlaveProcess::Start 首选端口范围超出 UDP 端口上限");
    }

    std::string last_error;
    // 21-3：顺手回收崩溃/强杀残留的陈旧 run 目录（mtime
    // 一小时阈值，在跑目录不受影响）
    std::error_code pec;
    PruneStaleRunDirs(std::filesystem::current_path(pec) / "xcplite_runs");
    for (int attempt = 0; attempt < kMaxPortAttempts; ++attempt) {
        const auto port = static_cast<std::uint16_t>(base + attempt * 4U);
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::current_path(ec) /
                                          "xcplite_runs" /
                                          ("run_" + std::to_string(port));
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            last_error = "无法创建 Slave 工作目录: " + ec.message();
            break;  // 环境问题换端口也无济于事
        }
        const std::uint32_t startup_delay_ms =
            attempt == 0 ? impl_->first_attempt_startup_delay_ms : 0U;
        if (!impl_->Spawn(port, dir, ready_token, startup_delay_ms)) {
            last_error =
                "无法启动 Slave 进程: " + std::string(XCPLITE_SLAVE_EXECUTABLE);
            break;  // 可执行文件问题换端口无意义
        }
        // 就绪必须同时满足：进程存活、本次子进程标记已写入、CONNECT 成功。
        // 仅 CONNECT 会误认同端口上已有的其他 XCP 服务。
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kProbeTimeoutMs);
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!impl_->Alive()) {
                break;  // Slave 早退（端口占用/启动失败）
            }
            if (HasReadyMarker(dir, ready_token) && ProbeOnce(port)) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kProbeIntervalMs));
        }
        if (ready) {
            impl_->started.store(true);
            return;
        }
        impl_->Kill();
        last_error =
            "端口 " + std::to_string(port) + " 上的 Slave 未就绪或已退出";
    }
    throw std::runtime_error("XcpliteSlaveProcess::Start 失败: " +
                             (last_error.empty() ? "未知原因" : last_error));
}

void XcpliteSlaveProcess::Stop() {
    if (!impl_->started.load()) {
        // 未成功 Start（例如 Start 抛异常时残留的进程）也要兜底清理
        impl_->Kill();
        return;
    }
    impl_->Kill();
    impl_->started.store(false);
    // 21-3：正常收尾即回收自身 run 目录；异常残留交给下次 Start 的
    // PruneStaleRunDirs 按时效清扫
    std::error_code rmec;
    if (!impl_->work_dir.empty()) {
        std::filesystem::remove_all(impl_->work_dir, rmec);
    }
}

bool XcpliteSlaveProcess::IsRunning() const {
    return impl_->started.load() && const_cast<Impl*>(impl_.get())->Alive();
}

std::uint16_t XcpliteSlaveProcess::Port() const { return impl_->port; }

const std::filesystem::path& XcpliteSlaveProcess::WorkDir() const {
    return impl_->work_dir;
}

std::filesystem::path XcpliteSlaveProcess::A2lPath() const {
    std::filesystem::path path =
        impl_->work_dir / (std::string(kXcpliteSlaveProject) + ".a2l");
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Slave A2L 未生成: " + path.string() +
                                 "（请确认 xcp_test_slave 正常启动）");
    }
    return path;
}

// ---------------------------------------------------------------------------
// A2L 轻量扫描
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief 安全解析 A2L 数值 token（0x 十六进制或十进制；含注释残片等其他
 *        token 时返回 nullopt 而不是抛异常）
 * @param token 待解析 token
 * @return 解析结果
 */
std::optional<std::uint32_t> ParseA2lNumber(const std::string& token) {
    try {
        std::size_t pos = 0;
        const unsigned long value = std::stoul(token, &pos, 0);
        if (pos == token.size()) {
            return static_cast<std::uint32_t>(value);
        }
    } catch (const std::exception&) {
        // 非数值 token：视为解析失败
    }
    return std::nullopt;
}

}  // namespace

std::optional<XcpliteSymbolInfo> ResolveA2lSymbol(
    const std::filesystem::path& a2l_path, const std::string& symbol) {
    std::ifstream in(a2l_path);
    if (!in) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> tokens = Tokenize(line);
        if (tokens.size() < 4 || tokens[0] != "/begin") {
            continue;
        }
        // 带 ECU 地址的三类对象行（XCPlite：全局标量注册为 MEASUREMENT，
        // 全局数组注册为 CHARACTERISTIC VAL_BLK，结构体注册为 INSTANCE）
        if ((tokens[1] != "MEASUREMENT" && tokens[1] != "INSTANCE" &&
             tokens[1] != "CHARACTERISTIC") ||
            tokens[2] != symbol) {
            continue;
        }
        // 定位扩展关键字（缺失时按扩展 0：SEG 模式 ext=0 会被省略打印）
        std::size_t ext_idx = tokens.size();
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i] == "ECU_ADDRESS_EXTENSION") {
                ext_idx = i;
                break;
            }
        }
        // 地址 token 按块类型区分（对 Slave 生成格式的实测核证；注释文本
        // 含空格会使固定下标失准，因此全部按关键字查找）：
        //  MEASUREMENT     → "ECU_ADDRESS" 关键字后的 token
        //  CHARACTERISTIC  → 块类型关键字（VAL_BLK/VALUE/CURVE/MAP/ASCII）后
        //  INSTANCE        → ECU_ADDRESS_EXTENSION 前一个 token
        std::optional<std::uint32_t> address;
        if (tokens[1] == "MEASUREMENT") {
            for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
                if (tokens[i] == "ECU_ADDRESS") {
                    address = ParseA2lNumber(tokens[i + 1]);
                    break;
                }
            }
        } else if (tokens[1] == "CHARACTERISTIC") {
            static constexpr const char* const kBlockKeywords[] = {
                "VAL_BLK", "VALUE", "CURVE", "MAP", "ASCII"};
            for (std::size_t i = 3; i + 1 < tokens.size(); ++i) {
                bool hit = false;
                for (const char* kw : kBlockKeywords) {
                    if (tokens[i] == kw) {
                        hit = true;
                        break;
                    }
                }
                if (hit) {
                    address = ParseA2lNumber(tokens[i + 1]);
                    break;
                }
            }
        } else if (ext_idx >= 2 && ext_idx < tokens.size()) {
            // INSTANCE（有扩展关键字）：地址在 ECU_ADDRESS_EXTENSION 前
            address = ParseA2lNumber(tokens[ext_idx - 1]);
        } else {
            // INSTANCE 且无扩展关键字：SEG 寻址 ext=0 被生成器省略
            // （printAddrExt 仅打印 ext>0），地址取 "/end" 前最后一个
            // 0x token（对 Slave 实测行格式，见协议调试实施记录）
            for (std::size_t i = 3; i < tokens.size(); ++i) {
                if (tokens[i] == "/end") {
                    break;
                }
                if (tokens[i].size() > 2 && tokens[i].rfind("0x", 0) == 0) {
                    const auto parsed = ParseA2lNumber(tokens[i]);
                    if (parsed) {
                        address = parsed;
                    }
                }
            }
        }
        if (!address) {
            continue;
        }
        XcpliteSymbolInfo info;
        info.address = *address;
        info.extension = 0U;
        if (ext_idx < tokens.size()) {
            const auto ext = ParseA2lNumber(tokens[ext_idx + 1]);
            if (!ext) {
                continue;  // 扩展 token 异常：整行视为解析失败，继续找
            }
            info.extension = static_cast<std::uint8_t>(*ext);
        }
        return info;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ResolveA2lTypedefSize(
    const std::filesystem::path& a2l_path, const std::string& typedef_name) {
    std::ifstream in(a2l_path);
    if (!in) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> tokens = Tokenize(line);
        if (tokens.size() < 4 || tokens[0] != "/begin" ||
            tokens[1] != "TYPEDEF_STRUCTURE" || tokens[2] != typedef_name) {
            continue;
        }
        // 大小是块头部（"/begin IF_DATA" 之前）的最后一个 0x token
        std::optional<std::uint32_t> size;
        for (std::size_t i = 3; i < tokens.size(); ++i) {
            if (tokens[i] == "/begin") {
                break;  // IF_DATA/XCP 段开始，头部到此为止
            }
            if (tokens[i].size() > 2 && tokens[i].rfind("0x", 0) == 0) {
                const auto parsed = ParseA2lNumber(tokens[i]);
                if (parsed) {
                    size = parsed;
                }
            }
        }
        if (size) {
            return size;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// gtest 夹具
// ---------------------------------------------------------------------------

void XcpliteSlaveTest::SetUp() {
    try {
        slave_.Start();
        // 尽早验证 A2L 已落盘（缺失时报错而不是让每个用例各自失败）
        const std::filesystem::path a2l = slave_.A2lPath();
        ASSERT_TRUE(std::filesystem::exists(a2l))
            << "A2L 不存在: " << a2l.string();
    } catch (const std::exception& e) {
        FAIL() << "XCPlite Slave 启动失败: " << e.what();
    }
}

void XcpliteSlaveTest::TearDown() { slave_.Stop(); }

std::unique_ptr<XcpMaster> XcpliteSlaveTest::MakeMaster(
    IEventListener* listener) const {
    UdpTransportConfig cfg;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = slave_.Port();
    cfg.local_host = "127.0.0.1";
    cfg.local_port = 0;
    cfg.receive_poll_interval_ms = 20;
    // 真实进程对手端：命令超时放宽到 2s（本机回环正常时远低于此）
    return std::make_unique<XcpMaster>(
        std::make_unique<UdpTransport>(cfg),
        CommandTimeouts{std::chrono::milliseconds(2000),
                        std::chrono::milliseconds(2000), 1},
        listener);
}

std::unique_ptr<XcpMaster> XcpliteSlaveTest::MakeConnectedMaster(
    IEventListener* listener) const {
    auto master = MakeMaster(listener);
    master->Connect();
    return master;
}

}  // namespace calmcar::xcp::test
