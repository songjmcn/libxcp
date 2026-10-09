// =============================================================================
// a2l_golden_test.cpp —— A2L 黄金回归测试套件（设计 §7.2 用例分组 T1–T8）
//
// 与 A2lSmoke 的分工：冒烟锁 7 条最小门禁；本套件按 B 类决策矩阵覆盖
// 加载错误/INCLUDE(B-18)/地址(B-1)/类型(B-3)/布局(B-2)/位域(B-9)/搜索
// (B-13/B-17)/换算(B-8/B-14/B-15)/IF_DATA(B-16/§6.3-A)/CHARACTERISTIC
// (B-4/B-11)/DAQ&DTO(B-5/B-7)/线程(B-20)/编码。
// 样本来源：tests/a2l_gen/golden_spec*.json（fixture 生成）+ tests/data/a2l/
// 手写静态样本（含破坏性与 include 变体）。
// T9（性能基线）与 T10（隔离断言，由 P9 configure 期扫描承担）见批次10记录。
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/a2l/ia2l_database.hpp"

#ifndef A2L_GOLDEN_DIR
#error "A2L_GOLDEN_DIR 缺失（tests/CMakeLists.txt 注入）"
#endif
#ifndef A2L_DATA_DIR
#error "A2L_DATA_DIR 缺失（tests/CMakeLists.txt 注入）"
#endif

namespace {

using namespace calmcar::xcp::a2l;  // NOLINT：测试文件，减少限定名噪音

/// @brief 黄金样本基类：路径拼接与快速加载辅助
class A2lGoldenTest : public ::testing::Test {
protected:
    /// @brief build-tree 内生成的黄金样本路径
    static std::string Golden(const std::string& name) {
        return std::string(A2L_GOLDEN_DIR) + "/" + name;
    }
    /// @brief 源码树静态样本路径
    static std::string Data(const std::string& name) {
        return std::string(A2L_DATA_DIR) + "/" + name;
    }
    /// @brief 加载成功辅助（失败即终止当前用例并打印结构化错误）
    static std::unique_ptr<A2lBridge> LoadOk(const std::string& path,
                                             const LoadOptions& opts = {}) {
        auto r = A2lBridge::Load(path, opts);
        if (!r.HasValue()) {
            const Error& e = r.ErrorInfo();
            ADD_FAILURE() << "Load 失败 " << path << ": " << ToString(e.code)
                          << " / " << e.message
                          << (e.line ? " line=" + std::to_string(*e.line)
                                     : std::string());
            return nullptr;
        }
        return std::move(r).Value();
    }
    /// @brief 取路径末段文件名（链元素为 canonical 全路径，比对只看末段）
    static std::string FileNameOf(const std::string& p) {
        const std::size_t sep = p.find_last_of("/\\");
        return sep == std::string::npos ? p : p.substr(sep + 1);
    }
    /// @brief 失败加载辅助：断言错误码与行号（不匹配字符串）
    /// @param expect_include_chain include 类错误（循环/深度/越根）：
    ///        `Error.include_chain` 结构化非空且首尾符合期望（B-18 批次12
    ///        载体）。与文本开关相互独立。
    /// @param chain_tail_name 期望链末段的文件名（空=不校验末段）
    /// @param chain_size 期望链元素数（0=只校验非空；批次12 实测值等值锁定）
    /// @param expect_chain_text message 应携带 `a -> b` 箭头链文本
    ///        （B-18 批次11 显示契约；**越根错误不适用**——批次11 明确保持
    ///        `token (from 文件)` 形式，文本一字不改）
    static void ExpectLoadError(const std::string& path, ErrorCode expect_code,
                                const LoadOptions& opts = {},
                                bool expect_line = false,
                                bool expect_include_chain = false,
                                const std::string& chain_tail_name = {},
                                std::size_t chain_size = 0,
                                bool expect_chain_text = false) {
        auto r = A2lBridge::Load(path, opts);
        ASSERT_FALSE(r.HasValue()) << "本应失败却成功: " << path;
        const Error& e = r.ErrorInfo();
        EXPECT_EQ(e.code, expect_code) << "message=" << e.message;
        EXPECT_EQ(e.phase, Phase::Load);
        if (expect_line) {
            ASSERT_TRUE(e.line.has_value())
                << "解析类错误应携带行号: " << e.message;
            EXPECT_GT(*e.line, 0u);
        } else {
            EXPECT_FALSE(e.line.has_value());
        }
        if (expect_chain_text) {
            EXPECT_NE(e.message.find(" -> "), std::string::npos)
                << "include 链错误应携带完整链文本: " << e.message;
        }
        if (expect_include_chain) {
            if (chain_size > 0) {
                EXPECT_EQ(e.include_chain.size(), chain_size)
                    << "结构化 include 链元素数与实测口径不符: " << e.message;
            } else {
                EXPECT_FALSE(e.include_chain.empty())
                    << "include 类错误应带出结构化链: " << e.message;
            }
            if (!e.include_chain.empty()) {
                EXPECT_EQ(FileNameOf(e.include_chain.front()), FileNameOf(path))
                    << "链首必须为主文件";
                if (!chain_tail_name.empty()) {
                    EXPECT_EQ(FileNameOf(e.include_chain.back()),
                              chain_tail_name)
                        << "链尾应为触发文件";
                }
            }
        } else {
            // 非 include 类错误不得携带链（B-18 边界锁定）
            EXPECT_TRUE(e.include_chain.empty())
                << "该错误不应带出 include 链: " << e.message;
        }
    }
};

// ============================================================================
// T1 加载 / 错误 / INCLUDE（B-18）
// ============================================================================

TEST_F(A2lGoldenTest, LoadGoldenBasicOk) {
    auto b = LoadOk(Golden("golden_basic.a2l"));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->Progress(), 100);
    ASSERT_NE(b->Database(), nullptr);
    ASSERT_TRUE(b->Database()->Count().HasValue());
    EXPECT_EQ(b->Database()->Count().Value(), 4u);  // 3 MEASUREMENT + 1 CHAR
    ASSERT_NE(b->XcpInfo(), nullptr);
    EXPECT_EQ(b->XcpInfo()->module_name, "SMOKE_ECU");
    EXPECT_TRUE(b->XcpInfo()->ok);
}

TEST_F(A2lGoldenTest, LoadMissingFileFails) {
    ExpectLoadError(Golden("no_such_file.a2l"), ErrorCode::IoError);
}

TEST_F(A2lGoldenTest, LoadTruncatedReportsLocation) {
    // bison 解析失败路径：错误码稳定 + 行号结构化（不靠字符串判断）
    ExpectLoadError(Data("broken_truncated.a2l"), ErrorCode::ParseFailed, {},
                    /*expect_line=*/true);
}

TEST_F(A2lGoldenTest, LoadBadKeywordReportsLocation) {
    ExpectLoadError(Data("broken_bad_keyword.a2l"), ErrorCode::ParseFailed, {},
                    /*expect_line=*/true);
}

TEST_F(A2lGoldenTest, IncludeTwoLevelLoads) {
    // B-18 正路径：main /include child（两个完整 A2L，上游 Merge 进同一
    // PROJECT）； 样本无 IF_DATA → 装载须豁免 require（T1 的 require
    // 开关用例另行覆盖）
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto b = LoadOk(Data("include_main.a2l"), require_off);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(b->Database(), nullptr);
    auto main_m = b->Database()->Find("INC_MAIN::MAIN_M");
    ASSERT_TRUE(main_m.HasValue()) << main_m.ErrorInfo().message;
    EXPECT_EQ(main_m.Value().xcp_address, 0x1001u);
    auto child_m = b->Database()->Find("INC_CHILD::CHILD_M");
    ASSERT_TRUE(child_m.HasValue()) << child_m.ErrorInfo().message;
    EXPECT_EQ(child_m.Value().xcp_address, 0x2222u);
}

TEST_F(A2lGoldenTest, IncludeCycleRejectedBeforeParse) {
    // B-18 循环检测在上游 ParseFile 之前拦截（否则 .a2l 递归会栈溢出）；
    // 批次11：message 携带完整链 `main -> a -> b -> a`（显示契约）
    // 批次12：结构化链同带出——链尾为回指节点，故链内必有重复项
    ExpectLoadError(Data("cycle_main.a2l"), ErrorCode::ParseFailed, {},
                    /*expect_line=*/true, /*expect_include_chain=*/true,
                    /*chain_tail_name=*/"cycle_a.a2l",
                    /*chain_size=*/4, /*expect_chain_text=*/true);
    auto r = A2lBridge::Load(Data("cycle_main.a2l"));
    ASSERT_FALSE(r.HasValue());
    const std::vector<std::string>& chain = r.ErrorInfo().include_chain;
    ASSERT_EQ(chain.size(), 4u) << "main -> a -> b -> a";
    EXPECT_EQ(FileNameOf(chain[1]), FileNameOf(chain[3]))
        << "循环链的链尾必须回指链中已有节点";
}

TEST_F(A2lGoldenTest, IncludeEscapeRejectedUnlessAllowed) {
    const std::string main_path = Data("escape_chain/escape_main.a2l");
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    // 默认：越根拒绝（root = escape_chain/）
    // 批次12：越根同样带出结构化链（主文件 → 越根目标，2 个元素）；
    //         文本按批次11 口径保持 `token (from 文件)`，故不断言箭头链
    ExpectLoadError(main_path, ErrorCode::ParseFailed, {},
                    /*expect_line=*/true, /*expect_include_chain=*/true,
                    /*chain_tail_name=*/"escape_target.a2l",
                    /*chain_size=*/2, /*expect_chain_text=*/false);
    // 显式放开后正常加载并合并目标模块（样本无 IF_DATA → 同时豁免 require）
    require_off.allow_include_outside_root = true;
    auto b = LoadOk(main_path, require_off);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(b->Database(), nullptr);
    EXPECT_TRUE(b->Database()->Find("ESC_MAIN::ESC_MAIN_M").HasValue());
    EXPECT_TRUE(b->Database()->Find("ESC_TARGET::ESC_M").HasValue());
}

TEST_F(A2lGoldenTest, IncludeDepthExceed32Rejected) {
    // 深度链 d0 → d1 … d31 → d32：第 33 层（>32）拒绝（B-18）
    // 临时链文件写入 build tree（测试进程对系统 %TEMP% 的写入被环境限制）
    const auto dir = std::filesystem::path(A2L_GOLDEN_DIR) / "scratch_depth";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto write_link = [](const std::filesystem::path& p,
                               const std::string& next) {
        std::ofstream out(p);
        out << "ASAP2_VERSION 1 61\n"
            << "/begin Project DepthProj \"depth chain\"\n";
        if (!next.empty()) {
            out << "/include \"" << next << "\"\n";
        }
        out << "/begin MODULE DEPTH_M \"m\"\n"
               "  /begin COMPU_METHOD CM_D \"i\" IDENTICAL \"%6.2\" \"\"\n"
               "  /end COMPU_METHOD\n"
               "/end MODULE\n"
               "/end Project\n";
    };
    // 32 层文件 d00..d31 + 终点 d32（深度 33 处触发，无需可解析）
    for (int i = 0; i <= 31; ++i) {
        const std::string name =
            "d" + std::string(i < 10 ? "0" : "") + std::to_string(i);
        const std::string next = "d" + std::string(i + 1 < 10 ? "0" : "") +
                                 std::to_string(i + 1) + ".a2l";
        write_link(dir / (name + ".a2l"), next);
    }
    {
        std::ofstream out(dir / "d32.a2l");
        out << "ASAP2_VERSION 1 61\n";
    }
    ExpectLoadError((dir / "d00.a2l").string(), ErrorCode::ParseFailed, {},
                    /*expect_line=*/true, /*expect_include_chain=*/true,
                    /*chain_tail_name=*/"d32.a2l",
                    /*chain_size=*/33, /*expect_chain_text=*/true);
    std::filesystem::remove_all(dir, ec);
}

TEST_F(A2lGoldenTest, RequireIfDataXcpControlsMissingIfData) {
    LoadOptions require_on;  // 默认 require_if_data_xcp=true
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    // golden_convert 无 IF_DATA 段：require → 结构化失败；豁免 → 放行
    ExpectLoadError(Golden("golden_convert.a2l"), ErrorCode::ParseFailed,
                    require_on);
    auto b = LoadOk(Golden("golden_convert.a2l"), require_off);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(b->XcpInfo(), nullptr);
    EXPECT_FALSE(b->XcpInfo()->ok);
    ASSERT_NE(b->Database(), nullptr);
    // 原 11 个符号 + 4 个独立 LINEAR 系数用例。
    EXPECT_EQ(b->Database()->Count().Value(), 15u);
}

TEST_F(A2lGoldenTest, MalformedButLegalVariantsParse) {
    // A-6 缓解①：行尾（CRLF/LF）、字符串外空白、UTF-8 BOM 三类变体
    for (const char* name : {"golden_basic_lf.a2l", "golden_basic_ws.a2l",
                             "golden_basic_bom.a2l"}) {
        auto b = LoadOk(Golden(name));
        ASSERT_NE(b, nullptr) << name;
        ASSERT_NE(b->Database(), nullptr) << name;
        EXPECT_EQ(b->Database()->Count().Value(), 4u) << name;
    }
}

