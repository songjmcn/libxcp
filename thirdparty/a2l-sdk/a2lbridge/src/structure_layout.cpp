// =============================================================================
// structure_layout.cpp —— STRUCTURE/INSTANCE 叶子展开解析器（批次18，16-B）
//
// 实现要点与纪律见 structure_layout.hpp 头注释。所有"拒绝"路径都产出
// LoadWarning（Phase::Load，Severity::Warning —— 单节点不可证不阻断实例），
// 错误消息带完整定位（实例 / 路径 / 类型链），禁止静默丢弃（B-19 message
// 只展示、参与判定的是 code/subject）。
// =============================================================================

#include "structure_layout.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "a2l_dto_map.hpp"

namespace calmcar::xcp::a2l::detail {
namespace {

/// @brief 递归深度上界（批次16 §5：栈溢出防护）
constexpr int kMaxDepth = 32;

/// @brief 单实例叶子预算（病态维度爆炸防护）
constexpr std::size_t kInstanceLeafBudget = 4096;

/// @brief 结构体数组索引展开的元素数上界
constexpr std::uint64_t kMaxStructArrayExpand = 1024;

/// @brief 注册表键分隔符（0x1F：不可能出现在 A2L IDENT 中）
constexpr char kKeySep = '\x1F';

/// @brief (MODULE, 类型名) 组合键
[[nodiscard]] std::string TypeKey(const std::string& module,
                                  const std::string& name) {
    std::string key;
    key.reserve(module.size() + 1 + name.size());
    key.append(module);
    key += kKeySep;
    key.append(name);
    return key;
}

/// @brief 无符号 16 进制文本（诊断消息用，不带 0x 前缀）
[[nodiscard]] std::string HexU64(std::uint64_t v) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    if (v == 0) {
        return "0";
    }
    std::string out;
    while (v != 0) {
        out.insert(out.begin(), kDigits[v & 0xFU]);
        v >>= 4U;
    }
    return out;
}

/// @brief 类型链文本（环/深度告警定位用）
[[nodiscard]] std::string JoinChain(const std::vector<std::string>& chain) {
    std::string out;
    for (const std::string& k : chain) {
        if (!out.empty()) {
            out += " -> ";
        }
        const auto sep = k.find(kKeySep);
        out += (sep == std::string::npos) ? k : k.substr(sep + 1);
    }
    return out;
}

/// @brief checked 乘法：a*b 溢出（uint64）返回 false
[[nodiscard]] bool MulU64(std::uint64_t a, std::uint64_t b,
                          std::uint64_t* out) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return false;
    }
    *out = a * b;
    return true;
}

/**
 * @brief MATRIX_DIM 乘积（任一维为 0 或溢出 → false）
 * @param dims 维度向量（非空）
 * @param out 乘积
 */
[[nodiscard]] bool ProductDims(const std::vector<std::uint64_t>& dims,
                               std::uint64_t* out) {
    std::uint64_t acc = 1;
    for (const std::uint64_t d : dims) {
        if (d == 0 || !MulU64(acc, d, &acc)) {
            return false;
        }
    }
    *out = acc;
    return true;
}

/// @brief 解析上下文：三张注册表 + 输出累加器
struct LayoutContext {
    /// @brief TYPEDEF_STRUCTURE 注册表（module\x1Fname → DTO）
    std::unordered_map<std::string, const liba2l::StructInfoDto*> structs;
    /// @brief TYPEDEF_MEASUREMENT 注册表（module\x1Fname → DTO）
    std::unordered_map<std::string, const liba2l::TypedefMeasurementDto*>
        scalars;
    std::vector<SymbolInfo> leaves;     ///< 输出叶子
    std::vector<LoadWarning> warnings;  ///< 输出告警
};

/// @brief 记录一条不可证拒绝（Phase::Load / Warning，见文件头纪律）
void Reject(LayoutContext& ctx, const std::string& module,
            const std::string& subject, std::string message) {
    LoadWarning w;
    w.code = ErrorCode::InvalidLayout;
    w.phase = Phase::Load;
    w.subject = subject;
    // subject 前缀 MODULE 限定（B-17：跨模块不歧义）
    if (!module.empty() && subject.rfind(module + "::", 0) != 0) {
        w.subject = module + "::" + subject;
    }
    w.message = std::move(message);
    ctx.warnings.push_back(std::move(w));
}

