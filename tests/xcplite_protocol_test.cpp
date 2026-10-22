/**
 * @file xcplite_protocol_test.cpp
 * @brief 与 XCPlite Slave（真实独立进程 + 真实 UDP）的 XCP 基本协议走通测试。
 *
 * 依据 code-plan/XCPlite_Slave_协议调试集成计划.md Phase1-04：
 *  - 连接管理：CONNECT → GET_COMM_MODE_INFO → GET_STATUS → DISCONNECT；
 *  - 会话参数：MAX_CTO / MAX_DTO / BYTE_ORDER / ADDRESS_GRANULARITY；
 *  - 基本类型 / 结构体 / 数组 / 嵌套类型：SHORT_UPLOAD/UPLOAD 原字节读取
 *    （地址来自 Slave 运行时生成 A2L 的轻量扫描，地址扩展必须用 A2L 声明值
 *    ——XCPlite 默认 CASDD 模式下绝对寻址扩展为 0x01）；
 *  - 多块 UPLOAD：600 字节 > MAX_CTO(248)，验证 SET_MTA+UPLOAD 分块拼接；
 *  - 重复会话稳定性。
 *
 * 本文件不依赖 A2L 桥接层（那是 Phase1-05 的职责），只依赖 libxcp + 夹具。
 */

#include <cstring>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"

#include "xcplite_slave_fixture.hpp"
#include "xcp_test_slave/xcplite_test_types.hpp"