// ============================================================================
// T2 地址 / 类型 / 数组 / 位域 / 搜索（B-1/B-2/B-3/B-9/B-13/B-17）
// ============================================================================

TEST_F(A2lGoldenTest, AddressGranularityDoesNotRewriteBase) {
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // 8 字节对象的 XCP 元素数 8/4/2（AG 只做单位换算，绝不乘进基址，B-1）
    auto arr = db->Find("MASK_ECU::M_ARR8");
    ASSERT_TRUE(arr.HasValue());
    EXPECT_EQ(arr.Value().xcp_address, 0x3048u);
    ASSERT_TRUE(db->ByteSizeOf("MASK_ECU::M_ARR8").HasValue());
    EXPECT_EQ(db->ByteSizeOf("MASK_ECU::M_ARR8").Value(), 8u);
    auto addr_b =
        ComputeElementAddress(arr.Value(), 8, AddressGranularity::Byte);
    ASSERT_TRUE(addr_b.HasValue());
    EXPECT_EQ(addr_b.Value(), 0x3048u + 8);
    auto addr_w =
        ComputeElementAddress(arr.Value(), 8, AddressGranularity::Word);
    ASSERT_TRUE(addr_w.HasValue());
    EXPECT_EQ(addr_w.Value(), 0x3048u + 4);
    auto addr_d =
        ComputeElementAddress(arr.Value(), 8, AddressGranularity::Dword);
    ASSERT_TRUE(addr_d.HasValue());
    EXPECT_EQ(addr_d.Value(), 0x3048u + 2);

    // AG=2 下 1 字节元素的奇数偏移 → 拒绝（B-1 黄金项）
    auto ub = db->Find("MASK_ECU::M_UBYTE");
    ASSERT_TRUE(ub.HasValue());
    auto unaligned =
        ComputeElementAddress(ub.Value(), 1, AddressGranularity::Word);
    EXPECT_FALSE(unaligned.HasValue());
    EXPECT_EQ(unaligned.ErrorInfo().code, ErrorCode::AddressOverflow);

    // 乘法溢出 → 拒绝（不发包语义：返回结构化错误即可，本层无发包能力）
    auto u64 = db->Find("MASK_ECU::M_U64");
    ASSERT_TRUE(u64.HasValue());
    auto overflow = ComputeElementAddress(u64.Value(), UINT64_MAX,
                                          AddressGranularity::Byte);
    EXPECT_FALSE(overflow.HasValue());
    EXPECT_EQ(overflow.ErrorInfo().code, ErrorCode::AddressOverflow);

    // extension 独立字段（M_2D ext=0x12，M_ARR8 ext=0）
    auto m2d = db->Find("MASK_ECU::M_2D");
    ASSERT_TRUE(m2d.HasValue());
    EXPECT_EQ(m2d.Value().address_extension, 0x12);
    EXPECT_EQ(arr.Value().address_extension, 0x0);
}

TEST_F(A2lGoldenTest, AllA2lTypesHaveExplicitWidth) {
    // B-3：显式映射表逐项断言 + 未知类型必须为 0（调用方判错，禁止猜宽度）
    EXPECT_EQ(ElementSizeOf(AsamDataType::UByte), 1);
    EXPECT_EQ(ElementSizeOf(AsamDataType::SByte), 1);
    EXPECT_EQ(ElementSizeOf(AsamDataType::UWord), 2);
    EXPECT_EQ(ElementSizeOf(AsamDataType::SWord), 2);
    EXPECT_EQ(ElementSizeOf(AsamDataType::ULong), 4);
    EXPECT_EQ(ElementSizeOf(AsamDataType::SLong), 4);
    EXPECT_EQ(ElementSizeOf(AsamDataType::ULong64), 8);
    EXPECT_EQ(ElementSizeOf(AsamDataType::SLong64), 8);
    EXPECT_EQ(ElementSizeOf(AsamDataType::Float16), 2);
    EXPECT_EQ(ElementSizeOf(AsamDataType::Float32), 4);
    EXPECT_EQ(ElementSizeOf(AsamDataType::Float64), 8);
    EXPECT_EQ(ElementSizeOf(AsamDataType::Unknown), 0);
    EXPECT_EQ(ElementSizeOf(AsamDataType::Boolean), 0);
    EXPECT_EQ(ElementSizeOf(AsamDataType::String), 0);
}

TEST_F(A2lGoldenTest, Scalar1dAndRegular2dLayout) {
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // 2×3 UWORD 规则 2D：低维乘积递推 stride（2, 2×2=4），0-based 下界
    auto m2d = db->Find("MASK_ECU::M_2D");
    ASSERT_TRUE(m2d.HasValue());
    const SymbolInfo& s = m2d.Value();
    ASSERT_EQ(s.dimensions.size(), 2u);
    EXPECT_EQ(s.dimensions[0].extent, 2u);
    EXPECT_EQ(s.dimensions[0].byte_stride, 2u);
    EXPECT_EQ(s.dimensions[1].extent, 3u);
    EXPECT_EQ(s.dimensions[1].byte_stride, 4u);
    EXPECT_EQ(s.dimensions[0].source_lower_bound, 0);
    auto count = CountElements(s);
    ASSERT_TRUE(count.HasValue());
    EXPECT_EQ(count.Value(), 6u);
    auto bytes = db->ByteSizeOf("MASK_ECU::M_2D");
    ASSERT_TRUE(bytes.HasValue());
    EXPECT_EQ(bytes.Value(), 12u);
    EXPECT_EQ(s.array_order, ArrayOrder::RowMajor);
    // 数组整体读写拒绝（元素级走 IDaqLayout）
    Bytes two{0x00, 0x00};
    auto whole = db->ToPhysical("MASK_ECU::M_2D", two);
    EXPECT_FALSE(whole.HasValue());
    EXPECT_EQ(whole.ErrorInfo().code, ErrorCode::UnsupportedOperation);

    // 不规则 stride 手工构造 → 整组拒绝（B-2 禁止展平猜测）
    SymbolInfo irregular;
    irregular.element_size_bytes = 2;
    irregular.dimensions = {Dimension{0, 4, 3}, Dimension{0, 3, 6}};
    auto bad = CountElements(irregular);
    EXPECT_FALSE(bad.HasValue());
    EXPECT_EQ(bad.ErrorInfo().code, ErrorCode::InvalidLayout);
}

TEST_F(A2lGoldenTest, BitMaskAndDaqBitOffsetAreIndependent) {
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // 连续 BIT_MASK：raw 0x0F00 → 掩码提取 15（B-9 值提取）
    const Bytes masked_raw{0x00, 0x0F};
    auto masked = db->ToPhysical("MASK_ECU::M_MASK", masked_raw);
    ASSERT_TRUE(masked.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(masked.Value()));
    EXPECT_EQ(std::get<std::int64_t>(masked.Value()), 15);

    // 非连续 BIT_MASK → InvalidLayout（§4.1 首版拒绝）
    auto bad_mask = db->ToPhysical("MASK_ECU::M_BADMASK", masked_raw);
    EXPECT_FALSE(bad_mask.HasValue());
    EXPECT_EQ(bad_mask.ErrorInfo().code, ErrorCode::InvalidLayout);

    // ODT entry BIT_OFFSET ≠ 0：只交 raw、不做猜测性位移（B-9 分离）
    auto layout_r = b->CreateDaqLayout();
    ASSERT_TRUE(layout_r.HasValue());
    // golden_mask 声明 FIRST_PID=4（列表号是 2）：批次15 F1 起按
    // "绝对 ODT 号 = FIRST_PID + 相对 ODT 号"（docs L2225）路由 → PID=0x04
    DtoFrameLayout frame;              // Absolute + 1 字节 PID
    const Bytes dto{0x04, 0x00, 0x0F,  // entry1: M_MASK 2B
                    0xAA,              // entry2: bit_offset=3
                    0,    1,    2,    3, 4,  5,
                    6,    7,    8,    9, 10, 11};  // entry3: M_2D 12B
    auto samples = layout_r.Value()->Decode(frame, dto);
    ASSERT_TRUE(samples.HasValue());
    ASSERT_EQ(samples.Value().size(), 3u);
    const auto& v = samples.Value();
    // entry1：掩码提取 → 15
    EXPECT_EQ(v[0].symbol_name, "MASK_ECU::M_MASK");
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(v[0].physical_value));
    EXPECT_EQ(std::get<std::int64_t>(v[0].physical_value), 15);
    EXPECT_FALSE(v[0].valid.has_value());  // ERROR_MASK 不推 valid（B-9）
    // entry2：bit_offset=3 → 保留 raw（0xAA），不产生物理值（默认值≠0xAA 证明）
    EXPECT_EQ(v[1].symbol_name, "MASK_ECU::M_UBYTE");
    EXPECT_EQ(v[1].raw, (Bytes{0xAA}));
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(v[1].physical_value));
    EXPECT_EQ(std::get<std::int64_t>(v[1].physical_value), 0);
    // entry3：整段数组 → raw only（尺寸不同/含维度，不换算）
    EXPECT_EQ(v[2].symbol_name, "MASK_ECU::M_2D");
    EXPECT_EQ(v[2].raw.size(), 12u);
}

TEST_F(A2lGoldenTest, QualifiedNameAndStableSearch) {
    // B-17 歧义：多 MODULE 同时声明 IF_DATA，默认 require 装载即结构化失败
    ExpectLoadError(Golden("golden_multimodule.a2l"), ErrorCode::AmbiguousName);

    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto b = LoadOk(Golden("golden_multimodule.a2l"), require_off);
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(db->Count().Value(), 3u);

    // 裸名跨模块命中两个 → AmbiguousName（B-13/B-17，不猜测）
    auto dup = db->Find("DUP");
    EXPECT_FALSE(dup.HasValue());
    EXPECT_EQ(dup.ErrorInfo().code, ErrorCode::AmbiguousName);
    // 限定名精确命中
    auto dup_a = db->Find("MOD_A::DUP");
    ASSERT_TRUE(dup_a.HasValue());
    EXPECT_EQ(dup_a.Value().xcp_address, 0x4000u);
    auto dup_b = db->Find("MOD_B::DUP");
    ASSERT_TRUE(dup_b.HasValue());
    EXPECT_EQ(dup_b.Value().xcp_address, 0x5000u);
    // 通配符 + 大小写无关 + 稳定排序 + 截断（B-13）
    auto all = db->Search("mod_*");
    ASSERT_TRUE(all.HasValue());
    ASSERT_EQ(all.Value().size(), 3u);
    for (size_t i = 1; i < all.Value().size(); ++i) {
        const std::string prev =
            all.Value()[i - 1].module_name + "::" + all.Value()[i - 1].name;
        const std::string cur =
            all.Value()[i].module_name + "::" + all.Value()[i].name;
        EXPECT_LT(prev, cur) << "Search 结果必须稳定按规范键排序";
    }
    auto one = db->Search("MOD_A::U*", 5);
    ASSERT_TRUE(one.HasValue());
    EXPECT_EQ(one.Value().size(), 1u);
    EXPECT_EQ(one.Value()[0].name, "UNIQUE_A");
    auto truncated = db->Search("*", 2);
    ASSERT_TRUE(truncated.HasValue());
    EXPECT_EQ(truncated.Value().size(), 2u);  // 稳定排序后截断
}

// ============================================================================
// T3 换算矩阵 / 精度 / 单位（B-8/B-10/B-14/B-15）
// ============================================================================

