/**
 * @file main.cpp
 * @brief 基于 XCPlite 的 XCP Slave 测试对手端（独立进程）。
 *
 * 依据 code-plan/XCPlite_Slave_协议调试集成计划.md §1.3 实现：
 *  - 绑定 127.0.0.1:<port> 的 UDP（XCP on Ethernet，Part 3）；
 *  - 运行时生成 A2L（WRITE_ALWAYS 模式 → 固定文件名 <project>.a2l，
 *    每次启动重写，保证 A2L 地址与本次进程一致）；
 *  - 注册基本类型 / 结构体 / 数组 / 嵌套类型测试变量与一个 Calibration
 *    Segment（供第二阶段写回测试）；
 *  - 主循环持续触发 DAQ 事件保持 Slave 活跃；SIGINT/SIGTERM 优雅退出。
 *
 * 关键实现约束（均来自对 XCPlite 源码的实测核证，不改 thirdparty）：
 *  - 默认配置为 CASDD 寻址：绝对寻址的地址扩展是 0x01（不是 0x00），
 *    Master 侧必须使用 A2L 中声明的 ECU_ADDRESS_EXTENSION；
 *  - XCPlite 生成的 A2L 无条件包含 `/include "XCP_104.aml"`，而本程序
 *    不携带 AML 文件 —— 启动时在工作目录写入空壳 AML，保证桥接层的
 *    include 链预扫描可解析；
 *  - IF_DATA XCP 的 TRANSPORT_LAYER 段仅在绑定地址首字节非 0 时写出，
 *    因此必须绑定 127.0.0.1 而不是 0.0.0.0，否则 A2L 无端点信息。
 *
 * 命令行：xcp_test_slave [port [startup_delay_ms [ready_token]]]（缺省端口 5556）。
 */

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "a2l.hpp"     // XCPlite A2L 生成 API（C++ 包装）
#include "xcplib.hpp"  // XCPlite 应用侧 API（C++ RAII 包装）

#include "xcplite_test_types.hpp"

namespace {

using calmcar::xcp::test::BlobPattern;
using namespace calmcar::xcp::test;  // NOLINT：测试类型与常量单一来源

/// @brief 主循环运行标记（信号处理置 false）
std::atomic<bool> g_running{true};

/**
 * @brief SIGINT/SIGTERM 处理：请求主循环退出
 * @param sig 信号编号（未使用）
 */
void OnSignal(int sig) {
    (void)sig;
    g_running.store(false);
}

// ---------------------------------------------------------------------------
// 测试变量定义（全部为绝对寻址的全局变量，地址扩展 0x01）
// ---------------------------------------------------------------------------

std::uint8_t g_basic_u8 = kExpectBasicU8;     ///< 基本类型 u8
std::int16_t g_basic_i16 = kExpectBasicI16;   ///< 基本类型 i16
std::uint32_t g_basic_u32 = kExpectBasicU32;  ///< 基本类型 u32
float g_basic_f32 = kExpectBasicF32;          ///< 基本类型 f32
double g_basic_f64 = kExpectBasicF64;         ///< 基本类型 f64

SimpleStruct_t g_simple_struct = {
    .simple_u8 = kExpectSimpleStruct.u8,
    .simple_i16 = kExpectSimpleStruct.i16,
    .simple_u32 = kExpectSimpleStruct.u32};  ///< 结构体变量

std::uint8_t g_array_u8[kArrayU8Size] = {0, 1, 2, 3, 4, 5, 6, 7};  ///< 数组
std::int16_t g_array_i16[kArrayI16Size] = {-10, -20, -30, -40};    ///< 数组
float g_array_f32[4] = {1.0F, 2.0F, 3.0F, 4.0F};                   ///< 数组

OuterStruct_t g_outer = {
    .outer_u8 = kExpectOuter.outer_u8,
    .outer_i16 = kExpectOuter.outer_i16,
    .nested_struct = {kExpectOuter.nested_struct.u8,
                      kExpectOuter.nested_struct.i16,
                      kExpectOuter.nested_struct.u32},
    .nested_array = {{kExpectOuter.nested_array[0].u8,
                      kExpectOuter.nested_array[0].i16,
                      kExpectOuter.nested_array[0].u32},
                     {kExpectOuter.nested_array[1].u8,
                      kExpectOuter.nested_array[1].i16,
                      kExpectOuter.nested_array[1].u32}},
    .outer_arr = {kExpectOuter.outer_arr[0], kExpectOuter.outer_arr[1],
                  kExpectOuter.outer_arr[2],
                  kExpectOuter.outer_arr[3]}};  ///< 嵌套结构体

SimpleStruct_t g_struct_array[3] = {
    {kExpectStructArray[0].u8, kExpectStructArray[0].i16,
     kExpectStructArray[0].u32},
    {kExpectStructArray[1].u8, kExpectStructArray[1].i16,
     kExpectStructArray[1].u32},
    {kExpectStructArray[2].u8, kExpectStructArray[2].i16,
     kExpectStructArray[2].u32}};  ///< 数组嵌套结构体

std::uint8_t g_blob[kBlobSize] = {};  ///< 大块变量（多块 UPLOAD 用，运行时填充
                                      ///  BlobPattern）

/// @brief Calibration 参数默认页（CalSeg 的 reference page）
const CalParamsT kDefaultCalParams = {.cal_factor = kExpectCalFactor,
                                      .cal_offset = kExpectCalOffset};

/**
 * @brief 在工作目录写入空壳 XCP_104.aml
 * @details XCPlite 生成的 A2L 固定包含 `/include "XCP_104.aml"`；libxcp 桥接层
 *          解析 include 链时要求目标存在。AML 仅是工具注解，协议调试不需要其
 *          内容，写一个只含注释的空壳即可。
 */
void WriteAmlStub() {
    std::FILE* f = std::fopen("XCP_104.aml", "w");
    if (f != nullptr) {
        // 用 A2L 合法的 C 样式注释：include 展开后不得引入非法 token
        std::fputs("/* libxcp 协议调试空壳 AML（供 A2L include 链解析）*/\n",
                   f);
        std::fclose(f);
    }
}

}  // namespace

