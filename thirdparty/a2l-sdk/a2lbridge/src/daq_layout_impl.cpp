// =============================================================================
// daq_layout_impl.cpp —— 冻结布局 DTO 解码器（§4.4 / B-5/B-7/B-1）
//
// 首里程碑只支持 STATIC 预定义列表（B-5）；动态配置返回 UnsupportedOperation。
// envelope 头部按 DtoFrameLayout 标志推导，长度与布局不符时整帧拒绝（B-7）。
// =============================================================================

#include <memory>
#include <string>
#include <utility>
#include <vector>

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
    /// @param lists 已冻结的 STATIC DAQ_LIST 布局（symbol_name
    /// 已在门面层反查填充）
    /// @param db
    /// 快照数据库（提供符号类型/换算；生命周期由门面保证不短于本对象）
    DaqLayoutImpl(std::vector<DaqListLayout> lists, const IA2lDatabase* db)
        : m_lists_(std::move(lists)), m_db_(db) {}

    Result<std::vector<DecodedDtoSample>> Decode(
        const DtoFrameLayout& frame_layout, BytesView dto) const override {
        if (dto.empty()) {
            return LayoutError(ErrorCode::BadArgument, "DTO 为空帧");
        }
        // PID 低 7 位为事件通道号（EXT 位在首版不解析），高 8 位（EPK）用于
        // 匹配 DAQ 列表号；这里以 EPK == number 匹配（绝对标识模式）。
        const std::uint16_t epk = static_cast<std::uint8_t>(dto[0]);
        const DaqListLayout* list = nullptr;
        for (const DaqListLayout& l : m_lists_) {
            if (l.number == epk) {
                list = &l;
                break;
            }
        }
        if (list == nullptr) {
            return LayoutError(ErrorCode::NotFound,
                               "PID 未匹配任何已冻结的 STATIC DAQ_LIST");
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
        for (const OdtLayout& odt : list->odts) {
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
        for (const OdtLayout& odt : list->odts) {
            for (const OdtEntryLayout& entry : odt.entries) {
                DecodedDtoSample sample;
                sample.symbol_name = entry.symbol_name;
                sample.address = entry.address;  // B-1 原值
                sample.address_extension = entry.address_extension;
                sample.raw.assign(payload.begin() + cursor,
                                  payload.begin() + cursor + entry.size_bytes);
                cursor += entry.size_bytes;

                if (entry.symbol_name.empty()) {
                    // 地址反查不到归属符号：保留 raw，不做猜测性解释
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
                    }
                    // 换算失败时保留 monostate：raw 仍是有效证据（B-8
                    // 不静默降级， 错误详情可经 LastError 通道展示）
                }
                samples.push_back(std::move(sample));
            }
        }
        return samples;
    }

    Result<std::size_t> PackedByteSize(
        const std::vector<std::string>& names) const override {
        if (names.empty()) {
            return LayoutError(ErrorCode::BadArgument, "名单为空");
        }
        std::size_t total = 0;
        for (const std::string& name : names) {
            bool matched = false;
            for (const DaqListLayout& l : m_lists_) {
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
    std::vector<DaqListLayout> m_lists_;  ///< 冻结布局（不可变）
    const IA2lDatabase* m_db_ = nullptr;  ///< 快照数据库（非拥有）
};

}  // namespace

namespace detail {

/// @brief 工厂：装配冻结布局解码器（门面 CreateDaqLayout 调用）
std::unique_ptr<IDaqLayout> MakeDaqLayout(std::vector<DaqListLayout> lists,
                                          const IA2lDatabase* db) {
    return std::make_unique<DaqLayoutImpl>(std::move(lists), db);
}

/// @brief 用数据库地址索引回填布局条目的符号归属（歧义地址留空）
void ResolveDaqListSymbols(std::vector<DaqListLayout>* lists,
                           const A2lDatabaseImpl& db) {
    for (DaqListLayout& l : *lists) {
        for (OdtLayout& odt : l.odts) {
            for (OdtEntryLayout& entry : odt.entries) {
                entry.symbol_name = FindByAddress(db, entry.address);
            }
        }
    }
}

}  // namespace detail

}  // namespace calmcar::xcp::a2l
