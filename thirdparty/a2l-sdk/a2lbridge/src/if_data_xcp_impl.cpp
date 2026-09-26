// =============================================================================
// if_data_xcp_impl.cpp —— IF_DATA XCP 领域装配（§4.3 / §6.3-A）
//
// SDK IfDataXcpDto/DaqListDto → 领域 IfDataXcpInfo 的组装入口；
// 具体字段映射复用 a2l_dto_map.cpp，本文件负责“提取是否成功”的判定语义。
// =============================================================================

#include <string>
#include <vector>

#include "a2l_dto_map.hpp"
#include "libxcp/a2l/if_data_xcp.hpp"

namespace calmcar::xcp::a2l {
namespace detail {

/**
 * @brief 由 SDK 快照组装 IF_DATA XCP 领域信息（声明见 a2l_dto_map.hpp）
 * @param doc 已加载完成的 SDK 文档对象
 * @param require_if_data_xcp 缺失/歧义时是否判为错误（LoadOptions 透传）
 * @return 成功：ok==true 且 transports/protocol_layer 就绪；
 *         require 为 false 且 A2L 无 IF_DATA XCP（或多 MODULE 未指定
 *         active_module，B-17）时返回 ok==false 的空结构；
 *         失败：结构化 Error（Phase::Load）。
 */
Result<IfDataXcpInfo> BuildIfDataXcp(liba2l::IDoc& doc,
                                     bool require_if_data_xcp) {
    IfDataXcpInfo info;
    liba2l::IfDataXcpDto dto;
    const liba2l::ErrorCode found = doc.GetIfDataXcp(&dto);
    if (found == liba2l::ErrorCode::kAmbiguousName) {
        // B-17：多 MODULE 同时声明 IF_DATA XCP 且未指定 active_module ——
        // 绝不静默取首个（SDK 已置 dto.ambiguous）
        if (require_if_data_xcp) {
            Error e =
                MapSdkError(liba2l::ErrorCode::kAmbiguousName, Phase::Load,
                            "多个 MODULE 声明 IF_DATA XCP，需通过 "
                            "LoadOptions::active_module 显式选择（B-17）");
            e.module = dto.module_name;
            return e;
        }
        info.ok = false;
        info.ambiguous = true;
        info.module_name = dto.module_name;
        info.last_error = "多个 MODULE 声明 IF_DATA XCP，按选项放行（B-17）";
        return info;
    }
    if (found == liba2l::ErrorCode::kNotFound) {
        if (require_if_data_xcp) {
            return MapSdkError(
                liba2l::ErrorCode::kParseFailed, Phase::Load,
                "A2L 中未找到 IF_DATA XCP 段（require_if_data_xcp）");
        }
        info.ok = false;
        info.last_error = "IF_DATA XCP 缺失，按选项放行";
        return info;
    }
    if (found != liba2l::ErrorCode::kOk) {
        return MapSdkError(found, Phase::Load, "读取 IF_DATA XCP 失败");
    }

    info.ok = true;
    info.from_xcp_plus = dto.from_xcp_plus;
    info.module_name = dto.module_name;  // B-17 module scope（批次10）
    ConvertIfDataXcp(dto, &info.protocol_layer, &info.transports);

    // XCP vs XCPplus 同参数差异（§6.3-A Info 级，批次10）
    info.plus_conflicts.reserve(dto.plus_conflicts.size());
    for (const liba2l::XcpPlusConflictDto& c : dto.plus_conflicts) {
        XcpPlusConflict conflict;
        conflict.parameter = c.parameter;
        conflict.xcp_value = c.xcp_value;
        conflict.xcpplus_value = c.xcpplus_value;
        info.plus_conflicts.push_back(std::move(conflict));
    }

    // DAQ 能力：批次10 起 SDK 导出完整 DAQ 能力块；无能力块的旧样本
    // 退回"以预定义列表存在性推导 static_supported"的保守策略。
    std::vector<liba2l::DaqListDto> lists;
    const liba2l::ErrorCode list_rc = doc.ListDaqLists(&lists);
    if (list_rc != liba2l::ErrorCode::kOk &&
        list_rc != liba2l::ErrorCode::kNotFound) {
        return MapSdkError(list_rc, Phase::Load, "读取 DAQ 列表失败");
    }
    DaqInfo daq;
    if (dto.daq_caps.has_value()) {
        daq = ConvertDaqCaps(*dto.daq_caps);
    } else {
        daq.static_supported = !lists.empty();
        daq.max_daq = static_cast<std::uint16_t>(lists.size());
    }
    for (const liba2l::DaqListDto& l : lists) {
        info.static_daq_lists.push_back(ConvertDaqList(l));
    }
    info.daq = daq;

    // 事件通道（批次10）：原始码透传 + 用 DAQ_LIST 的 EVENT_FIXED 反查
    // 该事件要 START 的 DAQ LIST 号
    info.event_channels.reserve(dto.events.size());
    for (const liba2l::EventChannelDto& e : dto.events) {
        info.event_channels.push_back(ConvertEventDto(e));
    }
    for (EventChannelInfo& ev : info.event_channels) {
        for (const DaqListLayout& list : info.static_daq_lists) {
            if (list.event_fixed.has_value() &&
                *list.event_fixed == ev.channel_number) {
                ev.daq_list_numbers.push_back(list.number);
            }
        }
    }
    return info;
}

}  // namespace detail

}  // namespace calmcar::xcp::a2l
