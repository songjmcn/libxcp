// =============================================================================
// a2l_smoke_test.cpp —— A2L 最小开发验证集冒烟测试（门禁清单见
// code-plan/A2L_CMake最小开发验证集_R4.md §5）：
//   G1 DLL 可加载 + ABI 三向校验（v3 可建 / v2、v4 必拒，批次12 ABI bump）；
//   G1b IDoc::LastErrorChain 通道契约（批次12 新增虚函数的边界三态）；
//   G2 生成的最小 A2L 可解析；
//   G3 符号可按 module::symbol 查询（含裸名与未命中路径）；
//   G4 B-1 地址/extension 原值保留 + AG 元素计数/地址推进；
//   G5 LINEAR 换算正确（正算 + 逆算 + 负例）；
//   G6 STATIC DAQ_LIST 冻结布局按 DTO 解出 raw 并换算；
//   （A2L 栈现为必编组件，无 OFF 模式；历史 G7 门禁见 code-plan 记录。）
//
// 期望值与 tests/a2l_gen/gen_a2l.py 的 SPEC/expected.json 逐项对应，
// 独立于 a2llib 推导（B-1/B-3/B-8/B-14/B-15 语义基线）。
// 本测试不依赖 GoogleTest：独立 main + 失败计数，作为 CTest 用例 A2lSmoke。
// =============================================================================

#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// 测试是 SDK 的合法消费者（P9 隔离只约束 a2lbridge/ 目录），
// 但常规业务路径仍应只经 a2l_bridge.hpp；此处仅为 G1 直查 ABI 工厂。
#include "liba2l/liba2l_api.hpp"
#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/compu_method.hpp"

#ifndef A2L_GOLDEN_PATH
#error "A2L_GOLDEN_PATH 编译定义缺失（由 tests/CMakeLists.txt 注入）"
#endif

namespace {

int g_checks = 0;    ///< 已执行断言数
int g_failures = 0;  ///< 失败断言数

/// @brief 轻量断言宏：打印通过/失败并计数
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        } else {                                                          \
            std::printf("[ OK ] %s\n", #cond);                            \
        }                                                                 \
    } while (false)

using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::AddressGranularity;
using calmcar::xcp::a2l::ByteOrder;
using calmcar::xcp::a2l::Bytes;
using calmcar::xcp::a2l::CharacteristicType;
using calmcar::xcp::a2l::ConversionKind;
using calmcar::xcp::a2l::ErrorCode;
using calmcar::xcp::a2l::IA2lDatabase;
using calmcar::xcp::a2l::IDaqLayout;
using calmcar::xcp::a2l::Phase;
using calmcar::xcp::a2l::PhysicalValue;
using calmcar::xcp::a2l::SymbolInfo;
using calmcar::xcp::a2l::SymbolKind;

/// @brief 黄金样本期望常量（与 gen_a2l.py SPEC 一致）
constexpr const char* kModule = "SMOKE_ECU";
constexpr std::uint64_t kArrayAddr = 0x8000;
constexpr std::uint64_t kLinearAddr = 0x8020;
constexpr std::uint64_t kByteAddr = 0x8010;
constexpr std::uint8_t kAddrExt = 0x12;
constexpr std::uint8_t kEpPid = 1;      ///< PID = FIRST_PID + 相对 ODT 号（basic: 1）
constexpr double kLinearPhys = 100.0;   ///< raw 200 * 0.5
constexpr std::int64_t kBytePhys = 42;  ///< IDENTICAL：raw 42

/// @brief double 物理值比较（0.5 系数二进制精确，容差仅防实现路径抖动）
bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

/// @brief G1：DLL 加载 + ABI 三向校验（当前版本可建、v-1 与 v+1 必拒）
void Gate1AbiCheck() {
    liba2l::IDoc* good = liba2l::CreateDoc(liba2l::kLibA2lAbiVersion);
    CHECK(good != nullptr);
    if (good != nullptr) {
        good->Release();
    }
    // 批次12：ABI 已 bump 到 3 —— 旧版头（v2）必须被新 DLL 拒绝，
    // 这是 R5 "DLL 与头不同步只靠 kLibA2lAbiVersion 兜底" 的直接验证
    liba2l::IDoc* old = liba2l::CreateDoc(liba2l::kLibA2lAbiVersion - 1);
    CHECK(old == nullptr);
    if (old != nullptr) {
        old->Release();
    }
    liba2l::IDoc* bad = liba2l::CreateDoc(liba2l::kLibA2lAbiVersion + 1);
    CHECK(bad == nullptr);
    if (bad != nullptr) {
        bad->Release();
    }
}