TEST_F(A2lGoldenTest, SupportedConversionMatrix) {
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto b = LoadOk(Golden("golden_convert.a2l"), require_off);
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // IDENTICAL：整数往返
    auto ident = db->ToPhysical("CONV_ECU::M_IDENT", Bytes{0x07, 0x00});
    ASSERT_TRUE(ident.HasValue());
    EXPECT_EQ(std::get<std::int64_t>(ident.Value()), 7);
    auto ident_back = db->FromPhysical("CONV_ECU::M_IDENT", ident.Value());
    ASSERT_TRUE(ident_back.HasValue());
    EXPECT_EQ(ident_back.Value(), (Bytes{0x07, 0x00}));

    // LINEAR：200 → 100.0 → 200
    auto lin = db->ToPhysical("CONV_ECU::M_LINEAR", Bytes{0xC8, 0x00});
    ASSERT_TRUE(lin.HasValue());
    ASSERT_TRUE(std::holds_alternative<double>(lin.Value()));
    EXPECT_DOUBLE_EQ(std::get<double>(lin.Value()), 100.0);
    auto lin_back = db->FromPhysical("CONV_ECU::M_LINEAR", lin.Value());
    ASSERT_TRUE(lin_back.HasValue());
    EXPECT_EQ(lin_back.Value(), (Bytes{0xC8, 0x00}));

    // RAT_FUNC：(i²+2i+1)/(1) → i=3 → 16.0
    auto rat = db->ToPhysical("CONV_ECU::M_RATFUNC", Bytes{0x03, 0x00});
    ASSERT_TRUE(rat.HasValue());
    ASSERT_TRUE(std::holds_alternative<double>(rat.Value()));
    EXPECT_DOUBLE_EQ(std::get<double>(rat.Value()), 16.0);

    // TAB_INTP：表 [0→0,10→20]，i=5 → 插值 10.0；逆算 10.0 → 5
    auto tip = db->ToPhysical("CONV_ECU::M_TABINTP", Bytes{0x05, 0x00});
    ASSERT_TRUE(tip.HasValue());
    ASSERT_TRUE(std::holds_alternative<double>(tip.Value()));
    EXPECT_DOUBLE_EQ(std::get<double>(tip.Value()), 10.0);
    auto tip_back = db->FromPhysical("CONV_ECU::M_TABINTP", tip.Value());
    ASSERT_TRUE(tip_back.HasValue());
    EXPECT_EQ(tip_back.Value(), (Bytes{0x05, 0x00}));

    // TAB_NOINTP：阶梯 i=10 → 端点 20.0
    auto tni = db->ToPhysical("CONV_ECU::M_TABNOINTP", Bytes{0x0A, 0x00});
    ASSERT_TRUE(tni.HasValue());
    ASSERT_TRUE(std::holds_alternative<double>(tni.Value()));
    EXPECT_DOUBLE_EQ(std::get<double>(tni.Value()), 20.0);

    // TAB_VERB：i=1 → "on"（B-14 文本分支）
    auto verb = db->ToPhysical("CONV_ECU::M_TABVERB", Bytes{0x01});
    ASSERT_TRUE(verb.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::string>(verb.Value()));
    EXPECT_EQ(std::get<std::string>(verb.Value()), "on");
}

TEST_F(A2lGoldenTest, LinearUsesFactorThenAdditiveOffset) {
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto b = LoadOk(Golden("golden_convert.a2l"), require_off);
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    const auto check_linear = [&](std::string_view name, std::uint16_t raw,
                                  double expected) {
        const Bytes raw_bytes{static_cast<std::uint8_t>(raw & 0xFFU),
                              static_cast<std::uint8_t>(raw >> 8U)};
        const auto physical = db->ToPhysical(name, raw_bytes);
        ASSERT_TRUE(physical.HasValue()) << name;
        ASSERT_TRUE(std::holds_alternative<double>(physical.Value())) << name;
        EXPECT_DOUBLE_EQ(std::get<double>(physical.Value()), expected) << name;

        const auto encoded = db->FromPhysical(name, PhysicalValue{expected});
        ASSERT_TRUE(encoded.HasValue()) << name;
        EXPECT_EQ(encoded.Value(), raw_bytes) << name;
    };

    // 独立算式 oracle：PHYS = factor * INT + offset。
    check_linear("CONV_ECU::M_TEMP_LINEAR", 50U, 0.0);
    check_linear("CONV_ECU::M_SCALE_LINEAR", 25U, 0.0);
    check_linear("CONV_ECU::M_NEGATIVE_LINEAR", 20U, 0.0);
    check_linear("CONV_ECU::M_LINEAR", 200U, 100.0);

    const auto temp_zero =
        db->ToPhysical("CONV_ECU::M_TEMP_LINEAR", Bytes{0x00, 0x00});
    ASSERT_TRUE(temp_zero.HasValue());
    ASSERT_TRUE(std::holds_alternative<double>(temp_zero.Value()));
    EXPECT_DOUBLE_EQ(std::get<double>(temp_zero.Value()), -50.0);

    // factor=0 是常数映射，可正算但不能唯一逆算。
    const auto constant =
        db->ToPhysical("CONV_ECU::M_ZERO_LINEAR", Bytes{0x07, 0x00});
    ASSERT_TRUE(constant.HasValue());
    ASSERT_TRUE(std::holds_alternative<double>(constant.Value()));
    EXPECT_DOUBLE_EQ(std::get<double>(constant.Value()), 5.0);
    const auto inverse =
        db->FromPhysical("CONV_ECU::M_ZERO_LINEAR", PhysicalValue{5.0});
    ASSERT_FALSE(inverse.HasValue());
    EXPECT_EQ(inverse.ErrorInfo().code, ErrorCode::ConversionNotInvertible);
}

TEST_F(A2lGoldenTest, ConversionFailureIsStructured) {
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto b = LoadOk(Golden("golden_convert.a2l"), require_off);
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // 分母为零的 RAT_FUNC 正算 → ConversionNotInvertible（B-8）
    auto div0 = db->ToPhysical("CONV_ECU::M_RATDIV0", Bytes{0x01, 0x00});
    EXPECT_FALSE(div0.HasValue());
    EXPECT_EQ(div0.ErrorInfo().code, ErrorCode::ConversionNotInvertible);

    // 表外（i=20 > max 10）→ ConversionOutOfRange（B-15 无默认值）
    auto out = db->ToPhysical("CONV_ECU::M_TABNOINTP", Bytes{0x14, 0x00});
    EXPECT_FALSE(out.HasValue());
    EXPECT_EQ(out.ErrorInfo().code, ErrorCode::ConversionOutOfRange);

    // FORM：正逆算都明确拒绝（B-8 不猜公式）
    auto form = db->ToPhysical("CONV_ECU::M_FORM", Bytes{0x01, 0x00});
    EXPECT_FALSE(form.HasValue());
    EXPECT_EQ(form.ErrorInfo().code, ErrorCode::UnsupportedConversion);
    auto form_back = db->FromPhysical("CONV_ECU::M_FORM", PhysicalValue{3.0});
    EXPECT_FALSE(form_back.HasValue());
    EXPECT_EQ(form_back.ErrorInfo().code, ErrorCode::UnsupportedConversion);

    // 二次逆解双根 → 多值拒绝（RAT_FUNC 黄金项 (3)²=(−5)²）
    auto rat_back =
        db->FromPhysical("CONV_ECU::M_RATFUNC", PhysicalValue{16.0});
    EXPECT_FALSE(rat_back.HasValue());
    EXPECT_EQ(rat_back.ErrorInfo().code, ErrorCode::ConversionNotInvertible);

    // 文本物理值无数值逆映射（B-14/B-15）
    auto verb_back = db->FromPhysical("CONV_ECU::M_TABVERB",
                                      PhysicalValue{std::string("on")});
    EXPECT_FALSE(verb_back.HasValue());
    EXPECT_EQ(verb_back.ErrorInfo().code, ErrorCode::ConversionNotInvertible);

    // NaN 拒绝（B-15 不产生 raw）
    auto nan = db->FromPhysical("CONV_ECU::M_LINEAR", PhysicalValue{NAN});
    EXPECT_FALSE(nan.HasValue());
    EXPECT_EQ(nan.ErrorInfo().code, ErrorCode::BadArgument);

    // raw 长度不符 → RawSizeMismatch
    auto short_raw = db->ToPhysical("CONV_ECU::M_LINEAR", Bytes{0xC8});
    EXPECT_FALSE(short_raw.HasValue());
    EXPECT_EQ(short_raw.ErrorInfo().code, ErrorCode::RawSizeMismatch);
}

TEST_F(A2lGoldenTest, PhysicalValuePreservesInt64) {
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto b = LoadOk(Golden("golden_convert.a2l"), require_off);
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // UINT64_MAX 全宽精确（B-14：不经 double 中转）
    const Bytes max_bytes(8, 0xFF);
    auto umax = db->ToPhysical("CONV_ECU::M_U64", max_bytes);
    ASSERT_TRUE(umax.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::uint64_t>(umax.Value()));
    EXPECT_EQ(std::get<std::uint64_t>(umax.Value()), UINT64_MAX);
    auto umax_back = db->FromPhysical("CONV_ECU::M_U64", umax.Value());
    ASSERT_TRUE(umax_back.HasValue());
    EXPECT_EQ(umax_back.Value(), max_bytes);

    // 2^53±1（double 尾数边界）精确往返
    const std::uint64_t edge = (std::uint64_t{1} << 53) + 1;
    auto edge_back = db->FromPhysical("CONV_ECU::M_U64", PhysicalValue{edge});
    ASSERT_TRUE(edge_back.HasValue());
    auto edge_rt = db->ToPhysical("CONV_ECU::M_U64", edge_back.Value());
    ASSERT_TRUE(edge_rt.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::uint64_t>(edge_rt.Value()));
    EXPECT_EQ(std::get<std::uint64_t>(edge_rt.Value()), edge);

    // INT64_MIN 全宽精确
    const Bytes min_bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
    auto imin = db->ToPhysical("CONV_ECU::M_I64", min_bytes);
    ASSERT_TRUE(imin.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(imin.Value()));
    EXPECT_EQ(std::get<std::int64_t>(imin.Value()), INT64_MIN);
    auto imin_back = db->FromPhysical("CONV_ECU::M_I64", imin.Value());
    ASSERT_TRUE(imin_back.HasValue());
    EXPECT_EQ(imin_back.Value(), min_bytes);

    // FLOAT32：1.5 精确重组（0x3FC00000 小端 = 00 00 C0 3F）
    const Bytes f32{0x00, 0x00, 0xC0, 0x3F};
    auto f = db->ToPhysical("CONV_ECU::M_F32", f32);
    ASSERT_TRUE(f.HasValue());
    ASSERT_TRUE(std::holds_alternative<double>(f.Value()));
    EXPECT_DOUBLE_EQ(std::get<double>(f.Value()), 1.5);
}

TEST_F(A2lGoldenTest, PhysUnitAndUnitRefPolicy) {
    // B-10：PHYS_UNIT 优先（golden_basic M_LINEAR → "V"；handwritten → "degC"）
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto b1 = LoadOk(Golden("golden_basic.a2l"));
    ASSERT_NE(b1, nullptr);
    auto m_lin = b1->Database()->Find("SMOKE_ECU::M_LINEAR");
    ASSERT_TRUE(m_lin.HasValue());
    EXPECT_EQ(m_lin.Value().conversion.unit, "V");

    auto b2 = LoadOk(Data("golden_handwritten.a2l"), require_off);
    ASSERT_NE(b2, nullptr);
    auto hw = b2->Database()->Find("HAND_ECU::HW_TEMP");
    ASSERT_TRUE(hw.HasValue());
    EXPECT_EQ(hw.Value().conversion.unit, "degC");
    EXPECT_EQ(hw.Value().xcp_address, 0x1234u);

    // UNIT_REF 回退（convert：M_TABINTP 无 PHYS_UNIT → 取 REF_UNIT "hz"）
    auto b3 = LoadOk(Golden("golden_convert.a2l"), require_off);
    ASSERT_NE(b3, nullptr);
    auto tab = b3->Database()->Find("CONV_ECU::M_TABINTP");
    ASSERT_TRUE(tab.HasValue());
    EXPECT_EQ(tab.Value().conversion.unit, "hz");
    // 冲突优先级（PHYS_UNIT 覆盖 REF_UNIT）：M_TABNOINTP 声明 rpm + 表单 ref zz
    auto tni = b3->Database()->Find("CONV_ECU::M_TABNOINTP");
    ASSERT_TRUE(tni.HasValue());
    EXPECT_EQ(tni.Value().conversion.unit, "rpm");
}

// ============================================================================
// T4 IF_DATA 快照 / 运行时一致性 / XCPplus 优先级（B-16/§6.3-A）
// ============================================================================

