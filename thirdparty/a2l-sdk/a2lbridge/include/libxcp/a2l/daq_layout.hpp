// =============================================================================
// daq_layout.hpp —— DAQ 能力、Event Channel 与 DTO 布局（设计文档 §4.4）
//
// 本头文件被 if_data_xcp.hpp 复用（DaqInfo/EventChannelInfo 前置于其中），
// 因此不依赖 SDK：领域类型独立定义，映射在 src/daq_layout_impl.cpp。
// =============================================================================

#ifndef LIBXCP_A2L_DAQ_LAYOUT_HPP_
#define LIBXCP_A2L_DAQ_LAYOUT_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::a2l {

/**
 * @brief DAQ 处理器能力（Allocator 的输入约束）
 * @details 首里程碑只启用 STATIC 路径（B-5）；dynamic_supported 仅保留能力位，
 *          使用动态配置时返回 UnsupportedOperation。
 */
struct DaqInfo {
    bool static_supported = false;   ///< 首里程碑唯一启用的执行路径（B-5）
    bool dynamic_supported = false;  ///< 仅保留能力；DynamicDaqNotImplemented
    std::uint16_t max_daq = 0;       ///< MAX_DAQ
    std::uint16_t max_event_channel = 0;  ///< MAX_EVENT_CHANNEL
    std::uint8_t min_daq = 0;             ///< MIN_DAQ
    std::uint8_t odt_entry_min_size_bytes =
        1;                                ///< GRANULARITY_ODT_ENTRY_SIZE_DAQ
    std::uint8_t max_odt_entry_size = 0;  ///< MAX_ODT_ENTRY_SIZE_DAQ

    /// @brief OPTIMISATION_TYPE → ALLOCATOR 打包对齐策略
    enum class OdtType : std::uint8_t {
        Default,
        Odt16,
        Odt32,
        Odt64,
        Alignment,
        MaxEntrySize
    };
    /// @brief ADDRESS_EXTENSION 声明模式
    enum class AddrExtMode : std::uint8_t { Free, PerOdt, PerDaq };
    /// @brief IDENTIFICATION_FIELD_TYPE（DTO envelope 判定依据）
    enum class IdFieldType : std::uint8_t {
        Absolute,
        RelativeByte,
        RelativeWord,
        RelativeWordAligned
    };

    OdtType odt_type = OdtType::Default;  ///< OPTIMISATION_TYPE
    AddrExtMode address_extension_mode =
        AddrExtMode::Free;  ///< ADDRESS_EXTENSION
    IdFieldType identification_field_type =
        IdFieldType::Absolute;             ///< IDENTIFICATION_FIELD_TYPE
    bool dto_counter_supported = false;    ///< DTO_CTR_SUPPORTED
    bool pid_off_supported = false;        ///< PID_OFF_SUPPORTED
    bool prescaler_supported = false;      ///< PRESCALER_SUPPORTED
    bool resume_supported = false;         ///< RESUME_SUPPORTED
    bool overflow_flag_supported = false;  ///< OVERLOAD_INDICATION != NONE
    std::optional<std::uint32_t>
        timestamp_max_size_bits;  ///< TIMESTAMP_SIZE（影响可用载荷）
};

/**
 * @brief Event Channel 描述（含 TIMING 与 DAQ_LIST 引用）
 * @details 批次10：time_cycle/time_unit 为 A2L/XCP 原始码；本仓库暂无
 * TIME_UNIT 码表（XCP 文档仅说明周期 = TIME_CYCLE × TIME_UNIT），因此
 * cycle_time_us 当前保持 0.0，换算待码表落地后实现（不臆造系数）。
 */
