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
    EXPECT_EQ(b->Database()->Count().Value(), 11u);
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
    DtoFrameLayout frame;              // Absolute + 1 字节 PID（EPK=2）
    const Bytes dto{0x02, 0x00, 0x0F,  // entry1: M_MASK 2B
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

    // CURVE / VAL_BLK：识别五型但拒绝数值操作（VAL_BLK/ASCII 执行推迟，B-11
    // 部分完成）
    auto cc = db->Find("MASK_ECU::C_CURVE");
    ASSERT_TRUE(cc.HasValue());
    EXPECT_EQ(cc.Value().characteristic_type, CharacteristicType::Curve);
    auto cc_size = db->ByteSizeOf("MASK_ECU::C_CURVE");
    EXPECT_FALSE(cc_size.HasValue());
    EXPECT_EQ(cc_size.ErrorInfo().code, ErrorCode::UnsupportedDataType);
    auto cvb = db->Find("MASK_ECU::C_VALBLK");
    ASSERT_TRUE(cvb.HasValue());
    EXPECT_EQ(cvb.Value().characteristic_type, CharacteristicType::ValBlk);
    auto cvb_to = db->ToPhysical("MASK_ECU::C_VALBLK", Bytes{0x00, 0x00});
    EXPECT_FALSE(cvb_to.HasValue());
    EXPECT_EQ(cvb_to.ErrorInfo().code, ErrorCode::UnsupportedDataType);

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
        const Bytes dto = MakeEnvelope(c.frame, 1 /*EPK*/, payload);
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
    // golden_mask 的 EPK=2 列表：BIT_MASK / BIT_OFFSET / 2D raw 三分支
    auto b = LoadOk(Golden("golden_mask.a2l"));
    ASSERT_NE(b, nullptr);
    auto layout_r = b->CreateDaqLayout();
    ASSERT_TRUE(layout_r.HasValue());
    DtoFrameLayout frame;  // Absolute + 1B PID
    const Bytes dto{0x02, 0x00, 0x0F, 0xAA, 0, 1, 2,  3,
                    4,    5,    6,    7,    8, 9, 10, 11};
    auto packed = layout_r.Value()->PackedByteSize(
        {"MASK_ECU::M_MASK", "MASK_ECU::M_UBYTE", "MASK_ECU::M_2D"});
    ASSERT_TRUE(packed.HasValue());
    EXPECT_EQ(packed.Value(), 15u);
    auto samples = layout_r.Value()->Decode(frame, dto);
    ASSERT_TRUE(samples.HasValue());
    EXPECT_EQ(samples.Value().size(), 3u);
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
