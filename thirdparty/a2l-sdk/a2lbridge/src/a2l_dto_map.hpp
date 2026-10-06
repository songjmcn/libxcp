// =============================================================================
// a2l_dto_map.hpp —— liba2l DTO → 桥接层领域类型的显式映射（内部头）
//
// 仅 src/ 使用；这是全工程唯一同时看到 liba2l 头与领域头的翻译单元集合的
// 公共点（R1：SDK 枚举不直接充当领域类型，逐项显式转换）。
// =============================================================================

#ifndef LIBXCP_A2L_SRC_A2L_DTO_MAP_HPP_
#define LIBXCP_A2L_SRC_A2L_DTO_MAP_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "liba2l/liba2l_api.hpp"
#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/a2l/daq_layout.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/a2l/if_data_xcp.hpp"
#include "libxcp/a2l/record_layout.hpp"

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

/**
 * @brief StructInfoDto → SymbolInfo（批次13，B-12：STRUCTURE/INSTANCE 元数据）
 * @details 只落"可识别"的字段：kind=Structure、data_type=Unknown、
 *          element_size_bytes=0（不可证 → 0，禁止猜宽度），INSTANCE 的
 *          ECU_ADDRESS 原值进 xcp_address（B-1 口径）。成员清单不展开为
 *          SymbolInfo 字段（首版边界），成员数与引用 TYPEDEF 名写进
 *          description 供展示。
 */
[[nodiscard]] SymbolInfo ConvertStructure(const liba2l::StructInfoDto& dto);

/**
 * @brief SymbolDto 的 RECORD_LAYOUT 字段 → 领域 RecordLayoutInfo（批次13 接线）
 * @param dto SDK 符号快照
 * @param out 命中时填充（kind/name/writable）
 * @return false = SDK 未给出版式类别（kNotProvided：无 DEPOSIT 或模块内查不到
 *         版式）。**此时调用方不得调用可执行性判定器**——"无版式信息"不等于
 *         "版式不可执行"，数值读写仍由 B-3 元素宽度门拒绝。
 */
bool ConvertRecordLayoutInfo(const liba2l::SymbolDto& dto,
                             RecordLayoutInfo* out);

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

/**
 * @brief 工厂：由 SDK DTO 全量列表构建快照数据库并建立地址反查索引
 * @param dtos MEASUREMENT/CHARACTERISTIC 符号快照
 * @param structs TYPEDEF_STRUCTURE/INSTANCE 元数据快照（批次13，B-12）
 * @param record_layouts 规范键 → RECORD_LAYOUT 元数据（批次13 R3 接线；
 *        只含 SDK 给出类别的 CHARACTERISTIC）
 * @param leaf_symbols 结构体成员叶子（批次18，STRUCTLEAF；由
 *        BuildStructureLeaves 解析产出）
 * @param leaf_warnings 叶子解析的不可证拒绝告警（批次18，随快照发布）
 */
[[nodiscard]] std::unique_ptr<A2lDatabaseImpl> MakeDatabase(
    std::vector<liba2l::SymbolDto> dtos,
    std::vector<liba2l::StructInfoDto> structs,
    std::unordered_map<std::string, RecordLayoutInfo> record_layouts,
    std::vector<SymbolInfo> leaf_symbols,
    std::vector<LoadWarning> leaf_warnings);

/// @brief 按基地址反查规范符号名；无匹配或歧义地址返回空串（禁止猜测归属）
[[nodiscard]] std::string FindByAddress(const A2lDatabaseImpl& db,
                                        std::uint64_t address);

/**
 * @brief 按基地址列出全部候选规范名（批次14，B-6 保留 aliases）
 * @details 与 FindByAddress 的区别：歧义地址在本函数里**照单全收**，
 *          归属判定仍交给调用方（不猜与留痕是两件事）。
 */
[[nodiscard]] std::vector<std::string> FindAllByAddress(
    const A2lDatabaseImpl& db, std::uint64_t address);

/// @brief 用数据库地址索引回填布局条目的符号归属（歧义地址留空）
void ResolveDaqListSymbols(std::vector<DaqListLayout>* lists,
                           const A2lDatabaseImpl& db);

/// @brief 工厂：装配冻结布局解码器（门面 CreateDaqLayout 调用）
[[nodiscard]] std::unique_ptr<IDaqLayout> MakeDaqLayout(
    DaqLayoutSnapshot snapshot, const IA2lDatabase* db);

/**
 * @brief 由本端 WRITE_DAQ 账本构建不可变布局快照（批次14，B-6）
 * @param ledger 账本条目（顺序不限，内部按 (daq, odt, entry) 归一化）
 * @param generation 配置代际（来自 `XcpMaster::DaqConfigGeneration()`）
 * @param db 桥接层快照数据库（按地址回填归属符号与 aliases）
 * @details 账本为空时返回"无路由的空快照"，解码一律 `InvalidLayout`——
 *          B-6 的"无实际账本 → 只交 raw，不猜顺序"就落在这里。
 */
[[nodiscard]] DaqLayoutSnapshot BuildSnapshotFromLedger(
    const std::vector<DaqLedgerEntryView>& ledger, std::uint32_t generation,
    const A2lDatabaseImpl& db);

/**
 * @brief 由 ECU 回读（READ_DAQ + 各列表 FIRST_PID）构建快照（B-6，T14-10）
 * @param readback `READ_DAQ` 逐条回读的 Entry（顺序不限，内部归一化）
 * @param list_pids 各列表 FIRST_PID（缺项的列表不生成 PID 路由）
 * @param generation 配置代际
 * @param db 符号数据库（按地址回填归属符号与 aliases）
 * @details 与 `BuildSnapshotFromLedger` 的分工：账本用于**可配置**列表
 *          （本端下发了什么就是什么），回读用于 **PREDEFINED** 列表
 *          （布局由 ECU 固化，只有 READ_DAQ 能证）。两者都产出 `routes`，
 *          解码一律按 PID → 单个 ODT 路由。回读为空 → 空快照 → 解码
 *          `InvalidLayout`（B-6 不猜）。
 */
[[nodiscard]] DaqLayoutSnapshot BuildSnapshotFromEcuReadback(
    const std::vector<DaqReadbackEntry>& readback,
    const std::vector<DaqListPid>& list_pids, std::uint32_t generation,
    const A2lDatabaseImpl& db);

/**
 * @brief 由 SDK 快照组装 IF_DATA XCP 领域信息（定义见 if_data_xcp_impl.cpp）
 * @param doc 已加载完成的 SDK 文档对象
 * @param require_if_data_xcp 缺失时是否判为错误（LoadOptions 透传）
 */
[[nodiscard]] Result<IfDataXcpInfo> BuildIfDataXcp(liba2l::IDoc& doc,
                                                   bool require_if_data_xcp);

}  // namespace calmcar::xcp::a2l::detail

#endif  // LIBXCP_A2L_SRC_A2L_DTO_MAP_HPP_