struct EventChannelInfo {
    std::string name;        ///< EVENT_CHANNEL 名字（DAQ_EVENT 引用它）
    std::string short_name;  ///< 短名（A2L short_name 字段，批次10）
    std::uint16_t channel_number = 0;  ///< 通道号
    std::uint8_t type = 1;  ///< 1=DAQ 2=STIM 3=DAQ_STIM（原始码，批次10）
    std::uint8_t max_daq_list =
        0;  ///< MAX_DAQ_LIST（0=暂不可分配，0xFF=无限制）
    double cycle_time_us =
        0.0;  ///< MIN_CYCLE_TIME / CYCLE_TIME（当前恒 0，见类注释）
    std::uint8_t time_cycle = 0;  ///< TIME_CYCLE 原始码（批次10）
    std::uint8_t time_unit = 0;   ///< TIME_UNIT 原始码（批次10）
    std::uint8_t priority = 0;    ///< 优先级（0xFF 最高，原始码）
    std::optional<std::uint8_t>
        consistency;  ///< CONSISTENCY 原始码（0=DAQ 1=EVENT 2=ODT 3=NONE）
    std::vector<std::uint16_t>
        daq_list_numbers;          ///< 该事件下要 START 的 DAQ LIST
    bool has_consistency = false;  ///< 是否声明 CONSISTENCY
};

/**
 * @brief 一条 DTO 条目的解析结果（供 DtoDecoder 回调使用）
 */
struct DecodedDtoSample {
    std::string symbol_name;  ///< 来自布局快照的符号名（module::name）
    std::vector<std::string>
        symbol_aliases;  ///< 同址全部候选（批次14，B-6 保留 aliases；
                         ///< symbol_name 为空时靠它提示"可能是哪些符号"）
    std::uint64_t address = 0;  ///< 条目基地址（ECU_ADDRESS 语义，B-1）
    std::uint8_t address_extension = 0;  ///< 独立扩展字节（B-1）
    Bytes raw;  ///< DTO 中的实际原始字节；AG 不改变字节内容
    PhysicalValue
        physical_value;         ///< 已套用 COMPU_METHOD 的 tagged value（B-14）
                                ///< **仅当 physical_valid 为真时可信**（F2）
    std::optional<bool> valid;  ///< 质量状态；首版不由 ERROR_MASK 猜测（B-9）
    /**
     * @brief `physical_value` 是否为本次真实换算结果（批次15，F2）
     * @details `PhysicalValue` 是 `std::variant`，其**默认构造值就是首选项
     *          `int64_t{0}`**。没有本标志时，"根本没换算"与"物理值恰好是 0"在
     *          消费侧完全不可区分 —— 界面会显示一排合法的 0。
     *          因此消费方**必须**先判本标志再取 `physical_value`；为 false 时
     *          只应使用 `raw`（B-8 不静默降级）。
     *          置 false 的已知情形：entry 无归属符号、位元素（`bit_offset≠0`，
     *          B-9）、entry 宽度与符号元素宽不等、多维数组、COMPU 换算失败。
     */
    bool physical_valid = false;
};

/**
 * @brief DTO envelope 的运行时冻结布局（B-7）
 * @details envelope
 * 头部字节数由这些标志推导；布局冲突时整帧拒绝（InvalidLayout）。
 */
struct DtoFrameLayout {
    DaqInfo::IdFieldType identification_field_type =
        DaqInfo::IdFieldType::Absolute;    ///< 标识字段类型
    bool first_odt = false;                ///< 携带 FIRST_ODT 字段
    bool counter_enabled = false;          ///< 携带 DTO 计数器
    bool timestamp_enabled = false;        ///< 携带时间戳
    std::uint8_t timestamp_size_bits = 0;  ///< 时间戳位宽（TIMESTAMP_SIZE）
    bool overflow_indicator = false;       ///< 携带 OVERLOAD 指示位
    bool pid_off =
        false;  ///< PID_OFF：识别字段缺席（批次11，对齐设计 §4.4，B-7）
    std::uint8_t header_bytes =
        1;  ///< envelope 固定头（PID）字节数，冻结后不可变
};