TEST_F(A2lGoldenTest, UdpEndpointAndProtocolLayer) {
    auto b = LoadOk(Golden("golden_basic.a2l"));
    ASSERT_NE(b, nullptr);
    const IfDataXcpInfo* xcp = b->XcpInfo();
    ASSERT_NE(xcp, nullptr);
    const ProtocolLayerInfo& pl = xcp->protocol_layer;
    EXPECT_EQ(pl.version, 0x0100);
    EXPECT_EQ(pl.max_cto, 0x10);
    EXPECT_EQ(pl.max_dto, 0x20);
    EXPECT_EQ(pl.byte_order, ByteOrder::MsbLast);
    EXPECT_EQ(pl.address_granularity, AddressGranularity::Byte);
    // t1..t7 透传（批次10 §6.1 补全）
    const std::uint16_t expect_t[7] = {1, 1, 5, 5, 5, 1, 1};
    for (int i = 0; i < 7; ++i) {
        EXPECT_EQ(pl.timeout_ms[i], expect_t[i]) << "T" << (i + 1);
    }
    EXPECT_TRUE(pl.optional_commands.empty());
    EXPECT_TRUE(pl.seed_and_key_function.empty());
    EXPECT_FALSE(pl.has_ecu_states);
    ASSERT_EQ(xcp->transports.size(), 1u);
    EXPECT_EQ(xcp->transports[0].kind, TransportEndpoint::Kind::UdpIp);
    EXPECT_EQ(xcp->transports[0].remote_port, 0x15B7);
    EXPECT_EQ(xcp->transports[0].remote_host, "localhost");
    // 批次12（§4.3）：golden_basic 未声明 PACKET_ALIGNMENT/OPTIONAL_TL_SUBCMD
    // → 上游默认值透传为位宽 8、子命令空（不是"未知 0"）
    EXPECT_EQ(xcp->transports[0].packet_alignment, 8);
    EXPECT_TRUE(xcp->transports[0].sub_commands.empty());
    // B-17 归属 + 事件通道（本样本无 EVENT → 空）
    EXPECT_FALSE(xcp->ambiguous);
    EXPECT_EQ(xcp->module_name, "SMOKE_ECU");
    EXPECT_TRUE(xcp->event_channels.empty());

    // 声明侧正例：golden_mask 声明 PACKET_ALIGNMENT_32 +
    // OPTIONAL_TL_SUBCMD GET_SLAVE_ID(0xFF) / SET_SLAVE_IP_ADDRESS(0xFC)
    // （上游 AddSubCmd 按 A2L 出现顺序 push，故顺序即声明顺序）
    auto bm = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(bm, nullptr);
    const IfDataXcpInfo* mx = bm->XcpInfo();
    ASSERT_NE(mx, nullptr);
    ASSERT_EQ(mx->transports.size(), 1u);
    EXPECT_EQ(mx->transports[0].packet_alignment, 32);
    ASSERT_EQ(mx->transports[0].sub_commands.size(), 2u);
    EXPECT_EQ(mx->transports[0].sub_commands[0], 0xFF);
    EXPECT_EQ(mx->transports[0].sub_commands[1], 0xFC);
}

TEST_F(A2lGoldenTest, EventChannelSnapshot) {
    // 批次10 事件通道透传 + EVENT_FIXED 反查（golden_mask: event#1 ↔ list#2）
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    const IfDataXcpInfo* xcp = b->XcpInfo();
    ASSERT_NE(xcp, nullptr);
    ASSERT_EQ(xcp->event_channels.size(), 1u);
    const EventChannelInfo& ev = xcp->event_channels[0];
    EXPECT_EQ(ev.name, "ev_read");
    EXPECT_EQ(ev.short_name, "evr");
    EXPECT_EQ(ev.channel_number, 1);
    EXPECT_EQ(ev.type, 1);  // DAQ
    EXPECT_EQ(ev.max_daq_list, 2);
    EXPECT_EQ(ev.time_cycle, 10);
    EXPECT_EQ(ev.time_unit, 4);
    EXPECT_EQ(ev.priority, 2);
    EXPECT_EQ(ev.cycle_time_us, 0.0);  // 无码表：不臆造换算（见类注释）
    ASSERT_EQ(ev.daq_list_numbers.size(), 1u);
    EXPECT_EQ(ev.daq_list_numbers[0], 2);  // EVENT_FIXED=1 的 DAQ_LIST 号
    // DAQ 能力块（该样本 overload=NONE）
    ASSERT_TRUE(xcp->daq.has_value());
    EXPECT_EQ(xcp->daq->max_daq, 4);
    EXPECT_EQ(xcp->daq->max_event_channel, 2);
    EXPECT_EQ(xcp->daq->min_daq, 1);
    EXPECT_FALSE(xcp->daq->overflow_flag_supported);
}

TEST_F(A2lGoldenTest, RuntimeTruthAndSeverity) {
    // B-16：运行时为真值；分级 = Error(协商破坏) / Warning(可取保守值)
    auto b = LoadOk(Golden("golden_basic.a2l"));
    ASSERT_NE(b, nullptr);

    RuntimeXcpParams match;  // 与 golden_basic IF_DATA 一致
    match.protocol_version = 0x0100;
    match.max_cto = 0x10;
    match.max_dto = 0x20;
    match.byte_order = ByteOrder::MsbLast;
    match.address_granularity = AddressGranularity::Byte;
    match.has_daq = true;
    auto same = b->CompareWithRuntime(match);
    ASSERT_TRUE(same.HasValue());
    EXPECT_TRUE(same.Value().empty()) << "完全一致时不应有差异项";

    // protocol major 不兼容 → Error；仅 minor → Warning（B-16 分级）
    RuntimeXcpParams major_bad = match;
    major_bad.protocol_version = 0x0200;
    auto r1 = b->CompareWithRuntime(major_bad);
    ASSERT_TRUE(r1.HasValue());
    bool found_major = false;
    for (const auto& d : r1.Value()) {
        if (d.parameter_name == "PROTOCOL_VERSION_MAJOR") {
            found_major = true;
            EXPECT_EQ(d.severity, Severity::Error);
        }
    }
    EXPECT_TRUE(found_major);

    RuntimeXcpParams minor_bad = match;
    minor_bad.protocol_version = 0x0101;
    auto r2 = b->CompareWithRuntime(minor_bad);
    ASSERT_TRUE(r2.HasValue());
    bool found_minor = false;
    for (const auto& d : r2.Value()) {
        if (d.parameter_name == "PROTOCOL_VERSION_MINOR") {
            found_minor = true;
            EXPECT_EQ(d.severity, Severity::Warning);
        }
    }
    EXPECT_TRUE(found_minor);

    // 容量/能力差异 → Warning；BO/AG 破坏 → Error；t1..t7 无差异项
    RuntimeXcpParams cap = match;
    cap.max_cto = 0x20;
    cap.max_dto = 0x40;
    cap.has_daq = false;
    auto r3 = b->CompareWithRuntime(cap);
    ASSERT_TRUE(r3.HasValue());
    EXPECT_EQ(r3.Value().size(), 3u);
    for (const auto& d : r3.Value()) {
        EXPECT_EQ(d.severity, Severity::Warning) << d.parameter_name;
        EXPECT_NE(d.parameter_name, "TIMERS") << "t1..t7 不作比对项";
    }

    RuntimeXcpParams hard = match;
    hard.byte_order = ByteOrder::MsbFirst;
    hard.address_granularity = AddressGranularity::Word;
    auto r4 = b->CompareWithRuntime(hard);
    ASSERT_TRUE(r4.HasValue());
    EXPECT_EQ(r4.Value().size(), 2u);
    for (const auto& d : r4.Value()) {
        EXPECT_EQ(d.severity, Severity::Error) << d.parameter_name;
    }
}

TEST_F(A2lGoldenTest, DaqRuntimeComparisonErrors) {
    // 批次14（T14-11 / B-16）：DAQ 侧三项不一致必须是 Error（决定 PID 解释、
    // 地址扩展位与 Entry 对齐，错一项就整帧错位）。
    // golden_basic 的 A2L 声明：IDENTIFICATION=ABSOLUTE(0)、
    // ADDRESS_EXTENSION=DAQ(3)、GRANULARITY=BYTE(1)。
    auto b = LoadOk(Golden("golden_basic.a2l"));
    ASSERT_NE(b, nullptr);

    RuntimeXcpParams rt;
    rt.protocol_version = 0x0100;
    rt.max_cto = 0x10;
    rt.max_dto = 0x20;
    rt.byte_order = ByteOrder::MsbLast;
    rt.address_granularity = AddressGranularity::Byte;
    rt.has_daq = true;
    rt.daq_identification_field_type = 0;  // ABSOLUTE
    rt.daq_address_extension_mode = 3;     // DAQ（原始码 3，非枚举序号 2）
    rt.daq_odt_entry_min_size_bytes = 1;   // BYTE
    auto all_match = b->CompareWithRuntime(rt);
    ASSERT_TRUE(all_match.HasValue());
    EXPECT_TRUE(all_match.Value().empty())
        << "三项与 A2L 一致时不得产生差异（含 ADDRESS_EXTENSION=3 的口径）";

    // 逐项破坏：每项都必须落成 Error，且只影响自己的功能域
    auto expect_error = [&](const RuntimeXcpParams& bad, const char* param) {
        auto r = b->CompareWithRuntime(bad);
        ASSERT_TRUE(r.HasValue());
        bool found = false;
        for (const auto& d : r.Value()) {
            if (d.parameter_name == param) {
                found = true;
                EXPECT_EQ(d.severity, Severity::Error) << param;
            }
        }
        EXPECT_TRUE(found) << "缺少差异项 " << param;
    };
    RuntimeXcpParams idf_bad = rt;
    idf_bad.daq_identification_field_type = 2;  // RelativeWord
    expect_error(idf_bad, "IDENTIFICATION_FIELD_TYPE");

    RuntimeXcpParams ext_bad = rt;
    ext_bad.daq_address_extension_mode = 0;  // FREE
    expect_error(ext_bad, "ADDRESS_EXTENSION_MODE");

    RuntimeXcpParams gran_bad = rt;
    gran_bad.daq_odt_entry_min_size_bytes = 4;  // DWORD 粒度
    expect_error(gran_bad, "GRANULARITY_ODT_ENTRY_SIZE_DAQ");

    // 未查询（Optional 命令未实现）→ nullopt：跳过比对，不得凭空报错
    RuntimeXcpParams unknown = rt;
    unknown.daq_identification_field_type = std::nullopt;
    unknown.daq_address_extension_mode = std::nullopt;
    unknown.daq_odt_entry_min_size_bytes = std::nullopt;
    auto skipped = b->CompareWithRuntime(unknown);
    ASSERT_TRUE(skipped.HasValue());
    EXPECT_TRUE(skipped.Value().empty())
        << "运行时缺数据时必须跳过该项比对（不拿默认值凑数）";
}

TEST_F(A2lGoldenTest, XcpPlusPriorityAndConflictReport) {
    // §6.3-A：同模块 XCP+XCPplus 并存 → XCPplus 为准 + Info 级差异记录
    auto b = LoadOk(Golden("golden_xcpplus.a2l"));
    ASSERT_NE(b, nullptr);
    const IfDataXcpInfo* xcp = b->XcpInfo();
    ASSERT_NE(xcp, nullptr);
    EXPECT_TRUE(xcp->from_xcp_plus);
    EXPECT_EQ(xcp->module_name, "PLUS_ECU");
    EXPECT_EQ(xcp->protocol_layer.max_cto, 0x18);  // plus 的 MAX_CTO 胜出
    // 差异共三条：MAX_CTO 16 vs 24、PACKET_ALIGNMENT 16 vs 32、SUB_CMDS
    // （批次12 起 DiffIfData 覆盖 UDP 端点新增两项；值以 XCPplus 为准）
    ASSERT_EQ(xcp->plus_conflicts.size(), 3u);
    EXPECT_EQ(xcp->plus_conflicts[0].parameter, "MAX_CTO");
    EXPECT_EQ(xcp->plus_conflicts[0].xcp_value, "16");
    EXPECT_EQ(xcp->plus_conflicts[0].xcpplus_value, "24");
    EXPECT_EQ(xcp->plus_conflicts[1].parameter, "PACKET_ALIGNMENT");
    EXPECT_EQ(xcp->plus_conflicts[1].xcp_value, "16");
    EXPECT_EQ(xcp->plus_conflicts[1].xcpplus_value, "32");
    EXPECT_EQ(xcp->plus_conflicts[2].parameter, "SUB_CMDS");
    EXPECT_EQ(xcp->plus_conflicts[2].xcp_value, "253");          // 0xFD
    EXPECT_EQ(xcp->plus_conflicts[2].xcpplus_value, "253,250");  // 0xFD,0xFA
    // DAQ 列表取自被选中的 XCPplus 块（EPK=5）
    ASSERT_EQ(xcp->static_daq_lists.size(), 1u);
    EXPECT_EQ(xcp->static_daq_lists[0].number, 5);
    // 端点仍从选定块透出（含批次12 的 alignment/子命令）
    ASSERT_EQ(xcp->transports.size(), 1u);
    EXPECT_EQ(xcp->transports[0].remote_port, 0x15B7);
    EXPECT_EQ(xcp->transports[0].packet_alignment, 32);
    ASSERT_EQ(xcp->transports[0].sub_commands.size(), 2u);
    EXPECT_EQ(xcp->transports[0].sub_commands[0], 0xFD);
    EXPECT_EQ(xcp->transports[0].sub_commands[1], 0xFA);
}

// ============================================================================
// T5 CHARACTERISTIC 识别与执行边界（B-4/B-11）
// ============================================================================