/**
 * @brief Slave 主入口
 * @param argc 参数个数
 * @param argv argv[1] = 监听端口（十进制，可选，默认 5556）
 * @return 0 正常退出；2 XCP Server 启动失败（端口占用等）；3 A2L 初始化失败；4 就绪标记写入失败
 */
int main(int argc, char** argv) {
    const std::uint16_t port =
        argc > 1
            ? static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10))
            : kXcpliteSlaveDefaultPort;
    const std::uint32_t startup_delay_ms =
        argc > 2 ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10))
                 : 0U;
    const std::string ready_token = argc > 3 ? argv[3] : "";

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    // 日志级别 0：协议调试由 Master 侧断言，Slave 不需要噪声输出
    XcpSetLogLevel(0);

    WriteAmlStub();

    // XCP 单例初始化（LOCAL 模式，无持久化文件依赖，每次启动全新状态）
    XcpInit(kXcpliteSlaveProject, "1.0", XCP_MODE_LOCAL);
    // 首次启动延迟仅供测试复现：端口已有服务时验证就绪标记不会误认该服务。
    if (startup_delay_ms > 0U) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(startup_delay_ms));
    }

    // 绑定 127.0.0.1（首字节非 0 → A2L 才会写出 TRANSPORT_LAYER 端点信息）
    const std::uint8_t addr[4] = {127, 0, 0, 1};
    if (!XcpEthServerInit(addr, port, /*use_tcp=*/false, 1024U * 64U)) {
        std::fprintf(stderr, "xcp_test_slave: server init failed on port %u\n",
                     port);
        return 2;
    }

    // A2L 生成：WRITE_ALWAYS → 固定文件名 <project>.a2l，每次运行重写
    if (!A2lInit(addr, port, /*use_tcp=*/false,
                 A2L_MODE_WRITE_ALWAYS | A2L_MODE_AUTO_GROUPS)) {
        std::fprintf(stderr, "xcp_test_slave: A2L init failed\n");
        return 3;
    }

    // 填充大块变量的确定性模式（放在注册前，保证 CONNECT 后立读即得正确值）
    for (std::size_t i = 0; i < kBlobSize; ++i) {
        g_blob[i] = BlobPattern(i);
    }

    // 测量事件（MSVC 平台无 linker section，SET_ID 回退为运行时
    // XcpCreateEvent 动态创建；此处只建事件，不切地址模式）
    DaqCreateEvent(testev);

    // --- Calibration Segment ---
    // 注意：CalSeg 构造函数内部会调用 A2lSetSegmentAddrMode 把 A2L 生成器的
    // 当前地址模式切到 SEG（段相对）。必须在它之后再显式切回绝对模式，
    // 否则后续所有测量变量都会以段相对地址注册（cpp_demo 同样的顺序约定）。
    auto calseg = CalSegCreate(kDefaultCalParams);
    A2lTypedefBegin(CalParamsT, &kDefaultCalParams,
                    "libxcp test slave calibration parameters");
    A2lTypedefParameterComponent(cal_factor, "Calibration factor", "", 0,
                                 65535);
    A2lTypedefParameterComponent(cal_offset, "Calibration offset", "mm",
                                 -1000.0, 1000.0);
    A2lTypedefEnd();
    // CreateA2lTypedefInstance 内部自带 SEG 模式切换，无需外部设置
    calseg.CreateA2lTypedefInstance("CalParamsT",
                                    "Calibration parameters instance");

    // --- 绝对寻址模式：此后注册的全部为全局变量（CASDD → 地址扩展 0x01）---
    A2lSetAbsoluteAddrMode(testev);

    // --- 结构体 typedef：SimpleStruct_t 与嵌套的 OuterStruct_t ---
    A2lTypedefBegin(SimpleStruct_t, &g_simple_struct,
                    "libxcp test typedef for SimpleStruct_t");
    A2lTypedefMeasurementComponent(simple_u8, "Simple struct byte field");
    A2lTypedefMeasurementComponent(simple_i16, "Simple struct word field");
    A2lTypedefMeasurementComponent(simple_u32, "Simple struct dword field");
    A2lTypedefEnd();

    A2lTypedefBegin(OuterStruct_t, &g_outer,
                    "libxcp test typedef for OuterStruct_t");
    A2lTypedefMeasurementComponent(outer_u8, "Outer byte field");
    A2lTypedefMeasurementComponent(outer_i16, "Outer word field");
    A2lTypedefComponent(nested_struct, SimpleStruct_t, 1);
    A2lTypedefComponent(nested_array, SimpleStruct_t, 2);
    // 成员标量数组（批次16 四形态之"固定数组"；生成 M_outer_arr +
    // STRUCTURE_COMPONENT ... MATRIX_DIM 4）
    A2lTypedefMeasurementArrayComponent(outer_arr, "Outer scalar array");
    A2lTypedefEnd();

    // --- 基本标量测量 ---
    A2lCreateMeasurement(g_basic_u8, "Basic uint8");
    A2lCreateMeasurement(g_basic_i16, "Basic int16");
    A2lCreateMeasurement(g_basic_u32, "Basic uint32");
    A2lCreateMeasurement(g_basic_f32, "Basic float");
    A2lCreateMeasurement(g_basic_f64, "Basic double");

    // --- 数组测量 ---
    A2lCreateMeasurementArray(g_array_u8, "Uint8 array");
    A2lCreateMeasurementArray(g_array_i16, "Int16 array");
    A2lCreateMeasurementArray(g_array_f32, "Float array");
    A2lCreateMeasurementArray(g_blob, "Blob for multi chunk upload");

    // --- 结构体 / 嵌套 / 数组嵌套结构体实例 ---
    A2lCreateTypedefInstance(g_simple_struct, SimpleStruct_t,
                             "Simple struct instance");
    A2lCreateTypedefInstance(g_outer, OuterStruct_t, "Nested struct instance");
    A2lCreateTypedefInstanceArray(g_struct_array, SimpleStruct_t, 3,
                                  "Array of SimpleStruct_t");

    // 显式定稿：A2L 完整落盘后再写子进程就绪标记。
    A2lFinalize();
    // 标记携带本次启动 token，防止父进程把同端口的其他 XCP 服务当成子进程。
    if (!ready_token.empty()) {
        std::ofstream marker(kXcpliteSlaveReadyMarkerFile,
                             std::ios::out | std::ios::trunc | std::ios::binary);
        marker << ready_token << '\n';
        if (!marker) {
            std::fprintf(stderr, "xcp_test_slave: failed to write ready marker\n");
            return 4;
        }
    }
    std::printf("xcp_test_slave ready on 127.0.0.1:%u, a2l=%s\n", port,
                A2lGetFilename());
    std::fflush(stdout);

    // 主循环：持续触发事件保持 Slave 活跃（变量值不修改，保证测试断言确定性）
    while (g_running.load()) {
        DaqTriggerEvent(testev);
        sleepUs(1000);
    }

    XcpDisconnect();
    XcpEthServerShutdown();
    return 0;
}
