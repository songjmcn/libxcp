/**
 * @file xcplite_write_test.cpp
 * @brief 与 XCPlite Slave 的写回（修改 ECU 端变量）集成测试（第二阶段）。
 *
 * 依据 code-plan/XCPlite_Slave_协议调试集成计划.md §3（Phase2）。
 *
 * 对 XCPlite 源码的写路径核证结论（实现前的"写前读"）：
 *  - 绝对寻址（ext=0x01）经 XcpSetMta 解析为进程指针并转 PTR 模式
 *    （xcplite.c:690-691），DOWNLOAD/SHORT_DOWNLOAD 直接写内存
 *    （XcpWriteMta PTR 分支）→ 全局测量/标定量可直接写回；
 *  - 段寻址（ext=0，CASDD）写 XcpCalSegWriteMemory（CalSeg 工作页 RCU）；
 *  - XCPlite 默认构建 **未启用** Seed&Key（xcp_cfg.h 的 XCP_ENABLE_SEED_KEY
 *    被注释，xcplite.c:2140+ 的 GET_SEED/UNLOCK 分支整体不编译）→
 *    GET_SEED 实际回 ERR_CMD_UNKNOWN。libxcp 的 Unlock() 必须如实报告
 *    该负响应，而不是假想"Length 0 = 未保护"短路。
 *
 * 每个用例独立 Slave 进程（夹具 SetUp/TearDown），写污染不跨用例。
 */

#include <cstring>
#include <memory>
#include <vector>

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

/// @brief 写回用例夹具（套件名对齐计划 DoD 的 ctest -R XcpliteWrite）
class XcpliteWriteTest : public test::XcpliteSlaveTest {};

/// @brief 把标量按会话字节序（PC 恒小端）编码为字节
template <typename T>
Bytes LeBytes(const T& value) {
    Bytes out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

// ---------------------------------------------------------------------------
// Phase2-01：写回基本变量
// ---------------------------------------------------------------------------

TEST_F(XcpliteWriteTest, WriteBasicU8) {
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_u8");
    ASSERT_TRUE(info.has_value());
    auto master = MakeConnectedMaster();

    master->WriteMemoryBytes(info->address, info->extension, Bytes{0xFEU});
    const Bytes readback =
        master->ReadMemoryBytes(info->address, info->extension, 1);
    ASSERT_EQ(readback.size(), 1U);
    EXPECT_EQ(readback[0], 0xFEU);
}

TEST_F(XcpliteWriteTest, WriteBasicU32AndFloat) {
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_u32");
    ASSERT_TRUE(info.has_value());
    auto master = MakeConnectedMaster();

    const std::uint32_t magic = 0xCAFEBABEU;
    master->WriteMemoryBytes(info->address, info->extension, LeBytes(magic));
    const Bytes readback =
        master->ReadMemoryBytes(info->address, info->extension, 4);
    ASSERT_EQ(readback.size(), 4U);
    std::uint32_t got = 0;
    std::memcpy(&got, readback.data(), 4);
    EXPECT_EQ(got, magic);

    // float 走同一条 SHORT_DOWNLOAD 通路
    const auto f = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_f32");
    ASSERT_TRUE(f.has_value());
    const float new_val = -12.25F;
    master->WriteMemoryBytes(f->address, f->extension, LeBytes(new_val));
    const Bytes fback = master->ReadMemoryBytes(f->address, f->extension, 4);
    ASSERT_EQ(fback.size(), 4U);
    float fgot = 0.0F;
    std::memcpy(&fgot, fback.data(), 4);
    EXPECT_FLOAT_EQ(fgot, new_val);
}

// ---------------------------------------------------------------------------
// Phase2-02：写回数组元素与结构体成员（字段级地址 = 基址 + offsetof）
// ---------------------------------------------------------------------------

TEST_F(XcpliteWriteTest, WriteArrayElement) {
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "g_array_i16");
    ASSERT_TRUE(info.has_value());
    auto master = MakeConnectedMaster();

    // 第 2 个元素（0 基）：地址 = 基址 + 2*sizeof(int16)
    const Address elem = info->address + 2U * sizeof(std::int16_t);
    const std::int16_t new_val = 1234;
    master->WriteMemoryBytes(elem, info->extension, LeBytes(new_val));

    const Bytes back = master->ReadMemoryBytes(elem, info->extension, 2);
    ASSERT_EQ(back.size(), 2U);
    std::int16_t got = 0;
    std::memcpy(&got, back.data(), 2);
    EXPECT_EQ(got, new_val);

    // 相邻元素不受影响：第 3 个元素仍为初始 -40
    const Bytes neighbour =
        master->ReadMemoryBytes(elem + 2U, info->extension, 2);
    ASSERT_EQ(neighbour.size(), 2U);
    std::int16_t nb = 0;
    std::memcpy(&nb, neighbour.data(), 2);
    EXPECT_EQ(nb, -40);
}

