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
 * @param require_if_data_xcp 缺失时是否判为错误（LoadOptions 透传）
 * @return 成功：ok==true 且 transports/protocol_layer 就绪；
 *         require 为 false 且 A2L 无 IF_DATA XCP 时返回 ok==false 的空结构；
 *         失败：结构化 Error（Phase::Load）。
 */
Result<IfDataXcpInfo> BuildIfDataXcp(liba2l::IDoc& doc,
                                     bool require_if_data_xcp) {
    IfDataXcpInfo info;
    liba2l::IfDataXcpDto dto;
    const liba2l::ErrorCode found = doc.GetIfDataXcp(&dto);
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
    ConvertIfDataXcp(dto, &info.protocol_layer, &info.transports);

    // DAQ 能力：当前 SDK 快照未导出 COMMON_PARAMETERS 明细（§6.3 已知缺口），
    // 以预定义列表的存在性推导 static_supported；其余能力位保持默认值。
    std::vector<liba2l::DaqListDto> lists;
    const liba2l::ErrorCode list_rc = doc.ListDaqLists(&lists);
    if (list_rc != liba2l::ErrorCode::kOk &&
        list_rc != liba2l::ErrorCode::kNotFound) {
        return MapSdkError(list_rc, Phase::Load, "读取 DAQ 列表失败");
    }
    DaqInfo daq;
    daq.static_supported = !lists.empty();
    for (const liba2l::DaqListDto& l : lists) {
        info.static_daq_lists.push_back(ConvertDaqList(l));
    }
    if (!lists.empty()) {
        // MAX_DAQ 用列表数下界（保守值；精确能力位待 SDK 后续导出）
        daq.max_daq = static_cast<std::uint16_t>(lists.size());
    }
    info.daq = daq;
    return info;
}

}  // namespace detail

}  // namespace calmcar::xcp::a2l