/// @brief G1b：`IDoc::LastErrorChain` 通道契约（批次12，ABI v3 新增方法）
/// @details 三条边界：① 空指针入参返回 kBadArgument；② 该失败**不得**改写
///          错误通道（查询方法不污染 LastError/LastErrorCode）；③ 正常入参
///          在未加载时返回 kOk + 空链（"无链"不是错误）。
void Gate1bLastErrorChainChannel() {
    liba2l::IDoc* doc = liba2l::CreateDoc(liba2l::kLibA2lAbiVersion);
    CHECK(doc != nullptr);
    if (doc == nullptr) {
        return;
    }
    const liba2l::ErrorCode before = doc->LastErrorCode();
    CHECK(doc->LastErrorChain(nullptr) == liba2l::ErrorCode::kBadArgument);
    CHECK(doc->LastErrorCode() == before);
    std::vector<std::string> chain;
    CHECK(doc->LastErrorChain(&chain) == liba2l::ErrorCode::kOk);
    CHECK(chain.empty());
    doc->Release();
}

/// @brief G3/G4：符号查询 + B-1 地址语义 + 元素计数/推进
void Gate3And4SymbolLookup(const IA2lDatabase& db) {
    // 规范键精确命中
    auto found = db.Find("SMOKE_ECU::M_ARRAY");
    CHECK(found.HasValue());
    if (!found.HasValue()) {
        return;
    }
    const SymbolInfo& s = found.Value();
    CHECK(s.kind == SymbolKind::Measurement);
    CHECK(s.module_name == kModule);
    CHECK(s.name == "M_ARRAY");
    // G4（B-1）：ECU_ADDRESS 原值直传，extension 独立 8-bit 字段
    CHECK(s.xcp_address == kArrayAddr);
    CHECK(s.address_extension == kAddrExt);
    // B-3：显式类型映射 → UWORD 元素宽 2
    CHECK(s.element_size_bytes == 2);
    // G4：ARRAY_SIZE 4 → 单维 extent/stride
    CHECK(s.dimensions.size() == 1);
    if (s.dimensions.size() == 1) {
        CHECK(s.dimensions[0].extent == 4);
        CHECK(s.dimensions[0].byte_stride == 2);
    }
    auto elements = calmcar::xcp::a2l::CountElements(s);
    CHECK(elements.HasValue() && elements.Value() == 4);
    auto byte_size = db.ByteSizeOf("SMOKE_ECU::M_ARRAY");
    CHECK(byte_size.HasValue() && byte_size.Value() == 8);
    // B-1 地址推进：element_address = base + byte_offset/AG（AG 绝不乘基址）
    auto addr_byte = calmcar::xcp::a2l::ComputeElementAddress(
        s, 2, AddressGranularity::Byte);
    CHECK(addr_byte.HasValue() && addr_byte.Value() == kArrayAddr + 4);
    auto addr_word = calmcar::xcp::a2l::ComputeElementAddress(
        s, 2, AddressGranularity::Word);
    CHECK(addr_word.HasValue() && addr_word.Value() == kArrayAddr + 2);

    // 裸名（全库唯一）命中同一符号（B-13）
    auto bare = db.Find("M_ARRAY");
    CHECK(bare.HasValue() && bare.Value().xcp_address == kArrayAddr);

    // 负路径：未命中 / 空名
    auto missing = db.Find("SMOKE_ECU::NO_SUCH");
    CHECK(!missing.HasValue() &&
          missing.ErrorInfo().code == ErrorCode::NotFound);
    auto empty = db.Find("");
    CHECK(!empty.HasValue() &&
          empty.ErrorInfo().code == ErrorCode::BadArgument);

    // CHARACTERISTIC：首里程碑仅元数据，数值能力显式拒绝（B-3/B-12）
    auto ch = db.Find("SMOKE_ECU::C_VALUE");
    CHECK(ch.HasValue());
    if (ch.HasValue()) {
        CHECK(ch.Value().kind == SymbolKind::Characteristic);
        CHECK(ch.Value().characteristic_type == CharacteristicType::Value);
        CHECK(ch.Value().data_type == calmcar::xcp::a2l::AsamDataType::Unknown);
        auto size = db.ByteSizeOf("SMOKE_ECU::C_VALUE");
        CHECK(!size.HasValue() &&
              size.ErrorInfo().code == ErrorCode::UnsupportedDataType);
    }
}

