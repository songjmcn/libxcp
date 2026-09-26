// ============================================================================
// liba2l.dll 导出实现层（设计文档 §5.3.3b R1–R6）
//
// 本文件是 SDK 中唯一允许 #include <a2l/...> 的翻译单元：上游类型在此终结，
// 一律拷贝为自有 DTO 后跨边界返回。上游异常全部在此捕获并转为
// ErrorCode + LastError（R4）。
// ============================================================================

#include "liba2l/liba2l_api.hpp"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

// --- 上游 a2llib（仅此 TU 可见） ---
#include "a2l/a2lfile.h"
#include "a2l/a2lproject.h"
#include "a2l/a2lstructs.h"
#include "a2l/characteristic.h"
#include "a2l/compumethod.h"
#include "a2l/computab.h"
#include "a2l/compuvtab.h"
#include "a2l/measurement.h"
#include "a2l/module.h"
#include "a2l/recordlayout.h"
#include "a2l/unit.h"
#include "a2l/xcp/commonparameters.h"
#include "a2l/xcp/daq.h"
#include "a2l/xcp/daqlist.h"
#include "a2l/xcp/event.h"
#include "a2l/xcp/protocollayer.h"
#include "a2l/xcp/timestamp.h"
#include "a2l/xcp/xcpdatablock.h"
#include "a2l/xcp/xcponudpip.h"