/// @brief 一个 ODT 内单条 entry 的冻结信息（布局快照内部元素）
struct OdtEntryLayout {
    std::uint8_t number = 0;  ///< entry 序号（1 基，A2L 源序）
    std::string symbol_name;  ///< 反查到的规范符号名；空表示无匹配（禁止猜测）
    std::uint64_t address = 0;           ///< ECU_ADDRESS 原值（B-1）
    std::uint8_t address_extension = 0;  ///< 独立扩展字节
    std::uint8_t size_bytes = 0;         ///< SIZE（字节数）
    std::uint8_t bit_offset = 0;  ///< BIT_OFFSET（B-9，与 BIT_MASK 分离）
    /**
     * @brief 同址全部候选符号名（批次14，B-6"保留 aliases"）
     * @details 基址相同的多符号（如联合体式共用地址）不构成"选一个"的理由：
     *          `symbol_name` 在歧义时保持空（不猜），但候选名单必须留痕，
     *          供上层提示"这条 entry 可能是下列符号之一"。
     */
    std::vector<std::string> symbol_aliases;
    /// @brief OVERLOAD 指示（本帧该 ODT 溢出；批次11 仅承载，值不可信）
    std::optional<bool> overflow{};
    /**
     * @brief `physical_value` 是否为本次真实换算结果（批次15，F2）
     * @details `PhysicalValue` 是 `std::variant`，其**默认构造值即首选项
     *          `int64_t{0}`**。没有本标志时，"根本没换算"与"物理值恰好是 0"
     *          在消费侧完全不可区分 —— 界面会显示一排合法的 0。
     *          因此消费方**必须**先判本标志再取 `physical_value`；为 false 时
     *          只应使用 `raw`（B-8 不静默降级）。
     *          置 false 的已知情形：entry 无归属符号、位元素（`bit_offset≠0`，
     *          B-9）、entry 宽度与符号元素宽不等、多维数组、COMPU 换算失败。
     */
    bool physical_valid = false;
};

/// @brief 一个 ODT 的冻结布局
struct OdtLayout {
    std::uint8_t number = 0;              ///< ODT 序号（1 基）
    std::vector<OdtEntryLayout> entries;  ///< 按 A2L 源顺序排列的条目
};

/// @brief 一个 DAQ_LIST 的冻结布局（STATIC 预定义列表）
struct DaqListLayout {
    std::uint16_t number = 0;     ///< EPK / DAQ 列表号
    std::vector<OdtLayout> odts;  ///< 预定义 ODT 序列
    std::optional<std::uint16_t>
        event_fixed;  ///< EVENT_FIXED（固定事件通道号；批次10，事件反查用）
    /// @brief FIRST_PID（批次14；Absolute ODT Number 下该列表首 ODT 的 PID）
    /// @details 批次15（F1/D1）起，**A2L 声明的 FIRST_PID 也会填这里并参与路由**
    ///          （此前它被丢弃，解码只能落回"PID == 列表号"，而列表号与别人的
    ///          PID 同属一个编号空间，撞上即静默用错布局 —— 批次15 记录 §1 L1）。
    ///          三类来源都填：A2L IF_DATA、本端账本（START_STOP_DAQ_LIST 响应）、
    ///          ECU 回读。有值即进 PID 路由表；无值的 A2L 列表才保留"列表号回退"。
    std::optional<std::uint8_t> first_pid;
};

/// @brief 布局快照的数据来源（B-6：解码必须知道自己用的是哪一类权威）
enum class DaqLayoutSource : std::uint8_t {
    A2lPredefined,  ///< 仅 A2L IF_DATA PREDEFINED（无实际下发证据）
    LocalLedger,    ///< 本端 SET_DAQ_PTR/WRITE_DAQ 账本（可配置 STATIC 权威）
    EcuReadback,    ///< GET_DAQ_LIST_INFO / READ_DAQ 回读（PREDEFINED 取证）
};