/// @brief G5：LINEAR 换算正逆算 + 负例（IDENTICAL 无符号域越界拒绝）
void Gate5LinearConversion(const IA2lDatabase& db) {
    auto found = db.Find("SMOKE_ECU::M_LINEAR");
    CHECK(found.HasValue());
    if (!found.HasValue()) {
        return;
    }
    const SymbolInfo& s = found.Value();
    CHECK(s.conversion.kind == ConversionKind::Linear);
    CHECK(s.xcp_address == kLinearAddr);
    CHECK(s.address_extension == kAddrExt);
    CHECK(s.read_write);

    // 正算：raw 0x00C8（小端 [C8 00]）→ 100.0
    const Bytes raw_le{0xC8, 0x00};
    auto phys = db.ToPhysical("SMOKE_ECU::M_LINEAR", raw_le);
    CHECK(phys.HasValue());
    if (phys.HasValue()) {
        const double* p = std::get_if<double>(&phys.Value());
        CHECK(p != nullptr && Near(*p, kLinearPhys));
    }

    // 逆算：100.0 → [C8 00]（B-15 唯一可逆；0.5 二进制精确）
    auto enc =
        db.FromPhysical("SMOKE_ECU::M_LINEAR", PhysicalValue{kLinearPhys});
    CHECK(enc.HasValue());
    if (enc.HasValue()) {
        const Bytes want{0xC8, 0x00};
        CHECK(enc.Value() == want);
    }

    // 负例 1：raw 长度 ≠ 元素宽 → RawSizeMismatch
    Bytes short_raw{0xC8};
    auto bad_size = db.ToPhysical("SMOKE_ECU::M_LINEAR", short_raw);
    CHECK(!bad_size.HasValue() &&
          bad_size.ErrorInfo().code == ErrorCode::RawSizeMismatch);

    // 负例 2：IDENTICAL 逆算负值进无符号域 → ConversionOutOfRange（B-15）
    auto neg = db.FromPhysical("SMOKE_ECU::M_BYTE", PhysicalValue{-1.0});
    CHECK(!neg.HasValue() &&
          neg.ErrorInfo().code == ErrorCode::ConversionOutOfRange);

    // 数组符号整体读写被拒（元素级走 IDaqLayout）
    auto arr = db.ToPhysical("SMOKE_ECU::M_ARRAY", raw_le);
    CHECK(!arr.HasValue() &&
          arr.ErrorInfo().code == ErrorCode::UnsupportedOperation);
}