/**
 * @brief 标量成员 → 叶子 SymbolInfo
 * @param tm 标量类型事实
 * @param inst_module 实例 MODULE
 * @param inst_name 实例限定路径前缀（含已展开的 .member/[idx] 链）
 * @param inst 实例寻址事实（基址/扩展/读写）
 * @param member_path 完整路径（instance.member.…）
 * @param byte_offset 相对实例基址的字节偏移
 * @param count 元素数（MATRIX_DIM 乘积；标量=1）
 * @return 组装好的叶子（宽度/换算/边界取自 tm；寻址取自实例+偏移）
 */
[[nodiscard]] SymbolInfo MakeScalarLeaf(
    const liba2l::TypedefMeasurementDto& tm, const std::string& inst_name,
    std::uint64_t base_address, std::uint8_t address_extension,
    bool instance_read_write, const std::string& member_path,
    std::uint64_t byte_offset, std::uint64_t count) {
    SymbolInfo info;
    info.module_name = tm.module_name;
    info.name = member_path;  // 全限定路径（MODULE::instance.member.…）
    info.description = tm.description.empty()
                           ? ("STRUCTLEAF of " + inst_name)
                           : (tm.description + " [leaf of " + inst_name + "]");
    // 叶子按测量对待：可执行门（GateExecutable）对 Measurement 放行，
    // 数值能力仍由 element_size / dimensions 门逐项把关
    info.kind = SymbolKind::Measurement;
    info.characteristic_type = CharacteristicType::None;
    info.data_type = ConvertDataType(tm.data_type);
    // B-1：ECU 地址 = 实例基址 + 成员字节偏移 + 元素索引步长已并入偏移；
    // AG 绝不乘入基址（批次16 §2 地址公式）。AG≠BYTE 的 ECU 上
    // "成员偏移按字节直加"是未经语料核证的外推，批次16 §5 风险表已记录。
    info.xcp_address = base_address + byte_offset;
    // 扩展从 INSTANCE 原值透传；成员级不存在独立扩展（ASAP2 语义核证）
    info.address_extension = address_extension;
    info.element_size_bytes = tm.element_size_bytes;
    if (count > 1) {
        // 多维成员数组 flatten 为单维（extent=乘积、stride=元素宽）：
        // 连续数组线性化后第 k 元素 byte_offset = k×width，ROW/COLUMN_DIR
        // 等价，与 ComputeElementAddress 线性索引口径一致
        Dimension d;
        d.extent = count;
        d.byte_stride = tm.element_size_bytes;
        info.dimensions.push_back(d);
    }
    info.array_order = ArrayOrder::RowMajor;
    // 字节序：typedef 块内声明优先；kUnknown 的回退与 ConvertSymbol 同一
    // 宽松口径（非 MsbFirst 即 MsbLast，实测 XCPlite 的 PC 目标恒小端）
    info.byte_order = ConvertByteOrder(tm.byte_order);
    info.bit_mask = tm.bit_mask;
    info.read_write = instance_read_write;  // 成员可写性 = 实例 READ_WRITE
    info.conversion = ConvertConversion(tm.conversion);
    // B-10：PHYS_UNIT 原文优先于 COMPU 的 REF_UNIT（与 ConvertSymbol 同规则）
    if (!tm.phys_unit.empty()) {
        info.conversion.unit = tm.phys_unit;
    }
    info.have_limit = tm.have_limit;
    info.lower_limit = tm.lower_limit;
    info.upper_limit = tm.upper_limit;
    return info;
}

/**
 * @brief 递归展开一个 TYPEDEF_STRUCTURE 的成员
 * @param ctx 解析上下文
 * @param st 待展开的结构类型
 * @param inst_name 实例名（诊断用）
 * @param base_address 实例基址
 * @param address_extension 实例地址扩展
 * @param instance_read_write 实例 READ_WRITE
 * @param prefix 当前路径前缀（进入时 = 已定位到 st 实例对象的路径）
 * @param struct_base 该结构对象相对实例基址的字节偏移
 * @param depth 当前递归深度
 * @param chain 类型访问链（环检测，含当前 st 的 TypeKey）
 * @param leaf_budget_left 本实例剩余叶子预算（按引用递减）
 */
void ExpandStruct(LayoutContext& ctx, const liba2l::StructInfoDto& st,
                  const std::string& inst_name, std::uint64_t base_address,
                  std::uint8_t address_extension, bool instance_read_write,
                  const std::string& prefix, std::uint64_t struct_base,
                  int depth, std::vector<std::string>& chain,
                  std::size_t& leaf_budget_left) {
    if (depth > kMaxDepth) {
        Reject(ctx, st.module_name, prefix,
               "结构递归深度超过上界 " + std::to_string(kMaxDepth) +
                   "，类型链: " + JoinChain(chain));
        return;
    }
    // 父类型 SIZE 不可证（0）→ 无法做越界证明，整层拒绝
    if (st.size_bytes == 0) {
        Reject(ctx, st.module_name, prefix,
               "TYPEDEF_STRUCTURE " + st.name +
                   " 的 SIZE 为 0/不可证，无法完成越界检查");
        return;
    }

    // 成员按 offset 升序展开（BuildStructureInfo 按名排序，这里换到布局序，
    // 使预算截断落在"布局尾部"而非名字尾部，可预期）
    std::vector<const liba2l::StructMemberDto*> members;
    members.reserve(st.members.size());
    for (const liba2l::StructMemberDto& m : st.members) {
        members.push_back(&m);
    }
    std::sort(
        members.begin(), members.end(),
        [](const liba2l::StructMemberDto* a, const liba2l::StructMemberDto* b) {
            return a->address_offset < b->address_offset;
        });

    for (const liba2l::StructMemberDto* m : members) {
        const std::string path = prefix + "." + m->name;
        if (m->typedef_name.empty()) {
            Reject(ctx, st.module_name, path,
                   "成员无类型引用（typedef_name 为空），不可证（B-3）");
            continue;
        }
        // MATRIX_DIM：标量成员 = 元素数；结构成员 = 结构对象个数
        std::uint64_t count = 1;
        if (!m->matrix_dim.empty() && !ProductDims(m->matrix_dim, &count)) {
            Reject(ctx, st.module_name, path,
                   "MATRIX_DIM 含 0 或乘积溢出，维度不可证");
            continue;
        }
        const std::string ref_key = TypeKey(st.module_name, m->typedef_name);

        // ① 标量成员：引用 TYPEDEF_MEASUREMENT
        auto scalar_it = ctx.scalars.find(ref_key);
        if (scalar_it != ctx.scalars.end()) {
            const liba2l::TypedefMeasurementDto& tm = *scalar_it->second;
            if (tm.element_size_bytes == 0) {
                Reject(ctx, st.module_name, path,
                       "成员类型 " + m->typedef_name +
                           " 宽度未知（B-3 禁止猜宽度）");
                continue;
            }
            std::uint64_t total_bytes = 0;
            if (!MulU64(count, tm.element_size_bytes, &total_bytes)) {
                Reject(ctx, st.module_name, path, "成员数组总字节数溢出");
                continue;
            }
            if (m->address_offset > st.size_bytes ||
                total_bytes > st.size_bytes - m->address_offset) {
                Reject(ctx, st.module_name, path,
                       "成员越界：offset(0x" + HexU64(m->address_offset) +
                           ") + size(0x" + HexU64(total_bytes) + ") > SIZE(0x" +
                           HexU64(st.size_bytes) + ")");
                continue;
            }
            if (leaf_budget_left == 0) {
                Reject(ctx, st.module_name, path,
                       "实例叶子预算耗尽（上限 " +
                           std::to_string(kInstanceLeafBudget) + "），截断");
                return;
            }
            --leaf_budget_left;
            ctx.leaves.push_back(
                MakeScalarLeaf(tm, inst_name, base_address, address_extension,
                               instance_read_write, path,
                               struct_base + m->address_offset, count));
            continue;
        }

        // ② 结构成员：引用 TYPEDEF_STRUCTURE（递归）
        auto struct_it = ctx.structs.find(ref_key);
        if (struct_it != ctx.structs.end()) {
            const liba2l::StructInfoDto& child = *struct_it->second;
            std::uint64_t child_size = child.size_bytes;
            std::uint64_t total_bytes = 0;
            if (child_size == 0 || !MulU64(child_size, count, &total_bytes)) {
                Reject(ctx, st.module_name, path,
                       "嵌套类型 " + m->typedef_name +
                           " SIZE 不可证或数组总字节溢出");
                continue;
            }
            if (m->address_offset > st.size_bytes ||
                total_bytes > st.size_bytes - m->address_offset) {
                Reject(ctx, st.module_name, path,
                       "嵌套成员越界：offset + size > SIZE");
                continue;
            }
            if (count > kMaxStructArrayExpand) {
                Reject(ctx, st.module_name, path,
                       "结构体数组元素数 " + std::to_string(count) +
                           " 超过索引展开上界 " +
                           std::to_string(kMaxStructArrayExpand) +
                           "（叶子只保留标量/标量数组，结构数组不整体收录）");
                continue;
            }
            // 环检测：child 已在访问链上 → 该分支拒绝（16-B06）
            if (std::find(chain.begin(), chain.end(), ref_key) != chain.end()) {
                Reject(ctx, st.module_name, path,
                       "类型递归环，类型链: " + JoinChain(chain) + " -> " +
                           m->typedef_name);
                continue;
            }
            chain.push_back(ref_key);
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string elem_prefix =
                    count == 1 ? path : (path + "[" + std::to_string(i) + "]");
                ExpandStruct(ctx, child, inst_name, base_address,
                             address_extension, instance_read_write,
                             elem_prefix,
                             struct_base + m->address_offset + i * child_size,
                             depth + 1, chain, leaf_budget_left);
            }
            chain.pop_back();
            continue;
        }

        // ③ 双注册表都查不到：类型引用缺失，不可证
        Reject(ctx, st.module_name, path,
               "成员引用的类型 " + m->typedef_name +
                   " 在 TYPEDEF_MEASUREMENT / TYPEDEF_STRUCTURE 中均不存在");
    }
}

}  // namespace

StructureLayoutResult BuildStructureLeaves(
    const std::vector<liba2l::StructInfoDto>& typedef_structs,
    const std::vector<liba2l::StructInfoDto>& instances,
    const std::vector<liba2l::TypedefMeasurementDto>& typedefs) {
    StructureLayoutResult result;
    LayoutContext ctx;
    ctx.structs.reserve(typedef_structs.size());
    for (const liba2l::StructInfoDto& st : typedef_structs) {
        // 同名 TYPEDEF 以先到为准（SDK 快照已按 MODULE,名排序，稳定）
        ctx.structs.emplace(TypeKey(st.module_name, st.name), &st);
    }
    ctx.scalars.reserve(typedefs.size());
    for (const liba2l::TypedefMeasurementDto& tm : typedefs) {
        ctx.scalars.emplace(TypeKey(tm.module_name, tm.name), &tm);
    }

    for (const liba2l::StructInfoDto& inst : instances) {
        if (!inst.is_instance) {
            continue;
        }
        const std::string ref_key = TypeKey(inst.module_name, inst.ref_typedef);
        auto it = ctx.structs.find(ref_key);
        if (inst.ref_typedef.empty() || it == ctx.structs.end()) {
            Reject(ctx, inst.module_name, inst.name,
                   "INSTANCE 引用的 TYPEDEF " +
                       (inst.ref_typedef.empty() ? std::string("<空>")
                                                 : inst.ref_typedef) +
                       " 不存在，实例无叶子");
            continue;
        }
        const liba2l::StructInfoDto& st = *it->second;
        // 实例级 MATRIX_DIM（批次18，16-B）：结构体数组实例逐元素展开为
        // name[0..count-1]，元素步长 = TYPEDEF SIZE（与成员结构数组同一
        // 线性索引口径，INDEX_MAP 不解析）。count=1/无维度 → 单实例直展开。
        std::uint64_t elem_count = 1;
        if (!inst.matrix_dim.empty() &&
            !ProductDims(inst.matrix_dim, &elem_count)) {
            Reject(ctx, inst.module_name, inst.name,
                   "INSTANCE MATRIX_DIM 含 0 或乘积溢出，数组不可证");
            continue;
        }
        if (elem_count > kMaxStructArrayExpand) {
            Reject(ctx, inst.module_name, inst.name,
                   "INSTANCE 元素数 " + std::to_string(elem_count) +
                       " 超过索引展开上界 " +
                       std::to_string(kMaxStructArrayExpand));
            continue;
        }
        std::size_t budget = kInstanceLeafBudget;
        for (std::uint64_t i = 0; i < elem_count; ++i) {
            const std::string prefix =
                elem_count == 1 ? inst.name
                                : (inst.name + "[" + std::to_string(i) + "]");
            std::vector<std::string> chain = {ref_key};
            ExpandStruct(ctx, st, inst.name, inst.address,
                         inst.address_extension, inst.read_write, prefix,
                         i * st.size_bytes, 1, chain, budget);
        }
    }
    result.leaves = std::move(ctx.leaves);
    result.warnings = std::move(ctx.warnings);
    return result;
}

}  // namespace calmcar::xcp::a2l::detail