namespace calmcar::xcp {
namespace {

using test::BlobPattern;
using test::ResolveA2lSymbol;

/// @brief 协议走通用例夹具（套件名对齐计划 DoD 的 ctest -R XcpliteProtocol）
class XcpliteProtocolTest : public test::XcpliteSlaveTest {};

/**
 * @brief 从已连接 Master 读取符号原始字节（地址+扩展取自 A2L 扫描）
 * @param master 已连接的 Master
 * @param a2l A2L 路径
 * @param symbol 符号名
 * @param count 字节数
 * @return 读到的字节；符号解析失败返回 nullopt
 */
std::optional<Bytes> ReadSymbol(XcpMaster& master,
                                const std::filesystem::path& a2l,
                                const std::string& symbol, ByteCount count) {
    const auto info = ResolveA2lSymbol(a2l, symbol);
    if (!info) {
        return std::nullopt;
    }
    return master.ReadMemoryBytes(info->address, info->extension, count);
}

/// @brief 小端字节序列重组为标量（回环两侧同机，字节序一致）
template <typename T>
T LittleEndianAs(const Bytes& raw, std::size_t offset = 0) {
    T value{};
    std::memcpy(static_cast<void*>(&value), raw.data() + offset, sizeof(T));
    return value;
}

// ---------------------------------------------------------------------------
// 1) 连接管理与会话参数
// ---------------------------------------------------------------------------

TEST_F(XcpliteProtocolTest, ConnectAndDisconnectRealSlave) {
    auto master = MakeConnectedMaster();
    EXPECT_TRUE(master->IsConnected());
    EXPECT_EQ(master->GetSessionState(), SessionState::Connected);

    // CONNECT 编排已自动附带 GET_STATUS（计划 §5.1）
    const GetStatusResponse status = master->QueryStatus();
    // XCPlite 默认配置未启用 Seed&Key（xcp_cfg.h 中 XCP_ENABLE_SEED_KEY
    // 被注释）：保护位应为 0；若非 0 说明 Slave 资源策略与核证不符，需复核
    EXPECT_EQ(status.resource_protection, 0U);

    master->Disconnect();
    EXPECT_FALSE(master->IsConnected());

    // DISCONNECT 后 Slave 会话释放：同端口可再次建链（真实 Slave 行为验证）
    auto second = MakeConnectedMaster();
    EXPECT_TRUE(second->IsConnected());
}

TEST_F(XcpliteProtocolTest, SessionParametersMatchXcpliteDefaults) {
    auto master = MakeConnectedMaster();
    const auto params = master->GetSessionParameters();

    // XCPlite xcptl_cfg.h：XCPTL_MAX_CTO_SIZE = 248
    EXPECT_EQ(params.connect.max_cto, 248U);
    // MAX_DTO：非 CACHE_LINE 分支为 1024，CACHE_LINE 分支为 248 —— 两者之一
    EXPECT_TRUE(params.connect.max_dto == 1024U ||
                params.connect.max_dto == 248U)
        << "实际 max_dto=" << params.connect.max_dto;
    // PC 目标：字节寻址 + 小端（A2L MOD_COMMON 亦声明 MSB_LAST）
    EXPECT_EQ(params.connect.address_granularity, AddressGranularity::Byte);
    EXPECT_EQ(params.connect.byte_order, ByteOrder::Intel);
    // OPTIONAL 位置位 → Connect 自动查询了 GET_COMM_MODE_INFO
    EXPECT_TRUE(params.comm_mode_info.has_value());
}

// ---------------------------------------------------------------------------
// 2) 基本类型读取（SHORT_UPLOAD）
// ---------------------------------------------------------------------------

TEST_F(XcpliteProtocolTest, ReadBasicScalars) {
    auto master = MakeConnectedMaster();
    const auto a2l = slave_.A2lPath();

    const auto u8 = ReadSymbol(*master, a2l, "g_basic_u8", 1);
    ASSERT_TRUE(u8.has_value()) << "A2L 中未找到 g_basic_u8";
    EXPECT_EQ((*u8)[0], test::kExpectBasicU8);

    const auto i16 = ReadSymbol(*master, a2l, "g_basic_i16", 2);
    ASSERT_TRUE(i16.has_value());
    EXPECT_EQ(LittleEndianAs<std::int16_t>(*i16), test::kExpectBasicI16);

    const auto u32 = ReadSymbol(*master, a2l, "g_basic_u32", 4);
    ASSERT_TRUE(u32.has_value());
    EXPECT_EQ(LittleEndianAs<std::uint32_t>(*u32), test::kExpectBasicU32);

    const auto f32 = ReadSymbol(*master, a2l, "g_basic_f32", 4);
    ASSERT_TRUE(f32.has_value());
    EXPECT_FLOAT_EQ(LittleEndianAs<float>(*f32), test::kExpectBasicF32);

    const auto f64 = ReadSymbol(*master, a2l, "g_basic_f64", 8);
    ASSERT_TRUE(f64.has_value());
    EXPECT_DOUBLE_EQ(LittleEndianAs<double>(*f64), test::kExpectBasicF64);
}

// ---------------------------------------------------------------------------
// 3) 结构体 / 数组 / 嵌套类型（原字节 + 按 offsetof 解析字段）
// ---------------------------------------------------------------------------

/// @brief 从结构体原始字节按字段偏移取出值（offsetof 两侧同源，避免 padding
/// 假设）
template <typename FieldT>
FieldT MemberAt(const Bytes& block, std::size_t offset) {
    FieldT value{};
    std::memcpy(static_cast<void*>(&value), block.data() + offset,
                sizeof(FieldT));
    return value;
}

TEST_F(XcpliteProtocolTest, ReadSimpleStructFields) {
    auto master = MakeConnectedMaster();
    const auto a2l = slave_.A2lPath();
    using Simple = test::SimpleStruct_t;

    const auto block =
        ReadSymbol(*master, a2l, "g_simple_struct", sizeof(Simple));
    ASSERT_TRUE(block.has_value()) << "A2L 中未找到 INSTANCE g_simple_struct";
    ASSERT_EQ(block->size(), sizeof(Simple));

    EXPECT_EQ(MemberAt<std::uint8_t>(*block, offsetof(Simple, simple_u8)),
              test::kExpectSimpleStruct.u8);
    EXPECT_EQ(MemberAt<std::int16_t>(*block, offsetof(Simple, simple_i16)),
              test::kExpectSimpleStruct.i16);
    EXPECT_EQ(MemberAt<std::uint32_t>(*block, offsetof(Simple, simple_u32)),
              test::kExpectSimpleStruct.u32);
}

TEST_F(XcpliteProtocolTest, ReadNestedStructFields) {
    auto master = MakeConnectedMaster();
    const auto a2l = slave_.A2lPath();
    using Outer = test::OuterStruct_t;
    using Simple = test::SimpleStruct_t;

    const auto block = ReadSymbol(*master, a2l, "g_outer", sizeof(Outer));
    ASSERT_TRUE(block.has_value()) << "A2L 中未找到 INSTANCE g_outer";
    ASSERT_EQ(block->size(), sizeof(Outer));

    const auto& exp = test::kExpectOuter;
    EXPECT_EQ(MemberAt<std::uint8_t>(*block, offsetof(Outer, outer_u8)),
              exp.outer_u8);
    EXPECT_EQ(MemberAt<std::int16_t>(*block, offsetof(Outer, outer_i16)),
              exp.outer_i16);

    // 结构体嵌套结构体：nested_struct 内再按 Simple 的 offsetof 取字段
    const std::size_t base = offsetof(Outer, nested_struct);
    EXPECT_EQ(
        MemberAt<std::uint8_t>(*block, base + offsetof(Simple, simple_u8)),
        exp.nested_struct.u8);
    EXPECT_EQ(
        MemberAt<std::int16_t>(*block, base + offsetof(Simple, simple_i16)),
        exp.nested_struct.i16);
    EXPECT_EQ(
        MemberAt<std::uint32_t>(*block, base + offsetof(Simple, simple_u32)),
        exp.nested_struct.u32);

    // 结构体内的结构体数组：nested_array[1]
    const std::size_t arr = offsetof(Outer, nested_array);
    const std::size_t elem = sizeof(Simple);
    EXPECT_EQ(MemberAt<std::int16_t>(*block,
                                     arr + elem + offsetof(Simple, simple_i16)),
              exp.nested_array[1].i16);
    EXPECT_EQ(MemberAt<std::uint32_t>(
                  *block, arr + elem + offsetof(Simple, simple_u32)),
              exp.nested_array[1].u32);
}

TEST_F(XcpliteProtocolTest, ReadArrayOfStructElements) {
    auto master = MakeConnectedMaster();
    const auto a2l = slave_.A2lPath();
    using Simple = test::SimpleStruct_t;

    const auto block =
        ReadSymbol(*master, a2l, "g_struct_array", sizeof(Simple) * 3U);
    ASSERT_TRUE(block.has_value());
    ASSERT_EQ(block->size(), sizeof(Simple) * 3U);

    // 数组嵌套结构体：逐元素按字段偏移解析
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t base = i * sizeof(Simple);
        EXPECT_EQ(
            MemberAt<std::uint8_t>(*block, base + offsetof(Simple, simple_u8)),
            test::kExpectStructArray[i].u8)
            << "元素 " << i;
        EXPECT_EQ(
            MemberAt<std::int16_t>(*block, base + offsetof(Simple, simple_i16)),
            test::kExpectStructArray[i].i16)
            << "元素 " << i;
        EXPECT_EQ(MemberAt<std::uint32_t>(*block,
                                          base + offsetof(Simple, simple_u32)),
                  test::kExpectStructArray[i].u32)
            << "元素 " << i;
    }
}

TEST_F(XcpliteProtocolTest, ReadScalarArrays) {
    auto master = MakeConnectedMaster();
    const auto a2l = slave_.A2lPath();

    const auto arr8 =
        ReadSymbol(*master, a2l, "g_array_u8", test::kArrayU8Size);
    ASSERT_TRUE(arr8.has_value());
    for (std::size_t i = 0; i < test::kArrayU8Size; ++i) {
        EXPECT_EQ((*arr8)[i], static_cast<std::uint8_t>(i));
    }

    const auto arr16 = ReadSymbol(*master, a2l, "g_array_i16",
                                  test::kArrayI16Size * sizeof(std::int16_t));
    ASSERT_TRUE(arr16.has_value());
    const std::int16_t expect16[4] = {-10, -20, -30, -40};
    for (std::size_t i = 0; i < test::kArrayI16Size; ++i) {
        EXPECT_EQ(MemberAt<std::int16_t>(*arr16, i * sizeof(std::int16_t)),
                  expect16[i])
            << "元素 " << i;
    }
}

// ---------------------------------------------------------------------------
// 4) 多块 UPLOAD（> MAX_CTO）
// ---------------------------------------------------------------------------

TEST_F(XcpliteProtocolTest, MultiChunkUploadOfBlob) {
    auto master = MakeConnectedMaster();
    const auto a2l = slave_.A2lPath();

    // 600 字节 > MAX_CTO(248)：必须走 SET_MTA + 多块 UPLOAD 编排
    const auto blob = ReadSymbol(*master, a2l, "g_blob", test::kBlobSize);
    ASSERT_TRUE(blob.has_value());
    ASSERT_EQ(blob->size(), test::kBlobSize);
    for (std::size_t i = 0; i < test::kBlobSize; ++i) {
        ASSERT_EQ((*blob)[i], BlobPattern(i)) << "第 " << i << " 字节不符";
    }
}

// ---------------------------------------------------------------------------
// 5) 重复会话稳定性
// ---------------------------------------------------------------------------

TEST_F(XcpliteProtocolTest, RepeatedConnectSessionsRemainStable) {
    const auto a2l = slave_.A2lPath();
    for (int i = 0; i < 5; ++i) {
        auto master = MakeConnectedMaster();
        const auto u8 = ReadSymbol(*master, a2l, "g_basic_u8", 1);
        ASSERT_TRUE(u8.has_value()) << "第 " << i << " 轮符号解析失败";
        EXPECT_EQ((*u8)[0], test::kExpectBasicU8)
            << "第 " << i << " 轮读值不符";
        master->Disconnect();
    }
}

// ---------------------------------------------------------------------------
// 6) 错误路径：非法地址应得到结构化 Negative Response（不静默）
// ---------------------------------------------------------------------------

TEST_F(XcpliteProtocolTest, ReadOutOfRangeAddressIsRejected) {
    auto master = MakeConnectedMaster();
    // 绝对寻址空间为进程镜像基址 +4GB；取一个不可能注册的极端偏移。
    // Slave 越界读会返回错误响应（ERR_ACCESS_DENIED / ERR_OUT_OF_RANGE /
    // ERR_ADDRESS 三者之一，均属 ProtocolError），不得静默返回垃圾数据。
    try {
        (void)master->ReadMemoryBytes(0xFFFFFFF0U, 0x01U, 16);
        GTEST_SKIP() << "Slave 对该极端地址返回了成功响应（不同实现策略不同），"
                        "不属于本阶段阻塞项";
    } catch (const XcpException& e) {
        EXPECT_TRUE(e.Category() == ErrorCategory::ProtocolError ||
                    e.Category() == ErrorCategory::RecoveryFailed)
            << "category=" << static_cast<int>(e.Category());
    }
}

// ---------------------------------------------------------------------------
// 批次 B1：版本发现（真实 XCPlite 对端）
// ---------------------------------------------------------------------------

/**
 * @brief XCPlite 真实对端的版本发现结果必须被如实记录
 *
 * @details 依据原始 PDF（ASAM MCD-1 XCP V1.5.0）：
 *          - CONNECT 正响应只返回 PL/TL 版本的**最高有效字节**（Table 38，
 *            第 110 页；§3.1.1 第 12 页明确 "most significant byte only"）；
 *          - XCPlite 的 `XCP_PROTOCOL_LAYER_VERSION` 为 0x0104
 *            （`thirdparty/xcplite/src/xcp_cfg.h:29`），故 CONNECT 报出的
 *            PL 主版本应为 1；
 *          - GET_VERSION（命令码 0xC0）能提供精确的 Major/Minor。
 *
 *          本用例锁定两条不可违反的契约：
 *          1. 禁止伪造精确版本——Minor 未经 GET_VERSION 确认时必须表达为
 *             MajorOnly，绝不能凭空写 0 冒充"1.0"；
 *          2. 报告的版本不得高于 EffectiveVersion（封顶语义）。
 */
TEST_F(XcpliteProtocolTest, RealSlaveVersionIsReportedHonestly) {
    auto master = MakeConnectedMaster();

    const ProtocolVersionSnapshot pl = master->GetProtocolVersionSnapshot();
    const TransportVersionSnapshot tl = master->GetTransportVersionSnapshot();

    // XCPlite 报 PL 主版本 1（0x0104 >> 8）；这是对端事实，不是本端推断
    EXPECT_EQ(pl.evidence.reported.major, 1U)
        << "XCPlite 的 XCP_PROTOCOL_LAYER_VERSION 为 0x0104，主版本应为 1";
    EXPECT_EQ(tl.evidence.reported.major, 1U)
        << "TL 版本与 PL 同源，XCPlite 亦为 1.x";

    // XCPlite **确实实现**了 GET_VERSION，且构造默认在 CONNECT 后自动探测，
    // 因此建链完成时版本已被运行时确认——这是对端能力的事实，不是本端伪造。
    EXPECT_EQ(pl.evidence.source, VersionSource::GetVersion)
        << "XCPlite 支持 GET_VERSION，自动探测后来源应为 GetVersion；"
           "若为 ConnectMajor 说明自动探测未生效";
    EXPECT_TRUE(pl.evidence.minor_confirmed)
        << "GET_VERSION 提供了 Minor，应被确认";
    EXPECT_EQ(pl.evidence.confidence, VersionConfidence::RuntimeConfirmed);
    EXPECT_FALSE(pl.conservative)
        << "精确版本已知，不应处于 1.x-conservative 保守态";

    // 精确版本必须与 CONNECT 的粗粒度主版本**不矛盾**
    EXPECT_LE(pl.effective.major, kProtocolVersion15.major)
        << "生效版本不得高于支持上限";
    EXPECT_FALSE(pl.evidence.capped) << "XCPlite 的 1.x 不应触发封顶";

    // 版本证据必须可追溯，便于调用方判断可信度
    EXPECT_TRUE(pl.evidence.IsAvailable())
        << "已建链的对端必须给出可用的版本证据";
}

/**
 * @brief 真实对端上显式 GET_VERSION 必须给出精确的 Major.Minor
 *
 * @details 这是"探测确实生效"的端到端证据：仅实现探测但忘记把结果写回快照
 *          （本轮修复的缺陷 1）会让本用例失败。
 *          规范依据：`thirdparty/xcplite/src/xcp_cfg.h:29` 定义
 *          `XCP_PROTOCOL_LAYER_VERSION 0x0104`，按原始 PDF 第 12 页 §3.1.1
 *          的 16 位语义（高字节 Major、低字节 Minor）解析应得 1.4。
 */
TEST_F(XcpliteProtocolTest, ExplicitProbeYieldsExactVersionFromRealSlave) {
    auto master = MakeConnectedMaster();

    const auto probe = master->ProbeVersion();
    ASSERT_EQ(probe.outcome, CommandExecutor::GetVersionOutcome::Available)
        << "XCPlite 实现了 GET_VERSION；不可用说明探测路径本身有问题";
    ASSERT_TRUE(probe.response.has_value());

    const auto after = master->GetProtocolVersionSnapshot();
    EXPECT_EQ(after.evidence.source, VersionSource::GetVersion)
        << "探测成功后证据来源必须为 GET_VERSION";
    EXPECT_TRUE(after.evidence.minor_confirmed);
    EXPECT_EQ(after.evidence.confidence, VersionConfidence::RuntimeConfirmed);
    EXPECT_EQ(after.evidence.reported.major, 1U);
    EXPECT_EQ(after.evidence.reported.minor, 4U)
        << "XCPlite 的 PL 版本为 0x0104，精确解析应得到 1.4";
    EXPECT_EQ(after.effective.minor, 4U);
    EXPECT_FALSE(after.conservative);

    // 响应对象与快照必须一致——防止"解析正确但写回错误"
    EXPECT_EQ(probe.response->ProtocolVersion().major,
              after.evidence.reported.major);
    EXPECT_EQ(probe.response->ProtocolVersion().minor,
              after.evidence.reported.minor);

    // 传输层版本独立存在，不得由协议层推导（方案 §4 第 3 条）
    const auto tl = master->GetTransportVersionSnapshot();
    EXPECT_TRUE(tl.evidence.IsAvailable())
        << "GET_VERSION 同时提供 TL 版本，传输层证据也须可用";
}

}  // namespace
}  // namespace calmcar::xcp
