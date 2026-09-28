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

}  // namespace

namespace detail {

// ----------------------------------------------------------------------------
// A2lDatabaseImpl 成员定义
// ----------------------------------------------------------------------------

A2lDatabaseImpl::A2lDatabaseImpl(
    std::vector<liba2l::SymbolDto> dtos,
    std::vector<liba2l::StructInfoDto> structs,
    std::unordered_map<std::string, RecordLayoutInfo> record_layouts) {
    m_record_layouts_ = std::move(record_layouts);
    m_symbols_.reserve(dtos.size() + structs.size());
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
        m_index_[MakeQualifiedKey(s.module_name, s.name)] = i;
        ++m_bare_counts_[ToLower(s.name)];
    }
}

void A2lDatabaseImpl::BuildAddressIndex() {
    std::unordered_map<std::uint64_t, std::string> first;
    std::vector<std::uint64_t> ambiguous;
    std::unordered_map<std::uint64_t, std::vector<std::string>> all;
    for (const SymbolInfo& s : m_symbols_) {
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
    std::unordered_map<std::string, RecordLayoutInfo> record_layouts) {
    auto db = std::make_unique<A2lDatabaseImpl>(
        std::move(dtos), std::move(structs), std::move(record_layouts));
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
