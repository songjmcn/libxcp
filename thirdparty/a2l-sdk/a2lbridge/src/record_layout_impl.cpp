// =============================================================================
// record_layout_impl.cpp —— RECORD_LAYOUT 可执行性判定（§6.3 / B-12）
//
// 首里程碑：RECORD_LAYOUT 属性细节未从 SDK 快照导出（已知缺口），
// 除明确的 VALUE/VAL_BLK 等价布局外一律判 Complex → 禁止数值读写，
// 禁止把结构当字节数组扁平化处理。
// =============================================================================

#include "libxcp/a2l/record_layout.hpp"

namespace calmcar::xcp::a2l {

Result<void> CheckRecordLayoutExecutable(
    const RecordLayoutInfo& layout) noexcept {
    switch (layout.kind) {
        case RecordLayoutKind::Value:
        case RecordLayoutKind::ValBlk:
        case RecordLayoutKind::FncValues:
            // 这三类在元素宽已知的单变量布局上可按 B-1/B-3 直接寻址
            return Result<void>{};
        case RecordLayoutKind::AxisPts: {
            Error e = detail::MakeError(
                ErrorCode::UnsupportedOperation, Phase::Layout,
                "AXIS_PTS 需连同轴换算一起处理，超出首里程碑范围");
            e.symbol = layout.name;
            return e;
        }
        case RecordLayoutKind::Complex: {
            Error e = detail::MakeError(
                ErrorCode::InvalidLayout, Phase::Layout,
                "复合布局含不可定位属性，拒绝按字节数组猜测（B-12）");
            e.symbol = layout.name;
            return e;
        }
        case RecordLayoutKind::Unknown:
        default: {
            Error e =
                detail::MakeError(ErrorCode::InvalidLayout, Phase::Layout,
                                  "RECORD_LAYOUT 未解析，禁止数值读写（B-12）");
            e.symbol = layout.name;
            return e;
        }
    }
}

}  // namespace calmcar::xcp::a2l
