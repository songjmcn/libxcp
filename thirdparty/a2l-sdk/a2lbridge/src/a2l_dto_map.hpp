// =============================================================================
// a2l_dto_map.hpp —— liba2l DTO → 桥接层领域类型的显式映射（内部头）
//
// 仅 src/ 使用；这是全工程唯一同时看到 liba2l 头与领域头的翻译单元集合的
// 公共点（R1：SDK 枚举不直接充当领域类型，逐项显式转换）。
// =============================================================================

#ifndef LIBXCP_A2L_SRC_A2L_DTO_MAP_HPP_
#define LIBXCP_A2L_SRC_A2L_DTO_MAP_HPP_

#include <memory>
#include <string>
#include <vector>

#include "liba2l/liba2l_api.hpp"
#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/a2l/daq_layout.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/a2l/if_data_xcp.hpp"

namespace calmcar::xcp::a2l::detail {

/**
 * @brief liba2l::ErrorCode → 桥接层 ErrorCode（B-19）
 * @param sdk_code SDK 整型化错误码
 * @param fallback_phase 无法归因时的默认阶段
 */
[[nodiscard]] Error MapSdkError(liba2l::ErrorCode sdk_code,
                                Phase fallback_phase, std::string_view message);

/// @brief SymbolDto → SymbolInfo（含 B-3 显式类型映射与换算载荷转换）
[[nodiscard]] SymbolInfo ConvertSymbol(const liba2l::SymbolDto& dto);

/// @brief AsamDataTypeDto → 领域类型（Boolean/BitText/String 归 Unknown，B-3）
[[nodiscard]] AsamDataType ConvertDataType(
    liba2l::AsamDataTypeDto dto) noexcept;

/// @brief CharacteristicTypeDto → 领域枚举（B-11）
[[nodiscard]] CharacteristicType ConvertCharType(
    liba2l::CharacteristicTypeDto dto) noexcept;

/// @brief ConversionDto → ConversionInfo（tagged payload，B-8）
[[nodiscard]] ConversionInfo ConvertConversion(
    const liba2l::ConversionDto& dto);

/// @brief IfDataXcpDto → ProtocolLayerInfo + TransportEndpoint
void ConvertIfDataXcp(const liba2l::IfDataXcpDto& dto, ProtocolLayerInfo* layer,
                      std::vector<TransportEndpoint>* transports) noexcept;

/// @brief DaqListDto → DaqListLayout（STATIC 预定义列表冻结，B-5/B-7）
[[nodiscard]] DaqListLayout ConvertDaqList(const liba2l::DaqListDto& dto);

/// @brief DaqCapsDto → DaqInfo（原始码显式枚举映射，禁止序号推断，批次10）
[[nodiscard]] DaqInfo ConvertDaqCaps(const liba2l::DaqCapsDto& dto);

/// @brief EventChannelDto → EventChannelInfo（原始码透传；daq_list_numbers
/// 由调用方回填，批次10）
[[nodiscard]] EventChannelInfo ConvertEventDto(
    const liba2l::EventChannelDto& dto);

/// @brief ByteOrderDto → 领域 ByteOrder（未知值回落 MsbLast 由调用方先行校验）
[[nodiscard]] ByteOrder ConvertByteOrder(liba2l::ByteOrderDto dto) noexcept;

/// @brief 规范键拼接：module::name（B-13/B-17）
[[nodiscard]] std::string MakeQualifiedKey(std::string_view module_name,
                                           std::string_view symbol_name);

/**
 * @class A2lDatabaseImpl
 * @brief 快照数据库前置声明（定义见 a2l_database_impl.cpp）
 */
class A2lDatabaseImpl;

/// @brief 工厂：由 SDK DTO 全量列表构建快照数据库并建立地址反查索引
[[nodiscard]] std::unique_ptr<A2lDatabaseImpl> MakeDatabase(
    std::vector<liba2l::SymbolDto> dtos);

/// @brief 按基地址反查规范符号名；无匹配或歧义地址返回空串（禁止猜测归属）
[[nodiscard]] std::string FindByAddress(const A2lDatabaseImpl& db,
                                        std::uint64_t address);

/// @brief 用数据库地址索引回填布局条目的符号归属（歧义地址留空）
void ResolveDaqListSymbols(std::vector<DaqListLayout>* lists,
                           const A2lDatabaseImpl& db);

/// @brief 工厂：装配冻结布局解码器（门面 CreateDaqLayout 调用）
[[nodiscard]] std::unique_ptr<IDaqLayout> MakeDaqLayout(
    std::vector<DaqListLayout> lists, const IA2lDatabase* db);

/**
 * @brief 由 SDK 快照组装 IF_DATA XCP 领域信息（定义见 if_data_xcp_impl.cpp）
 * @param doc 已加载完成的 SDK 文档对象
 * @param require_if_data_xcp 缺失时是否判为错误（LoadOptions 透传）
 */
[[nodiscard]] Result<IfDataXcpInfo> BuildIfDataXcp(liba2l::IDoc& doc,
                                                   bool require_if_data_xcp);

}  // namespace calmcar::xcp::a2l::detail

#endif  // LIBXCP_A2L_SRC_A2L_DTO_MAP_HPP_