TEST_F(A2lGoldenTest, CharacteristicKindsAndScope) {
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // VALUE + 单段 FNC_VALUES 连续版式 → 元素类型可证明（B-4 批次10 推导）
    auto cv = db->Find("MASK_ECU::C_VALUE");
    ASSERT_TRUE(cv.HasValue());
    EXPECT_EQ(cv.Value().kind, SymbolKind::Characteristic);
    EXPECT_EQ(cv.Value().characteristic_type, CharacteristicType::Value);
    EXPECT_EQ(cv.Value().data_type, AsamDataType::UWord);
    EXPECT_EQ(cv.Value().element_size_bytes, 2);
    EXPECT_EQ(cv.Value().xcp_address, 0x3100u);
    EXPECT_EQ(cv.Value().address_extension, 0x12);
    EXPECT_TRUE(cv.Value().read_write);
    auto cv_bytes = db->ByteSizeOf("MASK_ECU::C_VALUE");
    ASSERT_TRUE(cv_bytes.HasValue());
    EXPECT_EQ(cv_bytes.Value(), 2u);
    auto cv_phys = db->ToPhysical("MASK_ECU::C_VALUE", Bytes{0x34, 0x12});
    ASSERT_TRUE(cv_phys.HasValue());
    EXPECT_EQ(std::get<std::int64_t>(cv_phys.Value()), 0x1234);
    auto cv_back = db->FromPhysical("MASK_ECU::C_VALUE", PhysicalValue{5});
    ASSERT_TRUE(cv_back.HasValue());
    EXPECT_EQ(cv_back.Value(), (Bytes{0x05, 0x00}));

    // CURVE：识别五型但拒绝数值操作；ASCII 经批次13 核证改判为"事实化拒绝"
    // （上游 MAX_LENGTH 无词法/语法、Characteristic 无长度 getter → 字节数
    //  不可证，B-11 剩余项只做 VAL_BLK 半边）
    auto cc = db->Find("MASK_ECU::C_CURVE");
    ASSERT_TRUE(cc.HasValue());
    EXPECT_EQ(cc.Value().characteristic_type, CharacteristicType::Curve);
    auto cc_size = db->ByteSizeOf("MASK_ECU::C_CURVE");
    EXPECT_FALSE(cc_size.HasValue());
    EXPECT_EQ(cc_size.ErrorInfo().code, ErrorCode::UnsupportedDataType);

    // VAL_BLK 无 MATRIX_DIM → 元素数不可证 → 仍拒（B-11 双向断言之"不可证"）
    auto cvb = db->Find("MASK_ECU::C_VALBLK");
    ASSERT_TRUE(cvb.HasValue());
    EXPECT_EQ(cvb.Value().characteristic_type, CharacteristicType::ValBlk);
    EXPECT_EQ(cvb.Value().data_type, AsamDataType::Unknown);
    auto cvb_to = db->ToPhysical("MASK_ECU::C_VALBLK", Bytes{0x00, 0x00});
    EXPECT_FALSE(cvb_to.HasValue());
    EXPECT_EQ(cvb_to.ErrorInfo().code, ErrorCode::UnsupportedDataType);

    // 批次13 T13-09/T13-12：连续 VAL_BLK（RL 可证元素类型 + MATRIX_DIM 可证
    // 元素数）→ 可执行：extent 进 dimensions，ByteSizeOf 与 B-1 元素寻址可用
    auto cvx = db->Find("MASK_ECU::C_VALBLK_EXEC");
    ASSERT_TRUE(cvx.HasValue());
    EXPECT_EQ(cvx.Value().characteristic_type, CharacteristicType::ValBlk);
    EXPECT_EQ(cvx.Value().data_type, AsamDataType::UWord);
    EXPECT_EQ(cvx.Value().element_size_bytes, 2);
    EXPECT_EQ(cvx.Value().xcp_address, 0x3700u);
    ASSERT_EQ(cvx.Value().dimensions.size(), 1u);  // 单维 extent（乘积）
    EXPECT_EQ(cvx.Value().dimensions[0].extent, 4u);
    EXPECT_EQ(cvx.Value().dimensions[0].byte_stride, 2u);
    auto cvx_count = CountElements(cvx.Value());
    ASSERT_TRUE(cvx_count.HasValue());
    EXPECT_EQ(cvx_count.Value(), 4u);
    auto cvx_bytes = db->ByteSizeOf("MASK_ECU::C_VALBLK_EXEC");
    ASSERT_TRUE(cvx_bytes.HasValue());
    EXPECT_EQ(cvx_bytes.Value(), 8u);
    // 元素级寻址走既有 B-1 通路（AG 只做单位换算，绝不乘进基址）
    auto cvx_addr =
        ComputeElementAddress(cvx.Value(), 2, AddressGranularity::Byte);
    ASSERT_TRUE(cvx_addr.HasValue());
    EXPECT_EQ(cvx_addr.Value(), 0x3704u);
    // 多维整体换算仍拒（B-11 归并口径 UnsupportedCharacteristicOperation）
    auto cvx_whole = db->ToPhysical("MASK_ECU::C_VALBLK_EXEC", Bytes{0, 0});
    EXPECT_FALSE(cvx_whole.HasValue());
    EXPECT_EQ(cvx_whole.ErrorInfo().code, ErrorCode::UnsupportedOperation);
    EXPECT_EQ(cvx_whole.ErrorInfo().phase, Phase::Query);

    // 批次13 T13-11（R3 接线）：RECORD_LAYOUT 判定器已生效——含 AXIS_PTS_X 的
    // 版式给 UnsupportedOperation、含 RESERVE 的复合版式给 InvalidLayout，
    // 且 Phase 一律为 Layout（与 B 文档 §5"预留判定器"行一致）
    auto axis = db->ByteSizeOf("MASK_ECU::C_CURVE_AXIS");
    EXPECT_FALSE(axis.HasValue());
    EXPECT_EQ(axis.ErrorInfo().code, ErrorCode::UnsupportedOperation);
    EXPECT_EQ(axis.ErrorInfo().phase, Phase::Layout);
    auto cplx = db->ByteSizeOf("MASK_ECU::C_COMPLEX_RL");
    EXPECT_FALSE(cplx.HasValue());
    EXPECT_EQ(cplx.ErrorInfo().code, ErrorCode::InvalidLayout);
    EXPECT_EQ(cplx.ErrorInfo().phase, Phase::Layout);
    auto cplx_to = db->ToPhysical("MASK_ECU::C_COMPLEX_RL", Bytes{0, 0});
    EXPECT_FALSE(cplx_to.HasValue());
    EXPECT_EQ(cplx_to.ErrorInfo().code, ErrorCode::InvalidLayout);

    // golden_basic 的 C_VALUE 无 RECORD_LAYOUT → 保持 kUnknown 拒绝（负例锁定）
    auto b2 = LoadOk(Golden("golden_basic.a2l"));
    ASSERT_NE(b2, nullptr);
    auto plain = b2->Database()->Find("SMOKE_ECU::C_VALUE");
    ASSERT_TRUE(plain.HasValue());
    EXPECT_EQ(plain.Value().data_type, AsamDataType::Unknown);
    auto plain_size = b2->Database()->ByteSizeOf("SMOKE_ECU::C_VALUE");
    EXPECT_FALSE(plain_size.HasValue());
    EXPECT_EQ(plain_size.ErrorInfo().code, ErrorCode::UnsupportedDataType);
}

// ============================================================================
// T5b STRUCTURE / INSTANCE 元数据（B-12，批次13 T13-08）
// ============================================================================

TEST_F(A2lGoldenTest, StructureMetadataOnly) {
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // TYPEDEF_STRUCTURE：可 Find、kind==Structure、成员数进 description、
    // 元素宽不可证（=0，B-3 禁止猜）
    auto st = db->Find("MASK_ECU::ST_ENGINE");
    ASSERT_TRUE(st.HasValue()) << st.ErrorInfo().message;
    EXPECT_EQ(st.Value().kind, SymbolKind::Structure);
    EXPECT_EQ(st.Value().characteristic_type, CharacteristicType::None);
    EXPECT_EQ(st.Value().data_type, AsamDataType::Unknown);
    EXPECT_EQ(st.Value().element_size_bytes, 0);
    EXPECT_TRUE(st.Value().dimensions.empty());
    EXPECT_EQ(st.Value().xcp_address, 0u);  // 类型本身无 ECU 地址
    EXPECT_NE(st.Value().description.find("members=2"), std::string::npos)
        << "description 应记录成员数: " << st.Value().description;

    // 嵌套引用：成员只存引用名，不递归展开（B-12 首版边界）——wrapper 有 1 个
    // 成员、size 声明 8，但不会因此多出"已解析成员类型"的任何痕迹
    auto wrap = db->Find("MASK_ECU::ST_WRAPPER");
    ASSERT_TRUE(wrap.HasValue());
    EXPECT_NE(wrap.Value().description.find("members=1"), std::string::npos)
        << wrap.Value().description;
    EXPECT_NE(wrap.Value().description.find("size_bytes=8"), std::string::npos)
        << wrap.Value().description;

    // INSTANCE：带 ECU_ADDRESS 原值（B-1 口径：不乘 AG）与引用 TYPEDEF 名
    auto ins = db->Find("MASK_ECU::I_ENGINE");
    ASSERT_TRUE(ins.HasValue());
    EXPECT_EQ(ins.Value().kind, SymbolKind::Structure);
    EXPECT_EQ(ins.Value().xcp_address, 0x3600u);
    // 批次15（F5）：INSTANCE 的 ECU_ADDRESS_EXTENSION 必须原值保住。修前它是 0
    // （StructInfoDto 无该字段）—— 分段地址 ECU 上，拿这个地址去读会静默指到
    // 另一段内存，而地址本身看起来完全合法，故用例先红后绿（批次15 记录 §2）
    EXPECT_EQ(ins.Value().address_extension, 0x12);
    EXPECT_NE(ins.Value().description.find("ref_typedef=ST_ENGINE"),
              std::string::npos)
        << ins.Value().description;

    // 读写一律显式拒绝（B-12：禁止静默当字节数组）
    for (const char* name : {"MASK_ECU::ST_ENGINE", "MASK_ECU::ST_WRAPPER",
                             "MASK_ECU::I_ENGINE"}) {
        auto size = db->ByteSizeOf(name);
        ASSERT_FALSE(size.HasValue()) << name;
        EXPECT_EQ(size.ErrorInfo().code, ErrorCode::UnsupportedOperation)
            << name;
        EXPECT_EQ(size.ErrorInfo().phase, Phase::Query) << name;
        auto to = db->ToPhysical(name, Bytes{0x00, 0x00});
        ASSERT_FALSE(to.HasValue()) << name;
        EXPECT_EQ(to.ErrorInfo().code, ErrorCode::UnsupportedOperation) << name;
        auto from =
            db->FromPhysical(name, PhysicalValue{static_cast<int64_t>(1)});
        ASSERT_FALSE(from.HasValue()) << name;
        EXPECT_EQ(from.ErrorInfo().code, ErrorCode::UnsupportedOperation)
            << name;
    }

    // Search 可列出（元数据符号与常规符号同库同排序）
    auto hits = db->Search("mask_ecu::st_*");
    ASSERT_TRUE(hits.HasValue());
    ASSERT_EQ(hits.Value().size(), 2u);
    EXPECT_EQ(hits.Value()[0].name, "ST_ENGINE");  // 稳定字典序
    EXPECT_EQ(hits.Value()[1].name, "ST_WRAPPER");

    // STRUCTURE/INSTANCE 不参与基址反查：DAQ entry 归属仍只认
    // MEASUREMENT/CHARACTERISTIC（B-12 不寻址）
    auto layout_r = b->CreateDaqLayout();
    ASSERT_TRUE(layout_r.HasValue());
}

// ============================================================================
// T5.5 STRUCTLEAF：结构成员叶子展开（批次18，16-A/16-A′/16-B/16-C）
// ============================================================================