/// @brief G2 附带：IF_DATA XCP 快照参数（PROTOCOL_LAYER + UDP + DAQ 列表）
void CheckIfDataSnapshot(const A2lBridge& bridge) {
    const auto* xcp = bridge.XcpInfo();
    CHECK(xcp != nullptr);
    if (xcp == nullptr) {
        return;
    }
    CHECK(xcp->ok);
    CHECK(!xcp->from_xcp_plus);  // 样本使用 /begin IF_DATA XCP
    CHECK(xcp->protocol_layer.max_cto == 0x10);
    CHECK(xcp->protocol_layer.max_dto == 0x20);
    CHECK(xcp->protocol_layer.byte_order == ByteOrder::MsbLast);
    // 上游 PROTOCOL_LAYER 不消费 ADDRESS_GRANULARITY ident → 默认
    // BYTE（已知事实4）
    CHECK(xcp->protocol_layer.address_granularity == AddressGranularity::Byte);
    CHECK(xcp->transports.size() == 1);
    if (xcp->transports.size() == 1) {
        CHECK(xcp->transports[0].kind ==
              calmcar::xcp::a2l::TransportEndpoint::Kind::UdpIp);
        CHECK(xcp->transports[0].remote_port == 0x15B7);
        CHECK(xcp->transports[0].remote_host == "localhost");
        // 批次12：本样本未声明 PACKET_ALIGNMENT / OPTIONAL_TL_SUBCMD
        // → 上游默认位宽 8、子命令列表为空（黄金套件用 golden_mask 测正例）
        CHECK(xcp->transports[0].packet_alignment == 8);
        CHECK(xcp->transports[0].sub_commands.empty());
    }
    CHECK(xcp->daq.has_value() && xcp->daq->static_supported);
    // ---- 批次10 数据补全断言（§6.1：t1..t7 / DAQ 能力块 / 事件 / 归属） ----
    CHECK(!xcp->ambiguous);              // B-17：单 MODULE 自动选择，非歧义
    CHECK(xcp->module_name == kModule);  // B-17 module scope（批次10）
    const auto& t = xcp->protocol_layer.timeout_ms;
    CHECK(t[0] == 1 && t[1] == 1 && t[2] == 5 && t[3] == 5 && t[4] == 5 &&
          t[5] == 1 && t[6] == 1);  // T1..T7 = 1 1 5 5 5 1 1（gen SPEC）
    CHECK(xcp->protocol_layer.optional_commands
              .empty());  // 样本未声明 OPTIONAL_CMD
    CHECK(xcp->protocol_layer.seed_and_key_function.empty());
    CHECK(!xcp->protocol_layer.has_ecu_states);
    CHECK(xcp->event_channels.empty());  // 样本无 EVENT 块（黄金变体补测）
    CHECK(xcp->plus_conflicts.empty());  // 样本无 XCPplus 对照块
    if (xcp->daq.has_value()) {
        const auto& caps = *xcp->daq;
        CHECK(!caps.dynamic_supported);
        CHECK(caps.max_daq == 3);                   // DAQ STATIC 3 …
        CHECK(caps.max_event_channel == 2);         // … 2 …
        CHECK(caps.min_daq == 0);                   // … 0
        CHECK(caps.odt_entry_min_size_bytes == 1);  // GRANULARITY …_BYTE
        CHECK(caps.max_odt_entry_size == 4);        // … 4
        CHECK(caps.odt_type == calmcar::xcp::a2l::DaqInfo::OdtType::Default);
        CHECK(caps.address_extension_mode ==
              calmcar::xcp::a2l::DaqInfo::AddrExtMode::PerDaq);
        CHECK(caps.identification_field_type ==
              calmcar::xcp::a2l::DaqInfo::IdFieldType::Absolute);
        CHECK(caps.overflow_flag_supported);  // OVERLOAD_INDICATION_EVENT
        CHECK(!caps.prescaler_supported && !caps.resume_supported &&
              !caps.pid_off_supported && !caps.dto_counter_supported);
        CHECK(!caps.timestamp_max_size_bits.has_value());
    }
    CHECK(xcp->static_daq_lists.size() == 1);
    if (xcp->static_daq_lists.size() == 1) {
        const auto& list = xcp->static_daq_lists[0];
        CHECK(list.number == kEpPid);
        // 批次10：EVENT_FIXED 反查通道（事件联动链路的数据来源）
        CHECK(list.event_fixed.has_value() && *list.event_fixed == 1);
        CHECK(list.odts.size() == 1);
        if (!list.odts.empty()) {
            const auto& entries = list.odts[0].entries;
            CHECK(entries.size() == 2);
            if (entries.size() == 2) {
                // B-1：entry 地址/extension 原值 + 符号归属反查成功
                CHECK(entries[0].address == kLinearAddr);
                CHECK(entries[0].address_extension == kAddrExt);
                CHECK(entries[0].size_bytes == 2);
                CHECK(entries[0].symbol_name == "SMOKE_ECU::M_LINEAR");
                CHECK(entries[1].address == kByteAddr);
                CHECK(entries[1].size_bytes == 1);
                CHECK(entries[1].symbol_name == "SMOKE_ECU::M_BYTE");
            }
        }
    }
}