/**
 * @brief PID → (DAQ List, ODT) 路由表项（批次14，B-6）
 * @details XCP 的 DTO 识别字段在 Absolute ODT Number 下是**全局 ODT 号**
 *          （`绝对 ODT 号 = FIRST_PID + 相对 ODT 号`，
 *          docs/XCP_1.3.0_document.md L2225），因此解码必须以 PID 为键定位到
 *          **单个 ODT**，而不是"整个 DAQ List 的净荷"。
 */
struct DaqOdtRoute {
    std::uint8_t pid = 0;         ///< 该 ODT 的 PID
    std::uint16_t daq_list = 0;   ///< DAQ List 号（EPK）
    std::uint8_t odt_number = 0;  ///< ODT 号（0 基）
};

/**
 * @brief 不可变 DAQ 布局快照（设计 §4.4 / B-6，批次14 落地为真实类型）
 *
 * @details B-6 口径：
 *   - **可配置 STATIC** 以本端 WRITE_DAQ 账本为权威（`source=LocalLedger`，
 *     `routes` 非空）；
 *   - **PREDEFINED** 优先取 ECU 回读（`source=EcuReadback`）；
 *   - A2L 的 `static_daq_lists`
 * 只作候选与一致性约束（`source=A2lPredefined`）；
 *   - 账本缺失（`LocalLedger` 却无 `routes`）或 PID 无路由 →
 *     `IDaqLayout::Decode` 返回 `InvalidLayout`，**不猜符号与顺序**；
 *   - `generation` 与 `XcpMaster::DaqConfigGeneration()` 对齐：不为 0 且与
 *     当前代际不一致时，说明 ECU 配置已被 CLEAR/重写，快照即陈旧，同样拒绝。
 */
struct DaqLayoutSnapshot {
    DaqLayoutSource source = DaqLayoutSource::A2lPredefined;  ///< 来源
    std::uint32_t generation = 0;      ///< 配置代际（0 = 未知/不适用）
    std::vector<DaqListLayout> lists;  ///< EPK → ODT/Entry 序列
    std::vector<DaqOdtRoute> routes;   ///< PID → (daq, odt)；账本/回读才有
};

/**
 * @brief 本端 WRITE_DAQ 账本条目的**桥接侧视图**（批次14，B-6）
 *
 * @details 为什么要有这个独立类型：桥接层与 libxcp 是对等库（libxcp 禁止依赖
 *          桥接层，桥接层也不 include libxcp 头，见 a2lbridge/CMakeLists.txt
 *          的 P9 断言）。因此账本从 `XcpMaster::DaqLedger()` 交给上层后，由
 *          **调用方**逐条转成本视图（字段一一对应；AG 换算在调用方完成：
 *          `size_bytes = size_elements × AG`），桥接层只吃这份纯数据。
 *          构建入口见 `A2lBridge::CreateDaqLayoutFromLedger`。
 */
struct DaqLedgerEntryView {
    std::uint16_t daq_list = 0;          ///< DAQ List 号（EPK）
    std::uint8_t odt_number = 0;         ///< ODT 号（0 基，SET_DAQ_PTR 用值）
    std::uint8_t odt_entry = 0;          ///< ODT Entry 号（0 基）
    std::uint64_t address = 0;           ///< 地址原值（B-1：不乘 AG）
    std::uint8_t address_extension = 0;  ///< 独立扩展字节
    std::uint8_t size_bytes = 0;         ///< 该 Entry 的字节数（已按 AG 换算）
    /// @brief 位偏；**允许直接透传 XCP 原值**（批次15，F3）
    /// @details XCP 的 WRITE_DAQ/READ_DAQ 用 0xFF 表示"无位偏"（docs L1861），
    ///          A2L/桥接层用 0 表示无位偏。归一在 `BuildSnapshotFromLedger`
    ///          入口侧完成，调用方**不需要**自己转换 —— 此前要求调用方转换，
    ///          忘转的后果是整帧物理值静默不换算（批次14 的 E2E 真实踩过）。
    ///          非 0xFF 且非 0 的值按"真位元素"处理：保留 raw、不猜位移（B-9）。
    std::uint8_t bit_offset = 0;
    std::optional<std::uint8_t> pid;  ///< 该 ODT 的 PID（未 START 则空）
};