TEST_F(XcpliteWriteTest, WriteStructMemberField) {
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "g_simple_struct");
    ASSERT_TRUE(info.has_value());
    auto master = MakeConnectedMaster();
    using Simple = test::SimpleStruct_t;

    // 只改 simple_i16 字段（基址 + offsetof）
    const Address field =
        info->address + static_cast<Address>(offsetof(Simple, simple_i16));
    const std::int16_t new_val = -7;
    master->WriteMemoryBytes(field, info->extension, LeBytes(new_val));

    // 整块读回：i16 新值，其余字段保持初始
    const Bytes block = master->ReadMemoryBytes(
        info->address, info->extension, static_cast<ByteCount>(sizeof(Simple)));
    ASSERT_EQ(block.size(), sizeof(Simple));
    std::uint8_t u8 = 0;
    std::int16_t i16 = 0;
    std::uint32_t u32 = 0;
    std::memcpy(&u8, block.data() + offsetof(Simple, simple_u8), sizeof(u8));
    std::memcpy(&i16, block.data() + offsetof(Simple, simple_i16), sizeof(i16));
    std::memcpy(&u32, block.data() + offsetof(Simple, simple_u32), sizeof(u32));
    EXPECT_EQ(i16, new_val);
    EXPECT_EQ(u8, test::kExpectSimpleStruct.u8);
    EXPECT_EQ(u32, test::kExpectSimpleStruct.u32);
}

TEST_F(XcpliteWriteTest, WriteNestedStructViaLeafAddress) {
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "g_outer");
    ASSERT_TRUE(info.has_value());
    auto master = MakeConnectedMaster();
    using Outer = test::OuterStruct_t;
    using Simple = test::SimpleStruct_t;

    // 深一层路径：g_outer.nested_struct.simple_u32
    const Address leaf = info->address +
                         static_cast<Address>(offsetof(Outer, nested_struct)) +
                         static_cast<Address>(offsetof(Simple, simple_u32));
    const std::uint32_t new_val = 0x0BADC0DEU;
    master->WriteMemoryBytes(leaf, info->extension, LeBytes(new_val));
    const Bytes back = master->ReadMemoryBytes(leaf, info->extension, 4);
    ASSERT_EQ(back.size(), 4U);
    std::uint32_t got = 0;
    std::memcpy(&got, back.data(), 4);
    EXPECT_EQ(got, new_val);
}

// ---------------------------------------------------------------------------
// 多块写回（> SHORT_DOWNLOAD 单帧上限 → SET_MTA + DOWNLOAD 分块）
// ---------------------------------------------------------------------------

TEST_F(XcpliteWriteTest, MultiChunkDownloadOfBlob) {
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "g_blob");
    ASSERT_TRUE(info.has_value());
    auto master = MakeConnectedMaster();

    Bytes payload(test::kBlobSize);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(0xE0U + (i % 32U));
    }
    master->WriteMemoryBytes(info->address, info->extension,
                             BytesView{payload});

    const Bytes back = master->ReadMemoryBytes(
        info->address, info->extension, static_cast<ByteCount>(payload.size()));
    ASSERT_EQ(back.size(), payload.size());
    EXPECT_EQ(back, payload);
}

// ---------------------------------------------------------------------------
// CalSeg 段寻址写回（CASDD：ext=0，地址 0x80000000|seg<<16|offset）
// ---------------------------------------------------------------------------

TEST_F(XcpliteWriteTest, WriteCalSegmentParameter) {
    // 标定参数 kDefaultCalParams 的 A2L 地址即段相对编码；直接按符号解析
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "kDefaultCalParams");
    ASSERT_TRUE(info.has_value());
    // INSTANCE 行无 ECU_ADDRESS 关键字，地址来自 extension 前一 token
    EXPECT_EQ(info->extension, 0U) << "CalSeg 参数应为段寻址（ext=0）";
    EXPECT_EQ(info->address & 0x80000000U, 0x80000000U)
        << "段寻址地址高位置 1（XcpAddrEncodeSegIndex 约定）";

    auto master = MakeConnectedMaster();
    const std::uint16_t new_factor = 0x1234U;
    master->WriteMemoryBytes(info->address, info->extension,
                             LeBytes(new_factor));

    // 读回同段地址。XCPlite RCU 语义：写进入工作页，Slave 端"第二次锁后
    // 可见"（docs/CAL_RCU.md）；XCP 读路径读当前激活页，DOWNLOAD 后
    // 立读应为新值。
    const Bytes back =
        master->ReadMemoryBytes(info->address, info->extension, 2);
    ASSERT_EQ(back.size(), 2U);
    std::uint16_t got = 0;
    std::memcpy(&got, back.data(), 2);
    EXPECT_EQ(got, new_factor);
}

// ---------------------------------------------------------------------------
// Seed&Key：XCPlite 默认不支持 GET_SEED/UNLOCK，如实报告（不假想短路）
// ---------------------------------------------------------------------------

TEST_F(XcpliteWriteTest, UnlockAgainstXcpliteReportsCmdUnknown) {
    auto master = MakeConnectedMaster();
    // 回调本身不会被调用（GET_SEED 先失败）；给出确定性实现以满足签名
    const SeedKeyCalculator calc = [](Resource /*resource*/,
                                      BytesView /*seed*/) { return Bytes{}; };
    try {
        (void)master->Unlock(Resource::CalPag, calc);
        FAIL() << "XCPlite 默认未编译 GET_SEED/UNLOCK（xcp_cfg.h 核证），"
                  "Unlock 应报告 ERR_CMD_UNKNOWN 而不是静默成功";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::CmdUnknown));
    }
    // 负响应属正常命令级失败，会话必须保持可用（不触发 SYNCH 恢复）
    EXPECT_TRUE(master->IsConnected());
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_u8");
    ASSERT_TRUE(info.has_value());
    EXPECT_NO_THROW(
        (void)master->ReadMemoryBytes(info->address, info->extension, 1));
}

}  // namespace
}  // namespace calmcar::xcp