/// @brief G6：STATIC DTO 帧按冻结布局解码（含整帧拒绝负例与打包尺寸）
void Gate6DtoDecode(const A2lBridge& bridge) {
    auto layout_result = bridge.CreateDaqLayout();
    CHECK(layout_result.HasValue());
    if (!layout_result.HasValue()) {
        return;
    }
    const std::unique_ptr<IDaqLayout>& layout = layout_result.Value();
    calmcar::xcp::a2l::DtoFrameLayout frame;  // 默认：Absolute + 1 字节 PID 头

    // 合法帧：PID=1 + raw(M_LINEAR) + raw(M_BYTE)
    const Bytes dto{kEpPid, 0xC8, 0x00, 0x2A};
    auto samples = layout->Decode(frame, dto);
    CHECK(samples.HasValue());
    if (samples.HasValue()) {
        const auto& v = samples.Value();
        CHECK(v.size() == 2);
        if (v.size() == 2) {
            CHECK(v[0].symbol_name == "SMOKE_ECU::M_LINEAR");
            CHECK(v[0].address == kLinearAddr);
            CHECK(v[0].address_extension == kAddrExt);
            CHECK(v[0].raw == (Bytes{0xC8, 0x00}));
            const double* p0 = std::get_if<double>(&v[0].physical_value);
            CHECK(p0 != nullptr && Near(*p0, kLinearPhys));
            CHECK(v[1].symbol_name == "SMOKE_ECU::M_BYTE");
            CHECK(v[1].raw == (Bytes{0x2A}));
            const auto* p1 = std::get_if<std::int64_t>(&v[1].physical_value);
            CHECK(p1 != nullptr && *p1 == kBytePhys);
        }
    }

    // 负例：净荷短于冻结布局 → 整帧拒绝（B-7），不做部分解释
    const Bytes truncated{kEpPid, 0xC8};
    auto bad = layout->Decode(frame, truncated);
    CHECK(!bad.HasValue() && bad.ErrorInfo().code == ErrorCode::InvalidLayout);

    // 负例：PID 无匹配列表
    const Bytes wrong_pid{0x7F, 0xC8, 0x00, 0x2A};
    auto miss = layout->Decode(frame, wrong_pid);
    CHECK(!miss.HasValue() && miss.ErrorInfo().code == ErrorCode::NotFound);

    // 打包尺寸查询（Allocator 输入）
    auto packed =
        layout->PackedByteSize({"SMOKE_ECU::M_LINEAR", "SMOKE_ECU::M_BYTE"});
    CHECK(packed.HasValue() && packed.Value() == 3);
}

/// @brief 异步路径轻量回归：LoadAsync 完成回调后快照可用（B-20）
void CheckAsyncLoad() {
    std::promise<void> done;
    std::future<void> waiter = done.get_future();
    std::promise<calmcar::xcp::a2l::Result<void>> outcome;
    auto bridge = A2lBridge::LoadAsync(
        A2L_GOLDEN_PATH, {},
        [&outcome, &done](calmcar::xcp::a2l::Result<void> res) {
            outcome.set_value(std::move(res));
            done.set_value();
        });
    CHECK(bridge.HasValue());
    if (!bridge.HasValue()) {
        return;
    }
    const std::future_status status = waiter.wait_for(std::chrono::seconds(60));
    CHECK(status == std::future_status::ready);
    if (status != std::future_status::ready) {
        return;  // 超时：跳过对回调的取值（outcome 尚未 set，取即 UB）
    }
    calmcar::xcp::a2l::Result<void> res = outcome.get_future().get();
    CHECK(res.HasValue());
    if (res.HasValue() && bridge.Value()) {
        CHECK(bridge.Value()->Database() != nullptr);
        CHECK(bridge.Value()->XcpInfo() != nullptr);
    }
}

}  // namespace

/// @brief 入口：按 G1→G6 顺序执行；返回失败断言数作为进程退出码
int main() {
    // G1 —— DLL 可加载 + ABI 校验
    Gate1AbiCheck();
    // G1b —— LastErrorChain 通道契约（批次12 ABI v3）
    Gate1bLastErrorChainChannel();

    // G2 —— 生成的最小 A2L 可解析（同步 Load）
    auto bridge = A2lBridge::Load(A2L_GOLDEN_PATH);
    if (!bridge.HasValue()) {
        const auto& e = bridge.ErrorInfo();
        std::printf("[FAIL] Load 失败: %s / %s (line=%s)\n",
                    calmcar::xcp::a2l::ToString(e.code), e.message.c_str(),
                    e.line ? std::to_string(*e.line).c_str() : "-");
        ++g_checks;
        ++g_failures;
        return 1;
    }
    ++g_checks;
    std::printf("[ OK ] A2lBridge::Load %s\n", A2L_GOLDEN_PATH);
    CHECK(bridge.Value()->Progress() == 100);
    const IA2lDatabase* db = bridge.Value()->Database();
    CHECK(db != nullptr);
    if (db == nullptr) {
        return 1;
    }
    auto count = db->Count();
    CHECK(count.HasValue() &&
          count.Value() == 4);  // 3 MEASUREMENT + 1 CHARACTERISTIC

    Gate3And4SymbolLookup(*db);
    Gate5LinearConversion(*db);
    CheckIfDataSnapshot(*bridge.Value());
    Gate6DtoDecode(*bridge.Value());
    CheckAsyncLoad();

    std::printf("\nA2lSmoke: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