namespace liba2l {

namespace {

// ----------------------------------------------------------------------------
// 枚举映射（B-3：显式一对一，未列出的上游值一律落 kUnknown，禁止猜默认宽度）
// ----------------------------------------------------------------------------

/** 上游数据类型 → DTO。 */
AsamDataTypeDto MapDataType(a2l::A2lDataType t) {
    switch (t) {
        case a2l::A2lDataType::UBYTE:
            return AsamDataTypeDto::kUByte;
        case a2l::A2lDataType::SBYTE:
            return AsamDataTypeDto::kSByte;
        case a2l::A2lDataType::UWORD:
            return AsamDataTypeDto::kUWord;
        case a2l::A2lDataType::SWORD:
            return AsamDataTypeDto::kSWord;
        case a2l::A2lDataType::ULONG:
            return AsamDataTypeDto::kULong;
        case a2l::A2lDataType::SLONG:
            return AsamDataTypeDto::kSLong;
        case a2l::A2lDataType::A_UINT64:
            return AsamDataTypeDto::kAUint64;
        case a2l::A2lDataType::A_INT64:
            return AsamDataTypeDto::kAInt64;
        case a2l::A2lDataType::FLOAT16_IEEE:
            return AsamDataTypeDto::kFloat16Ieee;
        case a2l::A2lDataType::FLOAT32_IEEE:
            return AsamDataTypeDto::kFloat32Ieee;
        case a2l::A2lDataType::FLOAT64_IEEE:
            return AsamDataTypeDto::kFloat64Ieee;
        case a2l::A2lDataType::UNKNOWN:
        default:
            return AsamDataTypeDto::kUnknown;
    }
}

/** ASAM 类型 → 单元素字节宽（B-3 显式表；kUnknown → 0 由上层判错）。 */
std::uint8_t TypeWidthBytes(AsamDataTypeDto t) {
    switch (t) {
        case AsamDataTypeDto::kUByte:
        case AsamDataTypeDto::kSByte:
            return 1;
        case AsamDataTypeDto::kUWord:
        case AsamDataTypeDto::kSWord:
        case AsamDataTypeDto::kFloat16Ieee:
            return 2;
        case AsamDataTypeDto::kULong:
        case AsamDataTypeDto::kSLong:
        case AsamDataTypeDto::kFloat32Ieee:
            return 4;
        case AsamDataTypeDto::kAUint64:
        case AsamDataTypeDto::kAInt64:
        case AsamDataTypeDto::kFloat64Ieee:
            return 8;
        case AsamDataTypeDto::kUnknown:
        default:
            return 0;
    }
}

/** CHARACTERISTIC 类型 → DTO。 */
CharacteristicTypeDto MapCharType(a2l::A2lCharacteristicType t) {
    switch (t) {
        case a2l::A2lCharacteristicType::VALUE:
            return CharacteristicTypeDto::kValue;
        case a2l::A2lCharacteristicType::CURVE:
            return CharacteristicTypeDto::kCurve;
        case a2l::A2lCharacteristicType::MAP:
            return CharacteristicTypeDto::kMap;
        case a2l::A2lCharacteristicType::CUBOID:
            return CharacteristicTypeDto::kCuboid;
        case a2l::A2lCharacteristicType::CUBE_4:
            return CharacteristicTypeDto::kCube4;
        case a2l::A2lCharacteristicType::CUBE_5:
            return CharacteristicTypeDto::kCube5;
        case a2l::A2lCharacteristicType::VAL_BLK:
            return CharacteristicTypeDto::kValBlk;
        case a2l::A2lCharacteristicType::ASCII:
            return CharacteristicTypeDto::kAscii;
        case a2l::A2lCharacteristicType::UNKNOWN:
        default:
            return CharacteristicTypeDto::kNone;
    }
}

/** 字节序 → DTO（混合字内/字间序明确报 kUnknown，不做近似）。 */
ByteOrderDto MapByteOrder(a2l::A2lByteOrder t) {
    switch (t) {
        case a2l::A2lByteOrder::MSB_LAST:
            return ByteOrderDto::kMsbLast;
        case a2l::A2lByteOrder::MSB_FIRST:
            return ByteOrderDto::kMsbFirst;
        default:
            return ByteOrderDto::kUnknown;
    }
}

/** 存储顺序 → DTO。 */
ArrayOrderDto MapLayout(a2l::A2lLayout t) {
    switch (t) {
        case a2l::A2lLayout::ROW_DIR:
            return ArrayOrderDto::kRowDir;
        case a2l::A2lLayout::COLUMN_DIR:
            return ArrayOrderDto::kColumnDir;
        default:
            return ArrayOrderDto::kUnknown;
    }
}

/** 转换类别 → DTO（FORM 归入 kFormulaUnsupported，B-8 仅文本不执行）。 */
ConversionKindDto MapConversionType(a2l::A2lConversionType t) {
    switch (t) {
        case a2l::A2lConversionType::IDENTICAL:
            return ConversionKindDto::kIdentical;
        case a2l::A2lConversionType::LINEAR:
            return ConversionKindDto::kLinear;
        case a2l::A2lConversionType::RAT_FUNC:
            return ConversionKindDto::kRatFunc;
        case a2l::A2lConversionType::TAB_INTP:
            return ConversionKindDto::kTabIntp;
        case a2l::A2lConversionType::TAB_NOINTP:
            return ConversionKindDto::kTabNoIntp;
        case a2l::A2lConversionType::TAB_VERB:
            return ConversionKindDto::kTabVerb;
        case a2l::A2lConversionType::FORM:
            return ConversionKindDto::kFormulaUnsupported;
        case a2l::A2lConversionType::UNKNOWN:
        default:
            return ConversionKindDto::kNone;
    }
}

/** 传输层类型判定（按 XcpDataBlock
 * 中出现的首个传输层块，检查顺序固定保证确定性）。 */
XcpTransportDto PickTransport(const a2l::xcp::XcpDataBlock& blk) {
    if (!blk.GetXcpOnCans().empty()) return XcpTransportDto::kCan;
    if (!blk.GetXcpOnFlxs().empty()) return XcpTransportDto::kFlx;
    if (!blk.GetXcpOnUsbs().empty()) return XcpTransportDto::kUsb;
    if (!blk.GetXcpOnSxis().empty()) return XcpTransportDto::kSxi;
    if (!blk.GetXcpOnTcpIps().empty()) return XcpTransportDto::kTcpIp;
    if (!blk.GetXcpOnUdpIps().empty()) return XcpTransportDto::kUdpIp;
    if (!blk.GetXcpOnSimulinks().empty()) return XcpTransportDto::kSimulink;
    return XcpTransportDto::kNone;
}

// ----------------------------------------------------------------------------
// RECORD_LAYOUT 平坦性判定（B-4 最小推导前置，批次10）
// ----------------------------------------------------------------------------

/**
 * @brief 判断 RECORD_LAYOUT 是否为"纯标量 VALUE 版式"（除 FNC_VALUES
 * 外全默认）。
 *
 * 只要存在任何轴点/距离算子/保留字段/识别字段（哪怕 DataType==UNKNOWN 之外的
 * 残留），即拒绝推导 —— 元素类型必须能被唯一证明（B-4 禁止猜测）。
 * @param rl 上游 RECORD_LAYOUT。
 * @return true = 除 FNC_VALUES 外无任何布局字段。
 */
bool IsPlainValueLayout(const a2l::RecordLayout& rl) {
    auto dist_empty = [](const a2l::A2lDistOp& d) {
        return d.DataType == a2l::A2lDataType::UNKNOWN && d.Position == 0;
    };
    auto axis_empty = [](const a2l::A2lAxisPts& a) {
        return a.DataType == a2l::A2lDataType::UNKNOWN;
    };
    if (!axis_empty(rl.AxisPtsX()) || !axis_empty(rl.AxisPtsY()) ||
        !axis_empty(rl.AxisPtsZ()) || !axis_empty(rl.AxisPts4()) ||
        !axis_empty(rl.AxisPts5())) {
        return false;
    }
    if (rl.AxisRescaleX().DataType != a2l::A2lDataType::UNKNOWN) {
        return false;
    }
    const a2l::A2lDistOp* dists[] = {
        &rl.DistOpX(),    &rl.DistOpY(),    &rl.DistOpZ(),    &rl.DistOp4(),
        &rl.DistOp5(),    &rl.NoAxisPtsX(), &rl.NoAxisPtsY(), &rl.NoAxisPtsZ(),
        &rl.NoAxisPts4(), &rl.NoAxisPts5(), &rl.NoRescaleX(), &rl.OffsetX(),
        &rl.OffsetY(),    &rl.OffsetZ(),    &rl.Offset4(),    &rl.Offset5(),
        &rl.RipAddrW(),   &rl.RipAddrX(),   &rl.RipAddrY(),   &rl.RipAddrZ(),
        &rl.RipAddr4(),   &rl.RipAddr5(),   &rl.SrcAddrX(),   &rl.SrcAddrY(),
        &rl.SrcAddrZ(),   &rl.SrcAddr4(),   &rl.SrcAddr5(),   &rl.ShiftOpX(),
        &rl.ShiftOpY(),   &rl.ShiftOpZ(),   &rl.ShiftOp4(),   &rl.ShiftOp5(),
    };
    for (const a2l::A2lDistOp* d : dists) {
        if (!dist_empty(*d)) return false;
    }
    if (rl.FixNoAxisPtsX() != 0 || rl.FixNoAxisPtsY() != 0 ||
        rl.FixNoAxisPtsZ() != 0 || rl.FixNoAxisPts4() != 0 ||
        rl.FixNoAxisPts5() != 0) {
        return false;
    }
    if (!rl.ReservedList().empty()) return false;
    if (rl.Identification().DataType != a2l::A2lDataType::UNKNOWN) {
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// /include 图预扫描（B-18：循环检测 + 深度≤32 + 根目录约束，批次10）
//
// 上游 A2lScanner::FixIncludeFile() 自身无循环/深度/越根保护（.a2l 递归
// ParseFile 会在自包含场景栈溢出），因此必须在 ParseFile 之前拦截。
// 手段：只剔除注释后行扫 /include 指令（非完整词法）；解析不到/文件缺失
// 的 include 交给上游原样报错，本扫描只拦截"确定存在的"循环/深度/越根。
// ----------------------------------------------------------------------------

/** @brief 剔除行注释与块注释（保留字符串字面量，其内可能含 include 路径）。 */
std::string StripLineAndBlockComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool in_line = false;
    bool in_block = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';
        if (in_line) {
            if (c == '\n') {
                in_line = false;
                out.push_back('\n');
            }
            continue;
        }
        if (in_block) {
            if (c == '*' && n == '/') {
                in_block = false;
                ++i;
            }
            continue;
        }
        if (c == '/' && n == '/') {
            in_line = true;
            ++i;
            continue;
        }
        if (c == '/' && n == '*') {
            in_block = true;
            ++i;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

/** @brief 读整个文件为字符串（二进制读入；失败返回 false）。 */
bool ReadWholeFile(const std::filesystem::path& p, std::string* out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

/** @brief 从 "/include" 关键字后取下一条路径 token（跳过空白与引号）。 */
std::string ParseIncludeToken(const std::string& line, std::size_t from) {
    std::string token;
    std::size_t i = from + 8;  // 跳过 "/include" 本身
    while (
        i < line.size() &&
        (std::isspace(static_cast<unsigned char>(line[i])) || line[i] == '"')) {
        ++i;
    }
    while (i < line.size() &&
           !std::isspace(static_cast<unsigned char>(line[i])) &&
           line[i] != '"') {
        token.push_back(line[i]);
        ++i;
    }
    return token;
}

/** @brief target 是否位于 root 目录树内（weakly_canonical 后相对路径判定）。 */
bool IsWithinRoot(const std::filesystem::path& root,
                  const std::filesystem::path& target) {
    std::error_code ec;
    const std::filesystem::path rel =
        std::filesystem::relative(target, root, ec);
    if (ec) {
        return false;
    }
    const std::string s = rel.generic_string();
    return s.empty() || (s.rfind("../", 0) != 0 && s != "..");
}

/** @brief 取路径末段为窄字符串（错误消息拼接用；char8_t 无法与 char 相加）。 */
std::string PathNameUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.filename().u8string();
    return std::string(u8.begin(), u8.end());
}

/**
 * @brief 拼接完整 include 链文本（B-18 批次11：循环/深度错误的显示契约）。
 * @param chain 递归链（主文件 → 当前文件）。
 * @param tail 链尾文件（触发循环回指或深度超限的目标）。
 * @return 形如 `main.a2l -> a.a2l -> b.a2l -> a.a2l` 的箭头链。
 * @details B-19 下 message 仅作展示；业务逻辑仍只判 ErrorCode 不匹配文本。
 */
std::string FormatIncludeChain(const std::vector<std::filesystem::path>& chain,
                               const std::filesystem::path& tail) {
    std::string out;
    for (const auto& node : chain) {
        if (!out.empty()) {
            out += " -> ";
        }
        out += PathNameUtf8(node);
    }
    if (!out.empty()) {
        out += " -> ";
    }
    out += PathNameUtf8(tail);
    return out;
}

/**
 * @brief 递归扫描 include 图（单一链式 DFS，chain 即当前路径）。
 *
 * @param current 当前文件（已 canonical 化）。
 * @param root 主 A2L 所在目录（canonical，越根约束基准）。
 * @param allow_outside 是否允许越根（B-18 默认禁止）。
 * @param chain 当前递归链（含 current），重复即循环。
 * @param depth 当前深度（主文件 = 1；>32 拒绝）。
 * @param err 失败信息输出。
 * @param err_line 失败相关行号（0=无信息）。
 * @return true = 扫描通过；false = 存在循环/越根/超深。
 */
bool WalkIncludes(const std::filesystem::path& current,
                  const std::filesystem::path& root, bool allow_outside,
                  std::vector<std::filesystem::path>* chain, int depth,
                  std::string* err, std::uint32_t* err_line) {
    constexpr int kMaxDepth = 32;  // B-18：最大深度 32
    if (depth > kMaxDepth) {
        *err = "include depth exceeds " + std::to_string(kMaxDepth) + ": " +
               PathNameUtf8(current);
        return false;
    }
    std::string text;
    if (!ReadWholeFile(current, &text)) {
        return true;  // 读不到的文件交给上游报错
    }
    const std::string stripped = StripLineAndBlockComments(text);
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < stripped.size()) {
        const std::size_t eol = stripped.find('\n', pos);
        const std::string line = stripped.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        ++line_no;
        const std::size_t hit = line.find("/include");
        if (hit != std::string::npos) {
            const std::string token = ParseIncludeToken(line, hit);
            if (!token.empty()) {
                std::filesystem::path child(token);
                if (!child.is_absolute()) {
                    child = current.parent_path() / child;
                }
                std::error_code ec;
                const std::filesystem::path canon =
                    std::filesystem::weakly_canonical(child, ec);
                const std::filesystem::path resolved =
                    ec ? child.lexically_normal() : canon;
                if (!allow_outside && !IsWithinRoot(root, resolved)) {
                    *err = "include escapes A2L root: " + token + " (from " +
                           PathNameUtf8(current) + ")";
                    *err_line = static_cast<std::uint32_t>(line_no);
                    return false;
                }
                for (const auto& seen : *chain) {
                    if (seen == resolved) {
                        // B-18 批次11：message 携带完整链（B-19 显示契约，
                        // 业务仍只判 ErrorCode 不匹配文本）
                        *err = "include cycle detected: " +
                               FormatIncludeChain(*chain, resolved);
                        *err_line = static_cast<std::uint32_t>(line_no);
                        return false;
                    }
                }
                if (std::filesystem::exists(resolved) &&
                    std::filesystem::is_regular_file(resolved, ec)) {
                    // B-18 深度检查放在父层（递归调用点），以便携带引发
                    // 超深的 /include 所在行号（入口层无行号信息）
                    if (depth + 1 > kMaxDepth) {
                        *err = "include depth exceeds " +
                               std::to_string(kMaxDepth) + ": " +
                               FormatIncludeChain(*chain, resolved);
                        *err_line = static_cast<std::uint32_t>(line_no);
                        return false;
                    }
                    chain->push_back(resolved);
                    const bool ok =
                        WalkIncludes(resolved, root, allow_outside, chain,
                                     depth + 1, err, err_line);
                    chain->pop_back();
                    if (!ok) {
                        return false;
                    }
                }
                // 缺失/非常规文件不在此报错：交由上游 FixIncludeFile 原样报
            }
        }
        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 1;
    }
    return true;
}

/**
 * @brief Load 前置的 include 图预扫描入口（B-18）。
 * @param main_path 主 A2L 文件（UTF-8 路径，已确认存在）。
 * @param allow_outside 是否允许 include 越根。
 * @param err 失败信息；@param err_line 失败相关行号。
 * @return true = 通过（或无 include）。
 */
bool PreScanIncludes(const std::string& main_path, bool allow_outside,
                     std::string* err, std::uint32_t* err_line) {
    try {
        std::error_code ec;
        const std::filesystem::path main_canon =
            std::filesystem::weakly_canonical(
                std::filesystem::u8path(main_path), ec);
        const std::filesystem::path start =
            ec ? std::filesystem::u8path(main_path).lexically_normal()
               : main_canon;
        const std::filesystem::path root = start.parent_path();
        std::vector<std::filesystem::path> chain{start};
        return WalkIncludes(start, root, allow_outside, &chain, 1, err,
                            err_line);
    } catch (const std::exception& ex) {
        *err = std::string("include pre-scan failed: ") + ex.what();
        return false;
    }
}

/**
 * @brief 把上游 CompuMethod（连同其引用的 COMPU_TAB / UNIT）拍平成
 * ConversionDto。
 * @param cm     上游转换方法（可为空指针 → kind=kNone）。
 * @param module 所属模块（查 COMPU_TAB / UNIT）。
 * @return 转换快照。
 */
ConversionDto BuildConversion(const a2l::CompuMethod* cm,
                              const a2l::Module& module) {
    ConversionDto out;
    if (cm == nullptr) {
        return out;  // kind 默认 kNone
    }
    out.name = cm->Name();
    out.kind = MapConversionType(cm->Type());
    out.formula = cm->Formula();
    out.status_string_ref = cm->StatusStringRef();

    // 单位：RefUnit 名直接透出（UNIT 对象在 bridge 侧不需要更多结构）
    out.unit = cm->RefUnit();

    // LINEAR：语法 COEFFS_LINEAR offset factor 两参数；兼容退化的 COEFFS
    // 前两位。
    if (out.kind == ConversionKindDto::kLinear) {
        const auto& lin = cm->CoeffsLinear();
        const auto& co = cm->Coeffs();
        if (lin.size() >= 2) {
            out.coeffs.o = lin[0];  // 偏移 O
            out.coeffs.c = lin[1];  // 比例 C
            out.coeffs.f = 0.0;     // LINEAR 无独立常数项来源
        } else if (co.size() >= 2) {
            out.coeffs.o = co[0];
            out.coeffs.c = co[1];
            out.coeffs.f = 0.0;
        }
    } else if (out.kind == ConversionKindDto::kRatFunc) {
        // COEFFS N1 N2 N3 D1 D2 D3（上游 grammar 顺序）
        const auto& co = cm->Coeffs();
        if (co.size() >= 6) {
            out.coeffs.n1 = co[0];
            out.coeffs.n2 = co[1];
            out.coeffs.n3 = co[2];
            out.coeffs.d1 = co[3];
            out.coeffs.d2 = co[4];
            out.coeffs.d3 = co[5];
        }
    } else if (out.kind == ConversionKindDto::kTabIntp ||
               out.kind == ConversionKindDto::kTabNoIntp) {
        const a2l::CompuTab* tab = module.GetCompuTab(cm->CompuTabRef());
        if (tab != nullptr) {
            for (const auto& [in_val, out_val] : tab->KeyValueList()) {
                ConversionTableEntryDto e;
                e.input_min = in_val;
                e.input_max = in_val;
                e.numeric_output = true;
                e.numeric_value = out_val;
                e.output_text = std::to_string(out_val);
                out.table.push_back(e);
            }
        }
    } else if (out.kind == ConversionKindDto::kTabVerb) {
        const a2l::CompuVtab* vtab = module.GetCompuVtab(cm->CompuTabRef());
        if (vtab != nullptr) {
            for (const auto& [in_val, text] : vtab->KeyValueList()) {
                ConversionTableEntryDto e;
                e.input_min = in_val;
                e.input_max = in_val;
                e.numeric_output = false;
                e.output_text = text;
                out.table.push_back(e);
            }
        }
    }
    return out;
}

/**
 * @brief 采集一个 MEASUREMENT 的完整快照。
 * @param m      上游测量对象。
 * @param module 所属模块。
 * @return 符号 DTO。
 */
SymbolDto BuildMeasurementSymbol(const a2l::Measurement& m,
                                 const a2l::Module& module) {
    SymbolDto s;
    s.module_name = module.Name();
    s.name = m.Name();
    s.description = m.Description();
    s.kind = SymbolKindDto::kMeasurement;
    s.characteristic_type = CharacteristicTypeDto::kNone;
    s.data_type = MapDataType(m.DataType());
    s.element_size_bytes = TypeWidthBytes(s.data_type);
    s.xcp_address = m.EcuAddress();
    s.address_extension =
        static_cast<std::uint8_t>(m.EcuAddressExtension() & 0xFF);
    s.byte_order = MapByteOrder(m.ByteOrder());
    s.array_order = MapLayout(m.Layout());
    s.bit_mask = m.BitMask();
    s.error_mask = m.ErrorMask();
    // B-2 规则连续布局：stride = 前面低维元素数 × 元素宽（批次10 修正；
    // bridge 的 CountElements 以同一递推做二次校验，不规则即整组拒绝）
    std::uint64_t low_extent_product = 1;
    if (m.ArraySize() > 0) {
        DimensionDto d;
        d.extent = m.ArraySize();
        d.byte_stride = s.element_size_bytes;
        s.dimensions.push_back(d);
        low_extent_product *= m.ArraySize();
    }
    for (const auto dim : m.MatrixDim()) {
        DimensionDto d;
        d.extent = dim;
        d.byte_stride = low_extent_product * s.element_size_bytes;
        s.dimensions.push_back(d);
        low_extent_product *= dim;
    }
    // 多维规则性校验交给 bridge（B-1/B-15），SDK 只忠实透传源数据。
    s.read_write = m.ReadWrite();
    if (m.HaveLimit()) {
        s.have_limit = true;
        s.lower_limit = m.LowerLimit();
        s.upper_limit = m.UpperLimit();
    }
    s.phys_unit = m.PhysUnit();
    s.ref_memory_segment = m.RefMemorySegment();
    s.compu_method_name = m.Conversion();
    s.conversion =
        BuildConversion(module.GetCompuMethod(m.Conversion()), module);
    return s;
}

/**
 * @brief 采集一个 CHARACTERISTIC 的完整快照。
 * @param c      上游特性对象。
 * @param module 所属模块。
 * @return 符号 DTO。
 */
SymbolDto BuildCharacteristicSymbol(const a2l::Characteristic& c,
                                    const a2l::Module& module) {
    SymbolDto s;
    s.module_name = module.Name();
    s.name = c.Name();
    s.description = c.Description();
    s.kind = SymbolKindDto::kCharacteristic;
    s.characteristic_type = MapCharType(c.Type());
    // B-4 最小推导（批次10）：仅 CHARACTERISTIC VALUE 且其 RECORD_LAYOUT 为
    // "纯标量 VALUE 版式"（除单段 FNC_VALUES 外全默认、FNC_VALUES 起始
    // 位置为 0、ADDRESS_TYPE=DIRECT）时，才由 FNC_VALUES 的 DATA_TYPE 证明
    // 元素类型；VAL_BLK/ASCII/CURVE/MAP/复杂版式/缺版式一律保持 kUnknown，
    // 由 bridge 依 B-3 显式拒绝数值读写（禁止静默猜测）。
    s.data_type = AsamDataTypeDto::kUnknown;
    s.element_size_bytes = 0;
    if (c.Type() == a2l::A2lCharacteristicType::VALUE) {
        if (const a2l::RecordLayout* rl = module.GetRecordLayout(c.Deposit());
            rl != nullptr) {
            const a2l::A2lFncValue& fnc = rl->FncValues();
            if (fnc.DataType != a2l::A2lDataType::UNKNOWN &&
                fnc.Position == 0 &&
                fnc.AddressType == a2l::A2lAddressType::DIRECT &&
                IsPlainValueLayout(*rl)) {
                s.data_type = MapDataType(fnc.DataType);
                s.element_size_bytes = TypeWidthBytes(s.data_type);
            }
        }
    }
    s.xcp_address = c.Address();
    s.address_extension =
        static_cast<std::uint8_t>(c.EcuAddressExtension() & 0xFF);
    s.byte_order = MapByteOrder(c.ByteOrder());
    s.bit_mask = c.BitMask();
    s.read_write = !c.ReadOnly();
    if (c.HaveLimit()) {
        s.have_limit = true;
        s.lower_limit = c.LowerLimit();
        s.upper_limit = c.UpperLimit();
    }
    s.phys_unit = c.PhysUnit();
    s.ref_memory_segment = c.RefMemorySegment();
    s.compu_method_name = c.Conversion();
    s.conversion =
        BuildConversion(module.GetCompuMethod(c.Conversion()), module);
    return s;
}

// ----------------------------------------------------------------------------
// IF_DATA XCP 提取辅助（批次10：能力块/事件通道/§6.3-A 冲突比对）
// ----------------------------------------------------------------------------

/**
 * @brief 上游 DAQ 能力块 → DaqCapsDto（枚举存原始码，bridge 负责语义映射）。
 * @param dq 上游 DAQ 公共参数块。
 * @return 能力快照。
 */
DaqCapsDto BuildDaqCaps(const a2l::xcp::Daq& dq) {
    DaqCapsDto c;
    c.type = static_cast<std::uint8_t>(dq.GetType());
    c.max_daq = dq.GetMaxDaq();
    c.max_event_channel = dq.GetMaxEvent();
    c.min_daq = dq.GetMinDaq();
    c.optimisation = static_cast<std::uint8_t>(dq.GetOptimisationType());
    c.address_extension_mode =
        static_cast<std::uint8_t>(dq.GetAddressExtension());
    c.identification_field_type =
        static_cast<std::uint8_t>(dq.GetIdentificationFieldType());
    c.granularity =
        static_cast<std::uint8_t>(dq.GetGranularityOdtEntrySizeDaq());
    c.max_odt_entry_size = dq.GetMaxOdtEntrySize();
    c.overload_indicator = static_cast<std::uint8_t>(dq.GetOverloadIndicator());
    c.prescaler_supported = dq.GetPrescalerSupported();
    c.resume_supported = dq.GetResumeSupported();
    c.store_daq_supported = dq.GetStoreDaqSupported();
    c.dto_ctr_supported = dq.GetDtoCtrSupported();
    c.pid_off_supported = dq.GetPidOffSupported();
    c.odt_strict = dq.GetOptimisationTypeOdtStrict();
    if (const auto* ts = dq.GetTimestamp(); ts != nullptr) {
        using SS = a2l::xcp::TimestampSize;
        switch (ts->GetSize()) {
            case SS::SIZE_BYTE:
                c.timestamp_size_bits = 8;
                break;
            case SS::SIZE_WORD:
                c.timestamp_size_bits = 16;
                break;
            case SS::SIZE_DWORD:
                c.timestamp_size_bits = 32;
                break;
            case SS::NO_TIMESTAMP:
            default:
                break;  // 未声明 → nullopt
        }
    }
    return c;
}

/**
 * @brief 上游 EVENT 通道 → EventChannelDto（时间字段为原始码，见 DTO 注释）。
 * @param ev 上游事件通道。
 * @return 事件快照。
 */
EventChannelDto BuildEventDto(const a2l::xcp::Event& ev) {
    EventChannelDto e;
    e.name = ev.GetName();
    e.short_name = ev.GetShortName();
    e.number = ev.GetNumber();
    e.type =
        static_cast<std::uint8_t>(ev.GetType());  // 1=DAQ 2=STIM 3=DAQ_STIM
    e.max_daq_list = ev.GetMaxDaqList();
    e.time_cycle = ev.GetTimeCycle();
    e.time_unit = ev.GetTimeUnit();
    e.priority = ev.GetPriority();
    if (const auto& cons = ev.GetConsistency(); cons.has_value()) {
        e.consistency = static_cast<std::uint8_t>(*cons);
    }
    return e;
}

/**
 * @brief 在模块内按 §6.3-A 定位有效 XCP 块（只看 IsOk 的块）。
 *
 * 两遍搜索保证"XCPplus 在任何位置都优先于 plain XCP"：
 * 位置优先级 = 模块级（A2lObject 基类）> MEMORY_SEGMENT > MEMORY_LAYOUT。
 * @param m 上游模块。@param want_plus true=只找 XCPplus，false=只找 plain XCP。
 * @return 有效块；无则 nullptr。
 */
const a2l::xcp::XcpDataBlock* FindXcpBlock(const a2l::Module& m,
                                           bool want_plus) {
    const auto pick = [](const a2l::xcp::XcpDataBlock* b) {
        return (b != nullptr && b->IsOk()) ? b : nullptr;
    };
    if (want_plus) {
        if (const auto* b = pick(m.GetXcpPlusDataBlock())) return b;
        for (const auto& seg : m.ModPar().MemorySegmentList) {
            if (const auto* b = pick(seg.GetXcpPlusDataBlock())) return b;
        }
        for (const auto& lay : m.ModPar().MemoryLayoutList) {
            if (const auto* b = pick(lay.GetXcpPlusDataBlock())) return b;
        }
    } else {
        if (const auto* b = pick(m.GetXcpDataBlock())) return b;
        for (const auto& seg : m.ModPar().MemorySegmentList) {
            if (const auto* b = pick(seg.GetXcpDataBlock())) return b;
        }
        for (const auto& lay : m.ModPar().MemoryLayoutList) {
            if (const auto* b = pick(lay.GetXcpDataBlock())) return b;
        }
    }
    return nullptr;
}

/** @brief 追加一条 XCP vs XCPplus 参数差异（值相同则不记录）。 */
void AppendConflict(const char* param, const std::string& plain_value,
                    const std::string& plus_value,
                    std::vector<XcpPlusConflictDto>* out) {
    if (plain_value == plus_value) {
        return;
    }
    out->push_back(XcpPlusConflictDto{param, plain_value, plus_value});
}

/**
 * @brief §6.3-A：同参数两块取值差异比对（仅标量/端点字段，Info 级）。
 * @param plain plain XCP 块快照。@param plus XCPplus 块快照（以它为准）。
 * @param out 差异列表输出。
 */
void DiffIfData(const IfDataXcpDto& plain, const IfDataXcpDto& plus,
                std::vector<XcpPlusConflictDto>* out) {
    const auto bo_name = [](ByteOrderDto b) -> std::string {
        switch (b) {
            case ByteOrderDto::kMsbLast:
                return "MSB_LAST";
            case ByteOrderDto::kMsbFirst:
                return "MSB_FIRST";
            default:
                return "UNKNOWN";
        }
    };
    AppendConflict("PROTOCOL_VERSION", std::to_string(plain.protocol_version),
                   std::to_string(plus.protocol_version), out);
    AppendConflict("MAX_CTO", std::to_string(plain.max_cto),
                   std::to_string(plus.max_cto), out);
    AppendConflict("MAX_DTO", std::to_string(plain.max_dto),
                   std::to_string(plus.max_dto), out);
    AppendConflict("BYTE_ORDER", bo_name(plain.byte_order),
                   bo_name(plus.byte_order), out);
    AppendConflict("ADDRESS_GRANULARITY",
                   std::to_string(plain.address_granularity),
                   std::to_string(plus.address_granularity), out);
    AppendConflict("TRANSPORT",
                   std::to_string(static_cast<int>(plain.transport)),
                   std::to_string(static_cast<int>(plus.transport)), out);
    AppendConflict("UDP_PORT", std::to_string(plain.udp_port),
                   std::to_string(plus.udp_port), out);
    AppendConflict("UDP_HOST", plain.udp_host, plus.udp_host, out);
    AppendConflict("SEED_AND_KEY", plain.seed_and_key_function,
                   plus.seed_and_key_function, out);
}

}  // namespace

// ----------------------------------------------------------------------------
// Doc 实现类（DLL 内部；对消费方只暴露 IDoc 抽象）
// ----------------------------------------------------------------------------

/**
 * @class Doc
 * @brief IDoc 的唯一实现：持有 A2lFile 与派生快照缓存。
 *
 * 线程模型（B-20/A-11）：Load 成功后快照不可变，查询接口可并发调用；
 * LoadAsync 期间不得调用查询接口（由消费方保证，SDK 不加锁）。
 */
class Doc final : public IDoc {
public:
    Doc() = default;

    /** @brief 析构时回收异步解析线程（若仍在运行则 join，避免悬挂）。 */
    ~Doc() override {
        if (parse_thread_.joinable()) {
            parse_thread_.join();
        }
    }

    // ------------------------------------------------------------------ 加载

    /** @copydoc IDoc::Load */
    ErrorCode Load(const std::string& file_path,
                   bool module_information_only) noexcept override {
        try {
            ResetState();
            file_ = std::make_unique<a2l::A2lFile>();
            file_->Filename(file_path);
            file_->ParserType(
                module_information_only
                    ? a2l::A2lParserType::PARSE_MODULE_INFORMATION_ONLY
                    : a2l::A2lParserType::FULL_PARSING);
            // 预读检查：给缺失文件一个明确的 IoError，而不是上游泛化异常文本
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::u8path(file_path),
                                         ec)) {
                SetError(ErrorCode::kIoError, "file not found: " + file_path);
                return ErrorCode::kIoError;
            }
            // B-18 include 图预扫描（循环/深度/越根在上游解析前拦截，
            // 因为上游 FixIncludeFile 对 .a2l 递归 ParseFile 无任何保护）
            std::string include_err;
            std::uint32_t include_line = 0;
            if (!PreScanIncludes(file_path, allow_include_outside_root_,
                                 &include_err, &include_line)) {
                SetError(ErrorCode::kParseFailed, include_err, include_line);
                return ErrorCode::kParseFailed;
            }
            if (!file_->ParseFile()) {
                const std::uint32_t line =
                    file_->LineNo() > 0
                        ? static_cast<std::uint32_t>(file_->LineNo())
                        : 0;
                SetError(ErrorCode::kParseFailed,
                         "parse failed at line " +
                             std::to_string(file_->LineNo()) + ": " +
                             file_->LastError(),
                         line);
                return ErrorCode::kParseFailed;
            }
            BuildSnapshots();
            loaded_ = true;
            progress_ = 100;
            ClearError();
            return ErrorCode::kOk;
        } catch (const std::exception& ex) {
            SetError(ErrorCode::kInternal, ex.what());
            return ErrorCode::kInternal;
        } catch (...) {
            SetError(ErrorCode::kInternal, "unknown exception");
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::LoadAsync */
    ErrorCode LoadAsync(
        const std::string& file_path, bool module_information_only,
        std::function<void(int)> /*progress_cb*/,
        std::function<void(int)> completed_cb) noexcept override {
        if (!completed_cb) {
            SetError(ErrorCode::kBadArgument, "completed callback required");
            return ErrorCode::kBadArgument;
        }
        try {
            if (parse_thread_.joinable()) {
                parse_thread_.join();
            }
            // 后台线程复用同步 Load。进度回调暂不透传（上游 ProgressInfo 属另一
            // 解析路径，本 SDK 主用 ParseFile；保留接口形状供后续接入）。
            async_path_ = file_path;
            async_module_only_ = module_information_only;
            completed_cb_local_ = std::move(completed_cb);
            parse_thread_ = std::thread([this] { RunAsyncParse(); });
            return ErrorCode::kOk;
        } catch (const std::exception& ex) {
            SetError(ErrorCode::kInternal, ex.what());
            return ErrorCode::kInternal;
        } catch (...) {
            SetError(ErrorCode::kInternal, "unknown exception");
            return ErrorCode::kInternal;
        }
    }

    // ---------------------------------------------------- 加载期配置（批次10）

    /** @copydoc IDoc::SetActiveModule */
    void SetActiveModule(const std::string& module_name) noexcept override {
        active_module_ = module_name;
    }

    /** @copydoc IDoc::AllowIncludeOutsideRoot */
    void AllowIncludeOutsideRoot(bool allow) noexcept override {
        allow_include_outside_root_ = allow;
    }

    // ------------------------------------------------------------------ 查询

    /** @copydoc IDoc::LastError */
    const char* LastError() const noexcept override {
        return last_error_.c_str();
    }

    /** @copydoc IDoc::LastErrorCode */
    ErrorCode LastErrorCode() const noexcept override { return last_code_; }

    /** @copydoc IDoc::LastErrorLine */
    std::uint32_t LastErrorLine() const noexcept override {
        return last_error_line_;
    }

    /** @copydoc IDoc::Progress */
    int Progress() const noexcept override { return progress_; }

    /** @copydoc IDoc::ModuleCount */
    std::size_t ModuleCount() const noexcept override {
        if (!loaded_ || !file_) {
            return 0;
        }
        return file_->Project().Modules().size();
    }

    /** @copydoc IDoc::ListSymbols */
    ErrorCode ListSymbols(std::vector<SymbolDto>* out) const noexcept override {
        if (out == nullptr) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            *out = symbols_;  // 快照整体拷贝（R3）
            return ErrorCode::kOk;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::FindSymbol */
    ErrorCode FindSymbol(const std::string& qualified_or_unique_name,
                         SymbolDto* out) const noexcept override {
        if (out == nullptr || qualified_or_unique_name.empty()) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            const auto pos = qualified_or_unique_name.find("::");
            if (pos != std::string::npos) {
                // 限定名 module::symbol —— 精确命中
                const std::string mod = qualified_or_unique_name.substr(0, pos);
                const std::string sym =
                    qualified_or_unique_name.substr(pos + 2);
                for (const auto& s : symbols_) {
                    if (s.module_name == mod && s.name == sym) {
                        *out = s;
                        return ErrorCode::kOk;
                    }
                }
                return ErrorCode::kNotFound;
            }
            // 裸名：全局唯一才允许（B-13），多义报错不猜测
            const SymbolDto* hit = nullptr;
            int count = 0;
            for (const auto& s : symbols_) {
                if (s.name == qualified_or_unique_name) {
                    hit = &s;
                    ++count;
                }
            }
            if (count == 0) {
                return ErrorCode::kNotFound;
            }
            if (count > 1) {
                return ErrorCode::kAmbiguousName;
            }
            *out = *hit;
            return ErrorCode::kOk;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::GetIfDataXcp */
    ErrorCode GetIfDataXcp(IfDataXcpDto* out) const noexcept override {
        if (out == nullptr) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            *out = if_data_xcp_;
            if (out->ambiguous) {
                // B-17：多 MODULE 同时声明且未指定 active_module ——
                // 不静默取首个
                return ErrorCode::kAmbiguousName;
            }
            return out->present ? ErrorCode::kOk : ErrorCode::kNotFound;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::ListDaqLists */
    ErrorCode ListDaqLists(
        std::vector<DaqListDto>* out) const noexcept override {
        if (out == nullptr) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            *out = daq_lists_;
            return ErrorCode::kOk;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::GetConversion */
    ErrorCode GetConversion(const std::string& compu_method_name,
                            ConversionDto* out) const noexcept override {
        if (out == nullptr || compu_method_name.empty()) {
            return ErrorCode::kBadArgument;
        }
        if (!loaded_) {
            return ErrorCode::kNotInitialized;
        }
        try {
            for (const auto& kv : conversions_) {
                if (kv.first == compu_method_name) {
                    *out = kv.second;
                    return ErrorCode::kOk;
                }
            }
            return ErrorCode::kNotFound;
        } catch (...) {
            return ErrorCode::kInternal;
        }
    }

    /** @copydoc IDoc::Release */
    void Release() noexcept override { delete this; }

private:
    /** 清空一次加载产生的全部状态。 */
    void ResetState() noexcept {
        loaded_ = false;
        progress_ = 0;
        symbols_.clear();
        daq_lists_.clear();
        conversions_.clear();
        if_data_xcp_ = {};
        file_.reset();
    }

    /**
     * @brief 记录错误码与文本。
     * @param code 错误码；@param msg 文本；@param line 行号（0=无信息，
     * 消费方仅在 >0 时填 Error.line，B-19）。
     */
    void SetError(ErrorCode code, const std::string& msg,
                  std::uint32_t line = 0) {
        last_code_ = code;
        last_error_ = msg;
        last_error_line_ = line;
    }

    /** 清除错误状态。 */
    void ClearError() {
        last_code_ = ErrorCode::kOk;
        last_error_.clear();
        last_error_line_ = 0;
    }

    /** 异步线程体：跑同步 Load 并把结果码回抛完成回调。 */
    void RunAsyncParse() noexcept {
        ErrorCode code = ErrorCode::kInternal;
        try {
            code = Load(async_path_, async_module_only_);
        } catch (...) {
            code = ErrorCode::kInternal;
        }
        if (completed_cb_local_) {
            completed_cb_local_(static_cast<int>(code));
        }
        completed_cb_local_ = nullptr;
    }

    /** 解析成功后构建全部 DTO 快照（排序保证确定性，B-20 不可变快照）。 */
    void BuildSnapshots() {
        a2l::A2lProject& project = file_->Project();
        // B-17：先收集所有含有效 IF_DATA 块的 MODULE（unordered_map 顺序
        // 不稳定，但选择只依赖"恰好一个"或"按名精确匹配"，不依赖顺序）
        std::vector<const a2l::Module*> if_data_candidates;
        for (const auto& [mod_name, module_ptr] : project.Modules()) {
            if (!module_ptr) {
                continue;
            }
            const a2l::Module& module = *module_ptr;
            // MEASUREMENT
            for (const auto& [name, m] : module.Measurements()) {
                if (m) {
                    symbols_.push_back(BuildMeasurementSymbol(*m, module));
                }
            }
            // CHARACTERISTIC
            for (const auto& [name, c] : module.Characteristics()) {
                if (c) {
                    symbols_.push_back(BuildCharacteristicSymbol(*c, module));
                }
            }
            // COMPU_METHOD 全量快照（bridge 可按名二次查询）
            for (const auto& [name, cm] : module.CompuMethods()) {
                if (cm && conversions_.find(name) == conversions_.end()) {
                    conversions_.emplace(name,
                                         BuildConversion(cm.get(), module));
                }
            }
            if (FindXcpBlock(module, /*want_plus=*/true) != nullptr ||
                FindXcpBlock(module, /*want_plus=*/false) != nullptr) {
                if_data_candidates.push_back(&module);
            }
        }
        // B-17 IF_DATA 来源选择：指定 active_module 则精确匹配；未指定时
        // 恰一个候选才自动选中，多个候选标记 ambiguous（GetIfDataXcp 返回
        // kAmbiguousName，绝不静默取首个）。
        const a2l::Module* chosen = nullptr;
        if (!active_module_.empty()) {
            for (const a2l::Module* m : if_data_candidates) {
                if (m->Name() == active_module_) {
                    chosen = m;
                    break;
                }
            }
        } else if (if_data_candidates.size() == 1) {
            chosen = if_data_candidates[0];
        } else if (if_data_candidates.size() > 1) {
            if_data_xcp_.ambiguous = true;  // present 保持 false
        }
        if (chosen != nullptr) {
            ExtractIfDataXcp(*chosen);
        }
        // 确定性排序（上游 unordered_map 迭代顺序不稳定）
        std::sort(symbols_.begin(), symbols_.end(),
                  [](const SymbolDto& a, const SymbolDto& b) {
                      if (a.module_name != b.module_name)
                          return a.module_name < b.module_name;
                      return a.name < b.name;
                  });
    }

    /**
     * @brief 从选定模块提取 IF_DATA（§6.3-A 选块 + 冲突比对 + DAQ 快照）。
     * @param module 已经 B-17 选定的 MODULE。
     */
    void ExtractIfDataXcp(const a2l::Module& module) {
        // §6.3-A：XCPplus 在任何位置优先于 plain XCP（两遍搜索）
        const a2l::xcp::XcpDataBlock* plus = FindXcpBlock(module, true);
        const a2l::xcp::XcpDataBlock* plain = FindXcpBlock(module, false);
        const a2l::xcp::XcpDataBlock* chosen = (plus != nullptr) ? plus : plain;
        if (chosen == nullptr) {
            return;  // 理论不可达（候选已保证有有效块），防御性保留
        }

        if_data_xcp_.present = true;
        if_data_xcp_.from_xcp_plus = (chosen == plus);
        if_data_xcp_.module_name = module.Name();
        FillIfDataFromBlock(*chosen, &if_data_xcp_,
                            /*with_daq_lists=*/true);

        // §6.3-A：两块并存时对同参数差异生成 Info 级记录（以 XCPplus 为准）
        if (plus != nullptr && plain != nullptr) {
            IfDataXcpDto plain_dto;
            FillIfDataFromBlock(*plain, &plain_dto, /*with_daq_lists=*/false);
            DiffIfData(plain_dto, if_data_xcp_, &if_data_xcp_.plus_conflicts);
        }
    }

    /**
     * @brief 单个 XCP 块 → DTO 快照（公共参数 + 能力 + 事件 + 可选 DAQ 列表）。
     * @param blk 上游块。@param dto 目标 DTO（调用方已置
     * present/module_name）。
     * @param with_daq_lists 是否把预定义 DAQ_LIST 追加进 daq_lists_
     *        （冲突比对的临时填充为 false，避免重复追加）。
     */
    void FillIfDataFromBlock(const a2l::xcp::XcpDataBlock& blk,
                             IfDataXcpDto* dto, bool with_daq_lists) {
        const a2l::xcp::CommonParameters& cp = blk.GetCommonParameters();
        if (const auto* pl = cp.GetProtocolLayer(); pl != nullptr) {
            dto->protocol_version = pl->GetVersion();
            dto->max_cto = pl->GetMaxCto();
            dto->max_dto = pl->GetMaxDto();
            dto->byte_order =
                pl->GetByteOrder() == a2l::xcp::ByteOrder::BYTE_ORDER_MSB_FIRST
                    ? ByteOrderDto::kMsbFirst
                    : ByteOrderDto::kMsbLast;
            dto->default_byte_order =
                static_cast<std::uint8_t>(pl->GetByteOrder());
            dto->address_granularity =
                static_cast<std::uint8_t>(pl->GetAddressGranularity());
            // 批次10（§6.1 数据补全）：t1..t7、可选命令码、Seed&Key、ECU 状态
            for (std::size_t i = 0; i < dto->timers.size(); ++i) {
                dto->timers[i] =
                    pl->GetTimer(static_cast<a2l::xcp::ProtocolTimer>(i));
            }
            for (const auto cmd : pl->GetOptionalCommands()) {
                dto->optional_commands.push_back(
                    static_cast<std::uint8_t>(cmd));
            }
            dto->seed_and_key_function = pl->GetSeedAndKeyFunction();
            dto->has_ecu_states = !pl->GetEcuStates().empty();
        }
        dto->transport = PickTransport(blk);
        if (dto->transport == XcpTransportDto::kUdpIp) {
            const auto& udps = blk.GetXcpOnUdpIps();
            if (!udps.empty()) {
                dto->udp_port = udps.front().GetPort();
                dto->udp_host = udps.front().GetHostName();
            }
        }

        if (const auto* dq = cp.GetDaq(); dq != nullptr) {
            // DAQ 能力块 + 事件通道（批次10）
            dto->daq_caps = BuildDaqCaps(*dq);
            for (const auto& ev : dq->GetEventList()) {
                dto->events.push_back(BuildEventDto(ev));
            }
            // 预定义列表快照（仅选定块填充，避免冲突比对时重复追加）
            if (with_daq_lists) {
                for (const auto& list : dq->GetDaqList()) {
                    DaqListDto d;
                    d.number = list.GetNumber();
                    if (const auto& t = list.GetType(); t.has_value()) {
                        switch (*t) {
                            case a2l::xcp::DaqListType::DAQ:
                                d.type = DaqListTypeDto::kDaq;
                                break;
                            case a2l::xcp::DaqListType::STIM:
                                d.type = DaqListTypeDto::kStim;
                                break;
                            case a2l::xcp::DaqListType::DAQ_STIM:
                                d.type = DaqListTypeDto::kDaqStim;
                                break;
                            default:
                                d.type = DaqListTypeDto::kUnknown;
                                break;
                        }
                    }
                    d.max_odt = list.GetMaxOdt();
                    d.max_odt_entries = list.GetMaxOdtEntries();
                    d.first_pid = list.GetFirstPid();
                    d.event_fixed = list.GetEventFixed();
                    d.daq_packed_mode_supported =
                        list.GetDaqPackedModeSupported();
                    for (const auto& odt : list.GetPredefinedList()) {
                        OdtDto o;
                        o.number = odt.number;
                        for (const auto& e : odt.odt_entry_list) {
                            OdtEntryDto ed;
                            ed.number = e.number;
                            ed.address = e.address;
                            ed.address_extension = e.address_extension;
                            ed.size = e.size;
                            ed.bit_offset = e.bit_offset;
                            o.entries.push_back(ed);
                        }
                        d.predefined_odts.push_back(o);
                    }
                    daq_lists_.push_back(std::move(d));
                }
            }
        }
    }

    /** 上游解析器实例（DLL 内拥有，绝不出边界）。 */
    std::unique_ptr<a2l::A2lFile> file_;
    /** 符号快照（Load 后不可变）。 */
    std::vector<SymbolDto> symbols_;
    /** DAQ_LIST 快照。 */
    std::vector<DaqListDto> daq_lists_;
    /** COMPU_METHOD 名 → 转换快照。 */
    std::unordered_map<std::string, ConversionDto> conversions_;
    /** IF_DATA XCP 快照。 */
    IfDataXcpDto if_data_xcp_;
    /** 是否已成功加载。 */
    bool loaded_ = false;
    /** 进度百分比。 */
    int progress_ = 0;
    /** 最近错误码。 */
    ErrorCode last_code_ = ErrorCode::kOk;
    /** 最近错误文本。 */
    std::string last_error_;
    /** 最近错误的 A2L 行号（0=无信息；>0 才允许进 Error.line，B-19）。 */
    std::uint32_t last_error_line_ = 0;
    /** IF_DATA XCP 来源 MODULE（B-17；空=自动选择）。 */
    std::string active_module_;
    /** 是否允许 include 越根（B-18 默认禁止）。 */
    bool allow_include_outside_root_ = false;
    /** 异步解析线程。 */
    std::thread parse_thread_;
    /** 异步任务输入路径。 */
    std::string async_path_;
    /** 异步任务快速模式标志。 */
    bool async_module_only_ = false;
    /** 异步完成回调。 */
    std::function<void(int)> completed_cb_local_;
};

// ----------------------------------------------------------------------------
// 工厂（R2/R5）
// ----------------------------------------------------------------------------

/**
 * @brief 创建 IDoc 实例（DLL 侧 new；用毕 Release()）。
 * @param abi_version 调用方编译期 kLibA2lAbiVersion；不一致返回 nullptr。
 * @return 新对象或 nullptr。
 */
IDoc* CreateDoc(std::uint32_t abi_version) noexcept {
    try {
        if (abi_version != kLibA2lAbiVersion) {
            return nullptr;
        }
        return new Doc();
    } catch (...) {
        return nullptr;
    }
}

}  // namespace liba2l