/**
 * @brief 各 DAQ List 的 FIRST_PID（`GET_DAQ_LIST_INFO`/`START_STOP_DAQ_LIST`
 *        回读结果；B-6 的 EcuReadback 权威之一）
 */
struct DaqListPid {
    std::uint16_t daq_list = 0;  ///< DAQ List 号（EPK）
    std::uint8_t first_pid = 0;  ///< 该列表首个 ODT 的 PID
};

/**
 * @brief `READ_DAQ` 回读到的一条 ODT Entry（批次14，T14-10）
 * @details 字段与 libxcp `ReadDaqResponse` 一一对应；`size_bytes` 由调用方按
 *          AG 换算（`size_elements × AG`），位偏口径与账本视图一致
 *          （**0 = 无位偏**，XCP 的 0xFF 由调用方归一）。
 */
struct DaqReadbackEntry {
    std::uint16_t daq_list = 0;          ///< DAQ List 号（EPK）
    std::uint8_t odt_number = 0;         ///< ODT 号（0 基）
    std::uint8_t odt_entry = 0;          ///< Entry 号（0 基）
    std::uint64_t address = 0;           ///< 地址原值（B-1）
    std::uint8_t address_extension = 0;  ///< 独立扩展字节
    std::uint8_t size_bytes = 0;         ///< 字节数（已按 AG 换算）
    std::uint8_t bit_offset = 0;         ///< 位偏（0 = 无）
};

/**
 * @class IDaqLayout
 * @brief 冻结布局上的 DTO 解码器（STATIC 路径，B-5/B-7）。
 */
class IDaqLayout {
public:
    IDaqLayout() = default;
    virtual ~IDaqLayout() = default;

    IDaqLayout(const IDaqLayout&) = delete;
    IDaqLayout& operator=(const IDaqLayout&) = delete;
    IDaqLayout(IDaqLayout&&) = delete;
    IDaqLayout& operator=(IDaqLayout&&) = delete;

    /**
     * @brief 解码一帧 DTO
     * @param frame_layout envelope 冻结布局
     * @param dto 完整 DTO 字节（含 envelope 头）
     * @return 每个可归属 entry 的解码样本；envelope 长度与布局不符时
     *         返回 InvalidLayout（整帧拒绝，不做部分解释）
     */
    [[nodiscard]] virtual Result<std::vector<DecodedDtoSample>> Decode(
        const DtoFrameLayout& frame_layout, BytesView dto) const = 0;

    /**
     * @brief 计算指定符号集合打包后的净字节数（不含 envelope 头）
     * @param names 规范键或全库唯一裸名列表（须同属一个已冻结 DAQ_LIST）
     */
    [[nodiscard]] virtual Result<std::size_t> PackedByteSize(
        const std::vector<std::string>& names) const = 0;

    /**
     * @brief 本解码器所依据的布局代际（批次15，F4；B-6 陈旧快照抓手）
     * @return 构建快照时传入的 `generation`（A2L 侧来源恒 0 = 不适用）
     * @details 为什么只给取值器而不在 `Decode` 内部比对：桥接层不持有 ECU
     *          句柄，无法自知"现在该是第几代"，硬比对只能靠把代际塞进
     *          `Decode` 签名（改公开接口且影响既有解码用例）。因此这里给出
     *          **一行可写的调用方检查**：取布局时记下代际，`CLEAR_DAQ_LIST`
     *          或重连后重新取 `DaqConfigGeneration()`，两者不等就必须重建解码器
     *          —— 否则旧解码器会按旧布局静默出值（批次15 记录 §1 L4）。
     */
    [[nodiscard]] virtual std::uint32_t Generation() const noexcept = 0;
};

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_DAQ_LAYOUT_HPP_
