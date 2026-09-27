/**
 * @file xcplite_a2l_read_test.cpp
 * @brief A2L 加载 + 变量读取测试：桥接层解析 XCPlite 运行时生成的 A2L，
 *        符号名 → 地址/扩展 → 真实 UDP 读值 → 物理值换算。
 *
 * 依据 code-plan/XCPlite_Slave_协议调试集成计划.md Phase1-05。
 * 仅在 LIBXCP_BUILD_A2L=ON 时编译（LIBXCP_HAS_A2LBRIDGE 由 CMake 注入）。
 *
 * 关键点（对 XCPlite 源码核证的落地）：
 *  - Slave 默认 CASDD 寻址：绝对变量的 ECU_ADDRESS_EXTENSION = 0x01，
 *    桥接层解析出的 extension 必须原样透传给 Master 读路径；
 *  - A2L 的 IF_DATA XCP 只在 Slave 绑定 127.0.0.1（非 ANY）时写出
 *    XCP_ON_UDP_IP 端点，本用例据此验证"A2L 端点自动建链"；
 *  - 结构体成员的叶子路径解析属批次16 STRUCTLEAF（未完成）：本文件对
 *    INSTANCE 只做"元数据可查 + 基址原始字节读"验证，成员级 Find 路径
 *    在批次16完成前标记 SKIP。
 */

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/command_executor.hpp"
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_master.hpp"

#include "xcplite_slave_fixture.hpp"
#include "xcp_test_slave/xcplite_test_types.hpp"