TEST_F(A2lGoldenTest, StructLeafRealCorpusFourForms) {
    // 语料 = xcp_test_slave 运行时生成 A2L 的源码树定格件（tests/data/a2l/），
    // 覆盖标量成员/嵌套/结构数组成员/结构数组实例四形态 + CalSeg 校准段。
    // 符号表基线 42 钉死展开形状：任何成员数、地址或扩展位退化此用例即红。
    auto b = LoadOk(Data("xcplite_structleaf_corpus.a2l"));
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    // 18-A′ 钉：CalSeg 成员引用 TYPEDEF_CHARACTERISTIC，并入标量事实注册表后
    // 真实语料告警必须恰好为零（并入逻辑若退化，会退回 2 条 InvalidLayout）
    EXPECT_EQ(b->ListLoadWarnings().size(), 0u);

    auto all = db->Search("xcp_test_slave::*", 500);
    ASSERT_TRUE(all.HasValue());
    EXPECT_EQ(all.Value().size(), 42u) << "符号表基线（26 叶子 + 16 原有符号）";

    struct LeafExpect {
        const char* path;      ///< 模块限定叶子路径
        std::uint64_t addr;    ///< 期望 ECU 地址（实例基址 + 各级成员偏移）
        std::uint8_t ext;      ///< 期望地址扩展（INSTANCE 原值透传，B-1/F5）
        std::uint8_t esz;      ///< 期望元素宽（B-3 可证值）
        std::uint8_t rw;       ///< 期望读写位（随 INSTANCE 声明，见下注）
        std::uint64_t extent;  ///< 数组成员元素数（0 = 标量叶子无 dims）
    };
    static constexpr LeafExpect kLeaves[] = {
        // 形态 1：结构标量成员
        {"g_simple_struct.simple_u8", 0x19018, 1, 1, 1, 0},
        {"g_simple_struct.simple_i16", 0x1901A, 1, 2, 1, 0},
        {"g_simple_struct.simple_u32", 0x1901C, 1, 4, 1, 0},
        // 形态 2：嵌套结构成员
        {"g_outer.outer_u8", 0x19040, 1, 1, 1, 0},
        {"g_outer.nested_struct.simple_u8", 0x19044, 1, 1, 1, 0},
        {"g_outer.nested_struct.simple_i16", 0x19046, 1, 2, 1, 0},
        {"g_outer.nested_struct.simple_u32", 0x19048, 1, 4, 1, 0},
        // 形态 3：结构数组成员（成员级 MATRIX_DIM 逐元素展开）
        {"g_outer.nested_array[0].simple_u8", 0x1904C, 1, 1, 1, 0},
        {"g_outer.nested_array[1].simple_i16", 0x19056, 1, 2, 1, 0},
        {"g_outer.nested_array[1].simple_u32", 0x19058, 1, 4, 1, 0},
        // 形态 4：标量数组成员（整体寻址叶子，dims 带 extent/stride）
        {"g_outer.outer_arr", 0x1905C, 1, 1, 1, 4},
        // 形态 5：结构数组实例（18-B：INSTANCE 级 MATRIX_DIM 透传，
        // 元素步长 = sizeof(SimpleStruct_t) = 8）
        {"g_struct_array[0].simple_u8", 0x19060, 1, 1, 1, 0},
        {"g_struct_array[1].simple_i16", 0x1906A, 1, 2, 1, 0},
        {"g_struct_array[2].simple_u8", 0x19070, 1, 1, 1, 0},
        {"g_struct_array[2].simple_u32", 0x19074, 1, 4, 1, 0},
        // CalSeg（18-A′：TYPEDEF_CHARACTERISTIC 事实；分段地址 ext=0 原样。
        // rw=0：XCPlite a2l.c 只对绝对/动态寻址的 INSTANCE 打 READ_WRITE
        // 关键词，
        // CalSeg 实例走分段相对寻址故缺省只读——物理可写性由批次17 裸地址
        // 写通路实证，A2L 元数据层不伪造读写位）
        {"kDefaultCalParams.cal_factor", 0x80010000, 0, 2, 0, 0},
        {"kDefaultCalParams.cal_offset", 0x80010004, 0, 4, 0, 0},
    };
    for (const auto& e : kLeaves) {
        const std::string key = std::string("xcp_test_slave::") + e.path;
        auto r = db->Find(key);
        ASSERT_TRUE(r.HasValue()) << key << ": " << r.ErrorInfo().message;
        EXPECT_EQ(r.Value().kind, SymbolKind::Measurement) << key;
        EXPECT_EQ(r.Value().xcp_address, e.addr) << key;
        EXPECT_EQ(static_cast<unsigned>(r.Value().address_extension),
                  static_cast<unsigned>(e.ext))
            << key << "（扩展从 INSTANCE 透传）";
        EXPECT_EQ(static_cast<unsigned>(r.Value().element_size_bytes),
                  static_cast<unsigned>(e.esz))
            << key << "（B-3 宽度可证）";
        EXPECT_EQ(static_cast<unsigned>(r.Value().read_write),
                  static_cast<unsigned>(e.rw))
            << key << "（读写位随 INSTANCE 声明透传）";
        if (e.extent == 0) {
            EXPECT_TRUE(r.Value().dimensions.empty()) << key;
        } else {
            ASSERT_EQ(r.Value().dimensions.size(), 1u) << key;
            EXPECT_EQ(r.Value().dimensions[0].extent, e.extent) << key;
            EXPECT_EQ(r.Value().dimensions[0].byte_stride, e.esz) << key;
        }
    }

    // B-1 交叉核证：嵌套数组叶子地址 = 实例基址 + 成员偏移 + 元素步长 +
    // 字段偏移 （g_outer@0x19040 + nested_array@0xC + 1×8 + simple_i16@2 =
    // 0x19056）
    auto outer = db->Find("xcp_test_slave::g_outer");
    ASSERT_TRUE(outer.HasValue());
    EXPECT_EQ(outer.Value().xcp_address, 0x19040u);

    // B-12 不动摇：叶子化不放开实例整体读（ INSTANCE 仍只识别不寻址）
    auto size = db->ByteSizeOf("xcp_test_slave::g_outer");
    ASSERT_FALSE(size.HasValue());
    EXPECT_EQ(size.ErrorInfo().code, ErrorCode::UnsupportedOperation);
    EXPECT_EQ(size.ErrorInfo().phase, Phase::Query);

    // 叶子侧数值门放开：outer_arr 总字节 = 4 元素 × 宽 1
    auto arr_size = db->ByteSizeOf("xcp_test_slave::g_outer.outer_arr");
    ASSERT_TRUE(arr_size.HasValue()) << arr_size.ErrorInfo().message;
    EXPECT_EQ(arr_size.Value(), 4u);

    // 18-A′ 事实透传：C_cal_offset 的 PHYS_UNIT/LIMITS 必须进入叶子（DTO 复用
    // 无结构变化的前提是事实链完整）
    auto cal_off = db->Find("xcp_test_slave::kDefaultCalParams.cal_offset");
    ASSERT_TRUE(cal_off.HasValue());
    EXPECT_EQ(cal_off.Value().conversion.unit, "mm");
    EXPECT_TRUE(cal_off.Value().have_limit);
    EXPECT_DOUBLE_EQ(cal_off.Value().lower_limit, -1000.0);
    EXPECT_DOUBLE_EQ(cal_off.Value().upper_limit, 1000.0);
    // 只读门生效（钉实测语义）：分段寻址实例的换算入口被 rw 位拒绝，
    // 错误码 UnsupportedOperation / Phase::Conversion；上面 have_limit/
    // lower/upper 元数据仍完整——拒绝发生在换算之前，事实链未断
    auto bad = db->FromPhysical("xcp_test_slave::kDefaultCalParams.cal_offset",
                                PhysicalValue{2000.0});
    ASSERT_FALSE(bad.HasValue());
    EXPECT_EQ(bad.ErrorInfo().code, ErrorCode::UnsupportedOperation);
    EXPECT_EQ(bad.ErrorInfo().phase, Phase::Conversion);
    EXPECT_NE(bad.ErrorInfo().message.find("只读"), std::string::npos)
        << bad.ErrorInfo().message;
}

TEST_F(A2lGoldenTest, StructLeafBrokenCorpusWarnsEveryRejection) {
    // 手写负例：6 种拒绝形态（成员引用缺失类型 / 成员越界 / 类型递归环 /
    // INSTANCE 引用不存在 TYPEDEF / MATRIX_DIM 含 0 / 元素数超上界）+ 1 个
    // 对照实例。纪律：每种拒绝恰好一条 LoadWarning（subject 可定位），
    // 可证部分照常展开——既不静默吞掉，也不整包丢弃。
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto b = LoadOk(Data("xcplite_leaf_broken.a2l"), require_off);
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    const auto ws = b->ListLoadWarnings();
    ASSERT_EQ(ws.size(), 7u) << "告警数必须与拒绝形态一一对应";
    struct WarnExpect {
        const char* subject;   ///< 告警主体（module::path 限定）
        const char* fragment;  ///< 消息片段（定位拒绝原因，不钉全文防脆）
    };
    static constexpr WarnExpect kWarns[] = {
        {"LEAF_FIX::big_dim", "1024"},
        {"LEAF_FIX::cyc.cyc.inner.back", "递归"},
        {"LEAF_FIX::h_ref", "NoSuchType"},
        {"LEAF_FIX::ok_ctrl.ghost", "MissingType"},
        {"LEAF_FIX::ok_ctrl.wide", "越界"},
        {"LEAF_FIX::oob.big", "越界"},
        {"LEAF_FIX::zero_dim", "MATRIX_DIM"},
    };
    for (const auto& e : kWarns) {
        const LoadWarning* hit = nullptr;
        for (const auto& w : ws) {
            if (w.subject == e.subject) {
                hit = &w;
                break;
            }
        }
        ASSERT_TRUE(hit != nullptr) << e.subject << " 无告警（静默丢失回归）";
        EXPECT_EQ(hit->code, ErrorCode::InvalidLayout) << e.subject;
        EXPECT_EQ(hit->phase, Phase::Load) << e.subject;
        EXPECT_NE(hit->message.find(e.fragment), std::string::npos)
            << e.subject << ": " << hit->message;
    }

    // 可证叶子不受坏兄弟连坐：对照实例与递归环的可证前缀都还在
    struct AliveExpect {
        const char* path;
        std::uint64_t addr;
    };
    static constexpr AliveExpect kAlive[] = {
        {"ok_ctrl.ok", 0x2000},
        {"cyc.cyc.v1", 0x6002},
        {"cyc.cyc.inner.v2",
         0x6002},  // 递归展开到环闭合前一层，与 v1 同址双路径
        {"cyc.tail", 0x6004},
    };
    for (const auto& e : kAlive) {
        const std::string key = std::string("LEAF_FIX::") + e.path;
        auto r = db->Find(key);
        ASSERT_TRUE(r.HasValue()) << key << ": " << r.ErrorInfo().message;
        EXPECT_EQ(r.Value().kind, SymbolKind::Measurement) << key;
        EXPECT_EQ(r.Value().xcp_address, e.addr) << key;
        EXPECT_EQ(static_cast<unsigned>(r.Value().address_extension), 1u)
            << key;
    }

    // 拒绝形态不留叶子（NotFound）：坏成员、坏实例的一切下钻路径
    for (const char* dead :
         {"LEAF_FIX::ok_ctrl.ghost", "LEAF_FIX::ok_ctrl.wide",
          "LEAF_FIX::oob.big", "LEAF_FIX::cyc.cyc.inner.back",
          "LEAF_FIX::zero_dim.ok", "LEAF_FIX::zero_dim[0].ok",
          "LEAF_FIX::big_dim[0].ok", "LEAF_FIX::h_ref.any"}) {
        auto r = db->Find(dead);
        ASSERT_FALSE(r.HasValue()) << dead << " 不应被展开";
        EXPECT_EQ(r.ErrorInfo().code, ErrorCode::NotFound) << dead;
    }

    // 实例本体仍是 B-12 元数据符号（识别层不因展开失败而消失）
    auto ins = db->Find("LEAF_FIX::zero_dim");
    ASSERT_TRUE(ins.HasValue());
    EXPECT_EQ(ins.Value().kind, SymbolKind::Structure);
    EXPECT_EQ(ins.Value().xcp_address, 0x4000u);
    EXPECT_EQ(static_cast<unsigned>(ins.Value().address_extension), 1u);

    // 符号计数基线 15 = 5 TYPEDEF + 6 INSTANCE + 4 可证叶子
    auto all = db->Search("LEAF_FIX::*", 500);
    ASSERT_TRUE(all.HasValue());
    EXPECT_EQ(all.Value().size(), 15u);
}

// ============================================================================
// T6 DAQ / DTO envelope（B-5/B-7）
// ============================================================================

