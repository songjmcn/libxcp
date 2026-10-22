// =============================================================================
// a2l_database_impl.cpp —— 不可变快照数据库（§4.2 / B-13/B-14/B-15/B-17/B-20）
//
// 生命周期：Load 成功后由 A2lBridge::Impl 一次性构造并发布（B-20）；
// 加载中/失败时 Database() 为 nullptr，本类不存在“半初始化”状态。
// 只读方法可并发调用（内部无任何可变状态）。类定义见 a2l_database_impl.hpp。
// =============================================================================

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "a2l_database_impl.hpp"
#include "a2l_dto_map.hpp"
#include "libxcp/a2l/compu_method.hpp"
#include "libxcp/a2l/record_layout.hpp"

namespace calmcar::xcp::a2l {
namespace {

/// @brief 小写化（ASCII，用于大小写无关检索与唯一性统计）
std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/**
 * @brief 通配符匹配（`*` 任意串、`?` 单字符；大小写无关）
 * @param pattern 已小写的模式
 * @param text 已小写的文本
 */
bool WildcardMatch(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t star_t = 0;
    while (t < text.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_t = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

/// @brief 构造查询阶段的结构化错误
Error QueryError(ErrorCode code, std::string symbol, std::string message) {
    Error e;
    e.code = code;
    e.phase = Phase::Query;
    e.severity = Severity::Error;
    e.symbol = std::move(symbol);
    e.message = std::move(message);
    return e;
}

/**
 * @brief 计算符号到最高被占用字节的可证明范围（包含末尾元素）。
 * @param symbol 符号快照
 * @param byte_span 输出的地址跨度
 * @return 类型宽度、维度和步长均可证明且无溢出时返回true
 */
bool TryGetByteSpan(const SymbolInfo& symbol, std::uint64_t* byte_span) {
    if (byte_span == nullptr || symbol.element_size_bytes == 0) {
        return false;
    }
    std::uint64_t span = symbol.element_size_bytes;
    for (const Dimension& dimension : symbol.dimensions) {
        if (dimension.extent == 0 || dimension.byte_stride == 0) {
            return false;
        }
        const std::uint64_t steps = dimension.extent - 1;
        if (steps > (std::numeric_limits<std::uint64_t>::max() - span) /
                        dimension.byte_stride) {
            return false;
        }
        span += steps * dimension.byte_stride;
    }
    *byte_span = span;
    return true;
}

/**
 * @brief 按模块、可选REF名、地址扩展及完整字节范围关联唯一MEMORY_SEGMENT。
 * @param symbol 符号
 * @param segments 已解码的静态MEMORY_SEGMENT快照
 * @return 唯一匹配段；无匹配或多个匹配时为空
 */
std::optional<MemorySegmentInfo> FindUniqueMemorySegment(
    const SymbolInfo& symbol, const std::vector<MemorySegmentInfo>& segments) {
    if (symbol.kind == SymbolKind::Structure) {
        return std::nullopt;
    }
    std::uint64_t byte_span = 0;
    if (!TryGetByteSpan(symbol, &byte_span) || byte_span == 0 ||
        symbol.xcp_address >
            std::numeric_limits<std::uint64_t>::max() - byte_span) {
        return std::nullopt;
    }
    const std::uint64_t symbol_end = symbol.xcp_address + byte_span;
    const MemorySegmentInfo* match = nullptr;
    for (const MemorySegmentInfo& segment : segments) {
        if (segment.module_name != symbol.module_name || segment.size == 0 ||
            segment.address_extension != symbol.address_extension ||
            (!symbol.ref_memory_segment.empty() &&
             symbol.ref_memory_segment != segment.name) ||
            segment.address >
                std::numeric_limits<std::uint64_t>::max() - segment.size) {
            continue;
        }
        const std::uint64_t segment_end = segment.address + segment.size;
        if (symbol.xcp_address < segment.address || symbol_end > segment_end) {
            continue;
        }
        if (match != nullptr) {
            return std::nullopt;
        }
        match = &segment;
    }
    if (match == nullptr) {
        return std::nullopt;
    }
    return *match;
}

}  // namespace

namespace detail {

// ----------------------------------------------------------------------------
// A2lDatabaseImpl 成员定义
// ----------------------------------------------------------------------------

A2lDatabaseImpl::A2lDatabaseImpl(
    std::vector<liba2l::SymbolDto> dtos,
    std::vector<liba2l::StructInfoDto> structs,
    std::unordered_map<std::string, RecordLayoutInfo> record_layouts,
    std::vector<SymbolInfo> leaf_symbols,
    std::vector<LoadWarning> leaf_warnings,
    std::vector<MemorySegmentInfo> memory_segments) {
    m_record_layouts_ = std::move(record_layouts);
    m_memory_segments_ = std::move(memory_segments);
    // 批次18（16-B08）：resolver 的不可证拒绝告警并入加载告警清单
    // （同一 F6 纪律："某个事实被放弃了"必须可见）
    m_load_warnings_ = std::move(leaf_warnings);
    m_symbols_.reserve(dtos.size() + structs.size() + leaf_symbols.size());
    for (const liba2l::SymbolDto& dto : dtos) {
        SymbolInfo info = ConvertSymbol(dto);
        const std::string key = MakeQualifiedKey(info.module_name, info.name);
        m_index_[key] = m_symbols_.size();
        ++m_bare_counts_[ToLower(info.name)];
        m_symbols_.push_back(std::move(info));
    }
    // 批次13（B-12）：TYPEDEF_STRUCTURE / INSTANCE 作为"仅元数据符号"并入同一
    // 快照，故 Find/Search 天然可命中；同名冲突时保留
    // MEASUREMENT/CHARACTERISTIC 并跳过结构体条目（B-13
    // 唯一性不猜测，绝不覆盖既有键）。
    for (const liba2l::StructInfoDto& dto : structs) {
        SymbolInfo info = ConvertStructure(dto);
        const std::string key = MakeQualifiedKey(info.module_name, info.name);
        if (m_index_.find(key) != m_index_.end()) {
            // 批次15（F6）：跳过是**必要**的（B-13 同名唯一性不猜），但被丢弃的
            // 一方不能无声 —— 留一条告警，让 UI 能回答"我定义的结构体去哪了"
            LoadWarning warning;
            warning.code = ErrorCode::AmbiguousName;
            warning.phase = Phase::Load;
            warning.subject = key;
            warning.message =
                "STRUCTURE/INSTANCE 与已有符号同名，按 B-13 保留后者、跳过前者";
            m_load_warnings_.push_back(std::move(warning));
            continue;
        }
        m_index_[key] = m_symbols_.size();
        ++m_bare_counts_[ToLower(info.name)];
        m_symbols_.push_back(std::move(info));
    }
    // 批次18（16-B08）：结构体成员叶子作为"限定路径符号"并入同一快照。
    // 与本体（STRUCTURE/INSTANCE 元数据）互不覆盖：同名冲突按 B-13 保留
    // 既有符号并告警跳过叶子；裸名规则不受影响（叶子 name 含 "."/全路径，
    // 只有完整限定路径或 module:: 前缀才命中，禁止跨结构猜测 16-C01）。
    std::unordered_set<std::string> leaf_keys;
    for (SymbolInfo& leaf : leaf_symbols) {
        const std::string key = MakeQualifiedKey(leaf.module_name, leaf.name);
        if (m_index_.find(key) != m_index_.end()) {
            LoadWarning warning;
            warning.code = ErrorCode::AmbiguousName;
            warning.phase = Phase::Load;
            warning.subject = key;
            warning.message =
                "STRUCTLEAF 路径与已有符号同名，按 B-13 保留后者、跳过叶子";
            m_load_warnings_.push_back(std::move(warning));
            continue;
        }
        m_index_[key] = m_symbols_.size();
        ++m_bare_counts_[ToLower(leaf.name)];
        leaf_keys.insert(key);
        m_symbols_.push_back(std::move(leaf));
    }
    // 只有完整字节范围、MODULE、地址扩展和可选REF均唯一匹配时才附加PAGE门事实。
    for (SymbolInfo& symbol : m_symbols_) {
        symbol.memory_segment =
            FindUniqueMemorySegment(symbol, m_memory_segments_);
    }
    // 稳定排序：按全限定名字典序（Search 输出顺序承诺，B-13）
    std::sort(m_symbols_.begin(), m_symbols_.end(),
              [](const SymbolInfo& a, const SymbolInfo& b) {
                  return MakeQualifiedKey(a.module_name, a.name) <
                         MakeQualifiedKey(b.module_name, b.name);
              });
    m_index_.clear();
    m_bare_counts_.clear();
    for (std::size_t i = 0; i < m_symbols_.size(); ++i) {
        const SymbolInfo& s = m_symbols_[i];
        const std::string key = MakeQualifiedKey(s.module_name, s.name);
        m_index_[key] = i;
        ++m_bare_counts_[ToLower(s.name)];
        if (leaf_keys.find(key) != leaf_keys.end()) {
            m_leaf_positions_.insert(i);
        }
    }
}

void A2lDatabaseImpl::BuildAddressIndex() {
    std::unordered_map<std::uint64_t, std::string> first;
    std::vector<std::uint64_t> ambiguous;
    std::unordered_map<std::uint64_t, std::vector<std::string>> all;
    for (std::size_t i = 0; i < m_symbols_.size(); ++i) {
        const SymbolInfo& s = m_symbols_[i];
        // 批次18：叶子不参与基址反查（保持既有 FindByAddress/aliases 行为；
        // DAQ 成员归属检索到批次20 再接线）
        if (m_leaf_positions_.find(i) != m_leaf_positions_.end()) {
            continue;
        }
        // 批次13（B-12）：STRUCTURE/INSTANCE 不参与基址反查——它们不可寻址
        // （成员不展开），进入反查表只会让 DAQ entry 归属到一个无法读写的符号。
        if (s.kind == SymbolKind::Structure) {
            continue;
        }
        const std::string key = MakeQualifiedKey(s.module_name, s.name);
        auto it = first.find(s.xcp_address);
        if (it == first.end()) {
            first.emplace(s.xcp_address, key);
        } else if (it->second != key) {
            ambiguous.push_back(s.xcp_address);
        }
        // 别名表不因歧义而丢弃：B-6 要求"不猜归属"与"保留候选"同时成立
        all[s.xcp_address].push_back(key);
    }
    for (std::uint64_t addr : ambiguous) {
        first.erase(addr);  // 歧义地址禁止猜测归属（§7.1-A 禁止项）
    }
    m_addr_to_symbol_ = std::move(first);
    for (auto& [addr, keys] : all) {
        std::sort(keys.begin(), keys.end());  // 稳定顺序（B-13 输出稳定承诺）
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    }
    m_addr_to_all_ = std::move(all);
}

std::string A2lDatabaseImpl::FindByAddress(std::uint64_t address) const {
    auto it = m_addr_to_symbol_.find(address);
    return it != m_addr_to_symbol_.end() ? it->second : std::string{};
}

std::vector<std::string> A2lDatabaseImpl::FindAllByAddress(
    std::uint64_t address) const {
    auto it = m_addr_to_all_.find(address);
    return it != m_addr_to_all_.end() ? it->second : std::vector<std::string>{};
}

Result<SymbolInfo> A2lDatabaseImpl::Find(std::string_view name) const {
    return Resolve(name);
}

Result<std::vector<SymbolInfo>> A2lDatabaseImpl::Search(
    std::string_view pattern, std::size_t max_count) const {
    if (pattern.empty() || max_count == 0) {
        return QueryError(ErrorCode::BadArgument, std::string(pattern),
                          "检索模式为空或 max_count 为 0");
    }
    const std::string lowered = ToLower(pattern);
    std::vector<SymbolInfo> hits;
    for (const SymbolInfo& s : m_symbols_) {
        const std::string key = MakeQualifiedKey(s.module_name, s.name);
        if (WildcardMatch(lowered, ToLower(key))) {
            hits.push_back(s);
            if (hits.size() >= max_count) {
                break;  // 稳定序前缀截断
            }
        }
    }
    return hits;
}

Result<std::size_t> A2lDatabaseImpl::Count() const { return m_symbols_.size(); }

// 批次13（R1 + R3 接线）：数值读写的统一准入门。放在 ByteSizeOf /
// ToPhysical / FromPhysical 三处 Resolve 之后、任何计算之前，
// 保证"拒绝先于猜测"（B-3/B-4/B-12 同一纪律）。
Result<void> A2lDatabaseImpl::GateExecutable(const SymbolInfo& symbol,
                                             std::string_view name) const {
    if (symbol.kind == SymbolKind::Structure) {
        // B-12：STRUCTURE/INSTANCE 只识别元数据，不展开成员、不按字节数组读写
        return QueryError(ErrorCode::UnsupportedOperation, std::string(name),
                          "STRUCTURE/INSTANCE 仅元数据，禁止数值读写（B-12）");
    }
    if (symbol.kind != SymbolKind::Characteristic) {
        return Result<void>{};  // MEASUREMENT 无 RECORD_LAYOUT 概念
    }
    const auto it = m_record_layouts_.find(
        MakeQualifiedKey(symbol.module_name, symbol.name));
    if (it == m_record_layouts_.end()) {
        // SDK 未给版式类别（无 DEPOSIT / 查不到 RECORD_LAYOUT）：
        // 不臆断"不可执行"，继续由 B-3 元素宽度门拒绝
        return Result<void>{};
    }
    Result<void> check = CheckRecordLayoutExecutable(it->second);
    if (!check.HasValue()) {
        Error e = check.TakeError();
        if (e.symbol.empty()) {
            e.symbol = std::string(name);
        }
        if (e.module.empty()) {
            e.module = symbol.module_name;
        }
        return e;
    }
    return Result<void>{};
}

Result<std::size_t> A2lDatabaseImpl::ByteSizeOf(std::string_view name) const {
    Result<SymbolInfo> found = Resolve(name);
    if (!found.HasValue()) {
        return found.TakeError();
    }
    const SymbolInfo& s = found.Value();
    Result<void> gate = GateExecutable(s, name);
    if (!gate.HasValue()) {
        return gate.TakeError();
    }
    Result<std::uint64_t> elements = CountElements(s);
    if (!elements.HasValue()) {
        return elements.TakeError();
    }
    if (s.element_size_bytes == 0) {
        return QueryError(ErrorCode::UnsupportedDataType, std::string(name),
                          "元素宽度未知，无法计算字节数（B-3）");
    }
    if (elements.Value() >
        std::numeric_limits<std::uint64_t>::max() / s.element_size_bytes) {
        return QueryError(ErrorCode::InvalidLayout, std::string(name),
                          "元素数乘元素宽度溢出");
    }
    const std::uint64_t bytes = elements.Value() * s.element_size_bytes;
    if (bytes > std::numeric_limits<std::size_t>::max()) {
        return QueryError(ErrorCode::InvalidLayout, std::string(name),
                          "总字节数溢出");
    }
    return static_cast<std::size_t>(bytes);
}

Result<PhysicalValue> A2lDatabaseImpl::ToPhysical(std::string_view name,
                                                  BytesView raw) const {
    Result<SymbolInfo> found = Resolve(name);
    if (!found.HasValue()) {
        return found.TakeError();
    }
    const SymbolInfo& s = found.Value();
    Result<void> gate = GateExecutable(s, name);
    if (!gate.HasValue()) {
        return gate.TakeError();
    }
    if (!s.dimensions.empty()) {
        return QueryError(ErrorCode::UnsupportedOperation, std::string(name),
                          "数组/多维符号请走 IDaqLayout 元素级解码");
    }
    return EvaluateToPhysical(s, raw);
}

Result<PhysicalValue> A2lDatabaseImpl::ToPhysicalElement(
    std::string_view name, std::uint64_t element_index, BytesView raw) const {
    Result<SymbolInfo> found = Resolve(name);
    if (!found.HasValue()) {
        return found.TakeError();
    }
    const SymbolInfo& symbol = found.Value();
    Result<void> gate = GateExecutable(symbol, name);
    if (!gate.HasValue()) {
        return gate.TakeError();
    }
    if (symbol.kind == SymbolKind::Characteristic) {
        if (symbol.characteristic_type == CharacteristicType::Value) {
            if (!symbol.dimensions.empty() || element_index != 0) {
                return QueryError(ErrorCode::UnsupportedOperation,
                                  std::string(name), "VALUE只支持标量元素0");
            }
        } else if (symbol.characteristic_type != CharacteristicType::Curve ||
                   !symbol.fixed_curve_axis.has_value()) {
            return QueryError(ErrorCode::UnsupportedOperation,
                              std::string(name),
                              "仅支持VALUE标量与已验证固定轴CURVE元素");
        }
    } else if (symbol.kind != SymbolKind::Measurement) {
        return QueryError(ErrorCode::UnsupportedOperation, std::string(name),
                          "该符号类型不支持元素级数值解码");
    }
    Result<std::uint64_t> element_count = CountElements(symbol);
    if (!element_count.HasValue()) {
        return element_count.TakeError();
    }
    if (element_index >= element_count.Value()) {
        return QueryError(ErrorCode::BadArgument, std::string(name),
                          "元素索引超出已证明维度范围");
    }
    if (symbol.element_size_bytes == 0 ||
        raw.size() != symbol.element_size_bytes) {
        return QueryError(ErrorCode::RawSizeMismatch, std::string(name),
                          "单个raw元素长度必须等于元素宽度");
    }
    SymbolInfo scalar = symbol;
    scalar.dimensions.clear();
    scalar.fixed_curve_axis.reset();
    return EvaluateToPhysical(scalar, raw);
}

Result<Bytes> A2lDatabaseImpl::FromPhysical(std::string_view name,
                                            const PhysicalValue& value) const {
    Result<SymbolInfo> found = Resolve(name);
    if (!found.HasValue()) {
        return found.TakeError();
    }
    const SymbolInfo& s = found.Value();
    Result<void> gate = GateExecutable(s, name);
    if (!gate.HasValue()) {
        return gate.TakeError();
    }
    if (!s.dimensions.empty()) {
        return QueryError(ErrorCode::UnsupportedOperation, std::string(name),
                          "数组/多维符号不支持整体写入（首里程碑范围）");
    }
    return EvaluateFromPhysical(s, value);
}

Result<Bytes> A2lDatabaseImpl::FromPhysicalElement(
    std::string_view name, std::uint64_t element_index,
    const PhysicalValue& value) const {
    Result<SymbolInfo> found = Resolve(name);
    if (!found.HasValue()) {
        return found.TakeError();
    }
    const SymbolInfo& symbol = found.Value();
    Result<void> gate = GateExecutable(symbol, name);
    if (!gate.HasValue()) {
        return gate.TakeError();
    }
    if (symbol.kind != SymbolKind::Characteristic) {
        return QueryError(ErrorCode::UnsupportedOperation, std::string(name),
                          "元素级写入仅支持CHARACTERISTIC");
    }
    if (symbol.characteristic_type == CharacteristicType::Value) {
        if (!symbol.dimensions.empty() || element_index != 0) {
            return QueryError(ErrorCode::UnsupportedOperation,
                              std::string(name), "VALUE仅支持标量元素0");
        }
    } else if (symbol.characteristic_type != CharacteristicType::Curve ||
               !symbol.fixed_curve_axis.has_value()) {
        return QueryError(ErrorCode::UnsupportedOperation, std::string(name),
                          "元素级写入仅支持VALUE标量与已验证固定轴CURVE点");
    }
    Result<std::uint64_t> element_count = CountElements(symbol);
    if (!element_count.HasValue()) {
        return element_count.TakeError();
    }
    if (element_index >= element_count.Value()) {
        return QueryError(ErrorCode::BadArgument, std::string(name),
                          "元素索引超出已证明维度范围");
    }
    SymbolInfo scalar = symbol;
    scalar.dimensions.clear();
    scalar.fixed_curve_axis.reset();
    return EvaluateFromPhysical(scalar, value);
}

// B-13/B-17 解析：module::name 精确；裸名须全库唯一
Result<SymbolInfo> A2lDatabaseImpl::Resolve(std::string_view name) const {
    if (name.empty()) {
        return QueryError(ErrorCode::BadArgument, std::string(name),
                          "查询名为空");
    }
    if (name.find("::") != std::string_view::npos) {
        auto it = m_index_.find(std::string(name));
        if (it == m_index_.end()) {
            return QueryError(ErrorCode::NotFound, std::string(name),
                              "规范键未命中");
        }
        return m_symbols_[it->second];
    }
    const std::string lowered = ToLower(name);
    auto count_it = m_bare_counts_.find(lowered);
    if (count_it == m_bare_counts_.end()) {
        return QueryError(ErrorCode::NotFound, std::string(name),
                          "符号不存在（B-13）");
    }
    if (count_it->second > 1) {
        return QueryError(
            ErrorCode::AmbiguousName, std::string(name),
            "裸名在多个 MODULE 中重复，请使用 module::name（B-17）");
    }
    // 唯一命中：线性定位（数量少且此路径罕见，保持实现简单）
    for (const SymbolInfo& s : m_symbols_) {
        if (ToLower(s.name) == lowered) {
            return s;
        }
    }
    return QueryError(ErrorCode::NotFound, std::string(name), "内部索引不一致");
}

// ----------------------------------------------------------------------------
// 门面装配工厂
// ----------------------------------------------------------------------------

/// @brief 工厂：桥接层门面装配快照数据库（内部链接可见）
std::unique_ptr<A2lDatabaseImpl> MakeDatabase(
    std::vector<liba2l::SymbolDto> dtos,
    std::vector<liba2l::StructInfoDto> structs,
    std::unordered_map<std::string, RecordLayoutInfo> record_layouts,
    std::vector<SymbolInfo> leaf_symbols,
    std::vector<LoadWarning> leaf_warnings,
    std::vector<MemorySegmentInfo> memory_segments) {
    auto db = std::make_unique<A2lDatabaseImpl>(
        std::move(dtos), std::move(structs), std::move(record_layouts),
        std::move(leaf_symbols), std::move(leaf_warnings),
        std::move(memory_segments));
    db->BuildAddressIndex();
    return db;
}

/// @brief FindByAddress 的门面转发（保持实现细节集中于本翻译单元）
std::string FindByAddress(const A2lDatabaseImpl& db, std::uint64_t address) {
    return db.FindByAddress(address);
}

/// @brief FindAllByAddress 的门面转发（批次14，B-6 aliases）
std::vector<std::string> FindAllByAddress(const A2lDatabaseImpl& db,
                                          std::uint64_t address) {
    return db.FindAllByAddress(address);
}

}  // namespace detail

}  // namespace calmcar::xcp::a2l