namespace calmcar::xcp {
namespace {

namespace a2l = calmcar::xcp::a2l;

/// @brief A2L 读取用例夹具（套件名对齐计划 DoD 的 ctest -R XcpliteA2lRead）
class XcpliteA2lReadTest : public test::XcpliteSlaveTest {};

/**
 * @brief XCP 版本字节 → A2L PROTOCOL_LAYER 版本（uint16 风格）
 * @details 规范约定（docs/XCP_1.3.0_document.md）：CONNECT RES 的
 *          PROTOCOL_VERSION 单字节按高/低 nibble = major/minor（0x10=1.0，
 *          UdpTestSlave 即此口径）。
 *          XCPlite 偏离（协议调试实测，xcplite.c:2078）：发送
 *          XCP_PROTOCOL_LAYER_VERSION >> 8 = 0x01（仅 major 原值，低 nibble
 *          恒 0 且高 nibble 为 0）——该形态无法用 nibble 规则解释（会得
 *          V0.1），按对手端语义取 byte<<8 = 1.0 参与 major 比对；minor
 *          在 CONNECT 字节中丢失属 Slave 侧偏差，权威值只能经 GET_VERSION
 *          查询（超出本批范围，已记实施记录）。
 */
std::uint16_t VersionByteToA2l(std::uint8_t version_byte) {
    // XCPlite 特例形态（byte < 0x10，如 0x01）：nibble 解读会得到 V0.1，
    // 与对手端实际版本（1.04 → 报 1.0）矛盾；按其编码语义取 major=byte。
    if (version_byte != 0U && (version_byte >> 4) == 0U) {
        return static_cast<std::uint16_t>(version_byte << 8);
    }
    const auto major = static_cast<std::uint16_t>((version_byte >> 4) & 0x0F);
    const auto minor = static_cast<std::uint16_t>(version_byte & 0x0F);
    return static_cast<std::uint16_t>((major << 8) | minor);
}

/// @brief 限定符号名（XCPlite 的 MODULE 名即项目名）
std::string Qualified(const std::string& symbol) {
    return std::string(test::kXcpliteSlaveProject) + "::" + symbol;
}

// ---------------------------------------------------------------------------
// 1) 运行时生成 A2L 的加载
// ---------------------------------------------------------------------------

TEST_F(XcpliteA2lReadTest, LoadRuntimeGeneratedA2l) {
    auto loaded = a2l::A2lBridge::Load(slave_.A2lPath().string());
    ASSERT_TRUE(loaded.HasValue())
        << "Load 失败: " << a2l::ToString(loaded.ErrorInfo().code) << " / "
        << loaded.ErrorInfo().message;
    const auto& bridge = loaded.Value();
    ASSERT_NE(bridge->Database(), nullptr);

    // XCPlite 绑定了 127.0.0.1 → A2L 必须含 XCP_ON_UDP_IP 端点，
    // 且端口就是 Slave 实际监听端口（自动建链的前提）
    const a2l::IfDataXcpInfo* info = bridge->XcpInfo();
    ASSERT_NE(info, nullptr) << "IF_DATA XCP 缺失（require_if_data_xcp 默认 "
                                "true，加载应已失败而非此处空指针）";
    ASSERT_EQ(info->transports.size(), 1u);
    EXPECT_EQ(info->transports[0].kind, a2l::TransportEndpoint::Kind::UdpIp);
    EXPECT_EQ(info->transports[0].remote_port, slave_.Port());
    EXPECT_EQ(info->transports[0].remote_host, "127.0.0.1");
}

// ---------------------------------------------------------------------------
// 2) 符号查找 + 基本类型读值闭环（A2L 端点自动建链）
// ---------------------------------------------------------------------------

TEST_F(XcpliteA2lReadTest, FindAndReadBasicVariables) {
    auto loaded = a2l::A2lBridge::Load(slave_.A2lPath().string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const auto& bridge = loaded.Value();
    const a2l::IA2lDatabase* db = bridge->Database();
    ASSERT_NE(db, nullptr);

    // 用 A2L 声明的端点建链（不经夹具的探测路径，验证 A2L→Transport 直通）
    UdpTransportConfig cfg;
    cfg.remote_host = bridge->XcpInfo()->transports[0].remote_host;
    cfg.remote_port = bridge->XcpInfo()->transports[0].remote_port;
    cfg.local_host = "127.0.0.1";
    cfg.local_port = 0;
    cfg.receive_poll_interval_ms = 20;
    XcpMaster master(std::make_unique<UdpTransport>(cfg),
                     CommandTimeouts{std::chrono::milliseconds(2000),
                                     std::chrono::milliseconds(2000), 1});
    master.Connect();

    // u8：地址/扩展/尺寸来自 A2L；raw 读回后 IDENTICAL 换算
    auto sym = db->Find(Qualified("g_basic_u8"));
    ASSERT_TRUE(sym.HasValue())
        << "Find(g_basic_u8) 失败: " << sym.ErrorInfo().message;
    // CASDD 绝对寻址：扩展必须是 0x01（对 XCPlite 源码的核证结论）
    EXPECT_EQ(sym.Value().address_extension, 0x01);
    const Bytes u8_raw = master.ReadMemoryBytes(sym.Value().xcp_address,
                                                sym.Value().address_extension,
                                                sym.Value().element_size_bytes);
    ASSERT_EQ(u8_raw.size(), 1u);
    EXPECT_EQ(u8_raw[0], test::kExpectBasicU8);
    auto phys = db->ToPhysical(Qualified("g_basic_u8"), u8_raw);
    ASSERT_TRUE(phys.HasValue()) << phys.ErrorInfo().message;
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(phys.Value()));
    EXPECT_EQ(std::get<std::int64_t>(phys.Value()),
              static_cast<std::int64_t>(test::kExpectBasicU8));

    // u32：4 字节小端重组
    auto sym32 = db->Find(Qualified("g_basic_u32"));
    ASSERT_TRUE(sym32.HasValue()) << sym32.ErrorInfo().message;
    const Bytes u32_raw = master.ReadMemoryBytes(
        sym32.Value().xcp_address, sym32.Value().address_extension, 4);
    ASSERT_EQ(u32_raw.size(), 4u);
    const std::uint32_t u32 = static_cast<std::uint32_t>(u32_raw[0]) |
                              (static_cast<std::uint32_t>(u32_raw[1]) << 8) |
                              (static_cast<std::uint32_t>(u32_raw[2]) << 16) |
                              (static_cast<std::uint32_t>(u32_raw[3]) << 24);
    EXPECT_EQ(u32, test::kExpectBasicU32);
}

// ---------------------------------------------------------------------------
// 3) 数组：整段字节 + 元素地址推进
// ---------------------------------------------------------------------------

TEST_F(XcpliteA2lReadTest, ReadArrayElementsViaA2l) {
    auto loaded = a2l::A2lBridge::Load(slave_.A2lPath().string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const auto& bridge = loaded.Value();
    const a2l::IA2lDatabase* db = bridge->Database();
    auto arr = db->Find(Qualified("g_array_u8"));
    ASSERT_TRUE(arr.HasValue()) << arr.ErrorInfo().message;

    auto byte_size = db->ByteSizeOf(Qualified("g_array_u8"));
    ASSERT_TRUE(byte_size.HasValue()) << byte_size.ErrorInfo().message;
    EXPECT_EQ(byte_size.Value(), test::kArrayU8Size);

    // 整段读回
    auto master = MakeConnectedMaster();
    const Bytes raw = master->ReadMemoryBytes(
        arr.Value().xcp_address, arr.Value().address_extension,
        static_cast<ByteCount>(byte_size.Value()));
    ASSERT_EQ(raw.size(), test::kArrayU8Size);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        EXPECT_EQ(raw[i], static_cast<std::uint8_t>(i));
    }

    // 元素地址：AG=Byte 下第 5 个元素在基址 +5
    auto elem = a2l::ComputeElementAddress(arr.Value(), 5,
                                           a2l::AddressGranularity::Byte);
    ASSERT_TRUE(elem.HasValue()) << elem.ErrorInfo().message;
    EXPECT_EQ(elem.Value(), arr.Value().xcp_address + 5u);
    const Bytes e5 =
        master->ReadMemoryBytes(elem.Value(), arr.Value().address_extension, 1);
    ASSERT_EQ(e5.size(), 1u);
    EXPECT_EQ(e5[0], 5);
}

// ---------------------------------------------------------------------------
// 4) 结构体 / 嵌套：INSTANCE 元数据与基址整块读（成员叶子路径属批次16）
// ---------------------------------------------------------------------------

TEST_F(XcpliteA2lReadTest, StructInstanceBaseRead) {
    auto loaded = a2l::A2lBridge::Load(slave_.A2lPath().string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const a2l::IA2lDatabase* db = loaded.Value()->Database();

    // 桥接层对 INSTANCE/TYPEDEF_STRUCTURE 的收录是 B-12/B-13 的元数据行为；
    // 若当前版本未收录该符号则跳过（不 FAIL），成员级路径统一在下方 SKIP。
    auto sym = db->Find(Qualified("g_simple_struct"));
    if (!sym.HasValue()) {
        GTEST_SKIP() << "当前桥接层未收录 INSTANCE 符号（批次16 STRUCTLEAF "
                        "范围内），基址读验证由协议层用例覆盖";
    }
    auto master = MakeConnectedMaster();
    // 整块按 sizeof 读原始字节（B-12 禁止的是"把结构体当数值换算"，
    // 原始内存读不违规），再按 offsetof 解析字段
    const Bytes block = master->ReadMemoryBytes(
        sym.Value().xcp_address, sym.Value().address_extension,
        static_cast<ByteCount>(sizeof(test::SimpleStruct_t)));
    ASSERT_EQ(block.size(), sizeof(test::SimpleStruct_t));
    std::uint8_t u8 = 0;
    std::int16_t i16 = 0;
    std::uint32_t u32 = 0;
    std::memcpy(&u8, block.data() + offsetof(test::SimpleStruct_t, simple_u8),
                sizeof(u8));
    std::memcpy(&i16, block.data() + offsetof(test::SimpleStruct_t, simple_i16),
                sizeof(i16));
    std::memcpy(&u32, block.data() + offsetof(test::SimpleStruct_t, simple_u32),
                sizeof(u32));
    EXPECT_EQ(u8, test::kExpectSimpleStruct.u8);
    EXPECT_EQ(i16, test::kExpectSimpleStruct.i16);
    EXPECT_EQ(u32, test::kExpectSimpleStruct.u32);
}

TEST_F(XcpliteA2lReadTest, StructMemberLeafPaths) {
    auto loaded = a2l::A2lBridge::Load(slave_.A2lPath().string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const a2l::IA2lDatabase* db = loaded.Value()->Database();
    auto master = MakeConnectedMaster();

    using Simple = test::SimpleStruct_t;
    using Outer = test::OuterStruct_t;

    /// @brief 叶子：Find → 交叉校验地址（本体基址 + offsetof）→ 读 → 比对
    struct Case {
        std::string leaf;      ///< 限定叶子路径
        std::string base;      ///< 本体（INSTANCE）符号名
        std::size_t offset;    ///< offsetof 交叉校验基准
        std::size_t size;      ///< 元素字节宽
        std::uint64_t expect;  ///< 期望值（无符号位模式，小端）
    };
    const std::vector<Case> cases = {
        {"g_simple_struct.simple_i16", "g_simple_struct",
         offsetof(Simple, simple_i16), 2,
         static_cast<std::uint16_t>(test::kExpectSimpleStruct.i16)},
        {"g_simple_struct.simple_u32", "g_simple_struct",
         offsetof(Simple, simple_u32), 4, test::kExpectSimpleStruct.u32},
        {"g_outer.nested_struct.simple_u32", "g_outer",
         offsetof(Outer, nested_struct) + offsetof(Simple, simple_u32), 4,
         test::kExpectOuter.nested_struct.u32},
        {"g_outer.nested_array[1].simple_i16", "g_outer",
         offsetof(Outer, nested_array) + sizeof(Simple) +
             offsetof(Simple, simple_i16),
         2, static_cast<std::uint16_t>(test::kExpectOuter.nested_array[1].i16)},
        {"g_struct_array[2].simple_u8", "g_struct_array",
         2 * sizeof(Simple) + offsetof(Simple, simple_u8), 1,
         test::kExpectStructArray[2].u8},
    };

    for (const Case& c : cases) {
        const auto leaf = db->Find(Qualified(c.leaf));
        ASSERT_TRUE(leaf.HasValue())
            << "Find(" << c.leaf << ") 失败: " << leaf.ErrorInfo().message;
        EXPECT_EQ(leaf.Value().element_size_bytes, c.size) << c.leaf;
        EXPECT_EQ(leaf.Value().address_extension, 0x01) << c.leaf;
        // 交叉校验①：叶子地址 = 本体地址 + offsetof（C++ 布局为权威）
        const auto base = db->Find(Qualified(c.base));
        ASSERT_TRUE(base.HasValue()) << c.base;
        EXPECT_EQ(leaf.Value().xcp_address, base.Value().xcp_address + c.offset)
            << "叶子地址与 offsetof 交叉校验不符: " << c.leaf;
        // 交叉校验②：真实 ECU 读回 = 期望值
        const Bytes raw = master->ReadMemoryBytes(
            leaf.Value().xcp_address, leaf.Value().address_extension, c.size);
        ASSERT_EQ(raw.size(), c.size) << c.leaf;
        std::uint64_t got = 0;
        for (std::size_t b = 0; b < c.size; ++b) {
            got |= static_cast<std::uint64_t>(raw[b]) << (8U * b);
        }
        const std::uint64_t mask =
            (c.size >= 8) ? ~0ULL : ((1ULL << (8U * c.size)) - 1ULL);
        EXPECT_EQ(got & mask, c.expect & mask) << c.leaf;
    }

    // 成员标量数组（四形态之"固定数组"）：整体尺寸 + 元素地址推进 + 读值
    const auto arr = db->Find(Qualified("g_outer.outer_arr"));
    ASSERT_TRUE(arr.HasValue()) << arr.ErrorInfo().message;
    const auto arr_size = db->ByteSizeOf(Qualified("g_outer.outer_arr"));
    ASSERT_TRUE(arr_size.HasValue()) << arr_size.ErrorInfo().message;
    EXPECT_EQ(arr_size.Value(), 4u);
    const auto elem2 = a2l::ComputeElementAddress(
        arr.Value(), 2, a2l::AddressGranularity::Byte);
    ASSERT_TRUE(elem2.HasValue()) << elem2.ErrorInfo().message;
    EXPECT_EQ(elem2.Value(), arr.Value().xcp_address + 2u);
    const Bytes e2 = master->ReadMemoryBytes(elem2.Value(),
                                             arr.Value().address_extension, 1);
    ASSERT_EQ(e2.size(), 1u);
    EXPECT_EQ(e2[0], test::kExpectOuter.outer_arr[2]);

    // 换算通路：成员叶子按 TYPEDEF 的 COMPU（NO_COMPU → IDENTICAL）换算
    const auto phys = db->ToPhysical(Qualified("g_simple_struct.simple_u32"),
                                     Bytes{0x78U, 0x56U, 0x34U, 0x12U});
    ASSERT_TRUE(phys.HasValue()) << phys.ErrorInfo().message;
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(phys.Value()));
    EXPECT_EQ(std::get<std::int64_t>(phys.Value()),
              static_cast<std::int64_t>(test::kExpectSimpleStruct.u32));
}

// ---------------------------------------------------------------------------
// 5) A2L ↔ 运行时一致性（B-16）
// ---------------------------------------------------------------------------

TEST_F(XcpliteA2lReadTest, RuntimeConsistencyNoErrors) {
    auto loaded = a2l::A2lBridge::Load(slave_.A2lPath().string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const auto& bridge = loaded.Value();
    auto master = MakeConnectedMaster();
    const auto params = master->GetSessionParameters();
    const auto& c = params.connect;

    // 钉住对手端事实：XCPlite CONNECT PROTOCOL_VERSION 字节 = 0x01
    // （XCP_PROTOCOL_LAYER_VERSION>>8，仅 major；见 VersionByteToA2l 注释的
    //  偏差说明）。若未来对手端改为规范 nibble 编码，本断言会失败并提醒
    //  同步更新换算特例。
    EXPECT_EQ(c.protocol_layer_version, 0x01U);

    a2l::RuntimeXcpParams rt;
    rt.protocol_version = VersionByteToA2l(c.protocol_layer_version);
    rt.max_cto = c.max_cto;
    rt.max_dto = c.max_dto;
    rt.byte_order = (c.byte_order == ByteOrder::Intel)
                        ? a2l::ByteOrder::MsbLast
                        : a2l::ByteOrder::MsbFirst;
    switch (c.address_granularity) {
        case AddressGranularity::Word:
            rt.address_granularity = a2l::AddressGranularity::Word;
            break;
        case AddressGranularity::DWord:
            rt.address_granularity = a2l::AddressGranularity::Dword;
            break;
        case AddressGranularity::Byte:
        default:
            rt.address_granularity = a2l::AddressGranularity::Byte;
            break;
    }
    rt.has_daq = HasResource(c.resource_mask, Resource::Daq);

    auto diff = bridge->CompareWithRuntime(rt);
    ASSERT_TRUE(diff.HasValue()) << diff.ErrorInfo().message;
    int errors = 0;
    for (const auto& d : diff.Value()) {
        if (d.severity == a2l::Severity::Error) {
            ++errors;
            ADD_FAILURE() << "B-16 Error 项: " << d.parameter_name
                          << " a2l=" << d.a2l_value
                          << " runtime=" << d.runtime_value;
        }
    }
    EXPECT_EQ(errors, 0) << "真实 Slave 的运行时参数与 A2L 声明必须一致"
                            "（容量类差异允许 Warning）";
}

}  // namespace
}  // namespace calmcar::xcp