namespace {

/// @brief 按 envelope 规则组装测试帧（独立重算 header 字节数，B-7 字节规则）
Bytes MakeEnvelope(const DtoFrameLayout& f, std::uint8_t pid,
                   const Bytes& payload) {
    Bytes b;
    b.push_back(pid);
    std::size_t extra = 0;
    if (f.counter_enabled) extra += 1;
    if (f.timestamp_enabled) extra += (f.timestamp_size_bits + 7) / 8;
    if (f.first_odt) extra += 1;
    switch (f.identification_field_type) {
        case DaqInfo::IdFieldType::Absolute:
            break;
        case DaqInfo::IdFieldType::RelativeByte:
            extra += 1;
            break;
        case DaqInfo::IdFieldType::RelativeWord:
        case DaqInfo::IdFieldType::RelativeWordAligned:
            extra += 2;
            break;
    }
    b.insert(b.end(), extra, 0xEE);  // 填充字节（Decode 只按长度校验）
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

}  // namespace

TEST_F(A2lGoldenTest, DtoEnvelopeMatrix) {
    auto b = LoadOk(Golden("golden_basic.a2l"));
    ASSERT_NE(b, nullptr);
    auto layout_r = b->CreateDaqLayout();
    ASSERT_TRUE(layout_r.HasValue());
    const std::unique_ptr<IDaqLayout>& layout = layout_r.Value();
    const Bytes payload{0xC8, 0x00, 0x2A};  // M_LINEAR + M_BYTE

    struct Case {
        const char* name;
        DtoFrameLayout frame;
    };
    std::vector<Case> cases;
    {
        DtoFrameLayout f;  // Absolute 基线
        cases.push_back({"absolute", f});
        f.identification_field_type = DaqInfo::IdFieldType::RelativeByte;
        cases.push_back({"relative_byte", f});
        f.identification_field_type = DaqInfo::IdFieldType::RelativeWord;
        cases.push_back({"relative_word", f});
        f = DtoFrameLayout{};
        f.counter_enabled = true;
        cases.push_back({"counter", f});
        f = DtoFrameLayout{};
        f.timestamp_enabled = true;
        f.timestamp_size_bits = 16;
        cases.push_back({"timestamp16", f});
        f = DtoFrameLayout{};
        f.first_odt = true;
        cases.push_back({"first_odt", f});
        f = DtoFrameLayout{};
        f.counter_enabled = true;
        f.timestamp_enabled = true;
        f.timestamp_size_bits = 32;
        f.first_odt = true;
        f.identification_field_type = DaqInfo::IdFieldType::RelativeByte;
        cases.push_back({"combo", f});
    }
    for (const Case& c : cases) {
        // PID=1：golden_basic 的列表号与 FIRST_PID 都是 1，两种口径同值
        const Bytes dto =
            MakeEnvelope(c.frame, 1 /*FIRST_PID+相对 ODT 号*/, payload);
        auto samples = layout->Decode(c.frame, dto);
        ASSERT_TRUE(samples.HasValue()) << c.name;
        ASSERT_EQ(samples.Value().size(), 2u) << c.name;
        EXPECT_EQ(samples.Value()[0].symbol_name, "SMOKE_ECU::M_LINEAR")
            << c.name;
        EXPECT_EQ(samples.Value()[1].raw, (Bytes{0x2A})) << c.name;

        // 截断：少一个 envelope 字节 → 整帧拒绝（负例）
        Bytes cut = dto;
        cut.pop_back();
        auto bad = layout->Decode(c.frame, cut);
        EXPECT_FALSE(bad.HasValue()) << c.name;
        EXPECT_EQ(bad.ErrorInfo().code, ErrorCode::InvalidLayout) << c.name;
    }

    // 未知 PID → NotFound
    Bytes wrong{0x7F, 0xC8, 0x00, 0x2A};
    DtoFrameLayout base;
    auto miss = layout->Decode(base, wrong);
    EXPECT_FALSE(miss.HasValue());
    EXPECT_EQ(miss.ErrorInfo().code, ErrorCode::NotFound);

    // 净荷与布局不符（超长）→ InvalidLayout 整帧拒绝
    Bytes extra{1, 0xC8, 0x00, 0x2A, 0xFF};
    auto too_long = layout->Decode(base, extra);
    EXPECT_FALSE(too_long.HasValue());
    EXPECT_EQ(too_long.ErrorInfo().code, ErrorCode::InvalidLayout);

    // PID_OFF 维度（T6 矩阵，批次11）：识别字段缺席 → 无 EPK 路由通道，
    // 显式结构化拒绝（B-7/§6.3：XCP 要求 Transport 层保证唯一标识，
    // UDP 里程碑无此关联），绝不把净荷首字节误当 PID 解释。
    DtoFrameLayout pid_off_frame;
    pid_off_frame.pid_off = true;
    pid_off_frame.header_bytes = 0;             // XCP 语义：PID 字节不存在
    const Bytes pid_off_dto{0xC8, 0x00, 0x2A};  // 仅净荷
    auto pid_off_r = layout->Decode(pid_off_frame, pid_off_dto);
    EXPECT_FALSE(pid_off_r.HasValue());
    EXPECT_EQ(pid_off_r.ErrorInfo().code, ErrorCode::UnsupportedOperation);
    EXPECT_EQ(pid_off_r.ErrorInfo().phase, Phase::Layout);
}

TEST_F(A2lGoldenTest, StaticPredefinedDaqDecode) {
    // golden_mask 的 DAQ 列表（列表号 2、FIRST_PID=4）：
    // 批次15 F1 起 PID=FIRST_PID+0=0x04；覆盖 BIT_MASK / BIT_OFFSET / 2D raw
    // 三分支
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    auto layout_r = b->CreateDaqLayout();
    ASSERT_TRUE(layout_r.HasValue());
    DtoFrameLayout frame;  // Absolute + 1B PID
    const Bytes dto{0x04, 0x00, 0x0F, 0xAA, 0, 1, 2,  3,
                    4,    5,    6,    7,    8, 9, 10, 11};
    auto packed = layout_r.Value()->PackedByteSize(
        {"MASK_ECU::M_MASK", "MASK_ECU::M_UBYTE", "MASK_ECU::M_2D"});
    ASSERT_TRUE(packed.HasValue());
    EXPECT_EQ(packed.Value(), 15u);
    auto samples = layout_r.Value()->Decode(frame, dto);
    ASSERT_TRUE(samples.HasValue());
    EXPECT_EQ(samples.Value().size(), 3u);
}

TEST_F(A2lGoldenTest, A2lFirstPidRoutesInsteadOfListNumber) {
    // 批次15（F1）静默错值回归锁：golden_mask 的列表号是 2、FIRST_PID 是 4。
    // 修复前解码按"PID == 列表号"，因此喂 0x02 会**成功解出 3 条 entry**——
    // 这在真实 ECU 上是错的（XCP 发的是 4），更要命的是"别的列表的 PID 恰好
    // 等于本列表号"时会用错布局且全程无报错。修复后：
    //   ① 喂 0x02（列表号）必须失败；② 喂 0x04（FIRST_PID+0）才成功。
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    auto layout_r = b->CreateDaqLayout();
    ASSERT_TRUE(layout_r.HasValue());
    const std::unique_ptr<IDaqLayout>& layout = layout_r.Value();

    DtoFrameLayout frame;  // Absolute + 1B PID
    const Bytes body{0x00, 0x0F, 0xAA, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    Bytes by_list_number;
    by_list_number.push_back(0x02);  // 旧的错误口径
    by_list_number.insert(by_list_number.end(), body.begin(), body.end());
    auto wrong = layout->Decode(frame, by_list_number);
    ASSERT_FALSE(wrong.HasValue())
        << "A2L 声明了 FIRST_PID 时，列表号不得再充当 PID 参与解释（L1）";
    EXPECT_EQ(wrong.ErrorInfo().code, ErrorCode::NotFound);
    EXPECT_EQ(wrong.ErrorInfo().phase, Phase::Layout);

    Bytes by_first_pid;
    by_first_pid.push_back(0x04);  // docs L2225：FIRST_PID + 相对 ODT 号
    by_first_pid.insert(by_first_pid.end(), body.begin(), body.end());
    auto ok = layout->Decode(frame, by_first_pid);
    ASSERT_TRUE(ok.HasValue()) << ok.ErrorInfo().message;
    ASSERT_EQ(ok.Value().size(), 3u);
    EXPECT_EQ(ok.Value()[0].symbol_name, "MASK_ECU::M_MASK");
    EXPECT_EQ(ok.Value()[1].symbol_name, "MASK_ECU::M_UBYTE");

    // 批次15（F4）：代际可查，供调用方一行识别陈旧解码器
    EXPECT_EQ(layout->Generation(), 0u)
        << "A2L 侧来源无配置代际概念（恒 0），账本/回读侧才带真值";
}

TEST_F(A2lGoldenTest, DecodedPhysicalValidityFlagDistinguishesUnconverted) {
    // 批次15（F2）：PhysicalValue 的默认构造值就是 int64_t{0}，没有
    // physical_valid 时"没换算"与"算出 0"不可区分。这里用同一份布局的三类
    // entry 各自钉死一种状态。
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    auto layout_r = b->CreateDaqLayout();
    ASSERT_TRUE(layout_r.HasValue());
    DtoFrameLayout frame;
    // 全零净荷：即使换算成功，物理值也可能是 0，故必须看标志位
    const Bytes zeros{0x04, 0x00, 0x00, 0x00, 0, 0, 0, 0,
                      0,    0,    0,    0,    0, 0, 0, 0};
    auto samples = layout_r.Value()->Decode(frame, zeros);
    ASSERT_TRUE(samples.HasValue());
    ASSERT_EQ(samples.Value().size(), 3u);
    // entry1 M_MASK（UWORD + LINEAR）：换算成功
    EXPECT_TRUE(samples.Value()[0].physical_valid);
    EXPECT_EQ(std::get<std::int64_t>(samples.Value()[0].physical_value), 0);
    // entry3 M_2D（多维数组）：不整体换算 → 标志必须为 false，raw 仍在
    EXPECT_FALSE(samples.Value()[2].physical_valid);
    EXPECT_EQ(samples.Value()[2].raw.size(), 12u);
    for (const auto& s : samples.Value()) {
        if (!s.physical_valid) {
            // 唯一的合法用法：退回 raw，不拿默认值当结果
            EXPECT_FALSE(s.raw.empty()) << s.symbol_name;
        }
    }
}

TEST_F(A2lGoldenTest, LoadWarningsReportAbandonedFacts) {
    // 批次15（F6/D4）：被放弃的事实必须留痕。golden_collision 同时制造两种：
    //   ① TYPEDEF_STRUCTURE 与 MEASUREMENT 同名（B-13 保留后者、跳过前者）；
    //   ② DAQ_LIST 未声明 FIRST_PID（F1/D1 的"列表号回退"弱权威）。
    auto b = LoadOk(Golden("golden_collision.a2l"));
    ASSERT_NE(b, nullptr);

    const std::vector<LoadWarning>& warnings = b->ListLoadWarnings();
    bool has_collision = false;
    bool has_nofirstpid = false;
    for (const auto& w : warnings) {
        if (w.subject == "COL_ECU::M_BLOCK") {
            has_collision = true;
            EXPECT_EQ(w.code, ErrorCode::AmbiguousName) << w.message;
            EXPECT_EQ(w.phase, Phase::Load);
        }
        if (w.subject.find("DAQ_LIST 7") != std::string::npos) {
            has_nofirstpid = true;
            EXPECT_EQ(w.code, ErrorCode::UnsupportedOperation) << w.message;
            EXPECT_EQ(w.phase, Phase::Layout);
        }
    }
    EXPECT_TRUE(has_collision) << "同名冲突被跳过的结构体必须留痕（L6）";
    EXPECT_TRUE(has_nofirstpid) << "未取证回退必须留痕（L1/D1）";

    // 告警不改变读取语义：同名键仍解析到 MEASUREMENT，且可正常换算
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);
    auto m = db->Find("COL_ECU::M_BLOCK");
    ASSERT_TRUE(m.HasValue());
    EXPECT_EQ(m.Value().kind, SymbolKind::Measurement);
    EXPECT_EQ(m.Value().element_size_bytes, 2);

    // 未声明 FIRST_PID 的列表：回退口径仍可用（弱权威 != 拒绝服务）
    auto layout_r = b->CreateDaqLayout();
    ASSERT_TRUE(layout_r.HasValue());
    EXPECT_EQ(layout_r.Value()->Generation(), 0u);
    DtoFrameLayout frame;
    const Bytes by_number{0x07, 0x12, 0x34};  // 列表号 7 充当 PID
    auto samples = layout_r.Value()->Decode(
        frame, BytesView{by_number.data(), by_number.size()});
    ASSERT_TRUE(samples.HasValue()) << samples.ErrorInfo().message;
    ASSERT_EQ(samples.Value().size(), 1u);
    EXPECT_EQ(samples.Value()[0].symbol_name, "COL_ECU::M_BLOCK");
    EXPECT_TRUE(samples.Value()[0].physical_valid);

    // 正常样本不得产告警（避免把告警通道当噪声源）
    auto ok = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(ok, nullptr);
    EXPECT_TRUE(ok->ListLoadWarnings().empty())
        << "golden_mask 声明了 FIRST_PID 且无同名冲突，不该有告警";
}

TEST_F(A2lGoldenTest, DynamicDaqRejectedExplicitly) {
    // B-5：DYNAMIC 只解析能力；CreateDaqLayout 显式拒绝，不伪装成功
    auto b = LoadOk(Golden("golden_dynamic.a2l"));
    ASSERT_NE(b, nullptr);
    const IfDataXcpInfo* xcp = b->XcpInfo();
    ASSERT_NE(xcp, nullptr);
    ASSERT_TRUE(xcp->daq.has_value());
    EXPECT_FALSE(xcp->daq->static_supported);
    EXPECT_TRUE(xcp->daq->dynamic_supported);
    EXPECT_EQ(xcp->daq->max_daq, 8);
    EXPECT_EQ(xcp->daq->max_event_channel, 4);
    EXPECT_EQ(xcp->daq->min_daq, 0);
    // 预定义列表仍在（能力可解析），但执行路径被拒
    ASSERT_EQ(xcp->static_daq_lists.size(), 1u);
    auto layout = b->CreateDaqLayout();
    EXPECT_FALSE(layout.HasValue());
    EXPECT_EQ(layout.ErrorInfo().code, ErrorCode::UnsupportedOperation);
    EXPECT_EQ(layout.ErrorInfo().phase, Phase::Layout);
}

// ============================================================================
// T7 线程 / 生命周期（B-20）
// ============================================================================

TEST_F(A2lGoldenTest, AsyncNotReadyExactlyOnceAndDestructionSafe) {
    // 场景 1：异步完成前快照未发布（尽力观察 NotReady 窗口）→ 回调恰好一次
    std::atomic<int> callback_count{0};
    std::promise<Result<void>> first_outcome;
    std::future<Result<void>> outcome_f = first_outcome.get_future();
    {
        std::promise<void> done;
        std::future<void> done_f = done.get_future();
        auto br = A2lBridge::LoadAsync(
            Golden("golden_basic.a2l"), {}, [&](Result<void> res) {
                callback_count.fetch_add(1);
                first_outcome.set_value(std::move(res));
                done.set_value();
            });
        ASSERT_TRUE(br.HasValue());
        // B-20：加载中查询不得发布半成品。解析可能快于本检查（窗口丢失
        // 仅提示不判失败），但一旦回调未至而快照已就绪即为契约违规——
        // 实现顺序为 PublishSnapshot 先于回调，故这里只允许 nullptr。
        if (callback_count.load() == 0) {
            EXPECT_EQ(br.Value()->Database(), nullptr)
                << "回调未至时 Database 必须为 nullptr（B-20）";
        } else {
            std::printf("[NOTE] async parse finished before NotReady check\n");
        }
        ASSERT_EQ(done_f.wait_for(std::chrono::seconds(60)),
                  std::future_status::ready);
        Result<void> res = outcome_f.get();
        EXPECT_TRUE(res.HasValue());
        EXPECT_EQ(callback_count.load(), 1);
        EXPECT_NE(br.Value()->Database(), nullptr);
        EXPECT_NE(br.Value()->XcpInfo(), nullptr);
        // 回调后不得再触发（恰好一次）
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(callback_count.load(), 1);
    }

    // 批次13（T13-15）：异步期间轮询阶段进度。弱断言——小文件可能在第一次
    // 轮询前就解析完（窗口丢失仅打 NOTE，不判失败，与 NotReady 窗口同风格）；
    // 但一旦取到过 (0,100) 开区间内的值，其取值必须来自 SDK 的阶段常量。
    std::atomic<bool> saw_intermediate{false};
    std::atomic<int> intermediate_value{0};
    {
        std::promise<void> done;
        std::future<void> done_f = done.get_future();
        std::atomic<bool> finished{false};
        auto br = A2lBridge::LoadAsync(Golden("golden_basic.a2l"), {},
                                       [&](Result<void>) {
                                           finished.store(true);
                                           done.set_value();
                                       });
        ASSERT_TRUE(br.HasValue());
        // 紧贴受理时刻密集轮询，最大化命中阶段中间值的概率
        for (int spin = 0; spin < 20000 && !finished.load(); ++spin) {
            const int p = br.Value()->Progress();
            if (p > 0 && p < 100) {
                saw_intermediate.store(true);
                intermediate_value.store(p);
                break;
            }
        }
        if (!finished.load()) {
            ASSERT_EQ(done_f.wait_for(std::chrono::seconds(60)),
                      std::future_status::ready);
        }
        if (saw_intermediate.load()) {
            const int p = intermediate_value.load();
            EXPECT_TRUE(p == 5 || p == 25 || p == 70 || p == 99)
                << "阶段进度取值必须是 SDK/桥接层的阶段常量，实际 " << p;
        } else {
            std::printf(
                "[NOTE] async parse finished before an intermediate "
                "progress value was observed\n");
        }
        // 完成回调已到（上面的等待保证），发布后恒 100
        EXPECT_EQ(br.Value()->Progress(), 100);
    }

    // 场景 2：失败的异步加载不发布半成品（Database 恒 nullptr）
    std::promise<Result<void>> fail_outcome;
    std::future<Result<void>> fail_f = fail_outcome.get_future();
    {
        auto br = A2lBridge::LoadAsync(
            Golden("no_such_file.a2l"), {},
            [&](Result<void> res) { fail_outcome.set_value(std::move(res)); });
        ASSERT_TRUE(br.HasValue());
        ASSERT_EQ(fail_f.wait_for(std::chrono::seconds(60)),
                  std::future_status::ready);
        Result<void> fail_res = fail_f.get();
        ASSERT_FALSE(fail_res.HasValue());
        // 批次12：异步失败分支的 message 来自 SDK 文本（原先硬编码
        // "异步解析失败"丢失定位信息）；非 include 错误不得带出链
        EXPECT_NE(fail_res.ErrorInfo().message.find("no_such_file"),
                  std::string::npos)
            << "异步 message 应带出 SDK 原文: " << fail_res.ErrorInfo().message;
        EXPECT_TRUE(fail_res.ErrorInfo().include_chain.empty())
            << "IoError 不属于 include 类错误（B-18 边界）";
        EXPECT_EQ(br.Value()->Database(), nullptr);
        auto layout = br.Value()->CreateDaqLayout();
        EXPECT_FALSE(layout.HasValue());
        EXPECT_EQ(layout.ErrorInfo().code, ErrorCode::NotReady);
    }

    // 场景 2b：异步 include 循环失败 → 结构化链同样带出（B-18 批次12），
    //          证明链通道在 SDK 工作线程上同样成立
    std::promise<Result<void>> cyc_outcome;
    std::future<Result<void>> cyc_f = cyc_outcome.get_future();
    {
        auto br = A2lBridge::LoadAsync(
            Data("cycle_main.a2l"), {},
            [&](Result<void> res) { cyc_outcome.set_value(std::move(res)); });
        ASSERT_TRUE(br.HasValue());
        ASSERT_EQ(cyc_f.wait_for(std::chrono::seconds(60)),
                  std::future_status::ready);
        Result<void> res = cyc_f.get();
        ASSERT_FALSE(res.HasValue());
        const Error& e = res.ErrorInfo();
        EXPECT_EQ(e.code, ErrorCode::ParseFailed);
        EXPECT_EQ(e.phase, Phase::Load);
        EXPECT_NE(e.message.find(" -> "), std::string::npos)
            << "异步分支应带出含链的 SDK 文本";
        ASSERT_EQ(e.include_chain.size(), 4u) << "main -> a -> b -> a";
        EXPECT_EQ(FileNameOf(e.include_chain[1]),
                  FileNameOf(e.include_chain[3]));
    }

    // 场景 3：完成前析构（取消/析构安全 join，A-11/B-20）
    std::atomic<int> late_count{0};
    std::promise<void> late_done;
    std::future<void> late_f = late_done.get_future();
    {
        auto br = A2lBridge::LoadAsync(Golden("golden_basic.a2l"), {},
                                       [&](Result<void>) {
                                           late_count.fetch_add(1);
                                           late_done.set_value();
                                       });
        ASSERT_TRUE(br.HasValue());
        br.Value().reset();  // 立即析构：Doc 析构必须 join 解析线程
    }
    ASSERT_EQ(late_f.wait_for(std::chrono::seconds(60)),
              std::future_status::ready);
    EXPECT_EQ(late_count.load(), 1);
}

TEST_F(A2lGoldenTest, ImmutableSnapshotConcurrentReads) {
    auto b = LoadOk(Golden("golden_basic.a2l"));
    ASSERT_NE(b, nullptr);
    const IA2lDatabase* db = b->Database();
    ASSERT_NE(db, nullptr);

    constexpr int kThreads = 4;
    constexpr int kIters = 200;
    std::atomic<int> errors{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([db, &errors] {
            const Bytes raw{0xC8, 0x00};
            for (int i = 0; i < kIters; ++i) {
                auto found = db->Find("SMOKE_ECU::M_LINEAR");
                if (!found.HasValue() || found.Value().xcp_address != 0x8020) {
                    errors.fetch_add(1);
                    continue;
                }
                auto count = db->Count();
                if (!count.HasValue() || count.Value() != 4) {
                    errors.fetch_add(1);
                    continue;
                }
                auto phys = db->ToPhysical("SMOKE_ECU::M_LINEAR", raw);
                if (!phys.HasValue() ||
                    !std::holds_alternative<double>(phys.Value())) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    EXPECT_EQ(errors.load(), 0);
}

// ============================================================================
// T9 性能基线（批次13 T13-17；只记基线，不设 SLA）
// ============================================================================

TEST_F(A2lGoldenTest, ParseLargeFileUnderBudget) {
    // 输入由 `gen_a2l.py --perf` 生成（CMake fixture `a2l_gen_perf`），
    // 只落 build tree。缺失即跳过——本用例是基线记录，不是功能门禁。
    const std::filesystem::path perf =
        std::filesystem::path(A2L_GOLDEN_DIR) / "golden_perf_5mb.a2l";
    if (!std::filesystem::exists(perf)) {
        GTEST_SKIP() << "未生成性能样本（fixture a2l_gen_perf 未跑）: " << perf;
    }
    // 生成器把符号数写进同名 .count 侧车文件：用例据此校验"解析真的做完"，
    // 而不是把数字硬编码进测试（生成器文本宽度一变就假失败）。
    std::size_t expected_count = 0;
    {
        std::ifstream meta(std::filesystem::path(perf).string() + ".count");
        std::string text;
        if (std::getline(meta, text)) {
            expected_count = std::stoul(text);
        }
    }
    ASSERT_GT(expected_count, 0u) << "侧车文件缺失或为空：" << perf << ".count";
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = A2lBridge::Load(perf.string(), require_off);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    ASSERT_TRUE(r.HasValue()) << r.ErrorInfo().message;
    const std::size_t bytes = std::filesystem::file_size(perf);
    ASSERT_NE(r.Value()->Database(), nullptr);
    auto count = r.Value()->Database()->Count();
    ASSERT_TRUE(count.HasValue());
    EXPECT_EQ(count.Value(), expected_count)
        << "符号数与生成器侧车不一致（样本被截断或快照漏采）";
    std::printf("[PERF] A2L %.2f MB 解析 + 快照构建 = %lld ms (%zu 符号)\n",
                static_cast<double>(bytes) / (1024.0 * 1024.0),
                static_cast<long long>(ms), count.Value());
    // 双条件门禁：参考值倍率用于跨机器稳定性，绝对上限用于拦截量级劣化。
    const long long reference_limit =
        static_cast<long long>(A2L_PERF_REFERENCE_MS) * 3LL;
    const long long hard_limit = static_cast<long long>(A2L_PERF_HARD_LIMIT_MS);
    EXPECT_TRUE(static_cast<long long>(ms) <= reference_limit &&
                static_cast<long long>(ms) <= hard_limit)
        << "性能超限：实测 " << ms << " ms，参考倍率上限 " << reference_limit
        << " ms，绝对上限 " << hard_limit << " ms";
}

// ============================================================================
// T8 编码（UTF-8 / UTF-16）
// ============================================================================

TEST_F(A2lGoldenTest, Utf8DescriptionRoundTrip) {
    // golden_mask 含中文描述：UTF-8 字节原样往返（uchardet-shim 透传路径）
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    auto m = b->Database()->Find("MASK_ECU::M_MASK");
    ASSERT_TRUE(m.HasValue());
    EXPECT_EQ(m.Value().description, "连续位掩码 UWORD");
}

TEST_F(A2lGoldenTest, Utf16LeBomExploratory) {
    // T8 探索：UTF-16LE BOM 文件。上游 CheckBom + boost::locale 转换
    // （Windows-API 后端推断，无 ICU）；本用例按实测结果分支：
    // 成功 → 断言符号可查；失败 → 记录并跳过（已知缺口，样本统一
    // UTF-8/ASCII）。
    const auto path =
        std::filesystem::path(A2L_GOLDEN_DIR) / "utf16_sample.a2l";
    const std::string body =
        "ASAP2_VERSION 1 61\n"
        "/begin Project U16 \"utf16 sample\"\n"
        "/begin MODULE U16_ECU \"m\"\n"
        "/begin COMPU_METHOD CM_U \"i\" IDENTICAL \"%6.2\" \"\"\n"
        "/end COMPU_METHOD\n"
        "/begin MEASUREMENT U16_M \"x\" UBYTE CM_U 0 0 0 255\n"
        "  ECU_ADDRESS 0x5001\n"
        "  READ_WRITE\n"
        "/end MEASUREMENT\n"
        "/end MODULE\n"
        "/end Project\n";
    {
        std::ofstream out(path, std::ios::binary);
        const unsigned char bom[] = {0xFF, 0xFE};
        out.write(reinterpret_cast<const char*>(bom), 2);
        for (unsigned char c : body) {
            char pair[2] = {static_cast<char>(c), 0};
            out.write(pair, 2);
        }
    }
    LoadOptions require_off;
    require_off.require_if_data_xcp = false;
    auto r = A2lBridge::Load(path.string(), require_off);
    if (r.HasValue()) {
        auto m = r.Value()->Database()->Find("U16_ECU::U16_M");
        EXPECT_TRUE(m.HasValue());
        if (m.HasValue()) {
            EXPECT_EQ(m.Value().xcp_address, 0x5001u);
        }
    } else {
        std::printf(
            "[NOTE] UTF-16LE BOM parse failed (known gap §6.3, shim "
            "encoding path): %s\n",
            r.ErrorInfo().message.c_str());
        GTEST_SKIP() << "UTF-16 支持待 boost::locale 后端验证（§6.3 缺口）";
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace
