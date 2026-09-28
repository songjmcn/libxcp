// =============================================================================
// daq_layout_impl.cpp —— 冻结布局 DTO 解码器（§4.4 / B-5/B-7/B-1）
//
// 首里程碑只支持 STATIC 预定义列表（B-5）；动态配置返回 UnsupportedOperation。
// envelope 头部按 DtoFrameLayout 标志推导，长度与布局不符时整帧拒绝（B-7）。
// =============================================================================

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "a2l_database_impl.hpp"
#include "a2l_dto_map.hpp"
#include "libxcp/a2l/compu_method.hpp"
#include "libxcp/a2l/daq_layout.hpp"

namespace calmcar::xcp::a2l {
namespace {

Error LayoutError(ErrorCode code, std::string message) {
    Error e;
    e.code = code;
    e.phase = Phase::Layout;
    e.severity = Severity::Error;
    e.message = std::move(message);
    return e;
}

/**
 * @brief 计算 envelope 可变字段占用的字节数（不含 PID 头）
 * @param frame 冻结的帧布局
 * @return 附加头字节数；位压缩标识字段按 1 字节向上取整
 */
std::size_t EnvelopeExtraBytes(const DtoFrameLayout& frame) {
    std::size_t extra = 0;
    if (frame.counter_enabled) {
        extra += 1;  // DTO 计数器 1 字节
    }
    if (frame.timestamp_enabled) {
        extra += (frame.timestamp_size_bits + 7) / 8;
    }
    if (frame.first_odt) {
        extra += 1;  // FIRST_ODT 1 字节
    }
    switch (frame.identification_field_type) {
        case DaqInfo::IdFieldType::Absolute:
            break;  // EPK 已含在 PID 之外？——A2L 绝对标识即 DAQ
                    // 号本身，无额外字节
        case DaqInfo::IdFieldType::RelativeByte:
            extra += 1;
            break;
        case DaqInfo::IdFieldType::RelativeWord:
        case DaqInfo::IdFieldType::RelativeWordAligned:
            extra += 2;
            break;
    }
    return extra;
}

/// @brief 冻结布局解码器实现
class DaqLayoutImpl final : public IDaqLayout {
public:
    /// @brief 构造解码器
    /// @param snapshot 不可变布局快照（B-6：来源与代际随快照一起冻结）
    /// @param db
    /// 快照数据库（提供符号类型/换算；生命周期由门面保证不短于本对象）
    DaqLayoutImpl(DaqLayoutSnapshot snapshot, const IA2lDatabase* db)
        : m_snapshot_(std::move(snapshot)), m_db_(db) {}

    Result<std::vector<DecodedDtoSample>> Decode(
        const DtoFrameLayout& frame_layout, BytesView dto) const override {
        if (dto.empty()) {
            return LayoutError(ErrorCode::BadArgument, "DTO 为空帧");
        }
        // 批次14（R12）：入参是**完整 DTO 帧**（`dto[0]` 即 PID），与
        // libxcp 侧 `DtoPacket::data` 的定稿语义一致。
        // B-7 批次11：PID_OFF 关闭识别字段后无法路由帧（XCP 要求唯一标识由
        // Transport 层保证——UDP 通道无此关联），显式拒绝，不做猜测性分发。
        if (frame_layout.pid_off) {
            return LayoutError(ErrorCode::UnsupportedOperation,
                               "PID_OFF 帧首里程碑不支持（B-7：无 Transport "
                               "层 DAQ 列表关联通道）");
        }
        const std::uint8_t pid = dto[0];

        // ---- B-6 权威选择（批次15 F1/D1 后分三层）：
        //   ① PID 命中路由表 → 定位到**单个 ODT**（本端账本、ECU 回读、以及
        //      A2L 声明的 FIRST_PID 都进这张表，优先级最高）；
        //   ② 来源要求实际账本/回读却没命中 → 拒绝（不拿 A2L 顺序凑数）；
        //   ③ 纯 A2L 来源且该 PID 未被任何路由声明 → 只对**未声明 FIRST_PID**
        //      的列表回退到"PID == 列表号 + 整列表净荷"的旧口径。
        //      已声明 FIRST_PID 的列表不参与回退：列表号与其它列表的 PID 属于
        //      同一编号空间，若在有声明时仍回退，就会出现"列表号恰好撞上别人的
        //      PID → 用错列表的 ODT 布局切字节"，长度凑得上时全程无报错
        //      （批次15 记录 §1 L1）。
        const DaqListLayout* list = nullptr;
        const OdtLayout* odt_only = nullptr;
        const DaqOdtRoute* route = nullptr;
        for (const auto& r : m_snapshot_.routes) {
            if (r.pid == pid) {
                route = &r;
                break;
            }
        }
        if (route != nullptr) {
            for (const DaqListLayout& l : m_snapshot_.lists) {
                if (l.number != route->daq_list) {
                    continue;
                }
                for (const OdtLayout& o : l.odts) {
                    // OdtLayout::number 是 1 基（A2L 源序），路由表 0 基
                    if (o.number == route->odt_number + 1U) {
                        list = &l;
                        odt_only = &o;
                        break;
                    }
                }
                break;
            }
            if (list == nullptr || odt_only == nullptr) {
                return LayoutError(ErrorCode::InvalidLayout,
                                   "路由表指向的 (DAQ, ODT) "
                                   "在快照中无对应条目（账本与快照不一致）");
            }
        } else if (m_snapshot_.source != DaqLayoutSource::A2lPredefined) {
            return LayoutError(
                m_snapshot_.routes.empty() ? ErrorCode::InvalidLayout
                                           : ErrorCode::NotFound,
                m_snapshot_.routes.empty()
                    ? std::string(
                          "布局快照来源要求实际账本/回读，但路由表为空（B-6："
                          "无实际账本 → 只交 raw，不解释）")
                    : ("PID 0x" + std::to_string(pid) +
                       " 不在本端账本/回读的路由表内（B-6：不猜布局与顺序）"));
        } else {
            for (const DaqListLayout& l : m_snapshot_.lists) {
                if (l.first_pid.has_value()) {
                    continue;  // 声明过 FIRST_PID 的列表只认路由，不接受回退
                }
                if (l.number == static_cast<std::uint16_t>(pid)) {
                    list = &l;
                    break;
                }
            }
            if (list == nullptr) {
                return LayoutError(
                    ErrorCode::NotFound,
                    "PID 未匹配任何已冻结的 STATIC DAQ_LIST（A2L 未声明 "
                    "FIRST_PID，按列表号回退后仍未命中）");
            }
        }

        const std::size_t header =
            frame_layout.header_bytes + EnvelopeExtraBytes(frame_layout);
        if (dto.size() < header) {
            return LayoutError(ErrorCode::InvalidLayout,
                               "DTO 长度小于 envelope 头，整帧拒绝（B-7）");
        }
        BytesView payload = dto.subspan(header);

        // 逐 ODT entry 顺序消费净荷；总长必须恰好吻合，否则整帧拒绝
        std::size_t expected = 0;
        for (const OdtLayout& odt :
             (odt_only != nullptr)
                 ? std::span<const OdtLayout>{odt_only, 1}
                 : std::span<const OdtLayout>{list->odts.data(),
                                              list->odts.size()}) {
            for (const OdtEntryLayout& entry : odt.entries) {
                expected += entry.size_bytes;
            }
        }
        if (payload.size() != expected) {
            return LayoutError(ErrorCode::InvalidLayout,
                               "净荷长度与冻结布局不符：期望 " +
                                   std::to_string(expected) + "，实际 " +
                                   std::to_string(payload.size()));
        }

        std::vector<DecodedDtoSample> samples;
        std::size_t cursor = 0;
        for (const OdtLayout& odt :
             (odt_only != nullptr)
                 ? std::span<const OdtLayout>{odt_only, 1}
                 : std::span<const OdtLayout>{list->odts.data(),
                                              list->odts.size()}) {
            for (const OdtEntryLayout& entry : odt.entries) {
                DecodedDtoSample sample;
                sample.symbol_name = entry.symbol_name;
                // B-6：歧义地址的候选名单随样本一起交付（不择一丢弃）
                sample.symbol_aliases = entry.symbol_aliases;
                sample.address = entry.address;  // B-1 原值
                sample.address_extension = entry.address_extension;
                sample.raw.assign(payload.begin() + cursor,
                                  payload.begin() + cursor + entry.size_bytes);
                cursor += entry.size_bytes;

                if (entry.symbol_name.empty()) {
                    // 地址反查不到唯一归属符号：保留 raw（歧义时 aliases 里
                    // 有全部候选），不做猜测性解释（B-6）。physical_valid
                    // 保持 false —— 消费方不得把默认值 0 当成换算结果（F2）
                    samples.push_back(std::move(sample));
                    continue;
                }
                Result<SymbolInfo> found = m_db_->Find(entry.symbol_name);
                if (!found.HasValue()) {
                    return found.TakeError();
                }
                const SymbolInfo& s = found.Value();
                // B-9：entry 级 BIT_OFFSET 与符号级 bit_mask 分离建模。位偏移
                // 非零的 entry 无法仅凭本 entry 净荷定位（需整字上下文），
                // 首版保留 raw、不做猜测性位移。
                if (entry.bit_offset != 0) {
                    // 位元素：需整字上下文，首版不猜位移 → physical_valid 保持
                    // false，raw 原样交付（B-9 + F2）
                    samples.push_back(std::move(sample));
                    continue;
                }
                // 单 entry 的 size 可能小于元素宽（位段打包）：等宽才做换算
                if (s.element_size_bytes != 0 &&
                    entry.size_bytes == s.element_size_bytes &&
                    s.dimensions.empty()) {
                    Result<PhysicalValue> phys =
                        EvaluateToPhysical(s, sample.raw);
                    if (phys.HasValue()) {
                        sample.physical_value = std::move(phys.Value());
                        // F2：只有真正换算成功才置位。PhysicalValue 是 variant
                        // 且首选项是 int64_t，默认值就是 0 —— 没有这个标志，
                        // "换算没做"与"算出 0"在消费侧完全不可区分（批次14 的
                        // E2E 用例就因此静默显示过一排 0）
                        sample.physical_valid = true;
                    }
                    // 换算失败：physical_valid 保持 false，raw 仍是有效证据
                    // （B-8 不静默降级，错误详情经 Error.message 展示）
                }
                samples.push_back(std::move(sample));
            }
        }
        return samples;
    }

    /// @brief 构建时冻结的布局代际（批次15，F4：调用方识别陈旧解码器用）
    [[nodiscard]] std::uint32_t Generation() const noexcept override {
        return m_snapshot_.generation;
    }

    Result<std::size_t> PackedByteSize(
        const std::vector<std::string>& names) const override {
        if (names.empty()) {
            return LayoutError(ErrorCode::BadArgument, "名单为空");
        }
        std::size_t total = 0;
        for (const std::string& name : names) {
            bool matched = false;
            for (const DaqListLayout& l : m_snapshot_.lists) {
                for (const OdtLayout& odt : l.odts) {
                    for (const OdtEntryLayout& entry : odt.entries) {
                        if (entry.symbol_name == name) {
                            total += entry.size_bytes;
                            matched = true;
                        }
                    }
                }
            }
            if (!matched) {
                return LayoutError(ErrorCode::NotFound,
                                   "符号不在任何已冻结列表中：" + name);
            }
        }
        return total;
    }

private:
    DaqLayoutSnapshot m_snapshot_;  ///< 冻结布局快照（不可变，含来源与代际）
    const IA2lDatabase* m_db_ = nullptr;  ///< 快照数据库（非拥有）
};

}  // namespace

namespace detail {

/// @brief 工厂：装配冻结布局解码器（门面 CreateDaqLayout 调用）
std::unique_ptr<IDaqLayout> MakeDaqLayout(DaqLayoutSnapshot snapshot,
                                          const IA2lDatabase* db) {
    return std::make_unique<DaqLayoutImpl>(std::move(snapshot), db);
}
/// @brief 用数据库地址索引回填布局条目的符号归属（歧义留空 + 保留 aliases）
void ResolveDaqListSymbols(std::vector<DaqListLayout>* lists,
                           const A2lDatabaseImpl& db) {
    for (DaqListLayout& l : *lists) {
        for (OdtLayout& odt : l.odts) {
            for (OdtEntryLayout& entry : odt.entries) {
                entry.symbol_name = FindByAddress(db, entry.address);
                // B-6：同址多候选时 symbol_name 保持空（不猜归属），但全部
                // 候选名进 symbol_aliases 留痕，供上层提示与人工确认
                entry.symbol_aliases = FindAllByAddress(db, entry.address);
            }
        }
    }
}

/// @brief 由本端 WRITE_DAQ 账本构建不可变布局快照（B-6，批次14）
DaqLayoutSnapshot BuildSnapshotFromLedger(
    const std::vector<DaqLedgerEntryView>& ledger, std::uint32_t generation,
    const A2lDatabaseImpl& db) {
    DaqLayoutSnapshot snapshot;
    snapshot.source = DaqLayoutSource::LocalLedger;
    snapshot.generation = generation;
    if (ledger.empty()) {
        return snapshot;  // 空快照：解码一律 InvalidLayout（不拿 A2L 顺序凑数）
    }

    // 稳定归一化：按 (daq_list, odt_number, odt_entry) 升序，OdtLayout::number
    // 与 OdtEntryLayout::number 一律 1 基（与 A2L 侧口径一致）
    std::vector<DaqLedgerEntryView> sorted = ledger;
    std::sort(sorted.begin(), sorted.end(),
              [](const DaqLedgerEntryView& a, const DaqLedgerEntryView& b) {
                  if (a.daq_list != b.daq_list) return a.daq_list < b.daq_list;
                  if (a.odt_number != b.odt_number)
                      return a.odt_number < b.odt_number;
                  return a.odt_entry < b.odt_entry;
              });

    for (const auto& e : sorted) {
        DaqListLayout* list = nullptr;
        for (auto& l : snapshot.lists) {
            if (l.number == e.daq_list) {
                list = &l;
                break;
            }
        }
        if (list == nullptr) {
            DaqListLayout fresh;
            fresh.number = e.daq_list;
            snapshot.lists.push_back(std::move(fresh));
            list = &snapshot.lists.back();
        }
        OdtLayout* odt = nullptr;
        for (auto& o : list->odts) {
            if (o.number == static_cast<std::uint8_t>(e.odt_number + 1U)) {
                odt = &o;
                break;
            }
        }
        if (odt == nullptr) {
            OdtLayout fresh;
            fresh.number = static_cast<std::uint8_t>(e.odt_number + 1U);
            list->odts.push_back(std::move(fresh));
            odt = &list->odts.back();
        }
        OdtEntryLayout entry;
        entry.number = static_cast<std::uint8_t>(e.odt_entry + 1U);
        entry.address = e.address;
        entry.address_extension = e.address_extension;
        entry.size_bytes = e.size_bytes;
        // 批次15（F3）：位偏口径归一**收回桥接层**。XCP 的 WRITE_DAQ/READ_DAQ
        // 用 0xFF 表示"无位偏"（docs/XCP_1.3.0_document.md L1861），而
        // OdtEntryLayout::bit_offset 沿用 A2L 侧口径"0 = 无位偏"（B-9）。
        // 此前要求调用方自己转换，忘转的后果是整帧物理值静默不换算
        // （批次14 的 E2E 真实踩过），故改为入口侧吸收。
        entry.bit_offset =
            (e.bit_offset == 0xFFU) ? 0U : e.bit_offset;
        // 归属符号仍按地址反查（唯一才填，歧义只留 aliases）；查不到即留空
        entry.symbol_name = FindByAddress(db, e.address);
        entry.symbol_aliases = FindAllByAddress(db, e.address);
        odt->entries.push_back(std::move(entry));
        if (e.pid.has_value()) {
            // PID 路由去重与"歧义即撤路由"：同一 PID 被两个不同的
            // (daq, odt) 声称时，保留任何一条都是猜 —— 直接把该 PID 从路由表
            // 撤掉，解码会因"无路由"返回 InvalidLayout（B-6 不猜）。
            bool conflict = false;
            bool duplicate = false;
            for (const DaqOdtRoute& r : snapshot.routes) {
                if (r.pid == *e.pid) {
                    if (r.daq_list == e.daq_list &&
                        r.odt_number == e.odt_number) {
                        duplicate = true;  // 同 ODT 的多条 Entry 共享 PID
                    } else {
                        conflict = true;
                    }
                    break;
                }
            }
            if (conflict) {
                std::erase_if(snapshot.routes, [&e](const DaqOdtRoute& r) {
                    return r.pid == *e.pid;
                });
            } else if (!duplicate) {
                snapshot.routes.push_back(
                    DaqOdtRoute{*e.pid, e.daq_list, e.odt_number});
            }
        }
    }
    return snapshot;
}

DaqLayoutSnapshot BuildSnapshotFromEcuReadback(
    const std::vector<DaqReadbackEntry>& readback,
    const std::vector<DaqListPid>& list_pids, std::uint32_t generation,
    const A2lDatabaseImpl& db) {
    // 回读视图 → 账本视图（PID 由该列表的 FIRST_PID + 相对 ODT 号推得，
    // docs L2225；无 FIRST_PID 的列表不留 PID，解码自然走"无路由"拒绝）
    std::vector<DaqLedgerEntryView> as_ledger;
    as_ledger.reserve(readback.size());
    for (const auto& e : readback) {
        DaqLedgerEntryView v;
        v.daq_list = e.daq_list;
        v.odt_number = e.odt_number;
        v.odt_entry = e.odt_entry;
        v.address = e.address;
        v.address_extension = e.address_extension;
        v.size_bytes = e.size_bytes;
        v.bit_offset = e.bit_offset;
        for (const auto& p : list_pids) {
            if (p.daq_list == e.daq_list) {
                v.pid = static_cast<std::uint8_t>(p.first_pid + e.odt_number);
                break;
            }
        }
        as_ledger.push_back(v);
    }
    DaqLayoutSnapshot snapshot =
        BuildSnapshotFromLedger(as_ledger, generation, db);
    // 来源标记必须是 EcuReadback（布局由 ECU 固化，本端没有下发证据）
    snapshot.source = DaqLayoutSource::EcuReadback;
    return snapshot;
}

}  // namespace detail

}  // namespace calmcar::xcp::a2l
